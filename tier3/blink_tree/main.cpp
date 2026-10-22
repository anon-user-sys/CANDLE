#include <iostream>
#include <fstream>
#include <sstream>
#include <chrono>
#include <memory>
#include <vector>
#include <string>
#include <cstdint>
#include <algorithm>
#include <iomanip>

#include "pxl_helper.hpp"
#include "workload.h"
#include "request_container.h"
#include "benchmark_config.h"
#include "blink_tree.h"
#include "blink_tree_tasks.h"
#include "blink_tree_utils.h"
#include "blink_tree_scheduler_task.h"

#ifdef USE_CXL
pxl::runtime::Context* g_pxl_ctx = nullptr;
#endif

using namespace db::index::blinktree;
using namespace application::blinktree_benchmark;

// ====================================================================
// After phase 1, flush every node of the tree to CXL DRAM
// ====================================================================
void flush_entire_tree_to_cxl(BLinkTree<std::uint64_t, std::int64_t>* tree) {
    if (!tree) return;

    // 1. flush the root header
    cxl_inval_cache_range(tree, sizeof(BLinkTree<std::uint64_t, std::int64_t>));

    auto* root_node = tree->root();
    if (!root_node) return;

    // 2. walk the tree level by level (BFS) and flush every CXLNode (4 KB) to CXL DRAM
    std::vector<Node<std::uint64_t, std::int64_t>*> current_level = { root_node };
    while (!current_level.empty()) {
        std::vector<Node<std::uint64_t, std::int64_t>*> next_level;
        for (auto* node : current_level) {
            if (!node) continue;

            // flush every cache line of the node
            cxl_inval_cache_range(node, sizeof(Node<std::uint64_t, std::int64_t>));

            if (node->is_inner()) {
                std::uint16_t sz = node->size();
                for (std::uint16_t i = 0; i <= sz; ++i) {
                    auto* child = node->child_ptr(i);
                    if (child) next_level.push_back(child);
                }
            }
        }
        current_level = std::move(next_level);
    }
}


// BLINK_DEBUG_KEY=<key>: for analyzing validation failures - print the path from the root to key and its place in the leaf chain
static void debug_trace_key(BLinkTree<std::uint64_t, std::int64_t>* tree, std::uint64_t key) {
    auto dump = [](const char* tag, Node<std::uint64_t, std::int64_t>* n) {
        cxl_inval_cache_range(n, sizeof(*n));
        std::cerr << "  " << tag << " node=" << n << " lvl=" << int(n->level()) << (n->is_leaf() ? " leaf" : " inner")
                  << " size=" << n->size() << " high=" << n->high_key() << " rsib=" << n->right_sibling()
                  << " parent=" << n->parent();
        if (n->size() > 0) std::cerr << " first=" << n->leaf_key(0) << " last=" << n->leaf_key(n->size() - 1);
        std::cerr << "\n";
    };
    cxl_inval_cache_range(tree, sizeof(*tree));
    std::cerr << "[debug] descend to key " << key << "\n";
    auto* n = tree->root();
    while (n) {
        dump("path", n);
        if (n->is_leaf()) break;
        n = n->child(key);
    }
    auto* l = tree->root();
    while (l && !l->is_leaf()) { cxl_inval_cache_range(l, sizeof(*l)); l = l->child(0); }
    std::cerr << "[debug] leaf chain around key " << key << "\n";
    Node<std::uint64_t, std::int64_t>* prev = nullptr;
    std::uint64_t leaves = 0;
    while (l) {
        cxl_inval_cache_range(l, sizeof(*l));
        ++leaves;
        if (l->high_key() > key) {
            if (prev) dump("chain-prev", prev);
            dump("chain-hit ", l);
            if (l->right_sibling()) dump("chain-next", l->right_sibling());
            break;
        }
        prev = l;
        l = l->right_sibling();
    }
    std::cerr << "[debug] leaves walked: " << leaves << "\n";
}

