// Copyright 2009-2016 Sandia Corporation. Under the terms
// of Contract DE-AC04-94AL85000 with Sandia Corporation, the U.S.
// Government retains certain rights in this software.
// 
// Copyright (c) 2009-2016, Sandia Corporation
// All rights reserved.
// 
// This file is part of the SST software package. For license
// information, see the LICENSE file in the top level directory of the
// distribution.


#include <sst_config.h>
#ifdef SST_CONFIG_HAVE_PYTHON
#include <Python.h>
#endif

#include "sst/core/serialization.h"


#ifdef SST_CONFIG_HAVE_MPI
#include <mpi.h>
#endif

#include <iomanip>
#include <iostream>
#include <fstream>
#include <cinttypes>
#include <signal.h>

#include <sst/core/activity.h>
#include <sst/core/archive.h>
#include <sst/core/config.h>
#include <sst/core/configGraph.h>
#include <sst/core/factory.h>
#include <sst/core/rankInfo.h>
#include <sst/core/simulation.h>
#include <sst/core/timeLord.h>
#include <sst/core/timeVortex.h>
#include <sst/core/part/sstpart.h>
#include <sst/core/statapi/statoutput.h>

#include <sst/core/cputimer.h>

#include <sst/core/model/sstmodel.h>
#include <sst/core/model/pymodel.h>
#include <sst/core/memuse.h>
#include <sst/core/iouse.h>

#include <sys/resource.h>
#include <sst/core/interfaces/simpleNetwork.h>

#include <sst/core/objectComms.h>

// Configuration Graph Generation Options
#include <sst/core/configGraphOutput.h>
#include <sst/core/cfgoutput/pythonConfigOutput.h>
#include <sst/core/cfgoutput/dotConfigOutput.h>
#include <sst/core/cfgoutput/xmlConfigOutput.h>
#include <sst/core/cfgoutput/jsonConfigOutput.h>

using namespace SST::Core;
using namespace SST::Partition;
using namespace std;
using namespace SST;



static SST::Output g_output;



static void
SimulationSigHandler(int sig)
{
    Simulation::setSignal(sig);
    if ( sig == SIGINT || sig == SIGTERM ) {
        signal(sig, SIG_DFL); // Restore default handler
    }
}


static void setupSignals(uint32_t threadRank)
{
    if ( 0 == threadRank ) {
		if(SIG_ERR == signal(SIGUSR1, SimulationSigHandler)) {
			g_output.fatal(CALL_INFO, -1, "Installation of SIGUSR1 signal handler failed.\n");
		}
		if(SIG_ERR == signal(SIGUSR2, SimulationSigHandler)) {
			g_output.fatal(CALL_INFO, -1, "Installation of SIGUSR2 signal handler failed\n");
		}
		if(SIG_ERR == signal(SIGINT, SimulationSigHandler)) {
			g_output.fatal(CALL_INFO, -1, "Installation of SIGINT signal handler failed\n");
		}
		if(SIG_ERR == signal(SIGTERM, SimulationSigHandler)) {
			g_output.fatal(CALL_INFO, -1, "Installation of SIGTERM signal handler failed\n");
		}

		g_output.verbose(CALL_INFO, 1, 0, "Signal handler registration is completed\n");
    } else {
        /* Other threads don't want to receive the signal */
        sigset_t maskset;
        sigfillset(&maskset);
        pthread_sigmask(SIG_BLOCK, &maskset, NULL);
    }
}


