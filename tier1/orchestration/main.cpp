// Orchestration Tax Benchmark Suite
//
// Decomposes the fixed per-invocation overhead (α) of MX1 task dispatch into
// fifteen orthogonal axes:
//
//   T1  dispatch_sweep      — core α vs taskCount + OLS linear fit for α extraction
//   T2  warmup_convergence  — how many invocations until latency stabilises
//   T3  phase_split         — execute() doorbell write vs synchronize() wait
//   T4  map_reuse           — Map object reuse vs rebuild per invocation
//   T5  locality_mode       — CompactMode vs SpreadMode at fixed taskCount
//   T6  sub_count           — Sub allocation count vs dispatch latency (static)
//   T7  batch_size          — setBatchSize() effect: fewer MU threads, less NoC fan-out
//   T8  cluster_bitmap      — setClusterBitmap() direct cluster activation sweep
//   T9  arg_type            — Constant vs DeviceMemory: GVA→SPA lookup cost
//   T10 stream_pipelining   — async streams: pipelined vs sequential, queue depth
//   T11 prepare_cost        — createModule / createFunction / createJob / load / subAlloc
//   T12 ndarray_arg         — NDArray descriptor size vs DeviceMemory pointer
//   T13 concurrent_maps     — multiple Maps on one Job, dispatched from N host threads
//   T14 partial_subfree     — dynamic subFree(n) — re-routing cost (vs T6 static)
//   T15 persistent          — α amortization: in-kernel loop vs per-iter dispatch
//
// Output format: each test prints a CSV block preceded by a "# TEST:" header,
// so results can be piped directly to a file and parsed per-section.

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <future>
#include <sched.h>
#include <string>
#include <thread>
#include <vector>

#include "pxl/pxl.hpp"
#include "stats.hpp"
#include "timing.hpp"

static const char* KERNEL_FILE = "mu_kernel/mu_kernel.mubin";
static const char* KERNEL_FUNC = "null_kernel";

// ── tunables ──────────────────────────────────────────────────────────────────
static const int WARMUP = 20;   // invocations discarded before timing starts
static const int REPS   = 200;  // timed invocations per configuration
// ─────────────────────────────────────────────────────────────────────────────

static volatile pxl::PxlResult sink;  // prevents the compiler eliding execute()

// Resolved once in main() based on CPUID; used by all measurement loops.
// stop_fn points to stop() (portable) or stop_precise() (real silicon).
static uint64_t (*stop_fn)() = timing::stop;

// Guard: abort on PxlResult failure with a message.
static void check(pxl::PxlResult r, const char* msg)
{
    if (r != pxl::PxlResult::Success)
    {
        fprintf(stderr, "FATAL: %s\n", msg);
        std::exit(1);
    }
}

// ── shared setup ──────────────────────────────────────────────────────────────

struct Ctx
{
    pxl::runtime::Context*  ctx    = nullptr;
    pxl::runtime::Job*      job    = nullptr;
    pxl::kernel::Module*    module = nullptr;
    pxl::kernel::Function*  func   = nullptr;
};

static Ctx make_ctx(uint32_t numSub = 0)
{
    Ctx c;
    c.ctx = (numSub == 0) ? pxl::runtime::createContext(0)
                           : pxl::runtime::createContext(0);
    if (!c.ctx) { fprintf(stderr, "createContext failed\n"); std::exit(1); }

    c.job = (numSub == 0) ? c.ctx->createJob()
                           : c.ctx->createJob(numSub);
    if (!c.job) { fprintf(stderr, "createJob failed\n"); std::exit(1); }

    c.module = pxl::kernel::createModule(KERNEL_FILE);
    if (!c.module) { fprintf(stderr, "createModule failed: %s\n", KERNEL_FILE); std::exit(1); }

    check(c.job->load(c.module), "job->load");

    c.func = c.module->createFunction(KERNEL_FUNC);
    if (!c.func) { fprintf(stderr, "createFunction failed: %s\n", KERNEL_FUNC); std::exit(1); }

    return c;
}

static void destroy_ctx(Ctx& c)
{
    if (c.job)  c.ctx->destroyJob(c.job);
    if (c.ctx)  pxl::runtime::destroyContext(c.ctx);
    c = {};
}

// Inline dispatch+sync loop used by most tests.
// Returns samples in microseconds.
static std::vector<double> dispatch_loop(pxl::runtime::Map* map, int warmup,
                                          int reps, double ghz)
{
    int dummy = 0;
    for (int i = 0; i < warmup; i++)
    {
        sink = map->execute(dummy);
        map->synchronize();
    }

    std::vector<double> samples;
    samples.reserve(reps);
    for (int i = 0; i < reps; i++)
    {
        uint64_t t0 = timing::start();
        sink = map->execute(dummy);
        map->synchronize();
        uint64_t t1 = stop_fn();
        samples.push_back(timing::to_us(t1 - t0, ghz));
    }
    return samples;
}

// ── T1: dispatch sweep ────────────────────────────────────────────────────────
//
// Measures total dispatch latency (α) across a range of taskCounts.
// The curve shape exposes NoC congestion: a linear fit gives the y-intercept
// (fixed firmware overhead) and the slope (per-task NoC contribution).
//
// Note: taskCount in the SDK is the unit of parallelism visible to the host.
// The device maps these onto physical clusters internally. The mapping ratio
// (tasks/cluster) is hardware-specific and not exposed by this API.

static void t1_dispatch_sweep(double ghz)
{
    printf("\n# TEST: T1_dispatch_sweep\n");
    printf("# warmup=%d reps=%d\n", WARMUP, REPS);
    printf("# Linear fit y = alpha + slope*taskCount estimates the fixed firmware\n");
    printf("# overhead (alpha, us) and per-task NoC contribution (slope, us/task).\n");
    printf("taskCount,mean_us,std_us,min_us,p50_us,p95_us,p99_us,max_us,n\n");

    // Sweep spans the same range as the existing noop example plus an
    // extended upper end to capture the super-linear congestion regime.
    const std::vector<int> task_counts = {
        1, 2, 4, 8, 16, 32, 48, 64, 96, 128, 192, 256,
        384, 512, 768, 1024, 1536, 2048
    };

    std::vector<double> xs, ys;  // for linear fit
    Ctx c = make_ctx();
    for (int tc : task_counts)
    {
        auto map = c.job->buildMap(c.func, tc);
        if (!map) { fprintf(stderr, "buildMap failed for taskCount=%d\n", tc); continue; }

        auto samples = dispatch_loop(map, WARMUP, REPS, ghz);
        auto s = Stats::from(samples);

        char prefix[32];
        snprintf(prefix, sizeof(prefix), "%d", tc);
        s.print_csv(prefix);

        xs.push_back(static_cast<double>(tc));
        ys.push_back(s.mean);

        // Print device-side stats string as a comment for cross-validation.
        std::string dev_stats = map->getStats();
        if (!dev_stats.empty())
            printf("# getStats tc=%d: %s\n", tc, dev_stats.c_str());

        c.job->destroyMap(map);
    }
    destroy_ctx(c);

    // OLS linear fit to extract α (y-intercept) and β (slope).
    LinFit fit;
    if (LinFit::fit(xs, ys, fit))
    {
        printf("# OLS fit: alpha_us=%.4f  slope_us_per_task=%.6f  R2=%.4f\n",
               fit.alpha, fit.slope, fit.r2);
        printf("# Interpretation: alpha=%.2f us is the fixed firmware overhead;\n", fit.alpha);
        printf("#   each additional task adds ~%.4f us of NoC/dispatch cost.\n", fit.slope);
        printf("# Note: R2<0.9 indicates super-linear congestion — fit a polynomial.\n");
    }
}