struct BenchmarkOptions {
    std::uint16_t num_threads{4};
    std::uint64_t total_ops{1000000ULL};
    std::uint64_t seed{42};
    WorkloadPattern pattern{WorkloadPattern::WORKLOAD_A};
    std::string workload_str{"workload_a"};
    double skew{0.99};
    bool csv_out{false};
    std::string csv_file_path{""};
    std::uint32_t batch{32};        // requests per scheduler batch (one device round in CXL modes)
    std::uint32_t dtasks{0};        // device tasks per launch (0 = same as --threads)
    std::uint64_t keys{0};          // prefill keys (0 = max(1000, ops/10))
    bool scramble{false};           // scatter Zipf ranks over the key space
    std::string host_dispatch{"partitioned"}; // host modes: partitioned | scan | queue
    std::string pin{"node0"};       // worker pinning: node0 (CPUs of the CXL device's socket) | none
};

static std::uint64_t parse_ops_string(std::string str) {
    if (str.empty()) return 1000000ULL;
    char last_char = std::tolower(str.back());
    std::uint64_t multiplier = 1;

    if (last_char == 'k') { multiplier = 1000ULL; str.pop_back(); }
    else if (last_char == 'm') { multiplier = 1000000ULL; str.pop_back(); }
    else if (last_char == 'g') { multiplier = 1000000000ULL; str.pop_back(); }

    try { 
        return std::stoull(str) * multiplier; 
    } catch (...) { 
        std::cerr << "[Warning] Invalid ops format. Falling back to default (1M).\n";
        return 1000000ULL; 
    }
}