static void dump_partition(Config& cfg, ConfigGraph* graph, const RankInfo &size) {

	///////////////////////////////////////////////////////////////////////	
	// If the user asks us to dump the partionned graph.
	if(cfg.dump_component_graph_file != "") {
		if(cfg.verbose) {
			g_output.verbose(CALL_INFO, 1, 0,
				"# Dumping partitionned component graph to %s\n",
				cfg.dump_component_graph_file.c_str());
		}

        ofstream graph_file(cfg.dump_component_graph_file.c_str());
        ConfigComponentMap_t& component_map = graph->getComponentMap();

        for(uint32_t i = 0; i < size.rank; i++) {
            for ( uint32_t t = 0 ; t < size.thread ; t++ ) {
                graph_file << "Rank: " << i << "." << t << " Component List:" << std::endl;

                RankInfo r(i, t);
                for (ConfigComponentMap_t::const_iterator j = component_map.begin() ; j != component_map.end() ; ++j) {
                    if(j->rank == r) {
                        graph_file << "   " << j->name << " (ID=" << j->id << ")" << std::endl;
                        graph_file << "      -> type      " << j->type << std::endl;
                        graph_file << "      -> weight    " << j->weight << std::endl;
                        graph_file << "      -> linkcount " << j->links.size() << std::endl;
                        graph_file << "      -> rank      " << j->rank.rank << std::endl;
                        graph_file << "      -> thread    " << j->rank.thread << std::endl;
                    }
                }
            }
        }

        graph_file.close();

        if(cfg.verbose) {
            g_output.verbose(CALL_INFO, 2, 0,
                    "# Dump of partition graph is complete.\n");
        }
    }
}

static void do_graph_wireup(ConfigGraph* graph,
        SST::Simulation* sim, SST::Config* cfg, const RankInfo &world_size,
        const RankInfo &myRank, SimTime_t min_part) {

    if ( !graph->containsComponentInRank( myRank ) ) {
        g_output.output("WARNING: No components are assigned to rank: %u.%u\n", 
                myRank.rank, myRank.thread);
    }

    std::vector<ConfigGraphOutput*> graphOutputs;

    // User asked us to dump the config graph to a file in Python
    if(cfg->output_config_graph != "") {
        graphOutputs.push_back( new PythonConfigGraphOutput(cfg->output_config_graph.c_str()) );
    }

    // user asked us to dump the config graph in dot graph format
    if(cfg->output_dot != "") {
        graphOutputs.push_back( new DotConfigGraphOutput(cfg->output_dot.c_str()) );
    }

    // User asked us to dump the config graph in XML format (for energy experiments)
    if(cfg->output_xml != "") {
        graphOutputs.push_back( new XMLConfigGraphOutput(cfg->output_xml.c_str()) );
    }

    // User asked us to dump the config graph in JSON format (for OCCAM experiments)
    if(cfg->output_json != "") {
        graphOutputs.push_back( new JSONConfigGraphOutput(cfg->output_json.c_str()) );
    }

    for(size_t i = 0; i < graphOutputs.size(); i++) {
        graphOutputs[i]->generate(cfg, graph);
        delete graphOutputs[i];
    }

    sim->performWireUp( *graph, myRank, min_part );

}



typedef struct {
    RankInfo myRank;
    RankInfo world_size;
    Config *config;
    ConfigGraph *graph;
    SimTime_t min_part;

    // Time / stats information
    double build_time;
    double run_time;
    UnitAlgebra simulated_time;
    uint64_t max_tv_depth;
    uint64_t current_tv_depth;
    uint64_t sync_data_size;

} SimThreadInfo_t;


