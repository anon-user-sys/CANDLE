#include <csignal>
#include <exception>
#include <iostream>
#include <sstream>
#include "benchmark.h"

using namespace application::hash_join;

Benchmark* create_benchmark(int count_arguments, char **arguments, std::uint16_t &host_threads);
void print_usage(const char* program_name);

int main(int count_arguments, char **arguments) {
    std::uint16_t host_threads = 256U;
    Benchmark* benchmark = nullptr;
    try {
        benchmark = create_benchmark(count_arguments, arguments, host_threads);
    } catch (const std::exception& e) {
        std::cerr << "[❌ FATAL] " << e.what() << std::endl;
        return 1;
    }
    if (benchmark == nullptr) return 1;

#if defined(USE_CXL)
    // register the PXL handler so an abnormal exit (abort etc.) cleans up the device (without it the device can wedge)
    pxl::registerSignalHandler({SIGINT, SIGTERM, SIGSEGV, SIGBUS, SIGABRT});
#endif

    int exit_code = 0;
    try {
        std::uint16_t task_count = 0U;
        while ((task_count = benchmark->next_core_count()) != 0U) {
            MorselScheduler::get().initialize(host_threads);
            
            benchmark->run_build(); // Phase 1: Build
            benchmark->run_probe(); // Phase 2: Probe
            benchmark->run_merge(); // Phase 3: Merge & Report
            
            MorselScheduler::get().shutdown();
        }
    } catch (const std::exception& e) {
        // an exception leaving main calls std::terminate -> abort; catch it here so PXL resources are released
        std::cerr << "[❌ FATAL] " << e.what() << std::endl;
        MorselScheduler::get().shutdown();
        exit_code = 1;
    }

    delete benchmark;
    return exit_code;
}

Benchmark* create_benchmark(int count_arguments, char **arguments, std::uint16_t &host_threads) {
    std::string core_input = "1";
    std::uint16_t step = 1U, iterations = 1U, r_key = 0U, s_key = 1U;
    std::string out_file = "", batch_input = "128", R_FILE = "orders.tbl", S_FILE = "lineitem.tbl";
    double selectivity_pct = 100.0;
    std::uint16_t partitions = 0U;
    tiered::Options tier_options;
    std::uint32_t prefetch_window = 0U;
    bool owner_build = false;
    std::uint32_t probe_batches = 0U;
    bool resident = false;
    std::uint32_t mu_batch = 0U;
    bool host_direct_probe = false;
    bool build_input_cxl = false;

    for (int i = 1; i < count_arguments; ++i) {
        std::string arg = arguments[i];
        if (arg == "-h" || arg == "--help") { print_usage(arguments[0]); return nullptr; }
        else if ((arg == "-s" || arg == "--steps") && i + 1 < count_arguments) step = static_cast<std::uint16_t>(std::stoi(arguments[++i]));
        else if ((arg == "-i" || arg == "--iterations") && i + 1 < count_arguments) iterations = static_cast<std::uint16_t>(std::stoi(arguments[++i]));
        else if ((arg == "-t" || arg == "--threads") && i + 1 < count_arguments) host_threads = static_cast<std::uint16_t>(std::stoi(arguments[++i]));
        else if ((arg == "-o" || arg == "--out") && i + 1 < count_arguments) out_file = arguments[++i];
        else if (arg == "--batch" && i + 1 < count_arguments) batch_input = arguments[++i];
        else if (arg == "-R" && i + 1 < count_arguments) R_FILE = arguments[++i];
        else if (arg == "-R-key" && i + 1 < count_arguments) r_key = static_cast<std::uint16_t>(std::stoi(arguments[++i]));
        else if (arg == "-S" && i + 1 < count_arguments) S_FILE = arguments[++i];
        else if (arg == "-S-key" && i + 1 < count_arguments) s_key = static_cast<std::uint16_t>(std::stoi(arguments[++i]));
        else if ((arg == "-p" || arg == "--partitions") && i + 1 < count_arguments) partitions = static_cast<std::uint16_t>(std::stoi(arguments[++i]));
        else if (arg == "--owner-build") owner_build = true;
        else if (arg == "--probe-batches" && i + 1 < count_arguments) probe_batches = static_cast<std::uint32_t>(std::stoul(arguments[++i]));
        else if (arg == "--resident") resident = true;
        else if (arg == "--host-direct-probe") host_direct_probe = true;
        else if (arg == "--build-input-cxl") build_input_cxl = true;
        else if (arg == "--mu-batch" && i + 1 < count_arguments) mu_batch = static_cast<std::uint32_t>(std::stoul(arguments[++i]));
        else if (arg == "--prefetch" && i + 1 < count_arguments) prefetch_window = static_cast<std::uint32_t>(std::stoul(arguments[++i]));
        else if (arg == "--cxl-pct" && i + 1 < count_arguments) tier_options.cxl_pct = std::stod(arguments[++i]);
        else if (arg == "--numa-node" && i + 1 < count_arguments) tier_options.cxl_node = std::stoi(arguments[++i]);
        else if (arg == "--dram-node" && i + 1 < count_arguments) tier_options.dram_node = std::stoi(arguments[++i]);
        else if (arg == "--placement" && i + 1 < count_arguments) {
            std::string v = arguments[++i];
            if (v != "interleave" && v != "block") { std::cerr << "--placement must be interleave | block\n"; return nullptr; }
            tier_options.interleave = (v == "interleave");
        }
        else if ((arg == "-f" || arg == "--filter-pct") && i + 1 < count_arguments) { selectivity_pct = std::stod(arguments[++i]);}
        else if (arg[0] != '-') core_input = arg;
        else {
            std::cerr << "Unknown or invalid argument: " << arg << "\n";
            print_usage(arguments[0]);
            return nullptr;
        }
    }

    if (mu_batch > 0) {
        std::cerr << "[NOTE] --mu-batch " << mu_batch << ": tasks per MU thread set explicitly (Map::setBatchSize).\n";
    }
    if (resident && probe_batches == 0) {
        std::cerr << "--resident needs --probe-batches K\n";
        return nullptr;
    }
    if (prefetch_window > 32U) {
        std::cerr << "--prefetch must be in [0, 32]\n";
        return nullptr;
    }
    if (tier_options.cxl_pct < 0.0 || tier_options.cxl_pct > 100.0) {
        std::cerr << "--cxl-pct must be in [0, 100]\n";
        return nullptr;
    }

    std::vector<std::uint16_t> core_counts;
    auto colon_pos = core_input.find(':');
    if (colon_pos == std::string::npos) {
        core_counts.push_back(static_cast<std::uint16_t>(std::stoi(core_input)));
    } else {
        std::uint16_t start_core = static_cast<std::uint16_t>(std::stoi(core_input.substr(0, colon_pos)));
        std::uint16_t end_core = (colon_pos + 1 < core_input.size()) ? static_cast<std::uint16_t>(std::stoi(core_input.substr(colon_pos + 1))) : start_core;
        for (std::uint16_t i = start_core; i <= end_core; i += step) core_counts.push_back(i);
    }

    std::vector<std::uint32_t> build_probe_batches;
    auto batches = std::stringstream{batch_input};
    std::string batch;
    while (std::getline(batches, batch, ',')) build_probe_batches.emplace_back(std::stoul(batch));

    return new Benchmark(std::move(core_counts), iterations, std::move(build_probe_batches),
                         std::make_tuple(std::make_pair(R_FILE, r_key), std::make_pair(S_FILE, s_key)),
                         std::move(out_file), selectivity_pct, partitions, tier_options, prefetch_window, owner_build, probe_batches, resident, mu_batch, host_direct_probe, build_input_cxl);
}