static BenchmarkOptions parse_arguments(int argc, char* argv[]) {
    BenchmarkOptions opts;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];

        try {
            if ((arg == "-t" || arg == "--threads") && i + 1 < argc) {
                opts.num_threads = static_cast<std::uint16_t>(std::stoi(argv[++i]));
            } else if ((arg == "-o" || arg == "--ops") && i + 1 < argc) {
                opts.total_ops = parse_ops_string(argv[++i]);
            } else if ((arg == "-s" || arg == "--seed") && i + 1 < argc) {
                opts.seed = std::stoull(argv[++i]);
            } else if (arg.rfind("--batch=", 0) == 0) {
                opts.batch = static_cast<std::uint32_t>(parse_ops_string(arg.substr(8)));
            } else if (arg.rfind("--dtasks=", 0) == 0) {
                opts.dtasks = static_cast<std::uint32_t>(std::stoul(arg.substr(9)));
            } else if (arg.rfind("--keys=", 0) == 0) {
                opts.keys = parse_ops_string(arg.substr(7));
            } else if (arg == "--resident") {
#if defined(MODE_CXL_FULL)
                db::index::blinktree::g_resident.on = true;
#endif
            } else if (arg == "--no-combine") {
#if defined(MODE_CXL_HYBRID) || defined(MODE_CXL_FULL)
                db::index::blinktree::g_cxl_no_combine = true;
#endif
            } else if (arg == "--scramble") {
                opts.scramble = true;
            } else if (arg.rfind("--host-dispatch=", 0) == 0) {
                opts.host_dispatch = arg.substr(16);
                if (opts.host_dispatch != "partitioned" && opts.host_dispatch != "scan" && opts.host_dispatch != "queue"
                    && opts.host_dispatch != "batched")
                    throw std::invalid_argument("expected partitioned|scan|queue|batched");
            } else if (arg.rfind("--pin=", 0) == 0) {
                opts.pin = arg.substr(6);
                if (opts.pin != "node0" && opts.pin != "none") throw std::invalid_argument("expected node0|none");
            } else if (arg.rfind("--skew=", 0) == 0) {
                opts.skew = std::stod(arg.substr(7));
            } else if (arg == "--csv-out") {
                opts.csv_out = true;
            } else if (arg.rfind("--csv-file=", 0) == 0) {
                opts.csv_out = true;
                opts.csv_file_path = arg.substr(11);
            } else if (arg.rfind("--workload=", 0) == 0) {
                std::string wl = arg.substr(11);
                opts.workload_str = wl;
                if (wl == "workload_a" || wl == "a") opts.pattern = WorkloadPattern::WORKLOAD_A;
                else if (wl == "workload_b" || wl == "b") opts.pattern = WorkloadPattern::WORKLOAD_B;
                else if (wl == "workload_c" || wl == "c") opts.pattern = WorkloadPattern::WORKLOAD_C;
                else if (wl == "workload_d" || wl == "d") opts.pattern = WorkloadPattern::WORKLOAD_D;
                else if (wl == "workload_f" || wl == "f") opts.pattern = WorkloadPattern::WORKLOAD_F;
            } else if (arg == "-h" || arg == "--help") {
                std::cout << "Usage: " << argv[0] << " [options]\n"
                          << "  -t, --threads <num>    Number of worker threads (default: 4)\n"
                          << "  -o, --ops <num>        Total operations e.g. 1M, 10M (default: 1M)\n"
                          << "  --workload=<type>      workload_a | workload_b | workload_c | workload_d | workload_f\n"
                          << "  --skew=<float>         Zipfian theta parameter (default: 0.99)\n"
                          << "  --keys=<num>           Prefill keys e.g. 10M (default: max(1000, ops/10))\n"
                          << "  --batch=<num>          Requests per batch / device round (default: 32)\n"
                          << "  --dtasks=<num>         Device tasks per kernel launch (default: --threads)\n"
                          << "  --no-combine           Device modes: one device traversal per op (no request combining)\n"
                          << "  --resident             NDP@CXL: one resident device worker for the run, jobs posted per batch\n"
                          << "  --scramble             Scatter Zipf-hot keys over the key space (default: hot keys 1,2,3...)\n"
                          << "  --host-dispatch=<m>    Host modes: partitioned (default, per-worker op lists built before timing)\n"
                          << "                         | scan (every worker scans all ops) | queue (original dispatcher)\n"
                          << "                         | batched (NDP@CXL's batch pipeline on host threads, combining; use --batch)\n"
                          << "  --pin=<p>              node0 (default: workers on the CXL device's socket) | none\n"
                          << "  --csv-out              Output format in single CSV line for plotting\n"
                          << "  --csv-file=<path>      Save CSV results directly to file\n";
                std::exit(0);
            }
        } catch (const std::exception& e) {
            std::cerr << "[Error] Failed to parse argument '" << arg << "': " << e.what() << "\n";
            std::exit(1);
        }
    }
    return opts;
}