static void start_simulation(uint32_t tid, SimThreadInfo_t &info, Core::ThreadSafe::Barrier &barrier)
{
    info.myRank.thread = tid;
    double start_build = sst_get_cpu_time();

    if ( tid ) {
        /* already did Thread Rank 0 in main() */
        setupSignals(tid);
    }

    ////// Create Simulation Objects //////
    SST::Simulation* sim = Simulation::createSimulation(info.config, info.myRank, info.world_size, info.min_part);

    barrier.wait();

    sim->processGraphInfo( *info.graph, info.myRank, info.min_part );

    barrier.wait();
    
    // Perform the wireup.  Do this one thread at a time for now.  If
    // this ever changes, then need to put in some serialization into
    // performWireUp.
    for ( uint32_t i = 0; i < info.world_size.thread; ++i ) {
        if ( i == info.myRank.thread ) {
            // g_output.output("wiring up this thread %u\n", info.myRank.thread);
            do_graph_wireup(info.graph, sim, info.config, info.world_size, info.myRank, info.min_part);
        }
        barrier.wait();
    }

    barrier.wait();
    if ( tid == 0 ) {
        delete info.graph;
    }

    double start_run = sst_get_cpu_time();
    info.build_time = start_run - start_build;

#ifdef SST_CONFIG_HAVE_MPI
    if ( tid == 0 && info.world_size.rank > 1 ) {
        MPI_Barrier(MPI_COMM_WORLD);
    }
    barrier.wait();
#endif

    if ( info.config->runMode == Simulation::RUN || info.config->runMode == Simulation::BOTH ) {
        if ( info.config->verbose && 0 == tid ) {
            g_output.verbose(CALL_INFO, 1, 0, "# Starting main event loop\n");

            time_t the_time = time(0);
            struct tm* now = localtime( &the_time );

            g_output.verbose(CALL_INFO, 1, 0, "# Start time: %04u/%02u/%02u at: %02u:%02u:%02u\n",
                    (now->tm_year + 1900), (now->tm_mon+1), now->tm_mday,
                    now->tm_hour, now->tm_min, now->tm_sec);
        }
        // g_output.output("info.config.stopAtCycle = %s\n",info.config->stopAtCycle.c_str());
        sim->setStopAtCycle(info.config);

        if ( tid == 0 && info.world_size.rank > 1 ) {
            // If we are a MPI_parallel job, need to makes sure that all used
            // libraries are loaded on all ranks.
#ifdef SST_CONFIG_HAVE_MPI
            set<string> lib_names;
            set<string> other_lib_names;
            Factory::getFactory()->getLoadedLibraryNames(lib_names);
            // vector<set<string> > all_lib_names;

            // Send my lib_names to the next lowest rank
            if ( info.myRank.rank == (info.world_size.rank - 1) ) {
                Comms::send(info.myRank.rank - 1, 0, lib_names);
                lib_names.clear();
            }
            else {
                Comms::recv(info.myRank.rank + 1, 0, other_lib_names);
                for ( auto iter = other_lib_names.begin();
                        iter != other_lib_names.end(); ++iter ) {
                    lib_names.insert(*iter);
                }
                if ( info.myRank.rank != 0 ) {
                    Comms::send(info.myRank.rank - 1, 0, lib_names);
                    lib_names.clear();
                }
            }

            Comms::broadcast(lib_names, 0);
            Factory::getFactory()->loadUnloadedLibraries(lib_names);
#endif
        }
        barrier.wait();
        
        sim->initialize();

        barrier.wait();
        
        /* Run Simulation */
        sim->setup();
        barrier.wait();

        if ( 0 == info.myRank.thread )
            Simulation::signalStatisticsBegin();
        barrier.wait();

        /* Run Simulation */
        sim->run();
    // fprintf(stderr, "thread %u waiting on run finish barrier\n", tid);
        barrier.wait();
    // fprintf(stderr, "thread %u release from run finish barrier\n", tid);

        sim->finish();
    // fprintf(stderr, "thread %u waiting on finish() finish barrier\n", tid);
        barrier.wait();
    // fprintf(stderr, "thread %u release from finish() finish barrier\n", tid);

        // Tell the Statistics Output that the simulation is finished
        if ( 0 == info.myRank.thread )
            Simulation::signalStatisticsEnd();
    }

    barrier.wait();

    info.simulated_time = sim->getFinalSimTime();
    // g_output.output(CALL_INFO,"Simulation time = %s\n",info.simulated_time.toStringBestSI().c_str());
    
    double end_time = sst_get_cpu_time();
    info.run_time = end_time - start_run;

    info.max_tv_depth = sim->getTimeVortexMaxDepth();
    info.current_tv_depth = sim->getTimeVortexCurrentDepth();
//    info.sync_data_size = sim->getSyncQueueDataSize();

    delete sim;

}