// ── T2: warmup convergence ────────────────────────────────────────────────────
//
// Records per-invocation latency for N sequential dispatches without any
// prior warmup. Reveals the number of cold invocations the firmware needs
// before settling to steady-state α (firmware cache priming, JIT effects,
// cache line population of the control path, etc.).

static void t2_warmup_convergence(double ghz)
{
    printf("\n# TEST: T2_warmup_convergence\n");
    printf("# taskCount=64 no_prior_warmup\n");
    printf("invocation,latency_us\n");

    const int TC     = 64;
    const int N_COLD = 100;  // record this many sequential invocations cold

    Ctx c = make_ctx();
    auto map = c.job->buildMap(c.func, TC);
    if (!map) { fprintf(stderr, "T2: buildMap failed\n"); return; }

    int dummy = 0;
    for (int i = 0; i < N_COLD; i++)
    {
        uint64_t t0 = timing::start();
        sink = map->execute(dummy);
        map->synchronize();
        uint64_t t1 = stop_fn();
        printf("%d,%.4f\n", i, timing::to_us(t1 - t0, ghz));
    }

    c.job->destroyMap(map);
    destroy_ctx(c);
}

// ── T3: phase split ───────────────────────────────────────────────────────────
//
// Splits the round-trip into two phases to answer: how much of α is the
// host-side doorbell write (execute()), and how much is the device-to-host
// completion propagation (synchronize())?
//
// The paper attributes the full α to the "Black Box" delay. This test
// provides the first-level decomposition of that black box.

static void t3_phase_split(double ghz)
{
    printf("\n# TEST: T3_phase_split\n");
    printf("# warmup=%d reps=%d\n", WARMUP, REPS);
    printf("taskCount,execute_us_mean,execute_us_std,sync_us_mean,sync_us_std,total_us_mean,total_us_std\n");

    const std::vector<int> task_counts = {1, 16, 64, 128, 256, 512, 1024};

    Ctx c = make_ctx();
    for (int tc : task_counts)
    {
        auto map = c.job->buildMap(c.func, tc);
        if (!map) continue;

        // Warmup
        int dummy = 0;
        for (int i = 0; i < WARMUP; i++)
        {
            sink = map->execute(dummy);
            map->synchronize();
        }

        std::vector<double> exec_s, sync_s, total_s;
        exec_s.reserve(REPS); sync_s.reserve(REPS); total_s.reserve(REPS);

        for (int i = 0; i < REPS; i++)
        {
            uint64_t t0 = timing::start();
            sink = map->execute(dummy);
            uint64_t t1 = stop_fn();
            map->synchronize();
            uint64_t t2 = stop_fn();

            exec_s.push_back(timing::to_us(t1 - t0, ghz));
            sync_s.push_back(timing::to_us(t2 - t1, ghz));
            total_s.push_back(timing::to_us(t2 - t0, ghz));
        }

        auto es = Stats::from(exec_s);
        auto ss = Stats::from(sync_s);
        auto ts = Stats::from(total_s);
        printf("%d,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f\n",
               tc, es.mean, es.stddev, ss.mean, ss.stddev, ts.mean, ts.stddev);

        c.job->destroyMap(map);
    }
    destroy_ctx(c);
}

// ── T4: map reuse vs rebuild ──────────────────────────────────────────────────
//
// Tests whether the firmware caches state across invocations of the same Map
// object. If the firmware JITs task descriptors or populates routing tables
// lazily, reusing the same Map pointer may yield lower steady-state α than
// calling destroyMap+buildMap before each dispatch.
//
// Mode "reuse"  : one Map object, N sequential execute+synchronize calls.
// Mode "rebuild": destroyMap+buildMap before every execute+synchronize.

static void t4_map_reuse(double ghz)
{
    printf("\n# TEST: T4_map_reuse\n");
    printf("# warmup=%d reps=%d\n", WARMUP, REPS);
    printf("mode,taskCount,mean_us,std_us,min_us,p50_us,p95_us,p99_us,max_us,n\n");

    const std::vector<int> task_counts = {1, 64, 256};

    Ctx c = make_ctx();
    int dummy = 0;

    for (int tc : task_counts)
    {
        // --- reuse mode ---
        {
            auto map = c.job->buildMap(c.func, tc);
            if (!map) continue;

            auto samples = dispatch_loop(map, WARMUP, REPS, ghz);
            auto s = Stats::from(samples);

            char prefix[64];
            snprintf(prefix, sizeof(prefix), "reuse,%d", tc);
            s.print_csv(prefix);

            c.job->destroyMap(map);
        }

        // --- rebuild mode ---
        {
            // Warmup with a fresh map each time too
            for (int i = 0; i < WARMUP; i++)
            {
                auto m = c.job->buildMap(c.func, tc);
                sink = m->execute(dummy);
                m->synchronize();
                c.job->destroyMap(m);
            }

            std::vector<double> samples;
            samples.reserve(REPS);
            for (int i = 0; i < REPS; i++)
            {
                uint64_t t0 = timing::start();
                auto m = c.job->buildMap(c.func, tc);
                sink = m->execute(dummy);
                m->synchronize();
                c.job->destroyMap(m);
                uint64_t t1 = stop_fn();
                samples.push_back(timing::to_us(t1 - t0, ghz));
            }

            auto s = Stats::from(samples);
            char prefix[64];
            snprintf(prefix, sizeof(prefix), "rebuild,%d", tc);
            s.print_csv(prefix);
        }
    }

    destroy_ctx(c);
}

// ── T5: locality mode ─────────────────────────────────────────────────────────
//
// Tests whether the cluster assignment policy (compact vs spread) changes
// dispatch latency. CompactMode fills one cluster before spilling to the next;
// SpreadMode distributes tasks evenly across all available clusters.
//
// Hypothesis: SpreadMode increases NoC traversal distance for the control
// path, raising α at mid-range taskCounts but potentially improving data
// locality for memory-bound kernels (out of scope for null kernel).

static void t5_locality_mode(double ghz)
{
    printf("\n# TEST: T5_locality_mode\n");
    printf("# warmup=%d reps=%d\n", WARMUP, REPS);
    printf("mode,taskCount,mean_us,std_us,min_us,p50_us,p95_us,p99_us,max_us,n\n");

    const std::vector<int> task_counts = {1, 16, 64, 128, 256, 512, 1024};
    const struct { const char* name; pxl::LocalityMode mode; } modes[] = {
        { "compact", pxl::LocalityMode::CompactMode },
        { "spread",  pxl::LocalityMode::SpreadMode  },
    };

    Ctx c = make_ctx();
    for (auto& lm : modes)
    {
        for (int tc : task_counts)
        {
            auto map = c.job->buildMap(c.func, tc);
            if (!map) continue;
            map->setLocalityMode(lm.mode);

            auto samples = dispatch_loop(map, WARMUP, REPS, ghz);
            auto s = Stats::from(samples);

            char prefix[64];
            snprintf(prefix, sizeof(prefix), "%s,%d", lm.name, tc);
            s.print_csv(prefix);

            c.job->destroyMap(map);
        }
    }
    destroy_ctx(c);
}

// ── T6: sub count effect ──────────────────────────────────────────────────────
//
// Tests how allocating more Sub units to a Job affects dispatch latency.
// In the paper's language, this sweeps "active compute clusters": more Subs
// means the NoC control path must reach farther to initialise all targets,
// directly expanding the NoC Routing stage of α.
//
// Because subAlloc may fail if the device has limited Sub capacity, this test
// probes aggressively from 1 upwards and stops on the first failure.