int main(int argc, char* argv[]) {
    BenchmarkOptions opts = parse_arguments(argc, argv);
    if (opts.dtasks == 0) opts.dtasks = opts.num_threads;
    if (opts.batch == 0) opts.batch = 1;
    application::blinktree_benchmark::BenchmarkConfig::set_batch_size(opts.batch);

#ifdef MEASURE_LATENCY
    reset_global_latency_samples();
#endif

#ifdef USE_CXL
    std::unique_ptr<PxlResources> pxl_res;
    PxlUniquePtr<BLinkTree<std::uint64_t, std::int64_t>> cxl_tree_mem;
    try {
        pxl_res = initializePxl(opts.dtasks);
        if (!pxl_res) {
            std::cerr << "[Fatal Error] PXL Initialization failed.\n";
            return 1;
        }
        g_pxl_ctx = pxl_res->context;

#if defined(MODE_CXL_HYBRID)
        if (!g_pxl_func_write) {
            std::cerr << "[Fatal Error] Write kernel not initialized for Hybrid Mode.\n";
            return 1;
        }
#elif defined(MODE_CXL_FULL)
        if (!g_pxl_func_write || !g_pxl_func_lookup) {
            std::cerr << "[Fatal Error] Kernels not fully initialized for Full Mode.\n";
            return 1;
        }
#endif

        cxl_tree_mem = allocatePxlMemory<BLinkTree<std::uint64_t, std::int64_t>>(g_pxl_ctx, 1);
        new (cxl_tree_mem.get()) BLinkTree<std::uint64_t, std::int64_t>(0);
    } catch (const std::exception& e) {
        std::cerr << "[Fatal Error] PXL CXL Memory Allocation failed: " << e.what() << "\n";
        return 1;
    }
#endif

    // 1. Thread Scheduler & Tree Initialization
    if (opts.pin == "node0") {
        // the CXL device (PCI 0000:10:00.0) hangs off NUMA node 0; keep workers on that socket
        std::vector<int> cpus;
        std::ifstream f("/sys/devices/system/node/node0/cpulist");
        std::string list;
        std::getline(f, list);
        std::stringstream ss(list);
        for (std::string part; std::getline(ss, part, ',');) {
            const auto dash = part.find('-');
            const int lo = std::stoi(part.substr(0, dash));
            const int hi = dash == std::string::npos ? lo : std::stoi(part.substr(dash + 1));
            for (int c = lo; c <= hi; ++c) cpus.push_back(c);
        }
        BLinkTreeScheduler::get().set_pin_cpus(std::move(cpus));
    }
    BLinkTreeScheduler::get().initialize(opts.num_threads);

#ifdef USE_CXL
    BLinkTree<std::uint64_t, std::int64_t>* raw_tree_ptr = cxl_tree_mem.get();
#else
    auto shared_tree = std::make_unique<BLinkTree<std::uint64_t, std::int64_t>>(0);
    BLinkTree<std::uint64_t, std::int64_t>* raw_tree_ptr = shared_tree.get();
#endif

    std::vector<BLinkTree<std::uint64_t, std::int64_t>*> tree_shards(opts.num_threads, raw_tree_ptr);

// ====================================================================
    // 1. set up the workload and the validator before phase 1
    // ====================================================================
    auto workload = std::make_shared<Workload>(opts.total_ops, opts.seed, opts.pattern, opts.skew, opts.keys, opts.scramble);
    std::cout << "[Config] keys=" << workload->initial_keys() << " ops=" << opts.total_ops
              << " workload=" << opts.workload_str << " threads=" << opts.num_threads
              << " batch=" << opts.batch << " dtasks=" << opts.dtasks
              << " scramble=" << opts.scramble << " pin=" << opts.pin
#if defined(MODE_CXL_HYBRID) || defined(MODE_CXL_FULL)
              << " combine=" << !db::index::blinktree::g_cxl_no_combine
#endif
#if defined(MODE_CXL_FULL)
              << " resident=" << db::index::blinktree::g_resident.on
#endif
#if defined(MODE_HOST_DRAM) || defined(MODE_HOST_CXL)
              << " dispatch=" << opts.host_dispatch
#endif
              << "\n";

#ifdef ENABLE_VALIDATION
    auto validator = std::make_shared<GoldenReferenceValidator>(opts.num_threads, *workload);
#endif

    // ====================================================================
    // Phase 1: pre-population (prefill keys kept in step with the validator)
    // ====================================================================
    std::uint64_t prefill_count = workload->initial_keys(); // 1000ULL
    std::cout << "[Phase 1] Pre-populating tree with " << prefill_count << " keys...\n";

    class PrefillListener : public Listener<std::uint64_t, std::int64_t> {
    public:
        std::atomic<std::int64_t> pending_separators{0};
        void separator_started() override { pending_separators.fetch_add(1, std::memory_order_release); }
        void separator_finished() override { pending_separators.fetch_sub(1, std::memory_order_release); }
    #ifdef MEASURE_LATENCY
        void inserted([[maybe_unused]] std::uint16_t core_id, 
                      [[maybe_unused]] std::uint64_t key, 
                      [[maybe_unused]] std::int64_t value, 
                      [[maybe_unused]] TimePoint start_time = {}) override {}
#endif
    } prefill_listener;

    std::uint64_t p1_step = std::max<std::uint64_t>(1ULL, prefill_count / 100ULL);
    const auto prefill_start = std::chrono::steady_clock::now();
    g_cxl_inval_enabled = false; // host-only build phase: flush the whole tree once at the end
    for (std::uint64_t k = 1; k <= prefill_count; ++k) {
        InsertValueTask<std::uint64_t, std::int64_t, PrefillListener> prefill_task(
            k, static_cast<std::int64_t>(k * 10), raw_tree_ptr, prefill_listener
        );
        prefill_task.execute(0);

#ifdef ENABLE_VALIDATION
        // register the phase 1 prefill count with the validator so its index starts after the prefill
        if (validator) {
            validator->inserted(0, k, static_cast<std::int64_t>(k * 10));
        }
#endif

        if (k % p1_step == 0 || k == prefill_count) {
            double pct = (double)k / prefill_count * 100.0;
            std::cout << "\r[Phase 1] Progress: " << k << " / " << prefill_count 
                      << " (" << std::fixed << std::setprecision(1) << pct << "%)" << std::flush;
        }
    }
    std::cout << "\n[Phase 1] Waiting for background separator tasks...\n";

    // wait for background node splits still in progress
    auto last_p1_wait_report = std::chrono::steady_clock::now();
    while (prefill_listener.pending_separators.load(std::memory_order_acquire) > 0) {
        auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration_cast<std::chrono::milliseconds>(now - last_p1_wait_report).count() >= 500) {
            std::cout << "\r[Phase 1] Pending separators: " 
                      << prefill_listener.pending_separators.load() << " tasks remaining...   " << std::flush;
            last_p1_wait_report = now;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    #if defined(__x86_64__) || defined(_M_X64)
    _mm_sfence();
#endif

    g_cxl_inval_enabled = true;
    flush_entire_tree_to_cxl(raw_tree_ptr);
    std::cout << "[Phase 1] Prefill took " << std::fixed << std::setprecision(1)
              << std::chrono::duration<double>(std::chrono::steady_clock::now() - prefill_start).count() << " s";
#ifdef USE_CXL
    std::cout << " (" << CXLNode::pool_nodes << " nodes, "
              << CXLNode::pool_nodes * 4096.0 / (1 << 20) << " MiB)";
#endif
    std::cout << "\n";
    std::cout << "\r[Phase 1] Pre-population complete! All nodes flushed to CXL DRAM.            \n";
    
    // ====================================================================
    // Phase 2: skip the workload offset and build the containers
    // ====================================================================
    workload->skip(prefill_count);

#ifdef ENABLE_VALIDATION
    auto container = std::make_shared<RequestContainer>(
        0, application::blinktree_benchmark::BenchmarkConfig::max_parallel_requests(), *workload, validator
    );
#else
    auto container = std::make_shared<RequestContainer>(
        0, application::blinktree_benchmark::BenchmarkConfig::max_parallel_requests(), *workload
    );
#endif

    // host_cxl's per-worker op lists are built before the clock starts (setup excluded from host
    // throughput, user decision 2026-10-17; the setup time is still printed). Full mode has no setup:
    // its per-batch dedup/grouping runs inside the clock. The clock starts before any work is spawned.
    auto start_time = std::chrono::steady_clock::now();
    double setup_ms = 0.0;
#if defined(MODE_HOST_DRAM) || defined(MODE_HOST_CXL)
    if (opts.host_dispatch == "partitioned") {
        // per-worker op lists, built before the timer starts
        std::vector<std::vector<std::uint64_t>> parts(opts.num_threads);
        for (std::uint64_t i = workload->initial_keys(); i < workload->size(); ++i)
            parts[(*workload)[i].key() % opts.num_threads].push_back(i);
        setup_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start_time).count();
        start_time = std::chrono::steady_clock::now(); // exclude setup
        container->take_all();
        for (std::uint16_t w = 0; w < opts.num_threads; ++w) {
            auto list = std::make_shared<const std::vector<std::uint64_t>>(std::move(parts[w]));
            const std::uint64_t n = list->size();
            BLinkTreeScheduler::get().spawn(std::make_unique<HostPartitionTask>(
                w, opts.num_threads, container, raw_tree_ptr, 0, n, list), w);
        }
    } else if (opts.host_dispatch == "scan") {
        container->take_all();
        for (std::uint16_t w = 0; w < opts.num_threads; ++w) {
            BLinkTreeScheduler::get().spawn(std::make_unique<HostPartitionTask>(
                w, opts.num_threads, container, raw_tree_ptr, workload->initial_keys(), workload->size()), w);
        }
    } else
#endif
    {
#if defined(MODE_HOST_DRAM) || defined(MODE_HOST_CXL)
        application::blinktree_benchmark::g_host_batched = opts.host_dispatch == "batched";
#endif
        auto scheduler_task = std::make_unique<RequestSchedulerTask>(0, container, tree_shards);
        BLinkTreeScheduler::get().spawn(std::move(scheduler_task), 0);
    }

    // ====================================================================
    // Phase 2 execution (std::chrono::steady_clock throughout)
    // ====================================================================
    std::cout << "[Phase 2] Benchmark execution started (Threads: " << opts.num_threads << ")...\n";
    
    if (setup_ms > 0) std::cerr << "[Setup] host op-list partitioning " << std::fixed << std::setprecision(1) << setup_ms << " ms (excluded from throughput)\n";
    auto last_report = std::chrono::steady_clock::now();

    while (!container->is_finished()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));

        auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration_cast<std::chrono::milliseconds>(now - last_report).count() >= 500) {
            std::uint64_t done = container->finished_requests();
            std::uint64_t total = container->total_requests();
            double pct = (total > 0) ? ((double)done / total * 100.0) : 0.0;
            double elapsed_s = std::chrono::duration<double>(now - start_time).count();

            std::cout << "\r[Phase 2] Progress: " << done << " / " << total 
                      << " (" << std::fixed << std::setprecision(1) << pct << "%)"
                      << " | Elapsed: " << std::setprecision(1) << elapsed_s << "s" << std::flush;
            last_report = now;
        }
    }    

    auto end_time = std::chrono::steady_clock::now();
    std::cout << "\r[Phase 2] Execution complete! 100% processed.                                  \n";
    
    double elapsed_sec = std::chrono::duration<double>(end_time - start_time).count();
    if (elapsed_sec <= 0.0) elapsed_sec = 0.000001;

    BLinkTreeScheduler::get().shutdown();

