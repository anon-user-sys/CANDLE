// Orchestration Tax Benchmark Suite — Host-Only Counterpart
//
// Mirrors main.cpp test-for-test but replaces pxl device dispatch with
// std::thread fan-out: N threads spawned, each running a null function,
// then all joined.  Use this to isolate the host-side parallel-task
// orchestration overhead for direct comparison against the device path.
//
// Device analogue mapping:
//   execute()     → thread spawn loop
//   synchronize() → thread join loop
//   taskCount     → number of threads
//   Sub/cluster   → CPU cores / thread pool workers
//   Map reuse     → thread pool reuse vs fresh threads per dispatch
//
// Tests T5 (locality mode) and T8 (cluster bitmap) use sched_setaffinity
// to approximate the device scheduling concepts with CPU core sets.

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <mutex>
#include <pthread.h>
#include <sched.h>
#include <string>
#include <thread>
#include <vector>

#include "stats.hpp"
#include "timing.hpp"

static const int WARMUP = 20;
static const int REPS   = 200;

static uint64_t (*stop_fn)() = timing::stop;

// ── thread pool ───────────────────────────────────────────────────────────────
// Used by T4 (reuse mode) and T6 (sub count analogue).

struct Pool
{
    std::vector<std::thread> workers;
    std::mutex               mtx;
    std::condition_variable  cv_in, cv_out;
    std::vector<std::function<void()>> queue;
    int  in_flight = 0;
    bool quit      = false;

    explicit Pool(int n)
    {
        workers.resize(n);
        for (int i = 0; i < n; i++)
        {
            workers[i] = std::thread([this]()
            {
                while (true)
                {
                    std::function<void()> fn;
                    {
                        std::unique_lock<std::mutex> lk(mtx);
                        cv_in.wait(lk, [this]{ return quit || !queue.empty(); });
                        if (quit && queue.empty()) return;
                        fn = std::move(queue.back());
                        queue.pop_back();
                    }
                    fn();
                    {
                        std::lock_guard<std::mutex> lk(mtx);
                        in_flight--;
                    }
                    cv_out.notify_one();
                }
            });
        }
    }

    void dispatch(int count)
    {
        {
            std::lock_guard<std::mutex> lk(mtx);
            in_flight = count;
            for (int i = 0; i < count; i++)
                queue.push_back([](){});
        }
        cv_in.notify_all();
        std::unique_lock<std::mutex> lk(mtx);
        cv_out.wait(lk, [this]{ return in_flight == 0; });
    }

    ~Pool()
    {
        { std::lock_guard<std::mutex> lk(mtx); quit = true; }
        cv_in.notify_all();
        for (auto& w : workers) w.join();
    }
};

// ── helpers ───────────────────────────────────────────────────────────────────

static void pin_to_cpu0()
{
    cpu_set_t cs;
    CPU_ZERO(&cs);
    CPU_SET(0, &cs);
    if (sched_setaffinity(0, sizeof(cs), &cs) != 0)
        fprintf(stderr, "# Warning: sched_setaffinity failed — measurements may have jitter\n");
    else
        printf("# cpu_affinity: pinned to CPU 0\n");
}

// Spawn task_count threads each running a null function, then join all.
static void fan_out(int task_count)
{
    std::vector<std::thread> threads;
    threads.reserve(task_count);
    for (int i = 0; i < task_count; i++)
        threads.emplace_back([](){});
    for (auto& t : threads) t.join();
}

static std::vector<double> fan_out_loop(int task_count, int warmup, int reps, double ghz)
{
    for (int i = 0; i < warmup; i++) fan_out(task_count);

    std::vector<double> samples;
    samples.reserve(reps);
    for (int i = 0; i < reps; i++)
    {
        uint64_t t0 = timing::start();
        fan_out(task_count);
        uint64_t t1 = stop_fn();
        samples.push_back(timing::to_us(t1 - t0, ghz));
    }
    return samples;
}

static int num_cpus()
{
    return static_cast<int>(std::thread::hardware_concurrency());
}