static void t6_sub_count(double ghz)
{
    printf("\n# TEST: T6_sub_count\n");
    printf("# taskCount=64 warmup=%d reps=%d\n", WARMUP, REPS);

    // Query available Sub count so the sweep never exceeds hardware capacity.
    auto* probe = pxl::runtime::createContext(0);
    uint32_t max_sub = 1;
    if (probe)
    {
        max_sub = probe->availableSubCount();
        pxl::runtime::destroyContext(probe);
    }
    printf("# availableSubCount=%u\n", max_sub);
    printf("numSub,mean_us,std_us,min_us,p50_us,p95_us,p99_us,max_us,n\n");

    const int TC = 64;

    // Build sweep: powers of two up to max_sub, always including max_sub itself.
    std::vector<uint32_t> sub_counts;
    for (uint32_t ns = 1; ns < max_sub; ns *= 2)
        sub_counts.push_back(ns);
    sub_counts.push_back(max_sub);

    for (uint32_t ns : sub_counts)
    {
        auto* ctx = pxl::runtime::createContext(0);
        if (!ctx) break;

        auto* job = ctx->createJob(ns);
        if (!job)
        {
            fprintf(stderr, "T6: createJob(%u) failed\n", ns);
            pxl::runtime::destroyContext(ctx);
            break;
        }

        auto* module = pxl::kernel::createModule(KERNEL_FILE);
        if (!module || job->load(module) != pxl::PxlResult::Success)
        {
            fprintf(stderr, "T6: load failed for numSub=%u\n", ns);
            ctx->destroyJob(job); pxl::runtime::destroyContext(ctx);
            break;
        }

        auto* func = module->createFunction(KERNEL_FUNC);
        if (!func) { ctx->destroyJob(job); pxl::runtime::destroyContext(ctx); break; }

        auto* map = job->buildMap(func, TC);
        if (!map)
        {
            fprintf(stderr, "T6: buildMap failed for numSub=%u\n", ns);
            ctx->destroyJob(job); pxl::runtime::destroyContext(ctx);
            break;
        }

        auto samples = dispatch_loop(map, WARMUP, REPS, ghz);
        auto s = Stats::from(samples);

        char prefix[32];
        snprintf(prefix, sizeof(prefix), "%u", ns);
        s.print_csv(prefix);

        job->destroyMap(map);
        ctx->destroyJob(job);
        pxl::runtime::destroyContext(ctx);
    }
}

// ── T7: batchSize sweep ───────────────────────────────────────────────────────
//
// setBatchSize(n) groups n tasks into each MU thread, so the NoC control path
// must initialise taskCount/n MU threads instead of taskCount.  At the null-
// kernel level (zero compute per task) this isolates the per-thread dispatch
// fan-out cost: halving the active thread count should roughly halve the NoC
// routing portion of α while leaving the fixed firmware overhead unchanged.
//
// This is not in any existing SDK example and is not discussed in the paper.

static void t7_batch_size(double ghz)
{
    printf("\n# TEST: T7_batch_size\n");
    printf("# taskCount=64 warmup=%d reps=%d\n", WARMUP, REPS);
    printf("# batchSize controls tasks-per-MU-thread; active_threads = taskCount/batchSize\n");
    printf("batchSize,active_threads,mean_us,std_us,min_us,p50_us,p95_us,p99_us,max_us,n\n");

    const int TC = 64;
    const std::vector<int> batch_sizes = {1, 2, 4, 8, 16, 32, 64};

    Ctx c = make_ctx();
    for (int bs : batch_sizes)
    {
        if (bs > TC) break;

        auto map = c.job->buildMap(c.func, TC);
        if (!map) continue;
        map->setBatchSize(static_cast<uint32_t>(bs));

        auto samples = dispatch_loop(map, WARMUP, REPS, ghz);
        auto s = Stats::from(samples);

        char prefix[64];
        snprintf(prefix, sizeof(prefix), "%d,%d", bs, TC / bs);
        s.print_csv(prefix);

        c.job->destroyMap(map);
    }
    destroy_ctx(c);
}

// ── T8: cluster bitmap sweep ──────────────────────────────────────────────────
//
// setClusterBitmap(mask) directly controls which clusters the firmware
// activates for this Map, independent of taskCount.  This is the correct
// lever for the paper's "sweep active compute clusters from 1 to 512"
// experiment — T1's taskCount sweep is an indirect proxy; the bitmap gives
// exact control.
//
// Bitmap semantics (per API doc): bit k = 1 → cluster k is active;
// mask=0 → all clusters active (hardware default).
//
// We hold taskCount constant at 64 and sweep the population count of the mask
// from 1 to all-ones, measuring how NoC routing distance scales with the
// number of activated clusters.

static void t8_cluster_bitmap(double ghz)
{
    printf("\n# TEST: T8_cluster_bitmap\n");
    printf("# taskCount=64 warmup=%d reps=%d\n", WARMUP, REPS);
    printf("# mask=0 means all clusters active (hardware default)\n");

    const int TC = 64;

    // Query NumClusterPerSub so we never set bits beyond the valid range.
    // The bitmap is per-Sub and is NumClusterPerSub bits wide.
    // Exceeding this triggers a PXL assertion in setClusterBitmap().
    auto* probe_ctx = pxl::runtime::createContext(0);
    uint64_t num_clusters = 32;  // safe default if query fails
    if (probe_ctx)
    {
        probe_ctx->getAttribute(pxl::DeviceAttr::NumClusterPerSub, num_clusters);
        pxl::runtime::destroyContext(probe_ctx);
    }
    uint32_t max_valid_mask = (num_clusters >= 32)
                                  ? 0xffffffffu
                                  : (1u << static_cast<uint32_t>(num_clusters)) - 1u;

    printf("# NumClusterPerSub=%lu  max_valid_mask=0x%08x\n", num_clusters, max_valid_mask);
    printf("mask_hex,popcount,mean_us,std_us,min_us,p50_us,p95_us,p99_us,max_us,n\n");

    // Generate one mask per power-of-two popcount up to num_clusters,
    // then the all-clusters-active mask (0), skipping any that exceed max_valid_mask.
    std::vector<std::pair<uint32_t, int>> masks;  // (mask, popcount); popcount=-1 means "all"
    for (uint64_t pop = 1; pop <= num_clusters; pop *= 2)
    {
        uint32_t mask = (pop >= 32) ? 0xffffffffu : (1u << static_cast<uint32_t>(pop)) - 1u;
        if (mask > max_valid_mask) break;
        masks.emplace_back(mask, static_cast<int>(pop));
    }
    masks.emplace_back(0u, -1);  // 0 = all clusters active

    Ctx c = make_ctx();
    for (auto& [mask, pop] : masks)
    {
        auto map = c.job->buildMap(c.func, TC);
        if (!map) continue;
        map->setClusterBitmap(mask);

        auto samples = dispatch_loop(map, WARMUP, REPS, ghz);
        auto s = Stats::from(samples);

        char prefix[64];
        if (pop < 0)
            snprintf(prefix, sizeof(prefix), "0x%08x,all", mask);
        else
            snprintf(prefix, sizeof(prefix), "0x%08x,%d", mask, pop);
        s.print_csv(prefix);

        c.job->destroyMap(map);
    }
    destroy_ctx(c);
}