#if defined(MODE_CXL_HYBRID) || defined(MODE_CXL_FULL)
    {
        const auto& st = db::index::blinktree::g_cxl_stats;
        const double launches = static_cast<double>(st.launches.load());
        std::cerr << "[CXL] launches=" << st.launches.load()
                  << " avg_launch_us=" << std::fixed << std::setprecision(1) << (launches > 0 ? st.launch_ns.load() / launches / 1000.0 : 0.0)
                  << " device_lookups=" << st.device_lookups.load() << " host_locates=" << st.host_locates.load()
                  << " device_writes=" << st.device_writes.load() << " host_split_inserts=" << st.host_split_inserts.load()
                  << " (ops=" << opts.total_ops << ")\n";
        auto ms = [](const std::atomic<std::uint64_t>& v) { return v.load() / 1e6; };
        std::cerr << "[CXL-phases ms] batch_total=" << ms(st.batch_ns) << " launch=" << ms(st.launch_ns)
                  << " stage=" << ms(st.stage_ns) << " readback=" << ms(st.readback_ns)
                  << " readback_inval=" << ms(st.readback_inval_ns)
                  << " dedup=" << ms(st.dedup_ns) << " drain=" << ms(st.drain_ns)
                  << " locate_host=" << ms(st.locate_host_ns) << " post_inval=" << ms(st.post_inval_ns) << " replay=" << ms(st.replay_ns)
                  << " write_prep=" << ms(st.write_prep_ns) << " split=" << ms(st.split_ns)
                  << " report=" << ms(st.report_ns) << "\n";
    }