// ── T1: thread count sweep ────────────────────────────────────────────────────
// Device analogue: taskCount sweep.  Measures std::thread spawn+join overhead
// as a function of thread count.  Linear fit gives fixed spawn overhead (alpha)
// and per-thread cost (beta).

static void t1_thread_sweep(double ghz)
{
    printf("\n# TEST: T1_thread_sweep\n");
    printf("# warmup=%d reps=%d\n", WARMUP, REPS);
    printf("# Device analogue: T1_dispatch_sweep — taskCount → thread count\n");
    printf("# Linear fit y = alpha + slope*threadCount estimates fixed spawn overhead\n");
    printf("# and per-thread cost.\n");
    printf("taskCount,mean_us,std_us,min_us,p50_us,p95_us,p99_us,max_us,n\n");

    const std::vector<int> task_counts = {
        1, 2, 4, 8, 16, 32, 48, 64, 96, 128, 192, 256,
        384, 512, 768, 1024, 1536, 2048
    };

    std::vector<double> xs, ys;
    for (int tc : task_counts)
    {
        auto samples = fan_out_loop(tc, WARMUP, REPS, ghz);
        auto s = Stats::from(samples);

        char prefix[32];
        snprintf(prefix, sizeof(prefix), "%d", tc);
        s.print_csv(prefix);

        xs.push_back(static_cast<double>(tc));
        ys.push_back(s.mean);
    }

    LinFit fit;
    if (LinFit::fit(xs, ys, fit))
    {
        printf("# OLS fit: alpha_us=%.4f  slope_us_per_task=%.6f  R2=%.4f\n",
               fit.alpha, fit.slope, fit.r2);
        printf("# Interpretation: alpha=%.2f us is the fixed thread-spawn overhead;\n", fit.alpha);
        printf("#   each additional thread adds ~%.4f us of spawn/join cost.\n", fit.slope);
        printf("# Note: R2<0.9 suggests super-linear scaling (OS scheduler contention).\n");
    }
}

// ── T2: warmup convergence ────────────────────────────────────────────────────
// Device analogue: T2_warmup_convergence.  Records per-invocation latency cold.
// On the host, the first few thread spawns may be slower due to glibc thread
// stack pre-allocation and OS scheduler warm-up.

static void t2_warmup_convergence(double ghz)
{
    printf("\n# TEST: T2_warmup_convergence\n");
    printf("# taskCount=64 no_prior_warmup\n");
    printf("# Device analogue: T2_warmup_convergence\n");
    printf("invocation,latency_us\n");

    const int TC     = 64;
    const int N_COLD = 100;

    for (int i = 0; i < N_COLD; i++)
    {
        uint64_t t0 = timing::start();
        fan_out(TC);
        uint64_t t1 = stop_fn();
        printf("%d,%.4f\n", i, timing::to_us(t1 - t0, ghz));
    }
}

// ── T3: phase split — spawn vs join ──────────────────────────────────────────
// Device analogue: T3_phase_split (execute() vs synchronize()).
// Separates thread-spawn time from thread-join time.

static void t3_phase_split(double ghz)
{
    printf("\n# TEST: T3_phase_split\n");
    printf("# warmup=%d reps=%d\n", WARMUP, REPS);
    printf("# Device analogue: T3_phase_split — spawn→execute, join→synchronize\n");
    printf("taskCount,spawn_us_mean,spawn_us_std,join_us_mean,join_us_std,total_us_mean,total_us_std\n");

    const std::vector<int> task_counts = {1, 16, 64, 128, 256, 512, 1024};

    for (int tc : task_counts)
    {
        for (int i = 0; i < WARMUP; i++) fan_out(tc);

        std::vector<double> spawn_s, join_s, total_s;
        spawn_s.reserve(REPS); join_s.reserve(REPS); total_s.reserve(REPS);

        for (int i = 0; i < REPS; i++)
        {
            std::vector<std::thread> threads;
            threads.reserve(tc);

            uint64_t t0 = timing::start();
            for (int j = 0; j < tc; j++)
                threads.emplace_back([](){});
            uint64_t t1 = stop_fn();
            for (auto& t : threads) t.join();
            uint64_t t2 = stop_fn();

            spawn_s.push_back(timing::to_us(t1 - t0, ghz));
            join_s.push_back(timing::to_us(t2 - t1, ghz));
            total_s.push_back(timing::to_us(t2 - t0, ghz));
        }

        auto ss = Stats::from(spawn_s);
        auto js = Stats::from(join_s);
        auto ts = Stats::from(total_s);
        printf("%d,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f\n",
               tc, ss.mean, ss.stddev, js.mean, js.stddev, ts.mean, ts.stddev);
    }
}