// ── T9: argument type sweep ───────────────────────────────────────────────────
//
// The paper's §3.1 stage (1) includes GVA→SPA address translation for every
// device-memory pointer argument.  Scalar arguments (ArgType::Constant) bypass
// this step.  This test holds everything else fixed and varies only the number
// and type of arguments, isolating the translation cost within α.
//
// Kernels used (all zero-work, defined in mu_null.cpp):
//   null_kernel   : 1 scalar  (baseline)
//   null_3scalar  : 3 scalars (translation cost = 0, but larger descriptor)
//   null_1ptr     : 1 pointer (1 GVA→SPA lookup)
//   null_3ptr     : 3 pointers (3 GVA→SPA lookups)
//   null_6ptr     : 6 pointers (6 GVA→SPA lookups, approaching the 9-arg limit)
//
// Note: execute() with zero arguments is explicitly unsupported by the SDK
// (the overload returns Failure), so 1 scalar is the minimum baseline.

static void t9_arg_type(double ghz)
{
    printf("\n# TEST: T9_arg_type\n");
    printf("# taskCount=64 warmup=%d reps=%d\n", WARMUP, REPS);
    printf("kernel,arg_desc,mean_us,std_us,min_us,p50_us,p95_us,p99_us,max_us,n\n");

    const int TC = 64;

    Ctx c = make_ctx();

    // Allocate a small device buffer to back the pointer arguments.
    // The null kernels never dereference it; we just need a valid device address
    // so the SDK performs the GVA→SPA translation on the execute() path.
    int* dev_buf = reinterpret_cast<int*>(c.ctx->memAlloc(64 * sizeof(int)));
    if (!dev_buf)
    {
        fprintf(stderr, "T9: memAlloc failed\n");
        destroy_ctx(c);
        return;
    }

    const struct { const char* fname; const char* desc; } kernels[] = {
        { "null_kernel",  "1_scalar"  },
        { "null_3scalar", "3_scalars" },
        { "null_1ptr",    "1_ptr"     },
        { "null_3ptr",    "3_ptrs"    },
        { "null_6ptr",    "6_ptrs"    },
    };

    int dummy = 0;
    for (auto& k : kernels)
    {
        auto* fn = c.module->createFunction(k.fname);
        if (!fn)
        {
            fprintf(stderr, "T9: createFunction(%s) failed\n", k.fname);
            continue;
        }

        auto* map = c.job->buildMap(fn, TC);
        if (!map) continue;

        // Warmup
        for (int i = 0; i < WARMUP; i++)
        {
            if      (strcmp(k.fname, "null_kernel")  == 0) sink = map->execute(dummy);
            else if (strcmp(k.fname, "null_3scalar") == 0) sink = map->execute(dummy, dummy, dummy);
            else if (strcmp(k.fname, "null_1ptr")    == 0) sink = map->execute(dev_buf);
            else if (strcmp(k.fname, "null_3ptr")    == 0) sink = map->execute(dev_buf, dev_buf, dev_buf);
            else if (strcmp(k.fname, "null_6ptr")    == 0) sink = map->execute(dev_buf, dev_buf, dev_buf,
                                                                                dev_buf, dev_buf, dev_buf);
            map->synchronize();
        }

        // Timed
        std::vector<double> samples;
        samples.reserve(REPS);
        for (int i = 0; i < REPS; i++)
        {
            uint64_t t0 = timing::start();
            if      (strcmp(k.fname, "null_kernel")  == 0) sink = map->execute(dummy);
            else if (strcmp(k.fname, "null_3scalar") == 0) sink = map->execute(dummy, dummy, dummy);
            else if (strcmp(k.fname, "null_1ptr")    == 0) sink = map->execute(dev_buf);
            else if (strcmp(k.fname, "null_3ptr")    == 0) sink = map->execute(dev_buf, dev_buf, dev_buf);
            else if (strcmp(k.fname, "null_6ptr")    == 0) sink = map->execute(dev_buf, dev_buf, dev_buf,
                                                                                dev_buf, dev_buf, dev_buf);
            map->synchronize();
            uint64_t t1 = stop_fn();
            samples.push_back(timing::to_us(t1 - t0, ghz));
        }

        auto s = Stats::from(samples);
        char prefix[64];
        snprintf(prefix, sizeof(prefix), "%s,%s", k.fname, k.desc);
        s.print_csv(prefix);

        c.job->destroyMap(map);
    }

    c.ctx->memFree(dev_buf);
    destroy_ctx(c);
}

// ── T10: stream concurrency / async pipelining ────────────────────────────────
//
// T1–T9 all run execute → synchronize back-to-back, so they measure *serial* α.
// The async path is exposed by Stream + setStream() but unmeasured.  Two probes:
//
//   pipelined: K maps on K streams, fire all execute() calls, then sync the
//              last map.  If pipelining works, total wall-clock is ≪ K × α.
//
//   queue_depth: fire N execute() calls on one map without sync, then sync once.
//                Plots firmware submission-queue capacity / overlap.

static void t10_stream_pipelining(double ghz)
{
    printf("\n# TEST: T10_stream_pipelining\n");
    printf("# warmup=%d reps=%d\n", WARMUP, REPS);
    printf("# Sub-test A: pipelined K maps on K streams vs sequential K maps\n");
    printf("#   parallel_speedup = (K * sequential_per_map_us) / pipelined_total_us\n");
    printf("#   Speedup > 1 means streams overlap dispatch.\n");
    printf("# Sub-test B: queue depth — N execute() calls before single sync.\n");

    const int TC = 64;
    const std::vector<int> stream_counts = {1, 2, 4, 8};
    const std::vector<int> queue_depths  = {1, 2, 4, 8, 16, 32};

    Ctx c = make_ctx();
    int dummy = 0;

    // ── Sub-test A: K maps × K streams ────────────────────────────────────────
    printf("# Sub-test A: streams_pipelined\n");
    printf("mode,K,total_us_mean,total_us_std,per_map_us,n\n");

    for (int K : stream_counts)
    {
        // Build K independent Maps + K independent Streams.
        std::vector<pxl::runtime::Stream*> streams(K, nullptr);
        std::vector<pxl::runtime::Map*>    maps(K,    nullptr);
        bool stream_ok = true;
        for (int k = 0; k < K; k++)
        {
            streams[k] = pxl::runtime::createStream();
            if (!streams[k]) { stream_ok = false; break; }
            maps[k] = c.job->buildMap(c.func, TC);
            if (!maps[k]) { stream_ok = false; break; }
            maps[k]->setStream(streams[k]);
        }
        if (!stream_ok)
        {
            fprintf(stderr, "T10A: stream/map setup failed for K=%d (graceful skip)\n", K);
            for (auto* s : streams) if (s) pxl::runtime::destroyStream(s);
            for (auto* m : maps)    if (m) c.job->destroyMap(m);
            continue;
        }

        // Warmup
        for (int i = 0; i < WARMUP; i++)
        {
            for (int k = 0; k < K; k++) sink = maps[k]->execute(dummy);
            for (int k = 0; k < K; k++) maps[k]->synchronize();
        }

        // Pipelined: fire all K, then sync all K.
        std::vector<double> par_samples;
        par_samples.reserve(REPS);
        for (int i = 0; i < REPS; i++)
        {
            uint64_t t0 = timing::start();
            for (int k = 0; k < K; k++) sink = maps[k]->execute(dummy);
            for (int k = 0; k < K; k++) maps[k]->synchronize();
            uint64_t t1 = stop_fn();
            par_samples.push_back(timing::to_us(t1 - t0, ghz));
        }
        auto par_s = Stats::from(par_samples);

        // Sequential: execute+sync each map back-to-back (same K maps).
        std::vector<double> seq_samples;
        seq_samples.reserve(REPS);
        for (int i = 0; i < REPS; i++)
        {
            uint64_t t0 = timing::start();
            for (int k = 0; k < K; k++)
            {
                sink = maps[k]->execute(dummy);
                maps[k]->synchronize();
            }
            uint64_t t1 = stop_fn();
            seq_samples.push_back(timing::to_us(t1 - t0, ghz));
        }
        auto seq_s = Stats::from(seq_samples);

        printf("pipelined,%d,%.4f,%.4f,%.4f,%zu\n",
               K, par_s.mean, par_s.stddev, par_s.mean / K, par_s.n);
        printf("sequential,%d,%.4f,%.4f,%.4f,%zu\n",
               K, seq_s.mean, seq_s.stddev, seq_s.mean / K, seq_s.n);
        if (par_s.mean > 0)
            printf("# K=%d speedup_seq_over_par=%.2fx\n", K, seq_s.mean / par_s.mean);

        for (int k = 0; k < K; k++)
        {
            c.job->destroyMap(maps[k]);
            pxl::runtime::destroyStream(streams[k]);
        }
    }

    // ── Sub-test B: submission queue depth ────────────────────────────────────
    printf("# Sub-test B: queue_depth\n");
    printf("queue_depth,total_us_mean,total_us_std,per_dispatch_us,n\n");

    auto* qmap = c.job->buildMap(c.func, TC);
    if (qmap)
    {
        for (int i = 0; i < WARMUP; i++)
        {
            sink = qmap->execute(dummy);
            qmap->synchronize();
        }

        for (int N : queue_depths)
        {
            std::vector<double> samples;
            samples.reserve(REPS);
            for (int i = 0; i < REPS; i++)
            {
                uint64_t t0 = timing::start();
                for (int n = 0; n < N; n++) sink = qmap->execute(dummy);
                qmap->synchronize();  // single sync after N executes
                uint64_t t1 = stop_fn();
                samples.push_back(timing::to_us(t1 - t0, ghz));
            }
            auto s = Stats::from(samples);
            printf("%d,%.4f,%.4f,%.4f,%zu\n",
                   N, s.mean, s.stddev, s.mean / N, s.n);
        }
        c.job->destroyMap(qmap);
    }

    destroy_ctx(c);
}

