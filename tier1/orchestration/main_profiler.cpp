// Orchestration Tax Benchmark Suite — Profiler Edition
//
// Mirrors main.cpp test-for-test (T1..T14) but additionally enables the XPTI
// process-level profiler around every timed loop and emits a per-stage device
// breakdown alongside the wall-clock CSV.
//
// What the profiler adds that rdtsc cannot:
//   T1  α decomposed into Launch→Start, Start→End, End→Terminate stages
//   T2  reveals which stage shrinks during cold→warm convergence
//   T3  device-side validation of the host's execute()/synchronize() split
//   T5  per-cluster activation set verifies CompactMode vs SpreadMode placement
//   T7  per-MU thread set confirms batchSize actually reduced active threads
//   T8  cluster bitmap → activation set: the ground-truth verification
//   T9  attributes GVA→SPA cost to a specific device-side stage
//   T10 measures pipeline overlap as (sum-of-per-map device time / wall-clock)
//
// Output format:
//   - Same CSV blocks as main.cpp ("# TEST: TN_xxx ...")
//   - Followed by "# PROFILE: TN_xxx" blocks with stage-level rows
//
// Tests T2/T4/T6/T11/T13/T14 print the profile breakdown but the underlying
// tests don't change — the breakdown is descriptive rather than load-bearing.

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
#include "profiler_helpers.hpp"
#include "stats.hpp"
#include "timing.hpp"

static const char* KERNEL_FILE = "mu_kernel/mu_kernel.mubin";
static const char* KERNEL_FUNC = "null_kernel";

// ── tunables ──────────────────────────────────────────────────────────────────
static const int WARMUP = 20;
static const int REPS   = 200;
// ─────────────────────────────────────────────────────────────────────────────

static volatile pxl::PxlResult sink;

// -- dispatch result checking (A2) -------------------------------------------
// Every timed loop assigned execute()'s result to a write-only `sink` and
// discarded synchronize()'s entirely.  A REJECTED dispatch returns in well
// under a microsecond and was recorded as a legitimate (very fast) latency
// sample, pulling means DOWN.  Found via T12: rank>=2 NDArray shapes return
// 0.3-0.5 us, far below the device's own 13.34 us floor.
//
// We COUNT rather than abort: aborting mid-dispatch can leave this device
// wedged (recovery = reboot, shared machine), and would also kill T12 before
// it reports.  Each test prints a DISPATCH_CHECK line with its tally.
// STATS=0 disables every map->getStats() call, giving a zero-instrumentation
// baseline.  These calls all sit OUTSIDE the timed loops, so in principle they
// cannot inflate wall clock -- this switch exists to verify that empirically
// rather than assume it (XPTI was assumed cheap too).
static bool g_stats_on = true;

static std::atomic<uint64_t> g_calls{0};
static std::atomic<uint64_t> g_fails{0};
static pxl::PxlResult        g_first_fail = pxl::PxlResult::Success;

static inline pxl::PxlResult chk(pxl::PxlResult r)
{
    g_calls.fetch_add(1, std::memory_order_relaxed);
    if (r != pxl::PxlResult::Success)
    {
        if (g_fails.fetch_add(1, std::memory_order_relaxed) == 0) g_first_fail = r;
    }
    return r;
}
static uint64_t (*stop_fn)() = timing::stop;
static uint64_t SESSION_ID = 0;
static double   MU_FREQ_MHZ = 1000.0;

static void check(pxl::PxlResult r, const char* msg)
{
    if (r != pxl::PxlResult::Success)
    {
        fprintf(stderr, "FATAL: %s\n", msg);
        std::exit(1);
    }
}

struct Ctx
{
    pxl::runtime::Context*  ctx    = nullptr;
    pxl::runtime::Job*      job    = nullptr;
    pxl::kernel::Module*    module = nullptr;
    pxl::kernel::Function*  func   = nullptr;
};