// ── T4: pool reuse vs fresh threads ──────────────────────────────────────────
// Device analogue: T4_map_reuse (reusing the same Map object vs rebuild).
// Host reuse  : a pre-spawned thread pool submits work without recreating threads.
// Host rebuild: spawn fresh std::threads and join per invocation (same as fan_out).

static void t4_pool_reuse(double ghz)
{
    printf("\n# TEST: T4_pool_reuse\n");
    printf("# warmup=%d reps=%d\n", WARMUP, REPS);
    printf("# Device analogue: T4_map_reuse — pool reuse → Map reuse, fresh threads → rebuild\n");
    printf("mode,taskCount,mean_us,std_us,min_us,p50_us,p95_us,p99_us,max_us,n\n");

    const std::vector<int> task_counts = {1, 64, 256};

    for (int tc : task_counts)
    {
        // --- reuse: persistent thread pool ---
        {
            Pool pool(tc);

            for (int i = 0; i < WARMUP; i++) pool.dispatch(tc);

            std::vector<double> samples;
            samples.reserve(REPS);
            for (int i = 0; i < REPS; i++)
            {
                uint64_t t0 = timing::start();
                pool.dispatch(tc);
                uint64_t t1 = stop_fn();
                samples.push_back(timing::to_us(t1 - t0, ghz));
            }

            auto s = Stats::from(samples);
            char prefix[64];
            snprintf(prefix, sizeof(prefix), "reuse,%d", tc);
            s.print_csv(prefix);
        }

        // --- rebuild: fresh threads per dispatch ---
        {
            auto samples = fan_out_loop(tc, WARMUP, REPS, ghz);
            auto s = Stats::from(samples);
            char prefix[64];
            snprintf(prefix, sizeof(prefix), "rebuild,%d", tc);
            s.print_csv(prefix);
        }
    }
}

// ── T5: locality mode — compact vs spread affinity ───────────────────────────
// Device analogue: T5_locality_mode (CompactMode vs SpreadMode).
// Compact: threads are bound to contiguous core IDs starting from 0.
// Spread : threads are round-robin distributed across all available cores.

static void t5_locality_mode(double ghz)
{
    printf("\n# TEST: T5_locality_mode\n");
    printf("# warmup=%d reps=%d\n", WARMUP, REPS);
    printf("# Device analogue: T5_locality_mode — CPU affinity approximates cluster placement\n");
    printf("# compact: threads pinned to cores 0..min(tc,ncpu)-1\n");
    printf("# spread : threads round-robin across all %d cores\n", num_cpus());
    printf("mode,taskCount,mean_us,std_us,min_us,p50_us,p95_us,p99_us,max_us,n\n");

    const std::vector<int> task_counts = {1, 16, 64, 128, 256, 512, 1024};
    const int ncpu = num_cpus();

    auto affinity_fan_out = [&](int tc, bool spread)
    {
        std::vector<std::thread> threads;
        threads.reserve(tc);
        for (int i = 0; i < tc; i++)
        {
            threads.emplace_back([i, tc, ncpu, spread]()
            {
                int core = spread ? (i * ncpu / tc) : (i % ncpu);
                cpu_set_t cs; CPU_ZERO(&cs); CPU_SET(core, &cs);
                pthread_setaffinity_np(pthread_self(), sizeof(cs), &cs);
            });
        }
        for (auto& t : threads) t.join();
    };

    const struct { const char* name; bool spread; } modes[] = {
        { "compact", false },
        { "spread",  true  },
    };

    for (auto& lm : modes)
    {
        for (int tc : task_counts)
        {
            for (int i = 0; i < WARMUP; i++) affinity_fan_out(tc, lm.spread);

            std::vector<double> samples;
            samples.reserve(REPS);
            for (int i = 0; i < REPS; i++)
            {
                uint64_t t0 = timing::start();
                affinity_fan_out(tc, lm.spread);
                uint64_t t1 = stop_fn();
                samples.push_back(timing::to_us(t1 - t0, ghz));
            }

            auto s = Stats::from(samples);
            char prefix[64];
            snprintf(prefix, sizeof(prefix), "%s,%d", lm.name, tc);
            s.print_csv(prefix);
        }
    }
}