#endif

    // 5. Result Output
#ifdef ENABLE_VALIDATION
    for (std::uint16_t i = 0; i < opts.num_threads; ++i) {
        validator->requests_finished();
    }
    std::vector<BLinkTree<std::uint64_t, std::int64_t>*> unique_trees{ raw_tree_ptr };
    bool is_state_valid = validator->verify_final_state(unique_trees);
    if (const char* dk = std::getenv("BLINK_DEBUG_KEY")) debug_trace_key(raw_tree_ptr, std::stoull(dk));
    validator->report().print();

#ifdef USE_CXL
    reset_cxl_batch_buffers();
#endif

    if (!validator->report().passed.load() || !is_state_valid) return 1; 
    return 0;
#else
    double throughput_mops = (opts.total_ops / elapsed_sec) / 1000000.0;
    [[maybe_unused]] const double wall_mops = throughput_mops;
#if defined(MODE_HOST_DRAM) || defined(MODE_HOST_CXL)
    // batched host: same rule as NDP@CXL - dedup and replay (host prep) not counted
    if (opts.host_dispatch == "batched") {
        const auto& st = db::index::blinktree::g_cxl_stats;
        const double timed_s = (st.locate_host_ns.load() + st.host_write_ns.load() + st.report_ns.load()) / 1e9;
        if (timed_s > 0) throughput_mops = (opts.total_ops / timed_s) / 1000000.0;
        auto ms = [](const std::atomic<std::uint64_t>& v) { return v.load() / 1e6; };
        std::cerr << "[HOST-batched ms] batch_total=" << ms(st.batch_ns) << " dedup=" << ms(st.dedup_ns)
                  << " locate=" << ms(st.locate_host_ns) << " replay=" << ms(st.replay_ns)
                  << " write=" << ms(st.host_write_ns) << " report=" << ms(st.report_ns)
                  << " distinct=" << st.host_locates.load() << " writes=" << st.device_writes.load() << "\n";
    }