static Ctx make_ctx(uint32_t numSub = 0)
{
    (void)numSub;
    Ctx c;
    c.ctx = pxl::runtime::createContext(0);
    if (!c.ctx) { fprintf(stderr, "createContext failed\n"); std::exit(1); }
    c.job = c.ctx->createJob();
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

// Drain any events accumulated up to this point so subsequent reads start fresh.
static void drain_cursor(prof::Cursor& cur)
{
    (void)prof::read_host_events_delta(SESSION_ID, cur);
    (void)prof::read_device_events_delta(SESSION_ID, cur);
    (void)prof::read_l1_delta(SESSION_ID, cur);
    (void)prof::read_mu_delta(SESSION_ID, cur);
}

// Profiled dispatch loop: runs the same warmup+timed sequence as
// dispatch_loop() in main.cpp, but additionally captures the device events
// generated during the timed window and aggregates them into stage durations.
struct ProfResult
{
    std::vector<double>            wall_us;
    prof::DeviceStages             stages;
    std::vector<xpti::DeviceEvent> device_events;
    std::vector<xpti::HostEvent>   host_events;
    std::vector<xpti::L1RawData>   l1;
    std::vector<xpti::MURawData>   mu;
};

// FRESH=1: rebuild the dispatch handle before EVERY timed dispatch.
// A handle ages with each dispatch through it (per-dispatch state accumulates
// in DeviceInit), so the conventional warm-up-then-measure loop reports an
// aged handle's cost, not the fixed cost. Warm-up still runs -- it warms the
// process/context -- but each timed sample then gets a brand-new handle.
// The rebuild is OUTSIDE the timed region.
//
// `renew` must build the replacement, re-apply any per-handle configuration
// (setBatchSize, setClusterBitmap, ...), and only then destroy the old one.
// Tests that do not pass `renew` are unaffected by FRESH=1.
static bool g_fresh = false;
using RenewFn = std::function<void(pxl::runtime::Map*&)>;

// Build first, destroy second: if buildMap fails we keep a valid handle and
// count a failure, rather than leaving the caller with a dangling pointer.
static void renew_map(pxl::runtime::Job* job, pxl::kernel::Function* fn,
                      uint32_t tc, pxl::runtime::Map*& m,
                      const std::function<void(pxl::runtime::Map*)>& cfg = nullptr)
{
    pxl::runtime::Map* next = job->buildMap(fn, tc);
    if (!next) { g_fails.fetch_add(1); return; }
    if (cfg) cfg(next);
    job->destroyMap(m);
    m = next;
}

// `map` is taken by reference: under FRESH=1 the caller's pointer is updated to
// the live handle, so its later getStats()/destroyMap() stay valid.
static ProfResult profiled_dispatch_loop(pxl::runtime::Map*& map,
                                         int warmup, int reps, double ghz,
                                         const RenewFn& renew = nullptr)
{
    int dummy = 0;
    for (int i = 0; i < warmup; i++)
    {
        sink = chk(map->execute(dummy));
        chk(map->synchronize());
    }

    // Establish baseline AFTER warmup so warmup events do not pollute the profile.
    prof::Cursor cur;
    drain_cursor(cur);

    ProfResult r;
    r.wall_us.reserve(reps);
    for (int i = 0; i < reps; i++)
    {
        if (g_fresh && renew) renew(map);        // outside timing
        uint64_t t0 = timing::start();
        sink = chk(map->execute(dummy));
        chk(map->synchronize());
        uint64_t t1 = stop_fn();
        r.wall_us.push_back(timing::to_us(t1 - t0, ghz));
    }

    r.host_events   = prof::read_host_events_delta(SESSION_ID, cur);
    r.device_events = prof::read_device_events_delta(SESSION_ID, cur);
    r.l1            = prof::read_l1_delta(SESSION_ID, cur);
    r.mu            = prof::read_mu_delta(SESSION_ID, cur);
    r.stages        = prof::aggregate_device_events(r.device_events, MU_FREQ_MHZ);
    return r;
}

// ── P1: dispatch sweep with stage breakdown ───────────────────────────────────

static void p1_dispatch_sweep(double ghz)
{
    printf("\n# TEST: T1_dispatch_sweep\n");
    printf("# warmup=%d reps=%d (profiled)\n", WARMUP, REPS);
    printf("taskCount,mean_us,std_us,min_us,p50_us,p95_us,p99_us,max_us,n\n");

    const std::vector<int> task_counts = {
        1, 16, 64, 128, 256, 512, 1024, 2048, 4096, 8192, 16384, 32768, 65536, 131072
    };

    std::vector<double> xs, ys;
    Ctx c = make_ctx();

    // Stage breakdown + host events captured per taskCount.
    std::vector<std::tuple<int, prof::DeviceStages>> per_tc_stages;
    std::vector<std::pair<int, std::vector<xpti::HostEvent>>> per_tc_host;
    std::vector<std::pair<int, std::string>> gs_log;

    for (int tc : task_counts)
    {
        auto map = c.job->buildMap(c.func, tc);
        if (!map) continue;

        auto r = profiled_dispatch_loop(map, WARMUP, REPS, ghz,
            [&](pxl::runtime::Map*& m) { renew_map(c.job, c.func, tc, m); });
        auto s = Stats::from(r.wall_us);
        char prefix[32]; snprintf(prefix, sizeof(prefix), "%d", tc);
        s.print_csv(prefix);

        xs.push_back(static_cast<double>(tc));
        ys.push_back(s.mean);

        per_tc_stages.emplace_back(tc, std::move(r.stages));
        per_tc_host.emplace_back(tc, std::move(r.host_events));
        if (g_stats_on) gs_log.emplace_back(tc, map->getStats());
        c.job->destroyMap(map);
    }
    destroy_ctx(c);

    LinFit fit;
    if (LinFit::fit(xs, ys, fit))
    {
        printf("# OLS fit: alpha_us=%.4f  slope_us_per_task=%.6f  R2=%.4f\n",
               fit.alpha, fit.slope, fit.r2);
    }

    printf("# PROFILE: T1_dispatch_sweep — PXL getStats() phase decomposition\n");
    printf("# Source: map->getStats() (PXL API, no XPTI session, no overhead).\n");
    printf("taskCount,%s\n", prof::getstats_csv_header());
    for (auto& [tc, js] : gs_log)
    {
        char p[32]; snprintf(p, sizeof(p), "%d", tc);
        prof::print_getstats_csv(p, js);
    }

    printf("# PROFILE: T1_dispatch_sweep — device stage breakdown (us)\n");
    printf("taskCount,stage,mean_us,std_us,min_us,p50_us,p95_us,p99_us,max_us,n\n");
    for (auto& [tc, st] : per_tc_stages)
    {
        char p[32]; snprintf(p, sizeof(p), "%d", tc);
        prof::DeviceStages copy = st;  // print_stage_csv sorts in place.
        prof::print_device_stages(p, copy);
    }

    printf("# PROFILE: T1_dispatch_sweep — host API breakdown (us)\n");
    printf("taskCount,%s\n", prof::host_csv_header());
    for (auto& [tc, evs] : per_tc_host)
    {
        char p[32]; snprintf(p, sizeof(p), "%d", tc);
        prof::print_host_breakdown(p, evs);
    }
}

// ── P2: warmup convergence with per-rep device stages ─────────────────────────

// ─────────────────────────────────────────────────────────────────────────────
// T2 design note — "why taskCount=64?" and the T2_TC escape hatch
// ─────────────────────────────────────────────────────────────────────────────
// T2 fixes the task count and runs the SAME map N_COLD times with no prior
// warmup, to watch cold→warm convergence. Task count is a held-fixed control,
// not a swept variable. We use 64 for two reasons, both measured (2026-07-09):
//
//   (1) Representativeness. 64 sits in T1's flat fixed-cost floor: the variable
//       term is 64 * ~0.4us/task ≈ 26us, negligible against the ~4ms alpha. So
//       the warmup curve reflects pure orchestration bring-up (JIT/alloc/device
//       init), not per-task dispatch. It is also the smallest count that still
//       populates the XPTI device-stage profiler (tc=1/16 give n=0 rows). We
//       verified the warmup SHAPE is task-count-independent by running counts
//       {64,128,256,1024,4096} each in its own fresh process: cold→warm collapse
//       lands on invocation 1 in every case, and warm medians rise smoothly with
//       tc (1769→1864→2603→4129→5077 us), tracking the T1 slope. So 64 is a
//       representative point, not a cherry-pick.
//
//   (2) Methodological necessity — the "ugly" finding. T2 MUST be a single-count,
//       single-process measurement. Running multiple build/execute/destroy cycles
//       back-to-back in ONE process triggers a persistent runtime stall: the
//       FIRST sub-experiment is clean, but every subsequent one pins at ~1.3e6 us
//       per invocation (~730x blowup) regardless of its task count, and the
//       device-event pipeline goes silent (n=0). Same counts run perfectly in a
//       fresh process, so this is process-state CARRYOVER, not workload:
//       something in destroyMap()/destroy_ctx() is not released, and after the
//       first teardown the device is left degraded. Measured:
//         in-process sweep {64,1024,4096}:  pos#0 tc=64  warm_med=1819 us  CLEAN
//                                           pos#1 tc=1024 warm_med=1.34e6 STALLED
//                                           pos#2 tc=4096 warm_med=1.43e6 STALLED
//         isolated (T2_TC, fresh process):  tc=1024 warm_med=4129 us  CLEAN
//                                           tc=4096 warm_med=5077 us  CLEAN
//       This teardown/reuse cliff is itself a device/runtime result worth
//       reporting — not just an artifact to hide.
//
// T2_TC env override: if set (e.g. T2_TC=1024), run exactly ONE count in this
// process and return — the clean path for reproducing the isolated sweep by
// looping externally:  for tc in 64 128 256 1024 4096; do T2_TC=$tc ./main_profiler t2; done
// Unset → normal T2 (single fixed count 64, matching main.cpp).
// ─────────────────────────────────────────────────────────────────────────────
static void p2_warmup_convergence_one(double ghz, int TC, int N_COLD)
{
    printf("\n# T2 taskCount=%d no_prior_warmup (profiled)\n", TC);
    printf("taskCount,invocation,latency_us\n");

    Ctx c = make_ctx();
    auto map = c.job->buildMap(c.func, TC);
    if (!map) { fprintf(stderr, "P2: buildMap failed (tc=%d)\n", TC); destroy_ctx(c); return; }

    prof::Cursor cur;
    drain_cursor(cur);

    // B1 follow-up: T2_REBUILD=K rebuilds the Map every K invocations.
    // If the per-dispatch leak lives in the Map object, destroyMap() should
    // reset the drift; if it is process-wide, rebuilding changes nothing.
    int rebuild_every = 0;
    if (const char* e = getenv("T2_REBUILD")) { int v = atoi(e); if (v > 0) rebuild_every = v; }

    int dummy = 0;
    for (int i = 0; i < N_COLD; i++)
    {
        if (rebuild_every && i && i % rebuild_every == 0)
        {
            c.job->destroyMap(map);
            map = c.job->buildMap(c.func, TC);
            if (!map) { fprintf(stderr, "P2: rebuild failed at i=%d\n", i); break; }
        }
        uint64_t t0 = timing::start();
        sink = chk(map->execute(dummy));
        chk(map->synchronize());
        uint64_t t1 = stop_fn();
        printf("%d,%d,%.4f\n", TC, i, timing::to_us(t1 - t0, ghz));
    }

    printf("# PROFILE: T2 tc=%d — PXL getStats() phase decomposition\n", TC);
    printf("taskCount,%s\n", prof::getstats_csv_header());
    if (g_stats_on) { char gp[32]; snprintf(gp, sizeof(gp), "%d", TC);
      prof::print_getstats_csv(gp, map->getStats()); }

    auto dev_evs   = prof::read_device_events_delta(SESSION_ID, cur);
    auto host_evs  = prof::read_host_events_delta(SESSION_ID, cur);
    auto stages    = prof::aggregate_device_events(dev_evs, MU_FREQ_MHZ);

    printf("# PROFILE: T2_warmup_convergence tc=%d — aggregate device stages over all %d cold invocations\n", TC, N_COLD);
    printf("stage,mean_us,std_us,min_us,p50_us,p95_us,p99_us,max_us,n\n");
    char p[32]; snprintf(p, sizeof(p), "%d", TC);
    prof::print_device_stages(p, stages);

    printf("# PROFILE: T2_warmup_convergence tc=%d — host API breakdown\n", TC);
    printf("taskCount,%s\n", prof::host_csv_header());
    prof::print_host_breakdown(p, host_evs);

    c.job->destroyMap(map);
    destroy_ctx(c);
}

static void p2_warmup_convergence(double ghz)
{
    printf("\n# TEST: T2_warmup_convergence\n");

    // B1: T2_N overrides the invocation count so we can separate
    // dispatch-count from process-lifetime as the driver of the drift.
    //   T2_N=6000 in one process        vs
    //   T2_N=3000 twice, back-to-back   -> does the drift RESET at the
    // process boundary (process state) or CONTINUE (device/runtime state)?
    int N_COLD = 100;
    if (const char* e = getenv("T2_N")) { int v = atoi(e); if (v > 0) N_COLD = v; }

    // Optional reproduction hook: T2_TC=<n> runs a single arbitrary count in this
    // fresh process instead of the default 64. See the design note above for why
    // T2 is single-count/single-process. Unset → default behavior (tc=64).
    int tc = 64;
    if (const char* env = getenv("T2_TC"))
    {
        int v = atoi(env);
        if (v > 0) tc = v;
    }

    p2_warmup_convergence_one(ghz, tc, N_COLD);
}

// ── P3: phase split with device-side cross-validation ────────────────────────

static void p3_phase_split(double ghz)
{
    printf("\n# TEST: T3_phase_split\n");
    printf("# warmup=%d reps=%d (profiled)\n", WARMUP, REPS);
    printf("taskCount,execute_us_mean,execute_us_std,sync_us_mean,sync_us_std,total_us_mean,total_us_std\n");

    const std::vector<int> task_counts = {1, 16, 64, 128, 256, 512, 1024, 2048, 4096, 8192, 16384, 32768, 65536, 131072};

    Ctx c = make_ctx();
    std::vector<std::tuple<int, prof::DeviceStages>> per_tc_stages;
    std::vector<std::pair<int, std::vector<xpti::HostEvent>>> per_tc_host;

    for (int tc : task_counts)
    {
        auto map = c.job->buildMap(c.func, tc);
        if (!map) continue;

        int dummy = 0;
        for (int i = 0; i < WARMUP; i++) { sink = chk(map->execute(dummy)); chk(map->synchronize()); }

        prof::Cursor cur;
        drain_cursor(cur);

        std::vector<double> exec_s, sync_s, total_s;
        exec_s.reserve(REPS); sync_s.reserve(REPS); total_s.reserve(REPS);
        for (int i = 0; i < REPS; i++)
        {
            if (g_fresh) renew_map(c.job, c.func, tc, map);   // outside timing
            uint64_t t0 = timing::start();
            sink = chk(map->execute(dummy));
            uint64_t t1 = stop_fn();
            chk(map->synchronize());
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

        auto dev_evs  = prof::read_device_events_delta(SESSION_ID, cur);
        auto host_evs = prof::read_host_events_delta(SESSION_ID, cur);
        auto stages   = prof::aggregate_device_events(dev_evs, MU_FREQ_MHZ);
        per_tc_stages.emplace_back(tc, std::move(stages));
        per_tc_host.emplace_back(tc, std::move(host_evs));
        if (g_stats_on) { printf("# getStats_csv tc=%d\n", tc);
          char gp[32]; snprintf(gp, sizeof(gp), "%d", tc);
          prof::print_getstats_csv(gp, map->getStats()); }

        c.job->destroyMap(map);
    }
    destroy_ctx(c);

    printf("# PROFILE: T3_phase_split — device validates host phase split\n");
    printf("# host(execute) covers Launch path; host(sync) covers post-Launch wait.\n");
    printf("# device(launch_to_start) ≈ DeviceInit; (start_to_end) ≈ kernel time;\n");
    printf("# (end_to_terminate) ≈ DeviceFinalize.\n");
    printf("taskCount,stage,mean_us,std_us,min_us,p50_us,p95_us,p99_us,max_us,n\n");
    for (auto& [tc, st] : per_tc_stages)
    {
        char p[32]; snprintf(p, sizeof(p), "%d", tc);
        prof::DeviceStages copy = st;
        prof::print_device_stages(p, copy);
    }

    printf("# PROFILE: T3_phase_split — host API breakdown\n");
    printf("# Compare e.g. Map::execute (host) vs device(launch_to_start) for same tc.\n");
    printf("taskCount,%s\n", prof::host_csv_header());
    for (auto& [tc, evs] : per_tc_host)
    {
        char p[32]; snprintf(p, sizeof(p), "%d", tc);
        prof::print_host_breakdown(p, evs);
    }
}

// ── P4: map reuse vs rebuild (descriptive profiling) ──────────────────────────

static void p4_map_reuse(double ghz)
{
    printf("\n# TEST: T4_map_reuse\n");
    printf("# warmup=%d reps=%d (profiled)\n", WARMUP, REPS);
    printf("mode,taskCount,mean_us,std_us,min_us,p50_us,p95_us,p99_us,max_us,n\n");

    const std::vector<int> task_counts = {1, 64, 256};
    Ctx c = make_ctx();
    int dummy = 0;

    std::vector<std::tuple<std::string, int, prof::DeviceStages>> stages_log;
    std::vector<std::tuple<std::string, int, std::vector<xpti::HostEvent>>> host_log;

    for (int tc : task_counts)
    {
        // reuse mode
        {
            auto map = c.job->buildMap(c.func, tc);
            if (!map) continue;
            auto r = profiled_dispatch_loop(map, WARMUP, REPS, ghz);
            auto s = Stats::from(r.wall_us);
            char prefix[64]; snprintf(prefix, sizeof(prefix), "reuse,%d", tc);
            s.print_csv(prefix);
            stages_log.emplace_back("reuse", tc, std::move(r.stages));
            host_log.emplace_back("reuse", tc, std::move(r.host_events));
            c.job->destroyMap(map);
        }
        // rebuild mode
        {
            for (int i = 0; i < WARMUP; i++)
            {
                auto m = c.job->buildMap(c.func, tc);
                sink = chk(m->execute(dummy)); chk(m->synchronize());
                c.job->destroyMap(m);
            }
            prof::Cursor cur;
            drain_cursor(cur);

            std::vector<double> samples; samples.reserve(REPS);
            for (int i = 0; i < REPS; i++)
            {
                uint64_t t0 = timing::start();
                auto m = c.job->buildMap(c.func, tc);
                sink = chk(m->execute(dummy)); chk(m->synchronize());
                c.job->destroyMap(m);
                uint64_t t1 = stop_fn();
                samples.push_back(timing::to_us(t1 - t0, ghz));
            }
            auto s = Stats::from(samples);
            char prefix[64]; snprintf(prefix, sizeof(prefix), "rebuild,%d", tc);
            s.print_csv(prefix);

            auto dev_evs  = prof::read_device_events_delta(SESSION_ID, cur);
            auto host_evs = prof::read_host_events_delta(SESSION_ID, cur);
            auto st       = prof::aggregate_device_events(dev_evs, MU_FREQ_MHZ);
            stages_log.emplace_back("rebuild", tc, std::move(st));
            host_log.emplace_back("rebuild", tc, std::move(host_evs));
        }
    }
    destroy_ctx(c);

    printf("# PROFILE: T4_map_reuse — DeviceInit should shrink for reuse if firmware caches state\n");
    printf("mode,taskCount,stage,mean_us,std_us,min_us,p50_us,p95_us,p99_us,max_us,n\n");
    for (auto& [m, tc, st] : stages_log)
    {
        char p[64]; snprintf(p, sizeof(p), "%s,%d", m.c_str(), tc);
        prof::DeviceStages copy = st;
        prof::print_device_stages(p, copy);
    }

    printf("# PROFILE: T4_map_reuse — host API breakdown\n");
    printf("mode,taskCount,%s\n", prof::host_csv_header());
    for (auto& [m, tc, evs] : host_log)
    {
        char p[64]; snprintf(p, sizeof(p), "%s,%d", m.c_str(), tc);
        prof::print_host_breakdown(p, evs);
    }
}

// ── P5: locality mode + per-cluster activation set ───────────────────────────

static void p5_locality_mode(double ghz)
{
    printf("\n# TEST: T5_locality_mode\n");
    printf("# warmup=%d reps=%d (profiled)\n", WARMUP, REPS);
    printf("mode,taskCount,mean_us,std_us,min_us,p50_us,p95_us,p99_us,max_us,n\n");

    const std::vector<int> task_counts = {1, 16, 64, 128, 256, 512, 1024};
    const struct { const char* name; pxl::LocalityMode mode; } modes[] = {
        { "compact", pxl::LocalityMode::CompactMode },
        { "spread",  pxl::LocalityMode::SpreadMode  },
    };

    Ctx c = make_ctx();
    struct ActLog { std::string mode; int tc; std::vector<std::pair<uint32_t, uint32_t>> clusters; };
    std::vector<ActLog> activations;
    std::vector<std::tuple<std::string, int, std::vector<xpti::HostEvent>>> host_log;

    for (auto& lm : modes)
    {
        for (int tc : task_counts)
        {
            auto map = c.job->buildMap(c.func, tc);
            if (!map) continue;
            map->setLocalityMode(lm.mode);

            auto r = profiled_dispatch_loop(map, WARMUP, REPS, ghz);
            auto s = Stats::from(r.wall_us);
            char prefix[64]; snprintf(prefix, sizeof(prefix), "%s,%d", lm.name, tc);
            s.print_csv(prefix);

            ActLog log;
            log.mode = lm.name;
            log.tc = tc;
            log.clusters = prof::active_clusters_from_l1(r.l1);
            activations.push_back(std::move(log));
            host_log.emplace_back(lm.name, tc, std::move(r.host_events));

            c.job->destroyMap(map);
        }
    }
    destroy_ctx(c);

    // C5: the L1 / MU raw-counter probe streams return ZERO rows on this
    // platform (verified: deviceSamplingRate=1 over 2000 launches gives
    // L1=0, MU=0 while DeviceEvents gives 18,430).  The activation set is
    // therefore UNMEASURABLE here, and printing it emitted a column of 0s
    // that looked like a measurement.  Suppressed; do NOT reinstate without
    // first confirming the probe streams actually populate.
    {
        size_t seen = 0;
        for (auto& a : activations) seen += a.clusters.size();
        printf("# PROFILE: T5_locality_mode — activation set UNAVAILABLE "
               "(L1 probe stream empty, %zu rows); latency curve above is the result\n", seen);
    }

    printf("# PROFILE: T5_locality_mode — host API breakdown\n");
    printf("mode,taskCount,%s\n", prof::host_csv_header());
    for (auto& [m, tc, evs] : host_log)
    {
        char p[64]; snprintf(p, sizeof(p), "%s,%d", m.c_str(), tc);
        prof::print_host_breakdown(p, evs);
    }
}

// ── P6: sub count (descriptive profiling) ────────────────────────────────────

static void p6_sub_count(double ghz)
{
    printf("\n# TEST: T6_sub_count\n");
    printf("# taskCount=64 warmup=%d reps=%d (profiled)\n", WARMUP, REPS);

    auto* probe = pxl::runtime::createContext(0);
    uint32_t max_sub = 1;
    if (probe) { max_sub = probe->availableSubCount(); pxl::runtime::destroyContext(probe); }
    printf("# availableSubCount=%u\n", max_sub);
    printf("numSub,mean_us,std_us,min_us,p50_us,p95_us,p99_us,max_us,n\n");

    const int TC = 64;
    std::vector<uint32_t> sub_counts;
    for (uint32_t ns = 1; ns < max_sub; ns *= 2) sub_counts.push_back(ns);
    sub_counts.push_back(max_sub);

    std::vector<std::tuple<uint32_t, prof::DeviceStages>> stages_log;
    std::vector<std::pair<uint32_t, std::vector<xpti::HostEvent>>> host_log;

    for (uint32_t ns : sub_counts)
    {
        auto* ctx = pxl::runtime::createContext(0); if (!ctx) break;
        auto* job = ctx->createJob(ns);
        if (!job) { pxl::runtime::destroyContext(ctx); break; }
        auto* mod = pxl::kernel::createModule(KERNEL_FILE);
        if (!mod || job->load(mod) != pxl::PxlResult::Success)
        { ctx->destroyJob(job); pxl::runtime::destroyContext(ctx); break; }
        auto* fn = mod->createFunction(KERNEL_FUNC);
        if (!fn) { ctx->destroyJob(job); pxl::runtime::destroyContext(ctx); break; }
        auto* map = job->buildMap(fn, TC);
        if (!map) { ctx->destroyJob(job); pxl::runtime::destroyContext(ctx); break; }

        auto r = profiled_dispatch_loop(map, WARMUP, REPS, ghz,
            [&](pxl::runtime::Map*& m) { renew_map(job, fn, TC, m); });
        auto s = Stats::from(r.wall_us);
        char prefix[32]; snprintf(prefix, sizeof(prefix), "%u", ns);
        s.print_csv(prefix);

        stages_log.emplace_back(ns, std::move(r.stages));
        host_log.emplace_back(ns, std::move(r.host_events));
        if (g_stats_on) { printf("# getStats_csv numSub=%u\n", ns);
          char gp[32]; snprintf(gp, sizeof(gp), "%u", ns);
          prof::print_getstats_csv(gp, map->getStats()); }

        job->destroyMap(map); ctx->destroyJob(job); pxl::runtime::destroyContext(ctx);
    }

    printf("# PROFILE: T6_sub_count — DeviceInit should grow with numSub if NoC fan-out is the cost\n");
    printf("numSub,stage,mean_us,std_us,min_us,p50_us,p95_us,p99_us,max_us,n\n");
    for (auto& [ns, st] : stages_log)
    {
        char p[32]; snprintf(p, sizeof(p), "%u", ns);
        prof::DeviceStages copy = st;
        prof::print_device_stages(p, copy);
    }

    printf("# PROFILE: T6_sub_count — host API breakdown\n");
    printf("numSub,%s\n", prof::host_csv_header());
    for (auto& [ns, evs] : host_log)
    {
        char p[32]; snprintf(p, sizeof(p), "%u", ns);
        prof::print_host_breakdown(p, evs);
    }
}

// ── P7: batch size + active MU thread count verification ─────────────────────

static void p7_batch_size(double ghz)
{
    printf("\n# TEST: T7_batch_size\n");
    printf("# taskCount=64 warmup=%d reps=%d (profiled)\n", WARMUP, REPS);
    printf("batchSize,active_threads,mean_us,std_us,min_us,p50_us,p95_us,p99_us,max_us,n\n");

    const int TC = 64;
    const std::vector<int> batch_sizes = {1, 2, 4, 8, 16, 32, 64};

    Ctx c = make_ctx();
    struct MuLog { int bs; int expected; size_t observed; };
    std::vector<MuLog> mu_logs;
    std::vector<std::pair<int, std::vector<xpti::HostEvent>>> host_log;

    for (int bs : batch_sizes)
    {
        if (bs > TC) break;
        auto map = c.job->buildMap(c.func, TC);
        if (!map) continue;
        map->setBatchSize(static_cast<uint32_t>(bs));

        auto r = profiled_dispatch_loop(map, WARMUP, REPS, ghz,
            [&](pxl::runtime::Map*& m) {
                renew_map(c.job, c.func, TC, m, [&](pxl::runtime::Map* x) {
                    x->setBatchSize(static_cast<uint32_t>(bs)); });
            });
        auto s = Stats::from(r.wall_us);
        char prefix[64]; snprintf(prefix, sizeof(prefix), "%d,%d", bs, TC / bs);
        s.print_csv(prefix);

        auto active = prof::active_mus_from_mu_raw(r.mu);
        mu_logs.push_back({ bs, TC / bs, active.size() });
        if (g_stats_on) { printf("# getStats_csv batchSize=%d\n", bs);
          char gp[32]; snprintf(gp, sizeof(gp), "%d", bs);
          prof::print_getstats_csv(gp, map->getStats()); }
        host_log.emplace_back(bs, std::move(r.host_events));

        c.job->destroyMap(map);
    }
    destroy_ctx(c);

    // C5: the L1 / MU raw-counter probe streams return ZERO rows on this
    // platform (verified: deviceSamplingRate=1 over 2000 launches gives
    // L1=0, MU=0 while DeviceEvents gives 18,430).  The activation set is
    // therefore UNMEASURABLE here, and printing it emitted a column of 0s
    // that looked like a measurement.  Suppressed; do NOT reinstate without
    // first confirming the probe streams actually populate.
    {
        size_t seen = 0;
        for (auto& l : mu_logs) seen += l.observed;
        printf("# PROFILE: T7_batch_size — activation set UNAVAILABLE "
               "(MU probe stream empty, %zu rows); latency curve above is the result\n", seen);
    }

    printf("# PROFILE: T7_batch_size — host API breakdown\n");
    printf("batchSize,%s\n", prof::host_csv_header());
    for (auto& [bs, evs] : host_log)
    {
        char p[32]; snprintf(p, sizeof(p), "%d", bs);
        prof::print_host_breakdown(p, evs);
    }
}

// ── P8: cluster bitmap + ground-truth activation verification ────────────────

static void p8_cluster_bitmap(double ghz)
{
    printf("\n# TEST: T8_cluster_bitmap\n");
    printf("# taskCount=64 warmup=%d reps=%d (profiled)\n", WARMUP, REPS);

    const int TC = 64;
    auto* probe_ctx = pxl::runtime::createContext(0);
    uint64_t num_clusters = 32;
    if (probe_ctx) { probe_ctx->getAttribute(pxl::DeviceAttr::NumClusterPerSub, num_clusters);
                     pxl::runtime::destroyContext(probe_ctx); }
    uint32_t max_valid_mask = (num_clusters >= 32)
                                  ? 0xffffffffu
                                  : (1u << static_cast<uint32_t>(num_clusters)) - 1u;

    printf("# NumClusterPerSub=%lu  max_valid_mask=0x%08x\n", num_clusters, max_valid_mask);
    printf("mask_hex,popcount,mean_us,std_us,min_us,p50_us,p95_us,p99_us,max_us,n\n");

    std::vector<std::pair<uint32_t, int>> masks;
    for (uint64_t pop = 1; pop <= num_clusters; pop *= 2)
    {
        uint32_t mask = (pop >= 32) ? 0xffffffffu : (1u << static_cast<uint32_t>(pop)) - 1u;
        if (mask > max_valid_mask) break;
        masks.emplace_back(mask, static_cast<int>(pop));
    }
    masks.emplace_back(0u, -1);

    Ctx c = make_ctx();
    struct Act { uint32_t mask; int pop; std::vector<std::pair<uint32_t, uint32_t>> clusters; };
    std::vector<Act> acts;
    std::vector<std::tuple<uint32_t, int, std::vector<xpti::HostEvent>>> host_log;

    for (auto& [mask, pop] : masks)
    {
        auto map = c.job->buildMap(c.func, TC);
        if (!map) continue;
        map->setClusterBitmap(mask);

        const uint32_t mask_v = mask;   // structured bindings aren't capturable in C++17
        auto r = profiled_dispatch_loop(map, WARMUP, REPS, ghz,
            [&](pxl::runtime::Map*& m) {
                renew_map(c.job, c.func, TC, m, [mask_v](pxl::runtime::Map* x) {
                    x->setClusterBitmap(mask_v); });
            });
        auto s = Stats::from(r.wall_us);
        char prefix[64];
        if (pop < 0) snprintf(prefix, sizeof(prefix), "0x%08x,all", mask);
        else         snprintf(prefix, sizeof(prefix), "0x%08x,%d", mask, pop);
        s.print_csv(prefix);

        Act a; a.mask = mask; a.pop = pop;
        a.clusters = prof::active_clusters_from_l1(r.l1);
        acts.push_back(std::move(a));
        host_log.emplace_back(mask, pop, std::move(r.host_events));

        c.job->destroyMap(map);
    }
    destroy_ctx(c);

    // C5: the L1 / MU raw-counter probe streams return ZERO rows on this
    // platform (verified: deviceSamplingRate=1 over 2000 launches gives
    // L1=0, MU=0 while DeviceEvents gives 18,430).  The activation set is
    // therefore UNMEASURABLE here, and printing it emitted a column of 0s
    // that looked like a measurement.  Suppressed; do NOT reinstate without
    // first confirming the probe streams actually populate.
    {
        size_t seen = 0;
        for (auto& a : acts) seen += a.clusters.size();
        printf("# PROFILE: T8_cluster_bitmap — activation ground truth UNAVAILABLE "
               "(L1 probe stream empty, %zu rows); latency curve above is the result\n", seen);
    }

    printf("# PROFILE: T8_cluster_bitmap — host API breakdown\n");
    printf("mask_hex,popcount,%s\n", prof::host_csv_header());
    for (auto& [mask, pop, evs] : host_log)
    {
        char p[64];
        if (pop < 0) snprintf(p, sizeof(p), "0x%08x,all", mask);
        else         snprintf(p, sizeof(p), "0x%08x,%d", mask, pop);
        prof::print_host_breakdown(p, evs);
    }
}

// ── P9: arg type + per-stage attribution of GVA→SPA cost ─────────────────────

static void p9_arg_type(double ghz)
{
    printf("\n# TEST: T9_arg_type\n");
    printf("# taskCount=64 warmup=%d reps=%d (profiled)\n", WARMUP, REPS);
    printf("kernel,arg_desc,mean_us,std_us,min_us,p50_us,p95_us,p99_us,max_us,n\n");

    const int TC = 64;
    Ctx c = make_ctx();
    int* dev_buf = reinterpret_cast<int*>(c.ctx->memAlloc(64 * sizeof(int)));
    if (!dev_buf) { fprintf(stderr, "P9: memAlloc failed\n"); destroy_ctx(c); return; }

    const struct { const char* fname; const char* desc; } kernels[] = {
        { "null_kernel",  "1_scalar"  },
        { "null_3scalar", "3_scalars" },
        { "null_1ptr",    "1_ptr"     },
        { "null_3ptr",    "3_ptrs"    },
        { "null_6ptr",    "6_ptrs"    },
    };

    int dummy = 0;
    std::vector<std::tuple<std::string, prof::DeviceStages>> stages_log;
    std::vector<std::pair<std::string, std::vector<xpti::HostEvent>>> host_log;

    for (auto& k : kernels)
    {
        auto* fn = c.module->createFunction(k.fname);
        if (!fn) continue;
        auto* map = c.job->buildMap(fn, TC);
        if (!map) continue;

        // Warmup (kernel-specific argument shape).
        auto warmup_call = [&]() {
            if      (strcmp(k.fname, "null_kernel")  == 0) sink = chk(map->execute(dummy));
            else if (strcmp(k.fname, "null_3scalar") == 0) sink = chk(map->execute(dummy, dummy, dummy));
            else if (strcmp(k.fname, "null_1ptr")    == 0) sink = chk(map->execute(dev_buf));
            else if (strcmp(k.fname, "null_3ptr")    == 0) sink = chk(map->execute(dev_buf, dev_buf, dev_buf));
            else if (strcmp(k.fname, "null_6ptr")    == 0) sink = chk(map->execute(dev_buf, dev_buf, dev_buf,
                                                                                dev_buf, dev_buf, dev_buf));
            chk(map->synchronize());
        };
        for (int i = 0; i < WARMUP; i++) warmup_call();

        prof::Cursor cur; drain_cursor(cur);

        std::vector<double> samples; samples.reserve(REPS);
        for (int i = 0; i < REPS; i++)
        {
            uint64_t t0 = timing::start();
            warmup_call();  // same call shape; no extra warmup loop inside
            uint64_t t1 = stop_fn();
            samples.push_back(timing::to_us(t1 - t0, ghz));
        }

        auto s = Stats::from(samples);
        char prefix[64]; snprintf(prefix, sizeof(prefix), "%s,%s", k.fname, k.desc);
        s.print_csv(prefix);

        auto dev_evs  = prof::read_device_events_delta(SESSION_ID, cur);
        auto host_evs = prof::read_host_events_delta(SESSION_ID, cur);
        auto st       = prof::aggregate_device_events(dev_evs, MU_FREQ_MHZ);
        stages_log.emplace_back(k.desc, std::move(st));
        host_log.emplace_back(k.desc, std::move(host_evs));

        c.job->destroyMap(map);
    }

    c.ctx->memFree(dev_buf);
    destroy_ctx(c);

    printf("# PROFILE: T9_arg_type — pointer count → which device stage absorbs the cost?\n");
    printf("arg_desc,stage,mean_us,std_us,min_us,p50_us,p95_us,p99_us,max_us,n\n");
    for (auto& [d, st] : stages_log)
    {
        prof::DeviceStages copy = st;
        prof::print_device_stages(d.c_str(), copy);
    }

    printf("# PROFILE: T9_arg_type — host API breakdown\n");
    printf("arg_desc,%s\n", prof::host_csv_header());
    for (auto& [d, evs] : host_log)
        prof::print_host_breakdown(d.c_str(), evs);
}

// ── P10: stream pipelining + overlap ratio ───────────────────────────────────

static void p10_stream_pipelining(double ghz)
{
    printf("\n# TEST: T10_stream_pipelining\n");
    printf("# warmup=%d reps=%d (profiled)\n", WARMUP, REPS);

    const int TC = 64;
    const std::vector<int> stream_counts = {1, 2, 4, 8};

    Ctx c = make_ctx();
    int dummy = 0;

    printf("# Sub-test A: streams_pipelined\n");
    // p50 matters more than the mean here: run-to-run spread reaches ~60%% of
    // the mean at K=8, so the mean is dominated by a long tail.
    printf("mode,K,total_us_mean,total_us_std,total_us_p50,total_us_min,per_map_us,n\n");

    struct Overlap { int K; double wall_us; double sum_kernel_us; double seq_us; };
    std::vector<Overlap> overlaps;
    std::vector<std::pair<int, std::vector<xpti::HostEvent>>> host_log;

    for (int K : stream_counts)
    {
        std::vector<pxl::runtime::Stream*> streams(K, nullptr);
        std::vector<pxl::runtime::Map*>    maps(K,    nullptr);
        bool ok = true;
        for (int k = 0; k < K; k++)
        {
            streams[k] = pxl::runtime::createStream();
            if (!streams[k]) { ok = false; break; }
            maps[k] = c.job->buildMap(c.func, TC);
            if (!maps[k]) { ok = false; break; }
            maps[k]->setStream(streams[k]);
        }
        if (!ok)
        {
            fprintf(stderr, "P10A: setup failed for K=%d (graceful skip)\n", K);
            for (auto* s : streams) if (s) pxl::runtime::destroyStream(s);
            for (auto* m : maps)    if (m) c.job->destroyMap(m);
            continue;
        }

        for (int i = 0; i < WARMUP; i++)
        {
            for (int k = 0; k < K; k++) sink = chk(maps[k]->execute(dummy));
            for (int k = 0; k < K; k++) chk(maps[k]->synchronize());
        }

        prof::Cursor cur; drain_cursor(cur);

        // The two arms are INTERLEAVED within each rep, and the order is
        // alternated between reps.  Timing all of one arm and then all of the
        // other would leave the second arm's handles REPS dispatches older,
        // and handle aging costs ~3.27 us/dispatch (\S handle aging), which at
        // REPS~200 inflates the later arm by ~650 us on a ~1,200 us dispatch --
        // i.e. it would manufacture most of the reported speedup.
        std::vector<double> par_samples, seq_samples;
        par_samples.reserve(REPS); seq_samples.reserve(REPS);

        auto time_pipelined = [&]() {
            uint64_t t0 = timing::start();
            for (int k = 0; k < K; k++) sink = chk(maps[k]->execute(dummy));
            for (int k = 0; k < K; k++) chk(maps[k]->synchronize());
            return timing::to_us(stop_fn() - t0, ghz);
        };
        auto time_sequential = [&]() {
            uint64_t t0 = timing::start();
            for (int k = 0; k < K; k++) { sink = chk(maps[k]->execute(dummy)); chk(maps[k]->synchronize()); }
            return timing::to_us(stop_fn() - t0, ghz);
        };

        for (int i = 0; i < REPS; i++)
        {
            if (i & 1)   // alternate which arm goes first
            {
                seq_samples.push_back(time_sequential());
                par_samples.push_back(time_pipelined());
            }
            else
            {
                par_samples.push_back(time_pipelined());
                seq_samples.push_back(time_sequential());
            }
        }
        auto par_s = Stats::from(par_samples);
        auto seq_s = Stats::from(seq_samples);

        printf("pipelined,%d,%.4f,%.4f,%.4f,%.4f,%.4f,%zu\n",
               K, par_s.mean, par_s.stddev, par_s.p50, par_s.min,
               par_s.p50 / K, par_s.n);
        printf("sequential,%d,%.4f,%.4f,%.4f,%.4f,%.4f,%zu\n",
               K, seq_s.mean, seq_s.stddev, seq_s.p50, seq_s.min,
               seq_s.p50 / K, seq_s.n);

        // Overlap ratio: device-side total kernel time / wall-clock.
        // A ratio > 1 means overlap (multiple kernels active concurrently).
        auto dev_evs  = prof::read_device_events_delta(SESSION_ID, cur);
        auto host_evs = prof::read_host_events_delta(SESSION_ID, cur);
        auto stages   = prof::aggregate_device_events(dev_evs, MU_FREQ_MHZ);
        double sum_dev = 0;
        for (double d : stages.launch_to_terminate) sum_dev += d;
        double wall_total = par_s.mean * REPS;
        overlaps.push_back({ K, wall_total, sum_dev, seq_s.mean * REPS });
        host_log.emplace_back(K, std::move(host_evs));

        for (int k = 0; k < K; k++)
        {
            c.job->destroyMap(maps[k]);
            pxl::runtime::destroyStream(streams[k]);
        }
    }
    destroy_ctx(c);

    // C3: the old "overlap_ratio = sum_device_us / wall_us" is meaningless here.
    // The device is ~99.97% idle under a null kernel, so that quotient measures
    // device IDLENESS, not concurrency — it came out 0.0001-0.0004 regardless.
    // The honest metric is the sequential-vs-pipelined speedup, which is what
    // streams actually buy on the host submission path.
    printf("# PROFILE: T10_stream_pipelining — speedup = sequential_wall / pipelined_wall\n");
    printf("# NOTE: device-time/wall ratio deliberately NOT reported (see C3) —\n");
    printf("#       with a null kernel it measures device idleness, not overlap.\n");
    printf("K,wall_pipelined_total_us,wall_sequential_total_us,speedup\n");
    for (auto& o : overlaps)
    {
        double speedup = (o.wall_us > 0) ? (o.seq_us / o.wall_us) : 0.0;
        printf("%d,%.4f,%.4f,%.4f\n", o.K, o.wall_us, o.seq_us, speedup);
    }

    printf("# PROFILE: T10_stream_pipelining — host API breakdown (pipelined mode)\n");
    printf("K,%s\n", prof::host_csv_header());
    for (auto& [K, evs] : host_log)
    {
        char p[32]; snprintf(p, sizeof(p), "%d", K);
        prof::print_host_breakdown(p, evs);
    }
}

// ── P11: prepare cost (host events only — device idle during prepare) ────────

static void p11_prepare_cost(double ghz)
{
    printf("\n# TEST: T11_prepare_cost\n");
    printf("# warmup=%d reps=%d (profiled — host events only)\n", WARMUP, REPS);
    printf("stage,mean_us,std_us,min_us,p50_us,p95_us,p99_us,max_us,n\n");

    auto run_reps = [&](const char* name, std::function<double()> measure_one)
    {
        for (int i = 0; i < WARMUP; i++) (void)measure_one();
        std::vector<double> samples; samples.reserve(REPS);
        for (int i = 0; i < REPS; i++) samples.push_back(measure_one());
        auto s = Stats::from(samples);
        s.print_csv(name);
    };

    run_reps("create_module", [&]() {
        uint64_t t0 = timing::start();
        auto* m = pxl::kernel::createModule(KERNEL_FILE);
        uint64_t t1 = stop_fn();
        if (m) pxl::kernel::destroyModule(m);
        return timing::to_us(t1 - t0, ghz);
    });

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

    run_reps("create_context", [&]() {
        uint64_t t0 = timing::start();
        auto* ctx = pxl::runtime::createContext(0);
        uint64_t t1 = stop_fn();
        if (ctx) pxl::runtime::destroyContext(ctx);
        return timing::to_us(t1 - t0, ghz);
    });

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

    printf("# PROFILE: T11_prepare_cost — no device kernels execute during prepare;\n");
    printf("#   the device-event stream is intentionally empty for this test.\n");
}

// ── P12: NDArray arg cost + per-shape device stage breakdown ─────────────────

static void p12_ndarray_arg(double ghz)
{
    printf("\n# TEST: T12_ndarray_arg\n");
    printf("# taskCount=64 warmup=%d reps=%d (profiled)\n", WARMUP, REPS);
    printf("kernel,arg_desc,dims,mean_us,std_us,min_us,p50_us,p95_us,p99_us,max_us,n\n");

    const int TC = 64;
    Ctx c = make_ctx();
    int* dev_buf = reinterpret_cast<int*>(c.ctx->memAlloc(1024 * sizeof(int)));
    if (!dev_buf) { fprintf(stderr, "P12: memAlloc failed\n"); destroy_ctx(c); return; }

    using Shape = pxl::runtime::NDArray<int>::ShapeArray;
    auto make_shape = [](std::initializer_list<int64_t> dims) {
        Shape s; s.fill(0);
        size_t i = 0; for (auto d : dims) { if (i < s.size()) s[i++] = d; }
        return s;
    };
    struct ShapeCase { const char* tag; int dims; Shape s; };
    std::vector<ShapeCase> shape_cases = {
        { "1d", 1, make_shape({1024})         },
        { "3d", 3, make_shape({16, 8, 8})     },
        { "5d", 5, make_shape({4, 4, 4, 4, 4})},
    };

    struct Log { std::string desc; std::string tag; prof::DeviceStages st; std::vector<xpti::HostEvent> host; };
    std::vector<Log> logs;

    auto run_arity = [&](const char* fname, const char* desc, int n) {
        auto* fn = c.module->createFunction(fname);
        if (!fn) return;
        for (auto& sc : shape_cases)
        {
            auto* map = c.job->buildMap(fn, TC);
            if (!map) continue;
            pxl::runtime::NDArray<int> a(dev_buf, sc.s);
            pxl::runtime::NDArray<int> b(dev_buf, sc.s);
            pxl::runtime::NDArray<int> d(dev_buf, sc.s);

            auto warmup_call = [&]() {
                if (n == 1) sink = chk(map->execute(a));
                else        sink = chk(map->execute(a, b, d));
                chk(map->synchronize());
            };
            for (int i = 0; i < WARMUP; i++) warmup_call();

            prof::Cursor cur; drain_cursor(cur);

            std::vector<double> samples; samples.reserve(REPS);
            for (int i = 0; i < REPS; i++)
            {
                uint64_t t0 = timing::start();
                warmup_call();
                uint64_t t1 = stop_fn();
                samples.push_back(timing::to_us(t1 - t0, ghz));
            }
            auto s = Stats::from(samples);
            char prefix[64]; snprintf(prefix, sizeof(prefix), "%s,%s,%s", fname, desc, sc.tag);
            s.print_csv(prefix);

            auto dev_evs  = prof::read_device_events_delta(SESSION_ID, cur);
            auto host_evs = prof::read_host_events_delta(SESSION_ID, cur);
            Log L; L.desc = desc; L.tag = sc.tag;
            L.st   = prof::aggregate_device_events(dev_evs, MU_FREQ_MHZ);
            L.host = std::move(host_evs);
            logs.push_back(std::move(L));

            c.job->destroyMap(map);
        }
    };

    run_arity("null_1ndarray", "1_ndarray", 1);
    run_arity("null_3ndarray", "3_ndarrays", 3);

    c.ctx->memFree(dev_buf);
    destroy_ctx(c);

    printf("# PROFILE: T12_ndarray_arg — does descriptor size show up in launch_to_start?\n");
    printf("arg_desc,dims,stage,mean_us,std_us,min_us,p50_us,p95_us,p99_us,max_us,n\n");
    for (auto& L : logs)
    {
        char p[64]; snprintf(p, sizeof(p), "%s,%s", L.desc.c_str(), L.tag.c_str());
        prof::DeviceStages copy = L.st;
        prof::print_device_stages(p, copy);
    }

    printf("# PROFILE: T12_ndarray_arg — host API breakdown\n");
    printf("arg_desc,dims,%s\n", prof::host_csv_header());
    for (auto& L : logs)
    {
        char p[64]; snprintf(p, sizeof(p), "%s,%s", L.desc.c_str(), L.tag.c_str());
        prof::print_host_breakdown(p, L.host);
    }
}

// ── P13: concurrent maps (descriptive profiling) ──────────────────────────────

static void p13_concurrent_maps(double ghz)
{
    printf("\n# TEST: T13_concurrent_maps\n");
    printf("# taskCount=64 warmup=%d reps=%d (profiled)\n", WARMUP, REPS);
    printf("mode,K,wall_us_mean,wall_us_std,per_dispatch_us,n\n");

    const int TC = 64;
    const std::vector<int> K_values = {1, 2, 4, 8};

    Ctx c = make_ctx();
    int dummy = 0;

    struct Log { std::string mode; int K; prof::DeviceStages st; std::vector<xpti::HostEvent> host; };
    std::vector<Log> logs;

    for (int K : K_values)
    {
        std::vector<pxl::runtime::Map*> maps(K, nullptr);
        bool ok = true;
        for (int k = 0; k < K; k++) { maps[k] = c.job->buildMap(c.func, TC); if (!maps[k]) { ok = false; break; } }
        if (!ok)
        {
            fprintf(stderr, "P13: buildMap failed for K=%d\n", K);
            for (auto* m : maps) if (m) c.job->destroyMap(m);
            continue;
        }

        // Warmup
        for (int i = 0; i < WARMUP; i++)
            for (int k = 0; k < K; k++) { sink = chk(maps[k]->execute(dummy)); chk(maps[k]->synchronize()); }

        // concurrent
        {
            prof::Cursor cur; drain_cursor(cur);
            std::atomic<bool> go{false};
            std::vector<std::thread> ths; ths.reserve(K);
            for (int k = 0; k < K; k++)
            {
                ths.emplace_back([&, k]() {
                    while (!go.load(std::memory_order_acquire)) {}
                    int local = 0;
                    for (int i = 0; i < REPS; i++) { sink = chk(maps[k]->execute(local)); chk(maps[k]->synchronize()); }
                });
            }
            uint64_t t0 = timing::start();
            go.store(true, std::memory_order_release);
            for (auto& t : ths) t.join();
            uint64_t t1 = stop_fn();
            double total_us = timing::to_us(t1 - t0, ghz);
            double per = total_us / (K * REPS);
            printf("concurrent,%d,%.4f,%.4f,%.4f,%d\n", K, total_us, 0.0, per, K * REPS);

            auto dev      = prof::read_device_events_delta(SESSION_ID, cur);
            auto host_evs = prof::read_host_events_delta(SESSION_ID, cur);
            Log L; L.mode = "concurrent"; L.K = K;
            L.st   = prof::aggregate_device_events(dev, MU_FREQ_MHZ);
            L.host = std::move(host_evs);
            logs.push_back(std::move(L));
        }

        // Rebuild every handle before the serial arm.  The concurrent arm above
        // just issued K*REPS dispatches on these handles, and aging costs
        // ~3.27 us/dispatch, so reusing them here would hand the serial arm a
        // systematically slower baseline and inflate the concurrency speedup.
        // (T13 cannot interleave arms the way T10 does: the concurrent arm is a
        // single timed region spanning K threads.)  Both arms therefore start
        // from equally fresh handles, with an identical warmup applied.
        for (int k = 0; k < K; k++)
        {
            c.job->destroyMap(maps[k]);
            maps[k] = c.job->buildMap(c.func, TC);
            if (!maps[k]) { ok = false; break; }
        }
        if (!ok)
        {
            fprintf(stderr, "P13: rebuild before serial arm failed for K=%d\n", K);
            for (auto* m : maps) if (m) c.job->destroyMap(m);
            continue;
        }
        for (int i = 0; i < WARMUP; i++)
            for (int k = 0; k < K; k++) { sink = chk(maps[k]->execute(dummy)); chk(maps[k]->synchronize()); }

        // serial
        {
            prof::Cursor cur; drain_cursor(cur);
            uint64_t t0 = timing::start();
            for (int i = 0; i < REPS; i++)
                for (int k = 0; k < K; k++) { sink = chk(maps[k]->execute(dummy)); chk(maps[k]->synchronize()); }
            uint64_t t1 = stop_fn();
            double total_us = timing::to_us(t1 - t0, ghz);
            double per = total_us / (K * REPS);
            printf("serial,%d,%.4f,%.4f,%.4f,%d\n", K, total_us, 0.0, per, K * REPS);

            auto dev      = prof::read_device_events_delta(SESSION_ID, cur);
            auto host_evs = prof::read_host_events_delta(SESSION_ID, cur);
            Log L; L.mode = "serial"; L.K = K;
            L.st   = prof::aggregate_device_events(dev, MU_FREQ_MHZ);
            L.host = std::move(host_evs);
            logs.push_back(std::move(L));
        }

        for (auto* m : maps) c.job->destroyMap(m);
    }
    destroy_ctx(c);

    printf("# PROFILE: T13_concurrent_maps — does per-dispatch device time differ between modes?\n");
    printf("mode,K,stage,mean_us,std_us,min_us,p50_us,p95_us,p99_us,max_us,n\n");
    for (auto& L : logs)
    {
        char p[64]; snprintf(p, sizeof(p), "%s,%d", L.mode.c_str(), L.K);
        prof::DeviceStages copy = L.st;
        prof::print_device_stages(p, copy);
    }

    printf("# PROFILE: T13_concurrent_maps — host API breakdown\n");
    printf("mode,K,%s\n", prof::host_csv_header());
    for (auto& L : logs)
    {
        char p[64]; snprintf(p, sizeof(p), "%s,%d", L.mode.c_str(), L.K);
        prof::print_host_breakdown(p, L.host);
    }
}

// ── P14: dynamic subFree + before/after stage compare ────────────────────────

static void p14_partial_subfree(double ghz)
{
    printf("\n# TEST: T14_partial_subfree\n");
    printf("# taskCount=64 warmup=%d reps=%d (profiled)\n", WARMUP, REPS);

    auto* probe = pxl::runtime::createContext(0);
    uint32_t max_sub = 1;
    if (probe) { max_sub = probe->availableSubCount(); pxl::runtime::destroyContext(probe); }
    printf("# availableSubCount=%u\n", max_sub);
    printf("release_count,phase,mean_us,std_us,min_us,p50_us,p95_us,p99_us,max_us,n\n");

    const int TC = 64;
    std::vector<uint32_t> release_counts;
    if (max_sub >= 2)  release_counts.push_back(1);
    if (max_sub >= 4)  release_counts.push_back(max_sub / 4);
    if (max_sub >= 4)  release_counts.push_back(max_sub / 2);
    if (max_sub >= 2)  release_counts.push_back(max_sub - 1);

    struct Log { uint32_t rc; std::string phase; prof::DeviceStages st; std::vector<xpti::HostEvent> host; };
    std::vector<Log> logs;

    for (uint32_t rc : release_counts)
    {
        auto* ctx = pxl::runtime::createContext(0); if (!ctx) break;
        auto* job = ctx->createJob(max_sub); if (!job) { pxl::runtime::destroyContext(ctx); break; }
        auto* mod = pxl::kernel::createModule(KERNEL_FILE);
        if (!mod || job->load(mod) != pxl::PxlResult::Success)
        { ctx->destroyJob(job); pxl::runtime::destroyContext(ctx); continue; }
        auto* fn  = mod->createFunction(KERNEL_FUNC);
        if (!fn) { ctx->destroyJob(job); pxl::runtime::destroyContext(ctx); continue; }
        auto* map = job->buildMap(fn, TC);
        if (!map) { ctx->destroyJob(job); pxl::runtime::destroyContext(ctx); continue; }

        // Baseline
        auto r_base = profiled_dispatch_loop(map, WARMUP, REPS, ghz,
            [&](pxl::runtime::Map*& m) { renew_map(job, fn, TC, m); });
        auto base_s = Stats::from(r_base.wall_us);
        char prefix[64]; snprintf(prefix, sizeof(prefix), "%u,baseline_all_subs", rc);
        base_s.print_csv(prefix);
        logs.push_back({ rc, "baseline_all_subs",
                         std::move(r_base.stages),
                         std::move(r_base.host_events) });

        // Partial release
        job->subFree(rc);

        // After release — no fresh warmup, capture potential first-rep spike.
        prof::Cursor cur; drain_cursor(cur);
        std::vector<double> after; after.reserve(REPS);
        int dummy2 = 0;
        for (int i = 0; i < REPS; i++)
        {
            // Rebuilt handles pick up the job's reduced Sub set after subFree.
            if (g_fresh) renew_map(job, fn, TC, map);           // outside timing
            uint64_t t0 = timing::start();
            sink = chk(map->execute(dummy2)); chk(map->synchronize());
            uint64_t t1 = stop_fn();
            after.push_back(timing::to_us(t1 - t0, ghz));
        }
        auto after_s = Stats::from(after);
        snprintf(prefix, sizeof(prefix), "%u,after_release", rc);
        after_s.print_csv(prefix);

        auto dev      = prof::read_device_events_delta(SESSION_ID, cur);
        auto host_evs = prof::read_host_events_delta(SESSION_ID, cur);
        logs.push_back({ rc, "after_release",
                         prof::aggregate_device_events(dev, MU_FREQ_MHZ),
                         std::move(host_evs) });

        printf("# rc=%u delta_mean_us=%.4f\n", rc, after_s.mean - base_s.mean);

        job->destroyMap(map); ctx->destroyJob(job); pxl::runtime::destroyContext(ctx);
    }

    printf("# PROFILE: T14_partial_subfree — re-routing should show up as launch_to_start delta\n");
    printf("release_count,phase,stage,mean_us,std_us,min_us,p50_us,p95_us,p99_us,max_us,n\n");
    for (auto& L : logs)
    {
        char p[64]; snprintf(p, sizeof(p), "%u,%s", L.rc, L.phase.c_str());
        prof::DeviceStages copy = L.st;
        prof::print_device_stages(p, copy);
    }

    printf("# PROFILE: T14_partial_subfree — host API breakdown\n");
    printf("release_count,phase,%s\n", prof::host_csv_header());
    for (auto& L : logs)
    {
        char p[64]; snprintf(p, sizeof(p), "%u,%s", L.rc, L.phase.c_str());
        prof::print_host_breakdown(p, L.host);
    }
}

// ── P15: persistent kernel amortization (profiled) ───────────────────────────
//
// Same comparison as t15 in main.cpp, plus a host API breakdown per (iters, mode)
// so you can see how the host-side dispatch tax accumulates linearly in repeated
// mode but folds to a single launch in persistent mode.

static void p15_persistent_amortization(double ghz)
{
    // Multi-iteration dispatch: one launch whose kernel loops k times, versus
    // k separate one-iteration dispatches.
    //
    // FIXED 2026-09-14. The original shared ONE handle across both arms and the
    // whole sweep, so persistent@k=1024 ran on a handle aged ~11,000 dispatches;
    // handle aging alone predicted its cost to within 1-2%. Now:
    //   - FRESH=1 rebuilds the handle before EVERY timed dispatch in both arms;
    //   - the two arms are interleaved rep by rep;
    //   - p50 over REPS_K reps (was the mean over as few as 10);
    //   - the repeated arm is measured directly for k <= REPEATED_DIRECT_MAX,
    //     which checks that it scales linearly; above that it is derived as
    //     k * median(one-iteration dispatch), valid because dispatches on
    //     separately rebuilt handles are independent.
    // This is NOT a resident kernel: the iteration count is fixed at dispatch
    // time and no new work arrives. The resident pull model is
    // tier1/resident_worker (drain_bench).
    printf("\n# TEST: T15_multi_iteration_dispatch\n");
    const int TC = 64, REPS_K = 50, REPEATED_DIRECT_MAX = 64;
    const std::vector<int> ks = {1, 4, 16, 64, 256, 1024};
    printf("# taskCount=%d reps=%d fresh_handle=%s repeated_direct_max=%d\n",
           TC, REPS_K, g_fresh ? "ON" : "OFF", REPEATED_DIRECT_MAX);
    printf("iters,mode,p50_total_us,min_total_us,max_total_us,per_iter_us,reps\n");

    Ctx c = make_ctx();
    auto* fn = c.module->createFunction("persistent_noop");
    if (!fn) { fprintf(stderr, "P15: createFunction(persistent_noop) failed\n"); destroy_ctx(c); return; }
    auto* map = c.job->buildMap(fn, TC);
    if (!map) { fprintf(stderr, "P15: buildMap failed\n"); destroy_ctx(c); return; }

    for (int i = 0; i < WARMUP; i++) { sink = chk(map->execute(16)); chk(map->synchronize()); }

    // One timed dispatch of `iters` iterations; under FRESH=1 on a new handle.
    auto timed = [&](int iters) -> double {
        if (g_fresh) renew_map(c.job, fn, TC, map);           // outside timing
        uint64_t t0 = timing::start();
        sink = chk(map->execute(iters));
        chk(map->synchronize());
        uint64_t t1 = stop_fn();
        return timing::to_us(t1 - t0, ghz);
    };
    auto emit = [](int k, const char* mode, std::vector<double> v) {
        auto s = Stats::from(v);
        printf("%d,%s,%.4f,%.4f,%.4f,%.4f,%zu\n", k, mode, s.p50, s.min, s.max, s.p50 / k, s.n);
    };

    double one_shot_p50 = 0.0;
    for (int k : ks)
    {
        const bool direct = (k <= REPEATED_DIRECT_MAX);
        std::vector<double> pers, rep;
        pers.reserve(REPS_K); rep.reserve(REPS_K);
        for (int r = 0; r < REPS_K; r++)                       // arms interleaved
        {
            pers.push_back(timed(k));                          // 1 launch, k iterations
            if (direct)
            {
                double sum = 0.0;                              // k launches, 1 iteration each
                for (int j = 0; j < k; j++) sum += timed(1);
                rep.push_back(sum);
            }
        }
        emit(k, "persistent", pers);
        if (direct)
        {
            if (k == 1) { auto tmp = rep; one_shot_p50 = Stats::from(tmp).p50; }
            emit(k, "repeated", rep);
        }
        else
        {
            double d = one_shot_p50 * k;
            printf("%d,repeated_derived,%.4f,%.4f,%.4f,%.4f,0\n", k, d, d, d, one_shot_p50);
        }
        fflush(stdout);
    }

    c.job->destroyMap(map);
    destroy_ctx(c);
}

// ── main ──────────────────────────────────────────────────────────────────────

static const int NUM_TESTS = 15;

static void usage(const char* prog)
{
    fprintf(stderr,
        "Usage: %s [tests...]\n"
        "  Profiled variant: same tests as `main` plus per-stage device breakdown.\n"
        "  Tests: t1 .. t15  (default: all)\n", prog);
}

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

// Affinity control.
//
// NOTE: this runs before the first createContext(), and Linux threads inherit
// the creator's affinity mask — so pinning here confines the whole PXL runtime
// worker pool (measured: 11 threads), not just the measuring thread.  On this
// host CPU 0 is also the interrupt sink (RES rescheduling IPIs ~500x any other
// core), which injects scheduler noise into every host-side measurement.
//
//   BENCH_CPU=<n>   pin to core n            (default 0 — original behavior)
//   BENCH_CPU=none  do not pin; TSC is invariant (constant_tsc + nonstop_tsc
//                   + rdtscp) so cross-core timing stays valid.
static void pin_cpu()
{
    const char* env = getenv("BENCH_CPU");
    if (env && (strcmp(env, "none") == 0 || strcmp(env, "-1") == 0))
    {
        printf("# cpu_affinity: unpinned (runtime free to schedule)\n");
        return;
    }
    int cpu = 0;
    if (env) { int v = atoi(env); if (v >= 0) cpu = v; }
    cpu_set_t cs; CPU_ZERO(&cs); CPU_SET(cpu, &cs);
    if (sched_setaffinity(0, sizeof(cs), &cs) != 0)
        fprintf(stderr, "# Warning: sched_setaffinity failed\n");
    else
        printf("# cpu_affinity: pinned to CPU %d\n", cpu);
}

static void print_device_info()
{
    printf("# PLATFORM\n# devices: %u\n", pxl::runtime::getNumDevice());
    auto* ctx = pxl::runtime::createContext(0); if (!ctx) return;
    struct { pxl::DeviceAttr attr; const char* name; } attrs[] = {
        { pxl::DeviceAttr::TotalSubCount,    "TotalSubCount"    },
        { pxl::DeviceAttr::NumClusterPerSub, "NumClusterPerSub" },
        { pxl::DeviceAttr::NumMuPerCluster,  "NumMuPerCluster"  },
        { pxl::DeviceAttr::NumThreadPerMu,   "NumThreadPerMu"   },
    };
    for (auto& a : attrs) { uint64_t v = 0;
        if (ctx->getAttribute(a.attr, v) == pxl::PxlResult::Success)
            printf("# %s: %lu\n", a.name, v); }
    pxl::runtime::destroyContext(ctx);
}

int main(int argc, char* argv[])
{
    // Per pxl/runtime/map.hpp: when XPTI_ENABLE=1, the Map constructor
    // automatically attaches to the active XPTI session, which is what makes
    // DeviceEvent probes flow.  startProcessSession() alone is not enough —
    // each Map still has to opt in.  Setting the env var here (before any
    // PXL call) guarantees that for every test in this binary.
    // XPTI is opt-in per run.  It is NOT free: the device-probe opcode
    // round-trips inflate wall clock 1.4x-213x (measured), and seven of the
    // nine probe types (Mu, L1, L2, L3, MTC, PCU, Gaia) return zero rows on
    // this platform.  Only DeviceEvent yields data, and only because the
    // kernels call mu::profilerStart/End themselves.
    //
    // Wall-clock and phase decomposition come from rdtsc + map->getStats()
    // (a PXL API, not XPTI) and need no session at all.
    //
    //   XPTI=1  enable device-stage profiling (T3/T9/T12 need this)
    //   unset   no session; wall clock is undistorted   <-- default
    const bool xpti_on = [] {
        const char* e = getenv("XPTI");
        return e && e[0] == '1';
    }();
    { const char* e = getenv("STATS"); if (e && e[0]=='0') g_stats_on = false; }
    { const char* e = getenv("FRESH"); if (e && e[0]=='1') g_fresh = true; }
    printf("# fresh_handle: %s\n", g_fresh ? "ON (rebuild before every timed dispatch: T1 T3 T6 T7 T8 T14 T15)" : "OFF");
    printf("# getstats: %s\n", g_stats_on ? "ON" : "OFF");

    if (xpti_on) setenv("XPTI_ENABLE", "1", /*overwrite=*/0);
    else         setenv("XPTI_ENABLE", "0", /*overwrite=*/1);

    bool run[NUM_TESTS + 1] = {};
    if (argc == 1) { for (int i = 1; i <= NUM_TESTS; i++) run[i] = true; }
    else
    {
        for (int i = 1; i < argc; i++)
        {
            if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0)
            { usage(argv[0]); return 0; }
            int n = parse_test_arg(argv[i]);
            if (n > 0) { run[n] = true; continue; }
            fprintf(stderr, "Unknown argument: %s\n", argv[i]);
            usage(argv[0]); return 1;
        }
    }

    printf("# Orchestration Tax Benchmark Suite — Profiler Edition\n");
    printf("# kernel: %s  func: %s\n", KERNEL_FILE, KERNEL_FUNC);
    pin_cpu();

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

    // Start XPTI process session.  After this, all PXL host/device events flow
    // into a single accumulating buffer and we read deltas per-test.
    if (!xpti_on)
    {
        SESSION_ID = 0;
        printf("# xpti: OFF (set XPTI=1 for device stages; inflates wall clock)\n");
    }
    else
    {
        SESSION_ID = prof::start_session(0);
        if (SESSION_ID == 0)
        {
            fprintf(stderr, "# Warning: XPTI session not started — profile blocks will be empty\n");
        }
        else
        {
            MU_FREQ_MHZ = prof::mu_freq_mhz(SESSION_ID);
            printf("# xpti_session: %lu  mu_freq_mhz: %.1f\n", SESSION_ID, MU_FREQ_MHZ);
        }
    }

    // Per-test dispatch tally (A2).  A non-zero failure count means some
    // "latency" samples in that test are rejected dispatches returning early;
    // treat that test's data as void.
    auto run_test = [&](int n, const char* name, void (*fn)(double)) {
        if (!run[n]) return;
        g_calls.store(0); g_fails.store(0); g_first_fail = pxl::PxlResult::Success;
        fn(ghz);
        uint64_t c = g_calls.load(), f = g_fails.load();
        printf("# DISPATCH_CHECK %s: calls=%lu ok=%lu failed=%lu", name, c, c - f, f);
        if (f) printf("  first_fail_code=%d  <-- DATA INVALID", (int)g_first_fail);
        printf("\n");
        fflush(stdout);
    };

    run_test(1,  "T1",  p1_dispatch_sweep);
    run_test(2,  "T2",  p2_warmup_convergence);
    run_test(3,  "T3",  p3_phase_split);
    run_test(4,  "T4",  p4_map_reuse);
    run_test(5,  "T5",  p5_locality_mode);
    run_test(6,  "T6",  p6_sub_count);
    run_test(7,  "T7",  p7_batch_size);
    run_test(8,  "T8",  p8_cluster_bitmap);
    run_test(9,  "T9",  p9_arg_type);
    run_test(10, "T10", p10_stream_pipelining);
    run_test(11, "T11", p11_prepare_cost);
    run_test(12, "T12", p12_ndarray_arg);
    run_test(13, "T13", p13_concurrent_maps);
    run_test(14, "T14", p14_partial_subfree);
    run_test(15, "T15", p15_persistent_amortization);

    if (xpti_on) prof::end_session();

    printf("\n# Done.\n");
    return 0;
}