// ── T6: worker count sweep ────────────────────────────────────────────────────
// Device analogue: T6_sub_count (number of Sub allocations).
// Sweeps thread pool size at fixed task count.  More workers → lower latency
// until the pool is saturated; beyond that, OS scheduling overhead dominates.

static void t6_worker_count(double ghz)
{
    printf("\n# TEST: T6_worker_count\n");
    printf("# taskCount=64 warmup=%d reps=%d\n", WARMUP, REPS);
    printf("# Device analogue: T6_sub_count — thread pool size → Sub count\n");

    const int TC   = 64;
    const int ncpu = num_cpus();
    printf("# hardware_concurrency=%d\n", ncpu);
    printf("numWorkers,mean_us,std_us,min_us,p50_us,p95_us,p99_us,max_us,n\n");

    std::vector<int> worker_counts;
    for (int n = 1; n < ncpu; n *= 2) worker_counts.push_back(n);
    worker_counts.push_back(ncpu);

    for (int nw : worker_counts)
    {
        Pool pool(nw);

        for (int i = 0; i < WARMUP; i++) pool.dispatch(TC);

        std::vector<double> samples;
        samples.reserve(REPS);
        for (int i = 0; i < REPS; i++)
        {
            uint64_t t0 = timing::start();
            pool.dispatch(TC);
            uint64_t t1 = stop_fn();
            samples.push_back(timing::to_us(t1 - t0, ghz));
        }

        auto s = Stats::from(samples);
        char prefix[32];
        snprintf(prefix, sizeof(prefix), "%d", nw);
        s.print_csv(prefix);
    }
}

// ── T7: batch size sweep ──────────────────────────────────────────────────────
// Device analogue: T7_batch_size (tasks per MU thread).
// Groups TC tasks into batches; each thread handles TC/batchSize tasks.
// Fewer threads → lower spawn overhead, but less parallelism.

static void t7_batch_size(double ghz)
{
    printf("\n# TEST: T7_batch_size\n");
    printf("# taskCount=64 warmup=%d reps=%d\n", WARMUP, REPS);
    printf("# Device analogue: T7_batch_size — tasks per thread → tasks per MU thread\n");
    printf("batchSize,active_threads,mean_us,std_us,min_us,p50_us,p95_us,p99_us,max_us,n\n");

    const int TC = 64;
    const std::vector<int> batch_sizes = {1, 2, 4, 8, 16, 32, 64};

    for (int bs : batch_sizes)
    {
        if (bs > TC) break;
        const int nthreads = TC / bs;

        auto batched_fan_out = [nthreads]()
        {
            std::vector<std::thread> threads;
            threads.reserve(nthreads);
            for (int i = 0; i < nthreads; i++)
                threads.emplace_back([](){});
            for (auto& t : threads) t.join();
        };

        for (int i = 0; i < WARMUP; i++) batched_fan_out();

        std::vector<double> samples;
        samples.reserve(REPS);
        for (int i = 0; i < REPS; i++)
        {
            uint64_t t0 = timing::start();
            batched_fan_out();
            uint64_t t1 = stop_fn();
            samples.push_back(timing::to_us(t1 - t0, ghz));
        }

        auto s = Stats::from(samples);
        char prefix[64];
        snprintf(prefix, sizeof(prefix), "%d,%d", bs, nthreads);
        s.print_csv(prefix);
    }
}