#endif
#if defined(MODE_CXL_HYBRID)
    // Hybrid timed region, same split as NDP@CXL and batched Host@CXL: host locate (its lookups, incl. the
    // post-locate leaf invalidation) + request staging + write kernel + readback + split fallback +
    // reporting. Dedup, replay and write grouping (host prep) are not counted.
    {
        const auto& st = db::index::blinktree::g_cxl_stats;
        const double timed_s = (st.locate_host_ns.load() + st.stage_ns.load() + st.launch_ns.load() + st.readback_ns.load()
                                + st.split_ns.load() + st.report_ns.load()) / 1e9;
        if (timed_s > 0) throughput_mops = (opts.total_ops / timed_s) / 1000000.0;
    }
#endif
#if defined(MODE_CXL_FULL)
    // NDP@CXL timed region (user decisions 2026-10-17): request staging + execute->synchronize
    // (dispatch + device kernels) + result readback incl. invalidation + host split fallback +
    // reporting, i.e. the whole SDBI hand-off and the device work. Dedup, replay and write grouping
    // (host prep) are not counted. Wall-clock throughput is printed separately.
    {
        const auto& st = db::index::blinktree::g_cxl_stats;
        const double timed_s = (st.stage_ns.load() + st.launch_ns.load() + st.readback_ns.load() + st.split_ns.load() + st.report_ns.load()) / 1e9;
        if (timed_s > 0) throughput_mops = (opts.total_ops / timed_s) / 1000000.0;
    }
#endif
    double avg_lat_us = 0.0, p50_us = 0.0, p99_us = 0.0, p999_us = 0.0;