// ── T11: prepare cost decomposition ───────────────────────────────────────────
//
// T4 measures Map reuse vs rebuild but folds module load into setup.  The
// "prepare" side of orchestration is a separate budget that a short-lived
// program pays once.  Times each setup stage individually so you can compare
// it against the per-call α from T1.
//
// Stages timed (each in isolation, repeated REPS times with WARMUP):
//   create_module      — pxl::kernel::createModule(path)
//   create_function    — module->createFunction(name)
//   create_context     — pxl::runtime::createContext(0)
//   create_job         — context->createJob()  (default sub count)
//   job_load_module    — job->load(module)     (pre-parsed module)
//   job_load_path      — job->load(path)       (parse from disk)
//   sub_alloc_default  — job->subAlloc()       (all available)
//   destroy_job        — context->destroyJob(job)
//   destroy_context    — pxl::runtime::destroyContext(ctx)

static void t11_prepare_cost(double ghz)
{
    printf("\n# TEST: T11_prepare_cost\n");
    printf("# warmup=%d reps=%d\n", WARMUP, REPS);
    printf("# Each stage is timed in isolation; setup state is reset between reps.\n");
    printf("stage,mean_us,std_us,min_us,p50_us,p95_us,p99_us,max_us,n\n");

    auto run_reps = [&](const char* name, std::function<double()> measure_one)
    {
        for (int i = 0; i < WARMUP; i++) (void)measure_one();
        std::vector<double> samples;
        samples.reserve(REPS);
        for (int i = 0; i < REPS; i++) samples.push_back(measure_one());
        auto s = Stats::from(samples);
        s.print_csv(name);
    };

    // create_module: each rep parses the .mubin file fresh.
    run_reps("create_module", [&]() {
        uint64_t t0 = timing::start();
        auto* m = pxl::kernel::createModule(KERNEL_FILE);
        uint64_t t1 = stop_fn();
        if (m) pxl::kernel::destroyModule(m);
        return timing::to_us(t1 - t0, ghz);
    });

    // create_function: amortized over REPS, single module reused.
    {
        auto* mod = pxl::kernel::createModule(KERNEL_FILE);
        run_reps("create_function", [&]() {
            uint64_t t0 = timing::start();
            auto* fn = mod->createFunction(KERNEL_FUNC);
            uint64_t t1 = stop_fn();
            if (fn) mod->destroyFunction(fn);
            return timing::to_us(t1 - t0, ghz);
        });
        pxl::kernel::destroyModule(mod);
    }

    // create_context: each rep creates+destroys a context.
    run_reps("create_context", [&]() {
        uint64_t t0 = timing::start();
        auto* ctx = pxl::runtime::createContext(0);
        uint64_t t1 = stop_fn();
        if (ctx) pxl::runtime::destroyContext(ctx);
        return timing::to_us(t1 - t0, ghz);
    });

    // create_job: requires a persistent context.
    {
        auto* ctx = pxl::runtime::createContext(0);
        run_reps("create_job", [&]() {
            uint64_t t0 = timing::start();
            auto* j = ctx->createJob();
            uint64_t t1 = stop_fn();
            if (j) ctx->destroyJob(j);
            return timing::to_us(t1 - t0, ghz);
        });
        pxl::runtime::destroyContext(ctx);
    }

    // job_load_module: pre-parsed module path.
    {
        auto* ctx = pxl::runtime::createContext(0);
        auto* mod = pxl::kernel::createModule(KERNEL_FILE);
        run_reps("job_load_module", [&]() {
            auto* j = ctx->createJob();
            uint64_t t0 = timing::start();
            j->load(mod);
            uint64_t t1 = stop_fn();
            ctx->destroyJob(j);
            return timing::to_us(t1 - t0, ghz);
        });
        pxl::kernel::destroyModule(mod);
        pxl::runtime::destroyContext(ctx);
    }

    // job_load_path: parses .mubin from disk per rep.
    {
        auto* ctx = pxl::runtime::createContext(0);
        run_reps("job_load_path", [&]() {
            auto* j = ctx->createJob();
            uint64_t t0 = timing::start();
            j->load(KERNEL_FILE);
            uint64_t t1 = stop_fn();
            ctx->destroyJob(j);
            return timing::to_us(t1 - t0, ghz);
        });
        pxl::runtime::destroyContext(ctx);
    }

    // sub_alloc_default: time only the subAlloc; job creation is amortized.
    {
        auto* ctx = pxl::runtime::createContext(0);
        run_reps("sub_alloc_default", [&]() {
            auto* j = ctx->createJob();
            uint64_t t0 = timing::start();
            j->subAlloc();
            uint64_t t1 = stop_fn();
            j->subFree();
            ctx->destroyJob(j);
            return timing::to_us(t1 - t0, ghz);
        });
        pxl::runtime::destroyContext(ctx);
    }

    // destroy_job: time only the teardown.
    {
        auto* ctx = pxl::runtime::createContext(0);
        run_reps("destroy_job", [&]() {
            auto* j = ctx->createJob();
            uint64_t t0 = timing::start();
            ctx->destroyJob(j);
            uint64_t t1 = stop_fn();
            return timing::to_us(t1 - t0, ghz);
        });
        pxl::runtime::destroyContext(ctx);
    }

    // destroy_context: time only the teardown.
    run_reps("destroy_context", [&]() {
        auto* ctx = pxl::runtime::createContext(0);
        uint64_t t0 = timing::start();
        pxl::runtime::destroyContext(ctx);
        uint64_t t1 = stop_fn();
        return timing::to_us(t1 - t0, ghz);
    });
}