// ── T8: core mask sweep ───────────────────────────────────────────────────────
// Device analogue: T8_cluster_bitmap (restrict active clusters via bitmask).
// Restricts the thread pool to a subset of CPU cores via sched_setaffinity,
// sweeping population count from 1 to all cores.

static void t8_core_mask(double ghz)
{
    printf("\n# TEST: T8_core_mask\n");
    printf("# taskCount=64 warmup=%d reps=%d\n", WARMUP, REPS);
    printf("# Device analogue: T8_cluster_bitmap — CPU core mask → cluster bitmap\n");

    const int TC   = 64;
    const int ncpu = num_cpus();
    printf("# hardware_concurrency=%d\n", ncpu);
    printf("# mask=all means no restriction (all cores available)\n");
    printf("core_count,mean_us,std_us,min_us,p50_us,p95_us,p99_us,max_us,n\n");

    std::vector<int> core_counts;
    for (int n = 1; n < ncpu; n *= 2) core_counts.push_back(n);
    core_counts.push_back(ncpu);

    for (int nc : core_counts)
    {
        // Build a cpu_set with nc contiguous cores.
        cpu_set_t mask;
        CPU_ZERO(&mask);
        for (int i = 0; i < nc; i++) CPU_SET(i, &mask);

        auto masked_fan_out = [TC, &mask]()
        {
            std::vector<std::thread> threads;
            threads.reserve(TC);
            for (int i = 0; i < TC; i++)
            {
                threads.emplace_back([&mask]()
                {
                    pthread_setaffinity_np(pthread_self(), sizeof(mask), &mask);
                });
            }
            for (auto& t : threads) t.join();
        };

        for (int i = 0; i < WARMUP; i++) masked_fan_out();

        std::vector<double> samples;
        samples.reserve(REPS);
        for (int i = 0; i < REPS; i++)
        {
            uint64_t t0 = timing::start();
            masked_fan_out();
            uint64_t t1 = stop_fn();
            samples.push_back(timing::to_us(t1 - t0, ghz));
        }

        auto s = Stats::from(samples);
        char prefix[32];
        snprintf(prefix, sizeof(prefix), "%d", nc);
        s.print_csv(prefix);
    }
}

// ── T9: argument type sweep ───────────────────────────────────────────────────
// Device analogue: T9_arg_type (GVA→SPA translation cost per pointer arg).
// On the host, pointer passing has no address translation overhead; the
// expected result is flat latency across all arg types — which is itself
// a useful data point when compared against the device results.

static volatile int host_sink = 0;  // prevent elision