void print_usage(const char* program_name) {
    std::cout << "Usage: " << program_name << " [task_counts] [options]\n\n"
              << "Options:\n"
              << "  -t, --threads NUM    Number of host CPU worker threads (default: 256)\n"
              << "  -s, --steps NUM      Step size for task count range\n"
              << "  -i, --iterations NUM Iterations per configuration\n"
              << "  --batch BATCHES      Comma-separated batch sizes\n"
              << "  -f, --filter-pct NUM  Filter selectivity percentage 0.0-100.0 (default: 100.0)\n"
              << "  -p, --partitions NUM Number of hash table partitions (default: same as task count)\n"
              << "  -o, --out FILE       Output JSON result file\n"
              << "  --prefetch NUM       Device probe: batched prefetch window 1-32 (default 0 = off)\n"
              << "  --owner-build        Owner-computes build, no atomics: Mode 4 on the device (partition kernel + owner build),\n"
              << "                       Modes 2/3 on the host (count, scatter to CXL, owner insert)\n"
              << "  --build-input-cxl    Modes 2/3: host build reads orders keys from a cold CXL copy (as the device does)\n"
              << "  --probe-batches K    Probe lineitem in K consecutive batches (default 0 = one pass)\n"
              << "  --resident           Modes 3/4 with --probe-batches: one resident probe kernel (NDP@CXL)\n"
              << "  --mu-batch N         Device kernels: tasks per MU thread (Map::setBatchSize; default 0 = runtime's 16)\n"
              << "  --host-direct-probe  Host probe without partition routing (each task probes its own slice)\n"
              << "\nHost tiered mode (hashjoin_host_tiered, system-ram mode only):\n"
              << "  --cxl-pct NUM        Percent of every buffer placed on the CXL node, 0-100 (default: 0)\n"
              << "  --numa-node NUM      CXL NUMA node (default: 2)\n"
              << "  --dram-node NUM      Host DRAM NUMA node (default: 0)\n"
              << "  --placement MODE     interleave (default) | block\n"
              << "\nDevice tasks are not MU threads: the runtime runs 16 tasks per MU thread, so all 3,048\n"
              << "user MU threads need 48,768 tasks. --resident needs at most 3,048 tasks.\n";

}