int
main(int argc, char *argv[])
{

    
#ifdef SST_CONFIG_HAVE_MPI
    MPI_Init(&argc, &argv);

    int myrank = 0;
    int mysize = 0;
    MPI_Comm_rank(MPI_COMM_WORLD, &myrank);
    MPI_Comm_size(MPI_COMM_WORLD, &mysize);

    RankInfo world_size(mysize, 1);
    RankInfo myRank(myrank, 0);
#else
    int myrank = 0;
    RankInfo world_size(1, 1);
    RankInfo myRank(0, 0);
#endif
    Config cfg(world_size);


    // All ranks parse the command line
    if ( cfg.parseCmdLine(argc, argv) ) {
        return -1;
    }
    world_size.thread = cfg.getNumThreads();

    SSTModelDescription* modelGen = 0;

    if ( cfg.sdlfile != "NONE" ) {
        string file_ext = "";

        if(cfg.sdlfile.size() > 3) {
            file_ext = cfg.sdlfile.substr(cfg.sdlfile.size() - 3);

            if(file_ext == "xml" || file_ext == "sdl") {
                cfg.model_options = cfg.sdlfile;
                cfg.sdlfile = SST_INSTALL_PREFIX "/libexec/xmlToPython.py";
                file_ext = ".py";
            }
            if(file_ext == ".py") {
                modelGen = new SSTPythonModelDefinition(cfg.sdlfile, cfg.verbose, &cfg);
            }
            else {
                std::cerr << "Unsupported SDL file type: " << file_ext << std::endl;
                return -1;
            }
        } else {
            return -1;
        }

    }

    double start = sst_get_cpu_time();

    /* Build objected needed for startup */
    Factory *factory = new Factory(cfg.getLibPath());
    Output::setWorldSize(world_size, myrank);
    g_output = Output::setDefaultObject(cfg.output_core_prefix, cfg.getVerboseLevel(), 0, Output::STDOUT);

    
    g_output.verbose(CALL_INFO, 1, 0, "#main() My rank is (%u.%u), on %u/%u nodes/threads\n", myRank.rank,myRank.thread, world_size.rank, world_size.thread);

    // Get the memory before we create the graph
    const uint64_t pre_graph_create_rss = maxGlobalMemSize();

    ////// Start ConfigGraph Creation //////
    ConfigGraph* graph = NULL;

    double start_graph_gen = sst_get_cpu_time();
    graph = new ConfigGraph();

    // Only rank 0 will populate the graph
    if ( myRank.rank == 0 ) {
        if ( cfg.generator != "NONE" ) {
            generateFunction func = factory->GetGenerator(cfg.generator);
            func(graph,cfg.generator_options, world_size.rank);
        } else {
            graph = modelGen->createConfigGraph();
        }
    }
    
#ifdef SST_CONFIG_HAVE_MPI
    // Config is done - broadcast it
    if ( world_size.rank > 1 ) {
        Comms::broadcast(cfg, 0);
    }
#endif

    // Need to initialize TimeLord before we use UnitAlgebra
    Simulation::getTimeLord()->init(cfg.timeBase);

    if ( myRank.rank == 0 ) {
        graph->postCreationCleanup();

        // Check config graph to see if there are structural errors.
        if ( graph->checkForStructuralErrors() ) {
            g_output.fatal(CALL_INFO, -1, "Structure errors found in the ConfigGraph.\n");
        }
    }

    // Delete the model generator
    delete modelGen;
    modelGen = NULL;

    double end_graph_gen = sst_get_cpu_time();

    if ( myRank.rank == 0 ) {
        g_output.verbose(CALL_INFO, 1, 0, "# ------------------------------------------------------------\n");
        g_output.verbose(CALL_INFO, 1, 0, "# Graph construction took %f seconds.\n",
                (end_graph_gen - start_graph_gen));
    }

    ////// End ConfigGraph Creation //////


    ////// Start Partitioning //////
    double start_part = sst_get_cpu_time();

    // If this is a serial job, just use the single partitioner,
    // but the same code path
    if ( world_size.rank == 1 && world_size.thread == 1) cfg.partitioner = "single";
    SSTPartitioner* partitioner = SSTPartitioner::getPartitioner(cfg.partitioner, world_size, myRank, cfg.verbose);
    if ( partitioner == NULL ) {
        // Not a built in partitioner, see if this is a
        // partitioner contained in an element library.
        partitionFunction func = factory->GetPartitioner(cfg.partitioner);
        partitioner = func(world_size, myRank, cfg.verbose);
    }

    if ( partitioner->requiresConfigGraph() ) {
        partitioner->performPartition(graph);
    }
    else {
        PartitionGraph* pgraph;
        if ( myRank.rank == 0 ) {
            pgraph = graph->getCollapsedPartitionGraph();
        }
        else {
            pgraph = new PartitionGraph();
        }

        if ( myRank.rank == 0 || partitioner->spawnOnAllRanks() ) {
            partitioner->performPartition(pgraph);

            if ( myRank.rank == 0 ) graph->annotateRanks(pgraph);
        }

        delete pgraph;
    }

    delete partitioner;

    // Check the partitioning to make sure it is sane
    if ( myRank.rank == 0 ) {
        if ( !graph->checkRanks( world_size ) ) {
            g_output.fatal(CALL_INFO, 1,
                    "ERROR: Bad partitionning; partition included unknown ranks.\n");
        }
    }
    double end_part = sst_get_cpu_time();
    const uint64_t post_graph_create_rss = maxGlobalMemSize();

    if(myRank.rank == 0) {
        g_output.verbose(CALL_INFO, 1, 0, "# Graph partitioning took %lg seconds.\n", (end_part - start_part));
        g_output.verbose(CALL_INFO, 1, 0, "# Graph construction and partition raised RSS by %" PRIu64 " KB\n",
                (post_graph_create_rss - pre_graph_create_rss));
        g_output.verbose(CALL_INFO, 1, 0, "# ------------------------------------------------------------\n");


        // Output the partition information is user requests it
        dump_partition(cfg, graph, world_size);
    }

    ////// End Partitioning //////

    ////// Calculate Minimum Partitioning //////
    SimTime_t min_part = 0xffffffffffffffffl;
    if ( world_size.rank > 1 ) {
        // Check the graph for the minimum latency crossing a partition boundary
        if ( myRank.rank == 0 ) {
            ConfigComponentMap_t comps = graph->getComponentMap();
            ConfigLinkMap_t links = graph->getLinkMap();
            // Find the minimum latency across a partition
            for( ConfigLinkMap_t::iterator iter = links.begin();
                    iter != links.end(); ++iter ) {
                ConfigLink &clink = *iter;
                RankInfo rank[2];
                rank[0] = comps[clink.component[0]].rank;
                rank[1] = comps[clink.component[1]].rank;
                if ( rank[0].rank == rank[1].rank ) continue;
                if ( clink.getMinLatency() < min_part ) {
                    min_part = clink.getMinLatency();
                }
            }
        }
#ifdef SST_CONFIG_HAVE_MPI

        // Fix for case that probably doesn't matter in practice, but
        // does come up during some specific testing.  If there are no
        // links that cross the boundary and we're a multi-rank job,
        // we need to put in a sync interval to look for the exit
        // conditions being met.
        // if ( min_part == MAX_SIMTIME_T ) {
        //     // std::cout << "No links cross rank boundary" << std::endl;
        //     min_part = Simulation::getTimeLord()->getSimCycles("1us","");
        // }

        // broadcast(world, min_part, 0);
        Comms::broadcast(min_part, 0);
#endif
    }
    ////// End Calculate Minimum Partitioning //////

    if(cfg.enable_sig_handling) {
        g_output.verbose(CALL_INFO, 1, 0, "Signal handers will be registed for USR1, USR2, INT and TERM...\n");
        setupSignals(0);
    } else {
		// Print out to say disabled?
		g_output.verbose(CALL_INFO, 1, 0, "Signal handlers are disabled by user input\n");
    }

    ////// Broadcast Graph //////
#ifdef SST_CONFIG_HAVE_MPI
    if ( world_size.rank > 1 ) {
        Comms::broadcast(Params::keyMap, 0);
        Comms::broadcast(Params::keyMapReverse, 0);
        Comms::broadcast(Params::nextKeyID, 0);

        std::set<uint32_t> my_ranks;
        std::set<uint32_t> your_ranks;

        if ( 0 == myRank.rank ) {
            // Split the rank space in half
            for ( uint32_t i = 0; i < world_size.rank/2; i++ ) {
                my_ranks.insert(i);
            }

            for ( uint32_t i = world_size.rank/2; i < world_size.rank; i++ ) {
                your_ranks.insert(i);
            }

            // Need to send the your_ranks set and the proper
            // subgraph for further distribution                
            ConfigGraph* your_graph = graph->getSubGraph(your_ranks);
            int dest = *your_ranks.begin();
            Comms::send(dest, 0, your_ranks);
            Comms::send(dest, 0, *your_graph);
            your_ranks.clear();
        }
        else {
            Comms::recv(MPI_ANY_SOURCE, 0, my_ranks);
            Comms::recv(MPI_ANY_SOURCE, 0, *graph);
        }


        while ( my_ranks.size() != 1 ) {
            // This means I have more data to pass on to other ranks
            std::set<uint32_t>::iterator mid = my_ranks.begin();
            for ( unsigned int i = 0; i < my_ranks.size() / 2; i++ ) {
                ++mid;
            }

            your_ranks.insert(mid,my_ranks.end());
            my_ranks.erase(mid,my_ranks.end());

            ConfigGraph* your_graph = graph->getSubGraph(your_ranks);
            uint32_t dest = *your_ranks.begin();

            Comms::send(dest, 0, your_ranks);
            Comms::send(dest, 0, *your_graph);
            your_ranks.clear();
            delete your_graph;
        }

        if ( *my_ranks.begin() != myRank.rank) cout << "ERROR" << endl;

    }
#endif
    ////// End Broadcast Graph //////


    ///// Set up StatisticOutput /////

    StatisticOutput *so = Factory::getFactory()->CreateStatisticOutput(graph->getStatOutput(), graph->getStatOutputParams());
    if (NULL == so) {
        g_output.fatal(CALL_INFO, -1, " - Unable to instantiate Statistic Output %s\n", graph->getStatOutput().c_str());
    }

    if (false == so->checkOutputParameters()) {
        // If checkOutputParameters() fail, Tell the user how to use them and abort simulation
        g_output.output("Statistic Output (%s) :\n", so->getStatisticOutputName().c_str());
        so->printUsage();
        g_output.output("\n");

        g_output.output("Statistic Output Parameters Provided:\n");
        // for (Params::const_iterator it = graph->getStatOutputParams().begin(); it != graph->getStatOutputParams().end(); ++it ) {
        //     g_output.output("  %s = %s\n", Params::getParamName(it->first).c_str(), it->second.c_str());
        // }
        std::set<std::string> keys = graph->getStatOutputParams().getKeys();
        for (auto it = keys.begin(); it != keys.end(); ++it ) {
            g_output.output("  %s = %s\n", it->c_str(), graph->getStatOutputParams().find<std::string>(*it).c_str());
        }
        g_output.fatal(CALL_INFO, -1, " - Required Statistic Output Parameters not set\n");
    }

    // Set the Statistics Load Level into the Statistic Output
    so->setStatisticLoadLevel(graph->getStatLoadLevel());

    ///// End Set up StatisticOutput /////

    ////// Create Simulation //////
    Simulation::factory = factory;
    Simulation::statisticsOutput = so;
    Simulation::sim_output = g_output;
    Simulation::barrier.resize(world_size.thread);
    #ifdef USE_MEMPOOL
    /* Estimate that we won't have more than 128 sizes of events */
    Activity::memPools.reserve(world_size.thread * 128);
    #endif

    std::vector<std::thread> threads(world_size.thread);
    std::vector<SimThreadInfo_t> threadInfo(world_size.thread);
    for ( uint32_t i = 0 ; i < world_size.thread ; i++ ) {
        threadInfo[i].myRank = myRank;
        threadInfo[i].myRank.thread = i;
        threadInfo[i].world_size = world_size;
        threadInfo[i].config = &cfg;
        threadInfo[i].graph = graph;
        threadInfo[i].min_part = min_part;
    }

    double end_serial_build = sst_get_cpu_time();

    Output::setThreadID(std::this_thread::get_id(), 0);
    for ( uint32_t i = 1 ; i < world_size.thread ; i++ ) {
        threads[i] = std::thread(start_simulation, i, std::ref(threadInfo[i]), std::ref(Simulation::barrier));
        Output::setThreadID(threads[i].get_id(), i);
    }


    start_simulation(0, threadInfo[0], Simulation::barrier);
    for ( uint32_t i = 1 ; i < world_size.thread ; i++ ) {
        threads[i].join();
    }

    Simulation::shutdown();

    double total_end_time = sst_get_cpu_time();

    for ( uint32_t i = 1 ; i < world_size.thread ; i++ ) {
        // g_output.output(CALL_INFO,"simulated_time = %s, %s\n",threadInfo[0].simulated_time.toStringBestSI().c_str(),threadInfo[1].simulated_time.toStringBestSI().c_str());
        threadInfo[0].simulated_time = std::max(threadInfo[0].simulated_time, threadInfo[i].simulated_time);
        threadInfo[0].run_time = std::max(threadInfo[0].run_time, threadInfo[i].run_time);
        threadInfo[0].build_time = std::max(threadInfo[0].build_time, threadInfo[i].build_time);

        threadInfo[0].max_tv_depth = std::max(threadInfo[0].max_tv_depth, threadInfo[i].max_tv_depth);
        threadInfo[0].current_tv_depth += threadInfo[i].current_tv_depth;
        threadInfo[0].sync_data_size += threadInfo[i].sync_data_size;

    }

    double build_time = (end_serial_build - start) + threadInfo[0].build_time;
    double run_time = threadInfo[0].run_time;
    double total_time = total_end_time - start;


    double max_run_time = 0, max_build_time = 0, max_total_time = 0;

    uint64_t local_max_tv_depth = threadInfo[0].max_tv_depth;
    uint64_t global_max_tv_depth = 0;
    uint64_t local_current_tv_depth = threadInfo[0].current_tv_depth;
    uint64_t global_current_tv_depth = 0;
    
    uint64_t local_sync_data_size = threadInfo[0].sync_data_size;
    uint64_t global_max_sync_data_size = 0, global_sync_data_size = 0;

    uint64_t mempool_size = 0, max_mempool_size = 0, global_mempool_size = 0;
    uint64_t active_activities = 0, global_active_activities = 0;
#ifdef USE_MEMPOOL
    Activity::getMemPoolUsage(mempool_size, active_activities);
#endif

#ifdef SST_CONFIG_HAVE_MPI
    MPI_Allreduce(&run_time, &max_run_time, 1, MPI_DOUBLE, MPI_MAX, MPI_COMM_WORLD );
    MPI_Allreduce(&build_time, &max_build_time, 1, MPI_DOUBLE, MPI_MAX, MPI_COMM_WORLD );
    MPI_Allreduce(&total_time, &max_total_time, 1, MPI_DOUBLE, MPI_MAX, MPI_COMM_WORLD );
    MPI_Allreduce(&local_max_tv_depth, &global_max_tv_depth, 1, MPI_UINT64_T, MPI_MAX, MPI_COMM_WORLD );
    MPI_Allreduce(&local_current_tv_depth, &global_current_tv_depth, 1, MPI_UINT64_T, MPI_SUM, MPI_COMM_WORLD );
    MPI_Allreduce(&local_sync_data_size, &global_max_sync_data_size, 1, MPI_UINT64_T, MPI_MAX, MPI_COMM_WORLD );
    MPI_Allreduce(&local_sync_data_size, &global_sync_data_size, 1, MPI_UINT64_T, MPI_SUM, MPI_COMM_WORLD );
    MPI_Allreduce(&mempool_size, &max_mempool_size, 1, MPI_UINT64_T, MPI_MAX, MPI_COMM_WORLD );
    MPI_Allreduce(&mempool_size, &global_mempool_size, 1, MPI_UINT64_T, MPI_SUM, MPI_COMM_WORLD );
    MPI_Allreduce(&active_activities, &global_active_activities, 1, MPI_UINT64_T, MPI_SUM, MPI_COMM_WORLD );
#else
    max_build_time = build_time;
    max_run_time = run_time;
    max_total_time = total_time;
    global_max_tv_depth = local_max_tv_depth;
    global_current_tv_depth = local_current_tv_depth;
    global_max_sync_data_size = 0;
    global_max_sync_data_size = 0;
    max_mempool_size = mempool_size;
    global_mempool_size = mempool_size;
    global_active_activities = active_activities;
#endif

    const uint64_t local_max_rss     = maxLocalMemSize();
    const uint64_t global_max_rss    = maxGlobalMemSize();
    const uint64_t local_max_pf      = maxLocalPageFaults();
    const uint64_t global_pf         = globalPageFaults();
    const uint64_t global_max_io_in  = maxInputOperations();
    const uint64_t global_max_io_out = maxOutputOperations();

    if ( myRank.rank == 0 && ( cfg.verbose || cfg.printTimingInfo() ) ) {
        char ua_buffer[256];
        sprintf(ua_buffer, "%" PRIu64 "KB", local_max_rss);
        UnitAlgebra max_rss_ua(ua_buffer);

        sprintf(ua_buffer, "%" PRIu64 "KB", global_max_rss);
        UnitAlgebra global_rss_ua(ua_buffer);

        sprintf(ua_buffer, "%" PRIu64 "B", global_max_sync_data_size);
        UnitAlgebra global_max_sync_data_size_ua(ua_buffer);

        sprintf(ua_buffer, "%" PRIu64 "B", global_sync_data_size);
        UnitAlgebra global_sync_data_size_ua(ua_buffer);

        sprintf(ua_buffer, "%" PRIu64 "B", max_mempool_size);
        UnitAlgebra max_mempool_size_ua(ua_buffer);

        sprintf(ua_buffer, "%" PRIu64 "B", global_mempool_size);
        UnitAlgebra global_mempool_size_ua(ua_buffer);
        
        g_output.output( "\n");
        g_output.output("\n");
        g_output.output( "------------------------------------------------------------\n");
        g_output.output( "Simulation Timing Information:\n");
        g_output.output( "Build time:                      %f seconds\n", max_build_time);
        g_output.output( "Simulation time:                 %f seconds\n", max_run_time);
        g_output.output( "Total time:                      %f seconds\n", max_total_time);
        g_output.output( "Simulated time:                  %s\n", threadInfo[0].simulated_time.toStringBestSI().c_str());
        g_output.output( "\n");
        g_output.output( "Simulation Resource Information:\n");
        g_output.output( "Max Resident Set Size:           %s\n",
                max_rss_ua.toStringBestSI().c_str());
        g_output.output( "Approx. Global Max RSS Size:     %s\n",
                global_rss_ua.toStringBestSI().c_str());
        g_output.output( "Max Local Page Faults:           %" PRIu64 " faults\n",
                local_max_pf);
        g_output.output( "Global Page Faults:              %" PRIu64 " faults\n",
                global_pf);
        g_output.output( "Max Output Blocks:               %" PRIu64 " blocks\n",
                global_max_io_in);
        g_output.output( "Max Input Blocks:                %" PRIu64 " blocks\n",
                global_max_io_out);
        g_output.output( "Max mempool usage:               %s\n",
                max_mempool_size_ua.toStringBestSI().c_str());
        g_output.output( "Global mempool usage:            %s\n",
                global_mempool_size_ua.toStringBestSI().c_str());
        g_output.output( "Global active activities:        %" PRIu64 " activities\n",
                global_active_activities);
        g_output.output( "Current global TimeVortex depth: %" PRIu64 " entries\n",
                global_current_tv_depth);
        g_output.output( "Max TimeVortex depth:            %" PRIu64 " entries\n",
                global_max_tv_depth);
        g_output.output( "Max Sync data size:              %s\n",
                global_max_sync_data_size_ua.toStringBestSI().c_str());
        g_output.output( "Global Sync data size:           %s\n",
                global_sync_data_size_ua.toStringBestSI().c_str());
        g_output.output( "------------------------------------------------------------\n");
        g_output.output( "\n" );
        g_output.output( "\n" );

    }

#ifdef USE_MEMPOOL
    if ( cfg.event_dump_file != ""  ) {
        Output out("",0,0,Output::FILE, cfg.event_dump_file);
        if ( cfg.event_dump_file == "STDOUT" || cfg.event_dump_file == "stdout" ) out.setOutputLocation(Output::STDOUT);
        if ( cfg.event_dump_file == "STDERR" || cfg.event_dump_file == "stderr" ) out.setOutputLocation(Output::STDERR);
        Activity::printUndeletedActivites("",out, MAX_SIMTIME_T);
    }
#endif
    
#ifdef SST_CONFIG_HAVE_MPI
    if( 0 == myRank.rank ) {
#endif
        // Print out the simulation time regardless of verbosity.
        g_output.output("Simulation is complete, simulated time: %s\n", threadInfo[0].simulated_time.toStringBestSI().c_str());
#ifdef SST_CONFIG_HAVE_MPI
    }
#endif


#ifdef SST_CONFIG_HAVE_MPI
    // delete mpiEnv;
    MPI_Finalize();
#endif

    return 0;
}

