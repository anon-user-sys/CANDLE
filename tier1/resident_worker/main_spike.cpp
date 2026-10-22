// Persistent-kernel feasibility spike (host side).
//
// Launches the non-blocking persistent_puller kernel, then from a background
// driver thread enqueues work into a shared CXL ring buffer *after* launch and
// watches the device's heartbeat / processed counters via host-side cache
// flushes. Answers:
//   Q1 residency:  heartbeat keeps rising across a long idle gap (no watchdog kill)
//   Q2 visibility: device processes items enqueued after launch (PROC tracks TAIL)
//   Q3 doorbell:   optional asyncMessage per item lands in the host callback
//
// Must run as root (host load/store to memAlloc'd device memory SIGBUS otherwise).

#include <pxl/pxl.hpp>
#include <pxl/host_cache_flush.hpp>
#include <pxl/runtime/runtime_api.hpp>
#include <pxl/runtime/context.hpp>
#include <pxl/runtime/job.hpp>
#include <pxl/runtime/map.hpp>
#include <pxl/kernel/kernel_api.hpp>
#include <pxl/kernel/module.hpp>
#include <pxl/kernel/function.hpp>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>
#include <algorithm>
#include <immintrin.h>

static const char* KERNEL_FILE = "mu_kernel/persistent_puller.mubin";
static const char* KERNEL_FUNC = "persistent_puller";

// control-block indices (int64 slots, 64B-separated to avoid false sharing)
enum { H = 0, T = 8, STOP = 16, PROC = 24, HB = 32, MAXI = 40, MSGEN = 48, BUDGET = 49, STARTED = 56, RING = 64,
       TARGET = 64, HBATCH = 72, CYCLES = 80, SINKF = 88, DONE = 96, MODE = 104, TASKS = 128 };
static const long CAP = 1024;
static const int  SLOTS = 64 + (int)CAP;
static const double DEV_GHZ = 1.0;   // MU device clock (profiler: mu_freq_mhz=1000) => 1 cyc = 1 ns

static long* q = nullptr;  // shared CXL region; accessed via volatile casts below

static std::atomic<long> g_msg_count{0};
static std::atomic<long> g_msg_last{0};

static inline void vst(int i, long v) { *(volatile long*)&q[i] = v; }
static inline long vld(int i) { return *(volatile long*)&q[i]; }

static inline void hwrite(int i, long v) { vst(i, v); pxl::flushHostCache(&q[i], sizeof(long)); }
static inline long  dread(int i) { pxl::flushHostCache(&q[i], sizeof(long)); return vld(i); }

static double now_ms()
{
    using namespace std::chrono;
    return duration<double, std::milli>(steady_clock::now().time_since_epoch()).count();
}

// enqueue `n` items starting at absolute producer offset `base`
static void enqueue(long base, long n)
{
    for (long k = 0; k < n; k++)
    {
        long p = base + k;
        vst(RING + (p % CAP), 1000 + p);  // payload
    }
    pxl::flushHostCache(&q[RING], sizeof(long) * CAP);
    hwrite(T, base + n);  // publish new tail last
}

static int    g_idle_s = 60;             // residency idle duration (seconds), --idle N
// --enqueue: with --bench, also time host-side enqueue (a coherence /
// data-movement cost). Without it, --bench is the original coherence benchmark.
static bool   g_enqueue = false;
static bool   g_resid_ok = true;         // HB climbed monotonically the whole idle
static double g_resid_sustained_s = 0;   // seconds HB kept rising before any stall