// ── T12: NDArray argument cost ────────────────────────────────────────────────
//
// T9 covered ArgType::Constant and ArgType::DeviceMemory.  ArgType::NDArray
// is the third, with descriptor size ~sizeof(NDArray<int>) (~200 B with shape
// and strides arrays) vs 8 B for a pointer.  Compares 1 and 3 NDArrays at fixed
// tc=64, against the matching null_1ptr / null_3ptr from T9 — the delta is the
// descriptor-marshalling cost component of α.
//
// Also varies the NDArray dimensionality (1D, 3D, 5D) at fixed total size to
// see whether the firmware iterates the shape array linearly with rank.

static void t12_ndarray_arg(double ghz)
{
    printf("\n# TEST: T12_ndarray_arg\n");
    printf("# taskCount=64 warmup=%d reps=%d\n", WARMUP, REPS);
    printf("# Compare against T9 null_1ptr / null_3ptr at the same tc.\n");
    printf("kernel,arg_desc,dims,mean_us,std_us,min_us,p50_us,p95_us,p99_us,max_us,n\n");

    const int TC = 64;
    Ctx c = make_ctx();

    // 1024 ints is enough for shape={1024}, {16,8,8}, and {4,4,4,4,4}.
    int* dev_buf = reinterpret_cast<int*>(c.ctx->memAlloc(1024 * sizeof(int)));
    if (!dev_buf)
    {
        fprintf(stderr, "T12: memAlloc failed\n");
        destroy_ctx(c);
        return;
    }

    // Shape variants — same total elements, different ranks.
    using Shape = pxl::runtime::NDArray<int>::ShapeArray;
    auto make_shape = [](std::initializer_list<int64_t> dims) {
        Shape s; s.fill(0);
        size_t i = 0;
        for (auto d : dims) { if (i < s.size()) s[i++] = d; }
        return s;
    };

    struct ShapeCase { const char* tag; int dims; Shape s; };
    std::vector<ShapeCase> shape_cases = {
        { "1d", 1, make_shape({1024})         },
        { "3d", 3, make_shape({16, 8, 8})     },
        { "5d", 5, make_shape({4, 4, 4, 4, 4})},
    };

    int dummy = 0;
    (void)dummy;

    // null_1ndarray
    {
        auto* fn = c.module->createFunction("null_1ndarray");
        if (fn)
        {
            for (auto& sc : shape_cases)
            {
                auto* map = c.job->buildMap(fn, TC);
                if (!map) continue;

                pxl::runtime::NDArray<int> a(dev_buf, sc.s);

                for (int i = 0; i < WARMUP; i++)
                {
                    sink = map->execute(a);
                    map->synchronize();
                }
                std::vector<double> samples;
                samples.reserve(REPS);
                for (int i = 0; i < REPS; i++)
                {
                    uint64_t t0 = timing::start();
                    sink = map->execute(a);
                    map->synchronize();
                    uint64_t t1 = stop_fn();
                    samples.push_back(timing::to_us(t1 - t0, ghz));
                }
                auto s = Stats::from(samples);
                char prefix[64];
                snprintf(prefix, sizeof(prefix), "null_1ndarray,1_ndarray,%s", sc.tag);
                s.print_csv(prefix);
                c.job->destroyMap(map);
            }
        }
    }

    // null_3ndarray
    {
        auto* fn = c.module->createFunction("null_3ndarray");
        if (fn)
        {
            for (auto& sc : shape_cases)
            {
                auto* map = c.job->buildMap(fn, TC);
                if (!map) continue;

                pxl::runtime::NDArray<int> a(dev_buf, sc.s);
                pxl::runtime::NDArray<int> b(dev_buf, sc.s);
                pxl::runtime::NDArray<int> d(dev_buf, sc.s);

                for (int i = 0; i < WARMUP; i++)
                {
                    sink = map->execute(a, b, d);
                    map->synchronize();
                }
                std::vector<double> samples;
                samples.reserve(REPS);
                for (int i = 0; i < REPS; i++)
                {
                    uint64_t t0 = timing::start();
                    sink = map->execute(a, b, d);
                    map->synchronize();
                    uint64_t t1 = stop_fn();
                    samples.push_back(timing::to_us(t1 - t0, ghz));
                }
                auto s = Stats::from(samples);
                char prefix[64];
                snprintf(prefix, sizeof(prefix), "null_3ndarray,3_ndarrays,%s", sc.tag);
                s.print_csv(prefix);
                c.job->destroyMap(map);
            }
        }
    }

    c.ctx->memFree(dev_buf);
    destroy_ctx(c);
}

// ── T13: concurrent Maps on one Job ───────────────────────────────────────────
//
// Builds K Maps on the same Job and dispatches them from K host threads
// simultaneously.  Tests whether Job-internal dispatch is serialized — if it
// is, concurrent K-thread time ≈ K × single-thread time, putting a hard
// ceiling on multi-Map throughput on a single Job.
//
// concurrent: K threads, each owns one Map, each runs REPS dispatches.
//             Total wall-clock divided by REPS gives per-thread mean.
// serial:     1 thread runs K × REPS dispatches across the K maps in turn.

static void t13_concurrent_maps(double ghz)
{
    printf("\n# TEST: T13_concurrent_maps\n");
    printf("# taskCount=64 warmup=%d reps=%d\n", WARMUP, REPS);
    printf("# Compare concurrent K-thread total to K * serial single-thread mean.\n");
    printf("# Ratio < 1 implies parallel speedup; ratio ~ 1 implies serialization.\n");
    printf("mode,K,wall_us_mean,wall_us_std,per_dispatch_us,n\n");

    const int TC = 64;
    const std::vector<int> K_values = {1, 2, 4, 8};

    Ctx c = make_ctx();
    int dummy = 0;

    for (int K : K_values)
    {
        std::vector<pxl::runtime::Map*> maps(K, nullptr);
        bool ok = true;
        for (int k = 0; k < K; k++)
        {
            maps[k] = c.job->buildMap(c.func, TC);
            if (!maps[k]) { ok = false; break; }
        }
        if (!ok)
        {
            fprintf(stderr, "T13: buildMap failed for K=%d\n", K);
            for (auto* m : maps) if (m) c.job->destroyMap(m);
            continue;
        }

        // Warmup
        for (int i = 0; i < WARMUP; i++)
        {
            for (int k = 0; k < K; k++)
            {
                sink = maps[k]->execute(dummy);
                maps[k]->synchronize();
            }
        }

        // ── concurrent: K threads, each runs REPS dispatches on its map.
        {
            std::atomic<bool> go{false};
            std::vector<std::thread> ths;
            ths.reserve(K);

            for (int k = 0; k < K; k++)
            {
                ths.emplace_back([&, k]() {
                    while (!go.load(std::memory_order_acquire)) { /* spin */ }
                    int local_dummy = 0;
                    for (int i = 0; i < REPS; i++)
                    {
                        sink = maps[k]->execute(local_dummy);
                        maps[k]->synchronize();
                    }
                });
            }

            uint64_t t0 = timing::start();
            go.store(true, std::memory_order_release);
            for (auto& t : ths) t.join();
            uint64_t t1 = stop_fn();

            double total_us = timing::to_us(t1 - t0, ghz);
            // Mean per-dispatch (across K * REPS dispatches in this wall-clock).
            double per = total_us / (K * REPS);
            printf("concurrent,%d,%.4f,%.4f,%.4f,%d\n",
                   K, total_us, 0.0, per, K * REPS);
        }

        // ── serial: 1 thread, K maps round-robin, REPS rounds.
        {
            uint64_t t0 = timing::start();
            for (int i = 0; i < REPS; i++)
            {
                for (int k = 0; k < K; k++)
                {
                    sink = maps[k]->execute(dummy);
                    maps[k]->synchronize();
                }
            }
            uint64_t t1 = stop_fn();
            double total_us = timing::to_us(t1 - t0, ghz);
            double per = total_us / (K * REPS);
            printf("serial,%d,%.4f,%.4f,%.4f,%d\n",
                   K, total_us, 0.0, per, K * REPS);
        }

        for (auto* m : maps) c.job->destroyMap(m);
    }

    destroy_ctx(c);
}