static void t9_arg_type(double ghz)
{
    printf("\n# TEST: T9_arg_type\n");
    printf("# taskCount=64 warmup=%d reps=%d\n", WARMUP, REPS);
    printf("# Device analogue: T9_arg_type — host pointers have no GVA→SPA cost\n");
    printf("# Expected: flat latency across all arg types (contrast with device results)\n");
    printf("kernel,arg_desc,mean_us,std_us,min_us,p50_us,p95_us,p99_us,max_us,n\n");

    const int TC = 64;
    int dummy = 0;
    int buf[64] = {};
    int* ptr = buf;

    const struct { const char* name; const char* desc; int nptr; int nscalar; } kernels[] = {
        { "null_kernel",  "1_scalar",  0, 1 },
        { "null_3scalar", "3_scalars", 0, 3 },
        { "null_1ptr",    "1_ptr",     1, 0 },
        { "null_3ptr",    "3_ptrs",    3, 0 },
        { "null_6ptr",    "6_ptrs",    6, 0 },
    };

    for (auto& k : kernels)
    {
        auto run_tasks = [&]()
        {
            std::vector<std::thread> threads;
            threads.reserve(TC);
            for (int i = 0; i < TC; i++)
            {
                threads.emplace_back([&k, &dummy, &ptr]()
                {
                    // Touch args to prevent elision; mirrors device arg-parse work.
                    volatile int s = 0;
                    for (int j = 0; j < k.nscalar; j++) s += dummy;
                    for (int j = 0; j < k.nptr;    j++) s += *ptr;
                    host_sink = s;
                });
            }
            for (auto& t : threads) t.join();
        };

        for (int i = 0; i < WARMUP; i++) run_tasks();

        std::vector<double> samples;
        samples.reserve(REPS);
        for (int i = 0; i < REPS; i++)
        {
            uint64_t t0 = timing::start();
            run_tasks();
            uint64_t t1 = stop_fn();
            samples.push_back(timing::to_us(t1 - t0, ghz));
        }

        auto s = Stats::from(samples);
        char prefix[64];
        snprintf(prefix, sizeof(prefix), "%s,%s", k.name, k.desc);
        s.print_csv(prefix);
    }
}

// ── main ──────────────────────────────────────────────────────────────────────

static void usage(const char* prog)
{
    fprintf(stderr,
        "Usage: %s [tests...]\n"
        "  Tests: t1 t2 t3 t4 t5 t6 t7 t8 t9  (default: all)\n"
        "  t1  thread_sweep      — thread spawn+join overhead vs thread count\n"
        "  t2  warmup_convergence— cold-start latency series\n"
        "  t3  phase_split       — spawn time vs join time\n"
        "  t4  pool_reuse        — persistent pool reuse vs fresh threads per dispatch\n"
        "  t5  locality_mode     — compact vs spread CPU affinity\n"
        "  t6  worker_count      — thread pool size vs latency\n"
        "  t7  batch_size        — tasks-per-thread vs spawn overhead\n"
        "  t8  core_mask         — active CPU core count vs latency\n"
        "  t9  arg_type          — argument type (host has no GVA→SPA cost)\n",
        prog);
}

int main(int argc, char* argv[])
{
    bool run[10] = {};

    if (argc == 1)
    {
        for (int i = 1; i <= 9; i++) run[i] = true;
    }
    else
    {
        for (int i = 1; i < argc; i++)
        {
            if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0)
                { usage(argv[0]); return 0; }
            if (strlen(argv[i]) == 2 && argv[i][0] == 't')
            {
                int n = argv[i][1] - '0';
                if (n >= 1 && n <= 9) { run[n] = true; continue; }
            }
            fprintf(stderr, "Unknown argument: %s\n", argv[i]);
            usage(argv[0]); return 1;
        }
    }

    printf("# Orchestration Tax Benchmark Suite — Host Only\n");
    printf("# Device dispatch replaced with std::thread fan-out\n");

    pin_to_cpu0();

    if (timing::has_rdtscp())
    {
        stop_fn = timing::stop_precise;
        printf("# tsc_stop: rdtscp+lfence (precise)\n");
    }
    else
    {
        stop_fn = timing::stop;
        printf("# tsc_stop: lfence+rdtsc (portable)\n");
    }

    printf("# hardware_concurrency: %d\n", num_cpus());
    printf("# Calibrating TSC frequency...\n");
    double ghz = timing::tsc_ghz();
    printf("# tsc_freq_ghz: %.4f\n", ghz);

    if (run[1]) t1_thread_sweep(ghz);
    if (run[2]) t2_warmup_convergence(ghz);
    if (run[3]) t3_phase_split(ghz);
    if (run[4]) t4_pool_reuse(ghz);
    if (run[5]) t5_locality_mode(ghz);
    if (run[6]) t6_worker_count(ghz);
    if (run[7]) t7_batch_size(ghz);
    if (run[8]) t8_core_mask(ghz);
    if (run[9]) t9_arg_type(ghz);

    printf("\n# Done.\n");
    return 0;
}