static void driver(bool msgen)
{
    printf("[driver] started (msgen=%d, idle=%ds)\n", msgen ? 1 : 0, g_idle_s);
    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    long started = dread(STARTED);
    printf("[driver] t=%.0fms  STARTED=%ld  HB=%ld\n", 0.0, started, dread(HB));

    long produced = 0;
    double t0 = now_ms();

    // Warm-up: two quick enqueue rounds to confirm visibility works before the idle.
    for (int r = 0; r < 2; r++)
    {
        enqueue(produced, 40);
        produced += 40;
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
        printf("[driver] t=%.0fms  round %d: TAIL=%ld PROC=%ld HB=%ld msgs=%ld\n",
               now_ms() - t0, r, produced, dread(PROC), dread(HB), g_msg_count.load());
    }

    // RESIDENCY TEST (long): idle for g_idle_s, sampling HB every 5s. HB must keep
    // rising the whole time; if it ever stalls, that is a watchdog kill and we record
    // how long it sustained. PROC must stay flat (no spurious work) during the idle.
    long hb_start = dread(HB);
    long hb_prev  = hb_start;
    double ridle0 = now_ms();
    printf("[driver] t=%.0fms  === RESIDENCY IDLE %ds begin ===  HB=%ld PROC=%ld\n",
           now_ms() - t0, g_idle_s, hb_start, dread(PROC));
    for (int s = 5; s <= g_idle_s; s += 5)
    {
        std::this_thread::sleep_for(std::chrono::seconds(5));
        long hb_now = dread(HB);
        double dt = (now_ms() - ridle0) / 1000.0;
        bool rising = hb_now > hb_prev;
        printf("[driver] t=%.0fms  idle +%2ds  HB=%ld (delta=%ld) rising=%s PROC=%ld\n",
               now_ms() - t0, s, hb_now, hb_now - hb_prev, rising ? "yes" : "NO", dread(PROC));
        if (rising) g_resid_sustained_s = dt;
        else { g_resid_ok = false; break; }   // stall => watchdog kill point
        hb_prev = hb_now;
    }
    printf("[driver] t=%.0fms  === RESIDENCY IDLE end ===  HB rose %ld over %.0fs, sustained=%.0fs\n",
           now_ms() - t0, hb_prev - hb_start, (now_ms() - ridle0) / 1000.0, g_resid_sustained_s);
    long hb_before = hb_start, hb_after = hb_prev;   // for the summary verdict

    // VISIBILITY-AFTER-LONG-IDLE: enqueue more; device must still pick it up.
    enqueue(produced, 40);
    produced += 40;
    std::this_thread::sleep_for(std::chrono::milliseconds(400));
    printf("[driver] t=%.0fms  post-idle enqueue: TAIL=%ld PROC=%ld HB=%ld msgs=%ld\n",
           now_ms() - t0, produced, dread(PROC), dread(HB), g_msg_count.load());

    // Ask the kernel to exit.
    hwrite(STOP, 1);
    for (int i = 0; i < 200; i++)
    {
        if (dread(PROC) >= produced) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    // Verify echo proof on a few slots (device wrote payload+1).
    int echo_ok = 0, echo_bad = 0;
    for (long p = 0; p < produced && p < CAP; p++)
    {
        pxl::flushHostCache(&q[RING + (p % CAP)], sizeof(long));
        if (vld(RING + (p % CAP)) == 1000 + p + 1) echo_ok++; else echo_bad++;
    }

    printf("\n===== SPIKE RESULT =====\n");
    printf("produced=%ld  PROC=%ld  HB=%ld  msgs=%ld last_msg=%ld\n",
           produced, dread(PROC), dread(HB), g_msg_count.load(), g_msg_last.load());
    printf("Q1 residency (HB rose whole %ds idle): %s (delta=%ld, sustained=%.0fs)\n",
           g_idle_s, (g_resid_ok && hb_after > hb_before) ? "PASS" : "FAIL",
           hb_after - hb_before, g_resid_sustained_s);
    printf("Q2 visibility (device consumed post-launch enqueues): %s (PROC=%ld/%ld)\n",
           (dread(PROC) >= produced) ? "PASS" : "FAIL", dread(PROC), produced);
    printf("Q2 echo proof (payload+1 written back): %s (ok=%d bad=%d)\n",
           (echo_bad == 0 && echo_ok > 0) ? "PASS" : "FAIL", echo_ok, echo_bad);
    if (msgen)
        printf("Q3 doorbell (asyncMessage delivered): %s (msgs=%ld)\n",
               (g_msg_count.load() > 0) ? "PASS" : "FAIL", g_msg_count.load());
    printf("========================\n\n");
}

// Throughput bench: host pre-writes N task descriptors into device DRAM at unique
// sequential addresses (each read fresh, no per-item invalidate); the device
// (drain_bench) drains them all while timing ITSELF with getClockCycle, giving the
// pure per-task pull cost independent of host production.
static int run_bench(pxl::runtime::Context* ctx, pxl::runtime::Job* job,
                     pxl::kernel::Module* module, long N)
{
    auto* func = module->createFunction("drain_bench");
    if (!func) { fprintf(stderr, "createFunction(drain_bench) failed\n"); return 1; }

    long slots = TASKS + N;
    q = reinterpret_cast<long*>(ctx->memAlloc(sizeof(long) * slots));
    if (!q) { fprintf(stderr, "memAlloc(%ld) failed\n", slots); return 1; }

    printf("[bench] N=%ld tasks, buffer=%.1f MB, HBATCH=4096\n", N, slots * 8.0 / 1e6);
    for (int i = 0; i < TASKS; i++) vst(i, 0);
    vst(TARGET, N);
    vst(HBATCH, 4096);
    // ---- HOST ENQUEUE COST (2026-09-14) ----------------------------------
    // Previously all of this ran before t0, so the reported ns/task was
    // device-side only. Measured in three patterns. flushHostCache(opt=true)
    // is undocumented and may skip unchanged ranges, so every re-publish pass
    // below CHANGES the data (2 then back to 1) before flushing; the final
    // payload is still 1, so the checksum is unaffected.
    double te0 = now_ms();                            // cold: first publish, incl. first touch
    for (long i = 0; i < N; i++) vst(TASKS + i, 1);   // payload=1 => checksum == N
    vst(T, N);                                        // publish all work up front
    pxl::flushHostCache(q, sizeof(long) * slots);
    double enq_cold_ns = (now_ms() - te0) * 1e6 / N;

    double enq_warm_ns = 0.0, enq_incr_ns = 0.0;
    if (g_enqueue)
    {
        double tw0 = now_ms();                        // warm: ring reuse, one flush per batch
        for (long i = 0; i < N; i++) vst(TASKS + i, 2);
        pxl::flushHostCache(&q[TASKS], sizeof(long) * N);
        for (long i = 0; i < N; i++) vst(TASKS + i, 1);
        pxl::flushHostCache(&q[TASKS], sizeof(long) * N);
        enq_warm_ns = (now_ms() - tw0) * 1e6 / (2.0 * N);

        long M = N < 100000 ? N : 100000;             // incremental: store + flush per item
        double ti0 = now_ms();
        for (long i = 0; i < M; i++) { hwrite(TASKS + i, 2); hwrite(TASKS + i, 1); }
        enq_incr_ns = (now_ms() - ti0) * 1e6 / (2.0 * M);
    }

    auto* map = job->buildMap(func, 1);
    if (!map) { fprintf(stderr, "buildMap(drain_bench) failed\n"); return 1; }

    double t0 = now_ms();
    pxl::PxlResult r = map->execute(q);
    if (r != pxl::PxlResult::Success)
    { fprintf(stderr, "[bench] execute failed (result=%d)\n", (int)r); return 2; }
    map->synchronize();                 // blocks until drain_bench drains all N and returns
    double wall_s = (now_ms() - t0) / 1000.0;

    long cyc  = dread(CYCLES);
    long sink = dread(SINKF);
    long done = dread(DONE);

    double dev_ns   = (double)cyc / N / DEV_GHZ;       // device pure pull cost, ns/task
    double dev_Mtps = (dev_ns > 0) ? 1000.0 / dev_ns : 0;
    double wall_ns  = wall_s * 1e9 / N;                // end-to-end incl launch+sync
    double wall_Mtps = (N / wall_s) / 1e6;

    printf("\n===== THROUGHPUT (drain_bench) =====\n");
    printf("checksum: sink=%ld expected=%ld done=%ld  -> %s\n",
           sink, N, done, (sink == N && done == 1) ? "OK" : "MISMATCH!");
    printf("device drain: %ld cyc over %ld tasks\n", cyc, N);
    printf("  device pull cost : %.2f ns/task   (%.1f M tasks/s)\n", dev_ns, dev_Mtps);
    printf("  end-to-end (wall): %.2f ns/task   (%.1f M tasks/s, incl launch+sync over %.3fs)\n",
           wall_ns, wall_Mtps, wall_s);
    // No "vs host dispatch" comparison: drain_bench is a single-MU-thread
    // memory read plus cache-coherence flushes, not a dispatch cost.
    if (g_enqueue)
    {
        printf("host enqueue     : cold %.2f | warm %.2f | incremental(flush/item) %.2f ns/item\n",
               enq_cold_ns, enq_warm_ns, enq_incr_ns);
        printf("end-to-end/item  : cold %.2f (enqueue+wall) | steady %.2f (warm enqueue+device) | incremental %.2f ns\n",
               enq_cold_ns + wall_ns, enq_warm_ns + dev_ns, enq_incr_ns + dev_ns);
    }
    printf("====================================\n\n");

    job->destroyMap(map);
    return 0;
}

// ── Heterogeneous dispatch test ───────────────────────────────────────────────
// Can the resident poller run a DIFFERENT task type per item, and what does the
// dispatch cost? Sweeps mode (baseline / switch / indirect-call) x opcode mix
// pattern (homogeneous / alternating / random). The random pattern is the
// interesting one: in-order cores have weak branch prediction, so unpredictable
// opcode streams should be the worst case.
static uint64_t apply_op(uint64_t s, uint64_t v, int op)
{
    switch (op) { case 0: return s + v; case 1: return s ^ v;
                  case 2: return s > v ? s : v; default: return s - v; }
}

static int run_multi(pxl::runtime::Context* ctx, pxl::runtime::Job* job,
                     pxl::kernel::Module* module, long N, int nops)
{
    auto* func = module->createFunction("multi_bench");
    if (!func) { fprintf(stderr, "createFunction(multi_bench) failed\n"); return 1; }

    long slots = TASKS + N;
    q = reinterpret_cast<long*>(ctx->memAlloc(sizeof(long) * slots));
    if (!q) { fprintf(stderr, "memAlloc(%ld) failed\n", slots); return 1; }

    const char* mode_name[3] = { "baseline(no dispatch)", "switch", "fptr(indirect)" };
    const char* pat_name[3]  = { "homogeneous", "alternating", "random" };

    printf("[multi] N=%ld tasks, nops=%d, buffer=%.1f MB\n", N, nops, slots * 8.0 / 1e6);
    printf("\n%-22s %-12s %10s %10s %10s  %s\n",
           "mode", "pattern", "cyc/task", "ns/task", "M tasks/s", "checksum");
    printf("%s\n", "-------------------------------------------------------------------------------");

    double base_ns = 0;
    for (int mode = 0; mode <= 2; mode++)
    {
        // baseline ignores opcodes, so only run it once
        for (int pat = 0; pat < 3; pat++)
        {
            if (mode == 0 && pat > 0) continue;

            // build descriptors: desc = (val << 8) | op, and simulate expected sink
            uint64_t expect = 0, rnd = 88172645463325252ULL;
            for (long i = 0; i < N; i++)
            {
                int op;
                if (pat == 0)      op = 0;
                else if (pat == 1) op = (int)(i % nops);
                else { rnd ^= rnd << 13; rnd ^= rnd >> 7; rnd ^= rnd << 17; op = (int)(rnd % (uint64_t)nops); }
                uint64_t val  = (uint64_t)(i & 0xFFFF);
                uint64_t desc = (val << 8) | (uint64_t)op;
                vst(TASKS + i, (long)desc);
                expect = (mode == 0) ? (expect + desc) : apply_op(expect, val, op);
            }

            for (int i = 0; i < TASKS; i++) vst(i, 0);
            vst(TARGET, N); vst(HBATCH, 4096); vst(MODE, mode); vst(T, N);
            pxl::flushHostCache(q, sizeof(long) * slots);

            auto* map = job->buildMap(func, 1);
            if (!map) { fprintf(stderr, "buildMap failed\n"); return 1; }
            pxl::PxlResult r = map->execute(q);
            if (r != pxl::PxlResult::Success)
            { fprintf(stderr, "execute failed (result=%d)\n", (int)r); return 2; }
            map->synchronize();

            long cyc = dread(CYCLES);
            uint64_t sink = (uint64_t)dread(SINKF);
            double ns   = (double)cyc / N / DEV_GHZ;
            double mtps = (ns > 0) ? 1000.0 / ns : 0;
            if (mode == 0) base_ns = ns;

            printf("%-22s %-12s %10ld %10.2f %10.1f  %s\n",
                   mode_name[mode], (mode == 0 ? "-" : pat_name[pat]),
                   cyc, ns, mtps, (sink == expect) ? "OK" : "MISMATCH!");
            job->destroyMap(map);
        }
    }

    printf("\nbaseline = %.2f ns/task; dispatch overhead = (mode ns) - baseline.\n", base_ns);
    printf("checksum is order-dependent -> OK proves the device ran the CORRECT op per task.\n\n");
    return 0;
}

// ── Result-return bench (2026-10-08) ─────────────────────────────────────────
// Validates and prices device->host result return from a RUNNING kernel
// (ret_bench in mu_kernel/persistent_puller.cpp; layout must match). Every
// accepted result is checked word by word against host-computed values in a
// buffer poisoned before launch, with a per-run random salt so stale data from
// an earlier run can never pass (the 2026-10-04 "validated by luck" lesson).
//   streaming : all N tasks published before launch; a consumer thread accepts
//               result i per protocol, validates, and times host-side accept.
//   ping-pong : P=1; enqueue ONE task, wait for its result, repeat M times ->
//               round-trip handoff latency (enqueue + pull + publish + observe).
// early_bad  = accepted under the protocol but data wrong (protocol unsafe)
// final_bad  = still wrong after kernel completion (never written: harness bug)
enum { R_T = 0, R_STOP = 8, R_TARGET = 16, R_MODE = 24, R_P = 32, R_SALT = 40,
       R_BUDGET = 48, R_STARTED = 56, R_PROG = 128, R_PMAX = 3072 };
static const long R_TASKB = R_PROG + 8L * R_PMAX;
static const long R_POISON = 0x5A5A5A5A5A5A5A5AL;

static inline unsigned long rmix(unsigned long x)
{
    x ^= x >> 33; x *= 0xff51afd7ed558ccdUL; x ^= x >> 33;
    x *= 0xc4ceb9fe1a85ec53UL; x ^= x >> 33;
    return x;
}

static std::vector<std::atomic<uint8_t>>* g_got = nullptr;   // mode 2 doorbells

static int run_ret(pxl::runtime::Context* ctx, pxl::runtime::Job* job,
                   pxl::kernel::Module* module, long N, int mode, long P, long M)
{
    const char* mname[3] = { "counter", "tag", "msg" };
    const bool pp = M > 0;
    if (pp) { P = 1; N = M; }
    if (P < 1 || P > R_PMAX) { fprintf(stderr, "P out of range\n"); return 1; }

    auto* func = module->createFunction("ret_bench");
    if (!func) { fprintf(stderr, "createFunction(ret_bench) failed\n"); return 1; }

    const long resb  = R_TASKB + 8L * N;
    const long slots = resb + 8L * N;
    q = reinterpret_cast<long*>(ctx->memAlloc(sizeof(long) * slots));
    if (!q) { fprintf(stderr, "memAlloc(%ld) failed\n", slots); return 1; }

    const unsigned long salt =
        rmix((unsigned long)std::chrono::steady_clock::now().time_since_epoch().count()) | (1UL << 62);
    auto pay = [&](long i) { return rmix(salt ^ (unsigned long)i); };

    for (long k = 0; k < resb; k++) q[k] = 0;                            // control + tasks
    for (long k = resb; k < slots; k++) q[k] = R_POISON;                 // poisoned results
    if (!pp) for (long i = 0; i < N; i++) q[R_TASKB + 8L * i] = (long)pay(i);
    q[R_TARGET] = N; q[R_MODE] = mode; q[R_P] = P; q[R_SALT] = (long)salt;
    q[R_BUDGET] = 30L * 1000000000L;                                     // ~30 s cap
    q[R_T] = pp ? 0 : N;
    pxl::flushHostCache(q, sizeof(long) * slots);

    std::vector<std::atomic<uint8_t>> got(mode == 2 ? N : 0);
    for (auto& g : got) g.store(0);
    g_got = &got;

    auto* map = job->buildMap(func, (uint32_t)P);
    if (!map) { fprintf(stderr, "buildMap(ret_bench) failed\n"); return 1; }
    if (mode == 2)
        map->setMessageCallback([](void* m, void*) {
            long i = 0; std::memcpy(&i, m, sizeof(long));
            if (g_got && i >= 0 && (size_t)i < g_got->size()) (*g_got)[i].store(1);
        }, nullptr);

    long early_bad = 0, accepted = 0, seen_poison = 0;
    long first_bad[4] = {-1, -1, -1, -1};
    bool timeout = false;
    std::vector<double> lat;  lat.reserve(pp ? M : 0);
    double enq_ns_sum = 0, wall_ns = 0;
    std::vector<long> prog(P, 0);

    // accept result i under the protocol; return false on timeout
    auto accept = [&](long i) -> bool {
        const long r = resb + 8L * i;
        const long tag = (long)(salt + (unsigned long)i + 1);
        double ts = now_ms();
        for (unsigned spin = 0;; spin++)
        {
            if (mode == 0) {
                long lane = i % P, need = i / P + 1;
                if (prog[lane] >= need) break;
                pxl::flushHostCache(&q[R_PROG + 8L * lane], 64);
                prog[lane] = *(volatile long*)&q[R_PROG + 8L * lane];
                if (prog[lane] >= need) break;
            } else if (mode == 1) {
                pxl::flushHostCache(&q[r], 64);
                if (*(volatile long*)&q[r + 7] == tag) return true;   // read data from this fill
            } else {
                if (got[i].load(std::memory_order_acquire)) break;
            }
            if ((spin & 1023) == 0 && now_ms() - ts > 5000.0) return false;
        }
        pxl::flushHostCache(&q[r], 64);                              // modes 0/2: fetch data now
        return true;
    };
    auto validate = [&](long i) {
        const long r = resb + 8L * i;
        const unsigned long p = pay(i);
        bool ok = true, poison = false;
        for (int j = 0; j < 7; j++) {
            long v = *(volatile long*)&q[r + j];
            if (v == R_POISON) poison = true;
            if (v != (long)rmix(p + (unsigned long)j)) ok = false;
        }
        long t = *(volatile long*)&q[r + 7];
        if (t == R_POISON) poison = true;
        if (t != (long)(salt + (unsigned long)i + 1)) ok = false;
        if (!ok) { if (early_bad < 4) first_bad[early_bad] = i; early_bad++; if (poison) seen_poison++; }
    };

    pxl::PxlResult er = map->execute(q);
    if (er != pxl::PxlResult::Success)
    { fprintf(stderr, "[ret] execute failed (result=%d)\n", (int)er); return 2; }

    std::thread consumer([&]() {
        double t0 = now_ms();
        for (long i = 0; i < N; i++)
        {
            double a = 0;
            if (pp) {
                a = now_ms();
                q[R_TASKB + 8L * i] = (long)pay(i);
                pxl::flushHostCache(&q[R_TASKB + 8L * i], 64);
                hwrite(R_T, i + 1);
                enq_ns_sum += (now_ms() - a) * 1e6;
            }
            if (!accept(i)) { timeout = true; break; }
            validate(i);
            accepted++;
            if (pp) lat.push_back((now_ms() - a) * 1e6);
        }
        wall_ns = (now_ms() - t0) * 1e6;
        hwrite(R_STOP, 1);
    });
    map->synchronize();
    consumer.join();

    // after completion: every result must now be right, or the harness is broken
    pxl::flushHostCache(&q[resb], sizeof(long) * 8L * N);
    long final_bad = 0;
    for (long i = 0; i < accepted; i++) {
        const long r = resb + 8L * i;
        const unsigned long p = pay(i);
        for (int j = 0; j < 7; j++) if (q[r + j] != (long)rmix(p + (unsigned long)j)) { final_bad++; break; }
    }

    if (pp) {
        std::vector<double> s = lat; std::sort(s.begin(), s.end());
        auto pct = [&](double f) { return s.empty() ? 0.0 : s[(size_t)(f * (s.size() - 1))]; };
        double mean = 0; for (double v : s) mean += v; mean = s.empty() ? 0 : mean / s.size();
        printf("PP mode=%s M=%ld accepted=%ld early_bad=%ld poison=%ld final_bad=%ld timeout=%d "
               "rt_p50_ns=%.0f rt_p90_ns=%.0f rt_p99_ns=%.0f rt_mean_ns=%.0f enq_mean_ns=%.0f\n",
               mname[mode], M, accepted, early_bad, seen_poison, final_bad, (int)timeout,
               pct(0.5), pct(0.9), pct(0.99), mean, accepted ? enq_ns_sum / accepted : 0.0);
    } else {
        printf("RET mode=%s P=%ld N=%ld accepted=%ld early_bad=%ld poison=%ld final_bad=%ld timeout=%d "
               "host_ns_per_result=%.1f\n",
               mname[mode], P, N, accepted, early_bad, seen_poison, final_bad, (int)timeout,
               accepted ? wall_ns / accepted : 0.0);
    }
    if (early_bad) printf("  first bad indices: %ld %ld %ld %ld\n",
                          first_bad[0], first_bad[1], first_bad[2], first_bad[3]);
    g_got = nullptr;
    job->destroyMap(map);
    return (timeout || final_bad) ? 3 : 0;
}

// ── Burst publish bench (2026-10-08) ─────────────────────────────────────────
// Device side: burst_bench in mu_kernel/persistent_puller.cpp (layout must match).
// Reproduces the tpch_scan_filter 2026-10-04 failure shape (many lanes flushing
// multi-line chunks at once, readers chasing the counters) and tests fixes. The
// reader mirrors tpch_scan_filter's return_reader: invalidate the counter line,
// load, mfence, invalidate the new range, mfence, read. Every line is checked
// against host-computed values in a poisoned buffer with a per-run salt.
//   early_bad = line accepted by the protocol but wrong  (protocol unsafe)
//   final_bad = line still wrong after the kernel exited (harness bug)
enum { B_P = 0, B_C = 8, B_L = 16, B_MODE = 24, B_SALT = 32, B_PROG = 128 };
static const long B_DATA = B_PROG + 8L * 3072;
static const unsigned long B_TAGK = 0x7A6B5C4D3E2F1A0BUL;

static int run_burst(pxl::runtime::Context* ctx, pxl::runtime::Job* job,
                     pxl::kernel::Module* module, long P, long C, long L, int mode, int R)
{
    const char* mname[5] = { "counter", "linetag", "checksum", "atomic", "flushall" };
    if (P < 1 || P > 3072 || C < 1 || L < 1 || R < 1) { fprintf(stderr, "bad burst params\n"); return 1; }
    auto* func = module->createFunction("burst_bench");
    if (!func) { fprintf(stderr, "createFunction(burst_bench) failed\n"); return 1; }

    const long slots = B_DATA + P * C * L * 8;
    q = reinterpret_cast<long*>(ctx->memAlloc(sizeof(long) * slots));
    if (!q) { fprintf(stderr, "memAlloc(%.1f MB) failed\n", slots * 8.0 / 1e6); return 1; }
    const unsigned long salt =
        rmix((unsigned long)std::chrono::steady_clock::now().time_since_epoch().count()) | (1UL << 62);
    auto key = [](long t, long c, long l) {
        return ((unsigned long)t << 40) | ((unsigned long)c << 20) | (unsigned long)l; };
    auto line = [&](long t, long c, long l) { return &q[B_DATA + ((t * C + c) * L + l) * 8]; };

    for (long k = 0; k < B_DATA; k++) q[k] = 0;
    for (long k = B_DATA; k < slots; k++) q[k] = R_POISON;
    q[B_P] = P; q[B_C] = C; q[B_L] = L; q[B_MODE] = mode; q[B_SALT] = (long)salt;
    pxl::flushHostCache(q, sizeof(long) * slots);
    _mm_mfence();   // clflushopt is weakly ordered: the poison write-back must land before launch

    auto* map = job->buildMap(func, (uint32_t)P);
    if (!map) { fprintf(stderr, "buildMap(burst_bench) failed\n"); return 1; }

    std::atomic<bool> fin{false};
    std::atomic<long> checked{0}, early_bad{0}, poison{0}, retries{0}, stalls{0};
    std::atomic<long> first_bad{-1};
    std::vector<long> done(P, 0);
    std::vector<unsigned long> cumdone(P, 0);

    // true if line (t,c,l) holds exactly what the device should have written
    auto good = [&](long t, long c, long l, bool& is_poison) {
        const volatile long* ln = line(t, c, l);
        const unsigned long k = key(t, c, l), h = rmix(salt ^ k);
        bool ok = true;
        for (int w = 0; w < 7; w++) { long v = ln[w]; if (v == R_POISON) is_poison = true;
                                      if (v != (long)(h + (unsigned long)w)) ok = false; }
        long tg = ln[7]; if (tg == R_POISON) is_poison = true;
        if (tg != (long)((salt ^ k) ^ B_TAGK)) ok = false;
        return ok;
    };

    auto reader = [&](int rid) {
        for (;;)
        {
            const bool last = fin.load(std::memory_order_acquire);
            for (long t = rid; t < P; t += R)
            {
                volatile long* pl = &q[B_PROG + 8L * t];
                pxl::flushHostCache((void*)pl, 64);
                _mm_mfence();
                const long k = pl[0];
                const unsigned long cum = (unsigned long)pl[1];
                _mm_mfence();
                if (k <= done[t]) continue;
                const long c0 = done[t], nl = (k - c0) * L;
                long* src = line(t, c0, 0);
                double ts = now_ms();
                if (mode == 2) {                        // re-read until the checksum matches
                    for (;;) {
                        pxl::flushHostCache(src, (size_t)nl * 64);
                        _mm_mfence();
                        unsigned long sum = 0;
                        for (long i = 0; i < nl; i++) sum += (unsigned long)((volatile long*)src)[8 * i];
                        if (cumdone[t] + sum == cum) break;
                        retries++;
                        if (now_ms() - ts > 5000.0) { stalls++; break; }
                    }
                } else {
                    pxl::flushHostCache(src, (size_t)nl * 64);
                    _mm_mfence();
                    if (mode == 1)                      // re-read only lines whose tag is missing
                        for (long c = c0; c < k; c++)
                            for (long l = 0; l < L; l++) {
                                volatile long* ln = line(t, c, l);
                                const long tg = (long)((salt ^ key(t, c, l)) ^ B_TAGK);
                                while (ln[7] != tg) {
                                    retries++;
                                    pxl::flushHostCache((void*)ln, 64);
                                    _mm_mfence();
                                    if (now_ms() - ts > 5000.0) { stalls++; break; }
                                }
                            }
                }
                for (long c = c0; c < k; c++)
                    for (long l = 0; l < L; l++) {
                        bool ps = false;
                        if (!good(t, c, l, ps)) {
                            long exp = -1; first_bad.compare_exchange_strong(exp, (t << 32) | (c << 16) | l);
                            early_bad++; if (ps) poison++;
                        }
                        cumdone[t] += (unsigned long)line(t, c, l)[0];
                    }
                checked += nl;
                done[t] = k;
            }
            if (last) return;
        }
    };

    double t0 = now_ms();
    if (map->execute(q) != pxl::PxlResult::Success) { fprintf(stderr, "[burst] execute failed\n"); return 2; }
    std::vector<std::thread> th;
    for (int r = 0; r < R; r++) th.emplace_back(reader, r);
    const bool sok = map->synchronize() == pxl::PxlResult::Success;
    const double kern_ms = now_ms() - t0;
    fin.store(true, std::memory_order_release);
    for (auto& x : th) x.join();
    const double e2e_ms = now_ms() - t0;

    // After completion, re-check everything at increasing delays: data that is merely
    // LATE converges to 0 bad; data that is LOST (clobbered or never written) does not.
    long final_bad = 0, missing = 0;
    for (long t = 0; t < P; t++) if (done[t] != C) {
        if (missing < 4) printf("  missing lane %ld: reader saw %ld/%ld chunks\n", t, done[t], C);
        missing++;
    }
    const int delays_ms[4] = { 0, 10, 100, 1000 };
    long bad_at[4] = {0, 0, 0, 0}, prog_bad_at[4] = {0, 0, 0, 0};
    for (int d = 0; d < 4; d++) {
        if (d) std::this_thread::sleep_for(std::chrono::milliseconds(delays_ms[d] - delays_ms[d - 1]));
        pxl::flushHostCache(q, sizeof(long) * slots);
        _mm_mfence();
        long bad = 0, pbad = 0, shown = 0;
        for (long t = 0; t < P; t++) {
            if (mode != 3 && q[B_PROG + 8L * t] != C) pbad++;
            for (long c = 0; c < C; c++) for (long l = 0; l < L; l++) {
                bool ps = false;
                if (!good(t, c, l, ps)) {
                    if (d == 0 && shown++ < 4) printf("  final bad: lane %ld chunk %ld line %ld %s\n", t, c, l, ps ? "(poison)" : "(other)");
                    bad++;
                }
            }
        }
        bad_at[d] = bad; prog_bad_at[d] = pbad;
    }
    printf("  final_bad at +0/+10/+100/+1000 ms: %ld %ld %ld %ld   counters != C: %ld %ld %ld %ld\n",
           bad_at[0], bad_at[1], bad_at[2], bad_at[3], prog_bad_at[0], prog_bad_at[1], prog_bad_at[2], prog_bad_at[3]);
    final_bad = bad_at[3];
    const long fb = first_bad.load();
    printf("BURST mode=%s P=%ld C=%ld L=%ld R=%d lines=%ld early_bad=%ld poison=%ld retries=%ld stalls=%ld "
           "final_bad=%ld missing_lanes=%ld kern_ms=%.1f e2e_ms=%.1f sync_ok=%d\n",
           mname[mode], P, C, L, R, checked.load(), early_bad.load(), poison.load(), retries.load(),
           stalls.load(), final_bad, missing, kern_ms, e2e_ms, (int)sok);
    if (fb >= 0) printf("  first bad: lane %ld chunk %ld line %ld\n", fb >> 32, (fb >> 16) & 0xFFFF, fb & 0xFFFF);
    job->destroyMap(map);
    return !sok ? 3 : 0;   // data findings are reported, not treated as a device failure
}

int main(int argc, char** argv)
{
    bool msgen = false;
    long bench_n = 0;   // >0 => run throughput bench with this many tasks
    long multi_n = 0;   // >0 => run heterogeneous-dispatch test with this many tasks
    int  nops    = 4;   // distinct task types for --multi
    long ret_n   = 0;   // >0 => result-return bench (streaming) with this many tasks
    long pp_m    = 0;   // >0 => result-return ping-pong with this many round trips
    int  rmode   = 1;   // 0 counter | 1 tag | 2 msg
    long rpar    = 1;   // worker lanes for --ret
    long bP = 0, bC = 8, bL = 512; int bmode = 0, bR = 16;   // --burst P [--chunks C --lines L --bmode m --readers R]
    for (int i = 1; i < argc; i++)
    {
        if (strcmp(argv[i], "--msg") == 0) msgen = true;
        else if (strcmp(argv[i], "--idle") == 0 && i + 1 < argc) g_idle_s = atoi(argv[++i]);
        else if (strcmp(argv[i], "--enqueue") == 0) g_enqueue = true;
        else if (strcmp(argv[i], "--bench") == 0) bench_n = (i + 1 < argc && argv[i+1][0] != '-') ? atol(argv[++i]) : 1000000;
        else if (strcmp(argv[i], "--multi") == 0) multi_n = (i + 1 < argc && argv[i+1][0] != '-') ? atol(argv[++i]) : 1000000;
        else if (strcmp(argv[i], "--nops")  == 0 && i + 1 < argc) nops = atoi(argv[++i]);
        else if (strcmp(argv[i], "--ret") == 0 && i + 1 < argc) ret_n = atol(argv[++i]);
        else if (strcmp(argv[i], "--burst") == 0 && i + 1 < argc) bP = atol(argv[++i]);
        else if (strcmp(argv[i], "--chunks") == 0 && i + 1 < argc) bC = atol(argv[++i]);
        else if (strcmp(argv[i], "--lines") == 0 && i + 1 < argc) bL = atol(argv[++i]);
        else if (strcmp(argv[i], "--readers") == 0 && i + 1 < argc) bR = atoi(argv[++i]);
        else if (strcmp(argv[i], "--bmode") == 0 && i + 1 < argc) {
            const char* m = argv[++i];
            bmode = !strcmp(m, "linetag") ? 1 : !strcmp(m, "checksum") ? 2 : !strcmp(m, "atomic") ? 3
                  : !strcmp(m, "flushall") ? 4 : 0;
        }
        else if (strcmp(argv[i], "--pingpong") == 0 && i + 1 < argc) pp_m = atol(argv[++i]);
        else if (strcmp(argv[i], "--par") == 0 && i + 1 < argc) rpar = atol(argv[++i]);
        else if (strcmp(argv[i], "--mode") == 0 && i + 1 < argc) {
            const char* m = argv[++i];
            rmode = !strcmp(m, "counter") ? 0 : !strcmp(m, "msg") ? 2 : 1;
        }
    }

    auto* ctx = pxl::runtime::createContext(0);
    if (!ctx) { fprintf(stderr, "createContext failed\n"); return 1; }
    auto* job = ctx->createJob(1);
    if (!job) { fprintf(stderr, "createJob failed\n"); return 1; }
    auto* module = pxl::kernel::createModule(KERNEL_FILE);
    if (!module) { fprintf(stderr, "createModule failed: %s\n", KERNEL_FILE); return 1; }
    if (job->load(module) != pxl::PxlResult::Success) { fprintf(stderr, "job->load failed\n"); return 1; }

    if (bP > 0)
    {
        int rc = run_burst(ctx, job, module, bP, bC, bL, bmode, bR);
        ctx->destroyJob(job);
        pxl::runtime::destroyContext(ctx);
        return rc;
    }

    if (ret_n > 0 || pp_m > 0)
    {
        int rc = run_ret(ctx, job, module, ret_n, rmode, rpar, pp_m);
        ctx->destroyJob(job);
        pxl::runtime::destroyContext(ctx);
        return rc;
    }

    if (multi_n > 0)
    {
        int rc = run_multi(ctx, job, module, multi_n, nops);
        ctx->destroyJob(job);
        pxl::runtime::destroyContext(ctx);
        return rc;
    }

    if (bench_n > 0)
    {
        int rc = run_bench(ctx, job, module, bench_n);
        ctx->destroyJob(job);
        pxl::runtime::destroyContext(ctx);
        return rc;
    }

    auto* func = module->createFunction(KERNEL_FUNC);
    if (!func) { fprintf(stderr, "createFunction failed: %s\n", KERNEL_FUNC); return 1; }

    q = reinterpret_cast<long*>(ctx->memAlloc(sizeof(long) * SLOTS));
    if (!q) { fprintf(stderr, "memAlloc failed\n"); return 1; }

    // Hard cap = cycle budget sized to the idle test + margin; MAXI=0 (unlimited iters,
    // rely on budget + STOP). budget in cycles @~1GHz => (idle + 30s) headroom.
    long budget_cyc = (long)(g_idle_s + 30) * 1000000000L;
    for (int i = 0; i < SLOTS; i++) vst(i, 0);
    vst(MAXI,   0);              // unlimited iterations; bounded by budget + STOP
    vst(BUDGET, budget_cyc);    // wall-clock safety cap (device can't hang sync)
    vst(MSGEN,  msgen ? 1 : 0);
    pxl::flushHostCache(q, sizeof(long) * SLOTS);
    printf("[main] idle=%ds  budget=%ld cyc (~%lds @1GHz)\n", g_idle_s, budget_cyc, budget_cyc/1000000000L);

    auto* map = job->buildMap(func, 1);  // taskCount=1: one resident poller
    if (!map) { fprintf(stderr, "buildMap failed\n"); return 1; }

    map->setMessageCallback([](void* message, void*) {
        long m = 0; std::memcpy(&m, message, sizeof(long));
        g_msg_last.store(m);
        g_msg_count.fetch_add(1);
    }, nullptr);

    printf("[main] launching non-blocking kernel...\n");

    double te0 = now_ms();
    pxl::PxlResult r = map->execute(q);
    printf("[main] execute() returned after %.1f ms (result=%d) -- %s\n",
           now_ms() - te0, (int)r,
           (now_ms() - te0 < 500.0) ? "NON-BLOCKING (returned immediately)"
                                    : "blocked until kernel exit");

    if (r != pxl::PxlResult::Success)
    {
        fprintf(stderr, "[main] execute() FAILED (result=%d) -- launch rejected, "
                        "not starting driver/synchronize (would hang). Aborting.\n", (int)r);
        job->destroyMap(map);
        ctx->destroyJob(job);
        pxl::runtime::destroyContext(ctx);
        return 2;
    }

    std::thread drv(driver, msgen);
    map->synchronize();
    printf("[main] synchronize() returned\n");
    drv.join();

    job->destroyMap(map);
    ctx->destroyJob(job);
    pxl::runtime::destroyContext(ctx);
    return 0;
}