// ── T14: dynamic subFree (partial release) ────────────────────────────────────
//
// T6 measures *static* Sub allocation: each measurement uses a freshly created
// Job with a pre-determined numSub.  T14 measures *dynamic* re-routing: start
// with all Subs allocated, run a baseline dispatch, then partially release
// Subs via Job::subFree(n) and re-dispatch on the same Job.
//
// Rationale for keeping both:
//   T6:  "what is the dispatch cost when the firmware was *configured* with N subs?"
//   T14: "what does the firmware do when the active Sub set *changes mid-flight*?"
//
// The two questions diverge if the firmware caches routing tables: T14 may
// reveal a one-time re-routing penalty (or stale-route latency spike) that T6
// cannot see because every T6 measurement starts from a fresh, settled state.

static void t14_partial_subfree(double ghz)
{
    printf("\n# TEST: T14_partial_subfree\n");
    printf("# taskCount=64 warmup=%d reps=%d\n", WARMUP, REPS);
    printf("# Step sequence (per row): allocate all, dispatch (baseline),\n");
    printf("#   subFree(release_count), dispatch (after_release).\n");
    printf("# Diff (after_release - baseline) is the re-routing cost.\n");
    printf("# See T6 for the static-allocation counterpart.\n");

    auto* probe = pxl::runtime::createContext(0);
    uint32_t max_sub = 1;
    if (probe)
    {
        max_sub = probe->availableSubCount();
        pxl::runtime::destroyContext(probe);
    }
    printf("# availableSubCount=%u\n", max_sub);
    printf("release_count,phase,mean_us,std_us,min_us,p50_us,p95_us,p99_us,max_us,n\n");

    const int TC = 64;

    // Sweep: free 1, max/4, max/2, max-1 (don't free all — need at least one Sub).
    std::vector<uint32_t> release_counts;
    if (max_sub >= 2)  release_counts.push_back(1);
    if (max_sub >= 4)  release_counts.push_back(max_sub / 4);
    if (max_sub >= 4)  release_counts.push_back(max_sub / 2);
    if (max_sub >= 2)  release_counts.push_back(max_sub - 1);

    for (uint32_t rc : release_counts)
    {
        // Fresh context+job for each release_count to guarantee a settled start.
        auto* ctx = pxl::runtime::createContext(0);
        if (!ctx) break;
        auto* job = ctx->createJob(max_sub);
        if (!job) { pxl::runtime::destroyContext(ctx); break; }

        auto* mod = pxl::kernel::createModule(KERNEL_FILE);
        if (!mod || job->load(mod) != pxl::PxlResult::Success)
        {
            ctx->destroyJob(job); pxl::runtime::destroyContext(ctx);
            continue;
        }
        auto* fn = mod->createFunction(KERNEL_FUNC);
        if (!fn) { ctx->destroyJob(job); pxl::runtime::destroyContext(ctx); continue; }

        auto* map = job->buildMap(fn, TC);
        if (!map) { ctx->destroyJob(job); pxl::runtime::destroyContext(ctx); continue; }

        // Baseline dispatch with all Subs allocated.
        auto baseline = dispatch_loop(map, WARMUP, REPS, ghz);
        auto base_s = Stats::from(baseline);
        char prefix[64];
        snprintf(prefix, sizeof(prefix), "%u,baseline_all_subs", rc);
        base_s.print_csv(prefix);

        // Release rc Subs.  Note: subFree(n) frees up to n; if n exceeds the
        // currently allocated count, all are freed (per Job::subFree docs).
        job->subFree(rc);

        // After-release dispatch on the same Map.  No fresh warmup so we can
        // observe any one-time re-routing spike in the first few samples.
        std::vector<double> after;
        after.reserve(REPS);
        int dummy2 = 0;
        for (int i = 0; i < REPS; i++)
        {
            uint64_t t0 = timing::start();
            sink = map->execute(dummy2);
            map->synchronize();
            uint64_t t1 = stop_fn();
            after.push_back(timing::to_us(t1 - t0, ghz));
        }
        auto after_s = Stats::from(after);
        snprintf(prefix, sizeof(prefix), "%u,after_release", rc);
        after_s.print_csv(prefix);

        printf("# rc=%u delta_mean_us=%.4f (after - baseline)\n",
               rc, after_s.mean - base_s.mean);

        job->destroyMap(map);
        ctx->destroyJob(job);
        pxl::runtime::destroyContext(ctx);
    }
}

// ── T15: persistent kernel amortization ───────────────────────────────────────
//
// Quantifies how much α is amortized away by folding work into the kernel
// instead of issuing a fresh dispatch per work unit.
//
// Two invocation patterns of the same `persistent_noop(int iters)` kernel:
//   persistent : Map::execute(iters=N)            — kernel loops N times inside
//                                                   the device.  One α paid.
//   repeated   : Map::execute(iters=1)  ×N times  — host pays α per iter.
//
// Reading the table:
//   - per_iter_persistent → kernel cost only (α amortized over N iterations).
//   - per_iter_repeated   → α + kernel cost per iteration.
//   - alpha_amortized_per_iter = (repeated - persistent) / iters
//
// REPS shrinks for larger `iters` because repeated mode runtime scales as
// iters × α ≈ iters × 3 ms.  iters=1024 with reps=10 is ~30 s.

static void t15_persistent_amortization(double ghz)
{
    printf("\n# TEST: T15_persistent_amortization\n");
    printf("# Compares Map::execute(iters=N) vs Map::execute(iters=1) * N\n");
    printf("# persistent per_iter -> kernel cost; repeated per_iter -> alpha + kernel\n");
    printf("iters,mode,total_us_mean,total_us_std,per_iter_us,reps\n");

    const int TC = 64;
    const std::vector<std::pair<int, int>> sweep = {
        {   1, 200 },
        {   4, 100 },
        {  16, 100 },
        {  64,  50 },
        { 256,  20 },
        {1024,  10 },
    };

    Ctx c = make_ctx();
    auto* fn = c.module->createFunction("persistent_noop");
    if (!fn)
    {
        fprintf(stderr, "T15: createFunction(persistent_noop) failed\n");
        destroy_ctx(c);
        return;
    }
    auto* map = c.job->buildMap(fn, TC);
    if (!map)
    {
        fprintf(stderr, "T15: buildMap failed\n");
        destroy_ctx(c);
        return;
    }

    // Warmup once with a moderate iters value.
    for (int i = 0; i < WARMUP; i++)
    {
        sink = map->execute(16);
        map->synchronize();
    }

    for (auto [iters, reps] : sweep)
    {
        // ---- Persistent: one launch, kernel loops `iters` times inside.
        std::vector<double> p_samples;
        p_samples.reserve(reps);
        for (int i = 0; i < reps; i++)
        {
            uint64_t t0 = timing::start();
            sink = map->execute(iters);
            map->synchronize();
            uint64_t t1 = stop_fn();
            p_samples.push_back(timing::to_us(t1 - t0, ghz));
        }
        auto p_s = Stats::from(p_samples);

        // ---- Repeated: `iters` launches each doing one iteration.
        std::vector<double> r_samples;
        r_samples.reserve(reps);
        const int one = 1;
        for (int i = 0; i < reps; i++)
        {
            uint64_t t0 = timing::start();
            for (int j = 0; j < iters; j++)
            {
                sink = map->execute(one);
                map->synchronize();
            }
            uint64_t t1 = stop_fn();
            r_samples.push_back(timing::to_us(t1 - t0, ghz));
        }
        auto r_s = Stats::from(r_samples);

        printf("%d,persistent,%.4f,%.4f,%.4f,%d\n",
               iters, p_s.mean, p_s.stddev, p_s.mean / iters, reps);
        printf("%d,repeated,%.4f,%.4f,%.4f,%d\n",
               iters, r_s.mean, r_s.stddev, r_s.mean / iters, reps);

        double saved_per_iter = (r_s.mean - p_s.mean) / iters;
        printf("# iters=%d alpha_amortized_per_iter_us=%.4f\n",
               iters, saved_per_iter);
    }

    c.job->destroyMap(map);
    destroy_ctx(c);
}