#ifdef MEASURE_LATENCY
    std::vector<std::uint64_t> merged_ns;
    {
        std::lock_guard<std::mutex> lock(g_latency_registry_mutex);
        if (!g_tl_latency_buffer.samples.empty()) {
            g_global_latency_samples.insert(
                g_global_latency_samples.end(),
                g_tl_latency_buffer.samples.begin(),
                g_tl_latency_buffer.samples.end()
            );
            g_tl_latency_buffer.samples.clear();
        }
        merged_ns = std::move(g_global_latency_samples);
    }

    if (!merged_ns.empty()) {
        std::sort(merged_ns.begin(), merged_ns.end());
        double sum_ns = 0;
        for (auto ns : merged_ns) sum_ns += ns;
        avg_lat_us = (sum_ns / merged_ns.size()) / 1000.0;

        auto get_percentile_us = [&](double pct) -> double {
            std::size_t idx = static_cast<std::size_t>(pct * merged_ns.size());
            if (idx >= merged_ns.size()) idx = merged_ns.size() - 1;
            return static_cast<double>(merged_ns[idx]) / 1000.0;
        };

        p50_us = get_percentile_us(0.50);
        p99_us = get_percentile_us(0.99);
        p999_us = get_percentile_us(0.999);
    }
#endif

    if (opts.csv_out) {
        std::string csv_header = "workload,threads,ops,throughput_mops,avg_lat_us,p50_us,p99_us,p999_us\n";
        std::stringstream ss;
        ss << opts.workload_str << "," 
           << opts.num_threads << "," 
           << opts.total_ops << "," 
           << std::fixed << std::setprecision(3) << throughput_mops << "," 
           << std::setprecision(2) << avg_lat_us << "," 
           << p50_us << "," 
           << p99_us << "," 
           << p999_us << "\n";

        std::cout << csv_header << ss.str();

        if (!opts.csv_file_path.empty()) {
            bool file_exists = std::ifstream(opts.csv_file_path).good();
            std::ofstream file(opts.csv_file_path, std::ios::app);
            if (file.is_open()) {
                if (!file_exists) file << csv_header;
                file << ss.str();
                std::cout << "[Info] Benchmark results appended to " << opts.csv_file_path << "\n";
            } else {
                std::cerr << "[Error] Could not open file for writing: " << opts.csv_file_path << "\n";
            }
        }
    } else {
        std::cout << "\n=== B-Link Tree Performance Metrics ===\n";
#if defined(MODE_HOST_DRAM)
        std::cout << " * Architecture Mode : Mode 1 (Host CPU + Host DRAM)\n";
#elif defined(MODE_HOST_CXL)
        std::cout << " * Architecture Mode : Mode 2 (Host CPU + CXL Memory Pool)\n";
#elif defined(MODE_CXL_HYBRID)
        std::cout << " * Architecture Mode : Mode 3 (CXL Write Offload + Host Read Hybrid)\n";
#elif defined(MODE_CXL_FULL)
        std::cout << " * Architecture Mode : Mode 4 (All Operations Offloaded to CXL RISC-V)\n";
#endif

        std::cout << " * Throughput        : " << std::fixed << std::setprecision(3) << throughput_mops << " MOPS\n";
#if defined(MODE_CXL_FULL) || defined(MODE_CXL_HYBRID)
        std::cout << " * Wall throughput   : " << std::fixed << std::setprecision(3) << wall_mops << " MOPS (incl. pre-execute prep)\n";
#endif
#ifdef MEASURE_LATENCY
        std::cout << " * Avg Latency       : " << std::fixed << std::setprecision(2) << avg_lat_us << " us\n";
        std::cout << " * P50 Latency       : " << p50_us << " us\n";
        std::cout << " * P99 Latency       : " << p99_us << " us\n";
        std::cout << " * P99.9 Latency     : " << p999_us << " us\n";
#endif
    }

#ifdef USE_CXL
    reset_cxl_batch_buffers();
#endif

    return 0;
#endif
}