// ── main ──────────────────────────────────────────────────────────────────────

static void print_device_info()
{
    printf("# PLATFORM\n");
    printf("# devices: %u\n", pxl::runtime::getNumDevice());

    auto* ctx = pxl::runtime::createContext(0);
    if (!ctx) return;

    // Query architectural parameters that govern the orchestration cost model.
    struct { pxl::DeviceAttr attr; const char* name; } attrs[] = {
        { pxl::DeviceAttr::TotalSubCount,       "TotalSubCount"       },
        { pxl::DeviceAttr::NumClusterPerSub,    "NumClusterPerSub"    },
        { pxl::DeviceAttr::NumMuPerCluster,     "NumMuPerCluster"     },
        { pxl::DeviceAttr::NumThreadPerMu,      "NumThreadPerMu"      },
        { pxl::DeviceAttr::CxlMemoryCapacity,   "CxlMemoryCapacity"   },
    };
    for (auto& a : attrs)
    {
        uint64_t v = 0;
        if (ctx->getAttribute(a.attr, v) == pxl::PxlResult::Success)
            printf("# %s: %lu\n", a.name, v);
    }
    printf("# available_sub: %u\n",   ctx->availableSubCount());
    printf("# available_mem_gb: %.1f\n", ctx->availableMemSize() / (1024.0 * 1024 * 1024));

    pxl::runtime::destroyContext(ctx);
}

static void pin_to_cpu0()
{
    // Pin to CPU 0 (NUMA node 0, closest to the CXL slot) to eliminate
    // scheduler migration jitter from rdtsc measurements.
    cpu_set_t cs;
    CPU_ZERO(&cs);
    CPU_SET(0, &cs);
    if (sched_setaffinity(0, sizeof(cs), &cs) != 0)
        fprintf(stderr, "# Warning: sched_setaffinity failed — measurements may have jitter\n");
    else
        printf("# cpu_affinity: pinned to CPU 0\n");
}

static const int NUM_TESTS = 15;

static void usage(const char* prog)
{
    fprintf(stderr,
        "Usage: %s [tests...]\n"
        "  Tests: t1 t2 ... t14  (default: all)\n"
        "  t1  dispatch_sweep      — core α vs taskCount + OLS fit\n"
        "  t2  warmup_convergence  — cold-start latency series\n"
        "  t3  phase_split         — execute() vs synchronize() latency\n"
        "  t4  map_reuse           — reuse vs rebuild Map per dispatch\n"
        "  t5  locality_mode       — CompactMode vs SpreadMode\n"
        "  t6  sub_count           — Sub allocation count vs latency (static)\n"
        "  t7  batch_size          — setBatchSize() NoC fan-out reduction\n"
        "  t8  cluster_bitmap      — setClusterBitmap() direct cluster sweep\n"
        "  t9  arg_type            — Constant vs DeviceMemory translation cost\n"
        "  t10 stream_pipelining   — async streams & submission queue depth\n"
        "  t11 prepare_cost        — createModule/createJob/load/subAlloc/destroy\n"
        "  t12 ndarray_arg         — ArgType::NDArray descriptor cost vs DeviceMemory\n"
        "  t13 concurrent_maps     — multiple Maps on one Job, multi-thread dispatch\n"
        "  t14 partial_subfree     — dynamic subFree() re-routing cost (vs T6 static)\n"
        "  t15 persistent          — alpha amortization: in-kernel loop vs per-iter dispatch\n",
        prog);
}

// Parse "tN" or "tNN" (N up to NUM_TESTS).  Returns -1 on parse error.
static int parse_test_arg(const char* a)
{
    if (!a || a[0] != 't') return -1;
    const char* p = a + 1;
    if (*p == '\0') return -1;
    int n = 0;
    for (; *p; p++)
    {
        if (*p < '0' || *p > '9') return -1;
        n = n * 10 + (*p - '0');
        if (n > NUM_TESTS) return -1;
    }
    return (n >= 1 && n <= NUM_TESTS) ? n : -1;
}

int main(int argc, char* argv[])
{
    // run[1..NUM_TESTS] maps to T1..T14.
    bool run[NUM_TESTS + 1] = {};

    if (argc == 1)
    {
        for (int i = 1; i <= NUM_TESTS; i++) run[i] = true;
    }
    else
    {
        for (int i = 1; i < argc; i++)
        {
            if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0)
            {
                usage(argv[0]); return 0;
            }
            int n = parse_test_arg(argv[i]);
            if (n > 0) { run[n] = true; continue; }
            fprintf(stderr, "Unknown argument: %s\n", argv[i]);
            usage(argv[0]); return 1;
        }
    }

    printf("# Orchestration Tax Benchmark Suite\n");
    printf("# kernel: %s  func: %s\n", KERNEL_FILE, KERNEL_FUNC);

    pin_to_cpu0();

    // Select the best available stop primitive.
    // rdtscp gives tighter ordering on real silicon; fall back to lfence+rdtsc
    // on QEMU where rdtscp traps as an illegal instruction.
    if (timing::has_rdtscp())
    {
        stop_fn = timing::stop_precise;
        printf("# tsc_stop: rdtscp+lfence (precise)\n");
    }
    else
    {
        stop_fn = timing::stop;
        printf("# tsc_stop: lfence+rdtsc (portable, emulator-safe)\n");
    }

    print_device_info();

    printf("# Calibrating TSC frequency...\n");
    double ghz = timing::tsc_ghz();
    printf("# tsc_freq_ghz: %.4f\n", ghz);

    if (run[1])  t1_dispatch_sweep(ghz);
    if (run[2])  t2_warmup_convergence(ghz);
    if (run[3])  t3_phase_split(ghz);
    if (run[4])  t4_map_reuse(ghz);
    if (run[5])  t5_locality_mode(ghz);
    if (run[6])  t6_sub_count(ghz);
    if (run[7])  t7_batch_size(ghz);
    if (run[8])  t8_cluster_bitmap(ghz);
    if (run[9])  t9_arg_type(ghz);
    if (run[10]) t10_stream_pipelining(ghz);
    if (run[11]) t11_prepare_cost(ghz);
    if (run[12]) t12_ndarray_arg(ghz);
    if (run[13]) t13_concurrent_maps(ghz);
    if (run[14]) t14_partial_subfree(ghz);
    if (run[15]) t15_persistent_amortization(ghz);

    printf("\n# Done.\n");
    return 0;
}