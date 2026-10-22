// Persistent-kernel feasibility spike (device side) — v5.
//
// Decisive finding from v1-v4: a NON-BLOCKING kernel (MU_NON_BLOCKING_KERNEL_INIT,
// __mu_kernel_type__=1) launched via map->execute() never starts — every coredump
// shows PC=0x0 on all threads, i.e. the device never entered the kernel. That held
// for both readGlobal and plain-pointer access, so the memory-access method was a
// red herring; the non-blocking LAUNCH path itself does not run via execute().
//
// v5 pivots to a NORMAL blocking kernel (MU_KERNEL_ADD) — structurally identical to
// the benchmark kernels that do run — launched via execute()+synchronize(). It spins
// in a bounded loop while the host enqueues concurrently from another thread, which
// still answers:
//   Q2 visibility: does the running kernel see host enqueues (PROC tracks TAIL)?
//   Q1 residency(bounded): does it spin for seconds without a watchdog kill?
//
// The loop is bounded three ways so a blocking kernel can never hang synchronize():
// host STOP flag, a max-iteration cap, and a wall-clock cycle budget (getClockCycle,
// 32-bit wrap-safe so it is valid as long as the run stays < ~4.29 s at 1 GHz).

#include "mu/mu.hpp"

using mu::flushCache;
using mu::asyncMessage;
using mu::getClockCycle;

// 64B-separated slots (own cache line each) except BUDGET which shares MSGEN's line
// (both are host-written, device-read-once — no false-sharing hazard).
// TARGET..DONE are for the throughput bench (drain_bench); TASKS is the base of a
// flat N-entry task array. These alias the persistent_puller ring region but the two
// kernels never run on the same buffer, so it's harmless.
enum { H = 0, T = 8, STOP = 16, PROC = 24, HB = 32, MAXI = 40, MSGEN = 48, BUDGET = 49, STARTED = 56, RING = 64,
       TARGET = 64, HBATCH = 72, CYCLES = 80, SINKF = 88, DONE = 96, MODE = 104, TASKS = 128 };
static const long CAP = 1024;

static inline long ld(long* q, int i) { return *(volatile long*)&q[i]; }
static inline void st(long* q, int i, long v)
{
    *(volatile long*)&q[i] = v;
    flushCache(&q[i], sizeof(long));
}

void persistent_puller(long* q)
{
    if (mu::getTaskIdx() != 0) return;

    long maxiter = ld(q, MAXI);
    long msgen   = ld(q, MSGEN);
    long budget  = ld(q, BUDGET);   // cycle budget (0 = unlimited)

    st(q, STARTED, 1);

    long head = 0, iter = 0;

    // Wall-clock bound that survives the 32-bit clock wrap (2^32 cyc ~ 4.29s @1GHz).
    // We poll far faster than one wrap period, so each per-iteration delta
    // (uint32_t)(now-last) is small and correct across arbitrarily many wraps;
    // accumulate into a 64-bit total. budget is a 64-bit cycle count (0 = unlimited).
    uint32_t last = (uint32_t)getClockCycle();
    uint64_t elapsed_cyc = 0;

    for (;;)
    {
        iter++;
        st(q, HB, iter);

        flushCache(&q[T], sizeof(long));
        long tail = ld(q, T);

        while (head < tail)
        {
            long slot = head % CAP;
            flushCache(&q[RING + slot], sizeof(long));
            long payload = ld(q, RING + slot);
            st(q, RING + slot, payload + 1);   // echo proof
            head++;
            st(q, H, head);
            st(q, PROC, head);
            if (msgen) { long m = head; asyncMessage(&m, sizeof(m)); }
        }

        flushCache(&q[STOP], sizeof(long));
        if (ld(q, STOP) != 0 && head >= tail) break;
        if (maxiter > 0 && iter >= maxiter) break;

        uint32_t nowc = (uint32_t)getClockCycle();
        elapsed_cyc += (uint32_t)(nowc - last);   // wrap-safe per-iteration delta
        last = nowc;
        if (budget > 0 && elapsed_cyc > (uint64_t)budget) break;
    }

    mu::flushCacheAll();
}

MU_KERNEL_ADD(persistent_puller);   // v5: NORMAL blocking kernel


// ── Throughput bench ──────────────────────────────────────────────────────────
// Drains TARGET task descriptors from a flat array at TASKS (host pre-writes them
// at unique sequential addresses, so each is read fresh from DRAM — the realistic
// case for a task queue, no per-item invalidate). The device times ITSELF with
// getClockCycle (device = 1 GHz => cycles = ns), so we get the pure per-task pull
// cost independent of host production. HEAD/PROC are published every HBATCH tasks
// (amortized progress signal, as a real scheduler would). SINKF is a checksum
// (sum of payloads) proving every task was actually read, not skipped.
void drain_bench(long* q)
{
    if (mu::getTaskIdx() != 0) return;

    long target = ld(q, TARGET);
    long hbatch = ld(q, HBATCH); if (hbatch <= 0) hbatch = 4096;

    st(q, STARTED, 1);

    // Wait for the host to publish work (T = target), reading it fresh.
    for (;;) { flushCache(&q[T], sizeof(long)); if (ld(q, T) > 0) break; }

    long head = 0, sink = 0;
    uint32_t last = (uint32_t)getClockCycle();
    uint64_t acc = 0;

    while (head < target)
    {
        sink += ld(q, TASKS + head);   // dequeue one descriptor (fresh, sequential)
        head++;
        if ((head % hbatch) == 0)
        {
            st(q, H, head);            // publish progress (amortized over hbatch)
            st(q, PROC, head);
            uint32_t n = (uint32_t)getClockCycle();
            acc += (uint32_t)(n - last);   // wrap-safe accumulate
            last = n;
        }
    }
    uint32_t n = (uint32_t)getClockCycle();
    acc += (uint32_t)(n - last);

    st(q, SINKF, sink);
    st(q, CYCLES, (long)acc);
    st(q, H, target);
    st(q, PROC, target);
    st(q, DONE, 1);
    mu::flushCacheAll();
}

MU_KERNEL_ADD(drain_bench);


// ── Heterogeneous task dispatch ───────────────────────────────────────────────
// Feasibility test for a resident scheduler running DIFFERENT task types per
// item. Device code is compiled ahead of time and cannot load new code, so
// "different tasks" = the kernel carries a dispatch table and each descriptor
// carries an opcode:  desc = (value << 8) | opcode.
//
// MODE selects how the op is dispatched, so we can price each mechanism:
//   0  baseline  — no dispatch at all (sink += desc); the ~24 ns/task reference
//   1  switch    — switch on opcode, calling a noinline handler
//   2  fptr      — indirect call through a function-pointer table (mxtasking-style)
//
// The fn table is built at RUNTIME in locals rather than as a statically
// initialised global, to avoid needing pointer relocations in the mubin image.
// SINKF is an order-dependent checksum: it only matches if the device ran the
// CORRECT op for every task, so it validates dispatch, not just the read.

static unsigned long __attribute__((noinline)) op_add(unsigned long s, unsigned long v) { return s + v; }
static unsigned long __attribute__((noinline)) op_xor(unsigned long s, unsigned long v) { return s ^ v; }
static unsigned long __attribute__((noinline)) op_max(unsigned long s, unsigned long v) { return s > v ? s : v; }
static unsigned long __attribute__((noinline)) op_sub(unsigned long s, unsigned long v) { return s - v; }

void multi_bench(long* q)
{
    if (mu::getTaskIdx() != 0) return;

    long target = ld(q, TARGET);
    long hbatch = ld(q, HBATCH); if (hbatch <= 0) hbatch = 4096;
    long mode   = ld(q, MODE);

    unsigned long (*fns[4])(unsigned long, unsigned long);
    fns[0] = op_add; fns[1] = op_xor; fns[2] = op_max; fns[3] = op_sub;

    st(q, STARTED, 1);
    for (;;) { flushCache(&q[T], sizeof(long)); if (ld(q, T) > 0) break; }

    unsigned long sink = 0;
    long head = 0;
    uint32_t last = (uint32_t)getClockCycle();
    uint64_t acc = 0;

    // Mode is hoisted out of the loop so the measurement isn't polluted by a
    // per-task mode branch; each loop body differs only in the dispatch step.
    if (mode == 0)
    {
        while (head < target)
        {
            sink += (unsigned long)ld(q, TASKS + head);
            head++;
            if ((head % hbatch) == 0) { st(q, H, head); st(q, PROC, head);
                uint32_t n = (uint32_t)getClockCycle(); acc += (uint32_t)(n - last); last = n; }
        }
    }
    else if (mode == 1)
    {
        while (head < target)
        {
            unsigned long d = (unsigned long)ld(q, TASKS + head);
            unsigned long op = d & 0xFF, v = d >> 8;
            switch (op)
            {
                case 0:  sink = op_add(sink, v); break;
                case 1:  sink = op_xor(sink, v); break;
                case 2:  sink = op_max(sink, v); break;
                default: sink = op_sub(sink, v); break;
            }
            head++;
            if ((head % hbatch) == 0) { st(q, H, head); st(q, PROC, head);
                uint32_t n = (uint32_t)getClockCycle(); acc += (uint32_t)(n - last); last = n; }
        }
    }
    else
    {
        while (head < target)
        {
            unsigned long d = (unsigned long)ld(q, TASKS + head);
            unsigned long op = d & 0xFF, v = d >> 8;
            sink = fns[op & 3](sink, v);
            head++;
            if ((head % hbatch) == 0) { st(q, H, head); st(q, PROC, head);
                uint32_t n = (uint32_t)getClockCycle(); acc += (uint32_t)(n - last); last = n; }
        }
    }

    uint32_t n = (uint32_t)getClockCycle();
    acc += (uint32_t)(n - last);

    st(q, SINKF, (long)sink);
    st(q, CYCLES, (long)acc);
    st(q, H, target);
    st(q, PROC, target);
    st(q, DONE, 1);
    mu::flushCacheAll();
}

MU_KERNEL_ADD(multi_bench);


// ── Result-return bench ───────────────────────────────────────────────────────
// Can a resident worker hand RESULTS back to the host while it keeps running?
// 2026-10-04 (tpch_scan_filter scan_filter_pub): a progress counter flushed after
// the data it covers can reach memory FIRST -- flushCache is posted and
// `fence rw,rw` does not order its write-back. This bench validates and prices
// three publish protocols against a poisoned result buffer:
//   mode 0 counter : flush data line, fence, per-lane progress counter + flush
//                    (the known-unsafe pattern -> negative control)
//   mode 1 tag     : 7 data words + seq tag in ONE 64 B line, one flush; the host
//                    polls that line's own tag (safe iff a line writes back whole)
//   mode 2 msg     : flush data line, fence, asyncMessage(index) doorbell
// P lanes (taskIdx < P) serve tasks i = lane, lane+P, ... ; P>1 reproduces the
// concurrent write-back bursts that exposed the race. Tasks and results are one
// 64 B line each. Bounded by TARGET, host STOP and a cycle BUDGET so a blocking
// kernel can never hang synchronize(). Layout must match main_spike.cpp.
enum { R_T = 0, R_STOP = 8, R_TARGET = 16, R_MODE = 24, R_P = 32, R_SALT = 40,
       R_BUDGET = 48, R_STARTED = 56, R_PROG = 128, R_PMAX = 3072 };
static const long R_TASKB = R_PROG + 8L * R_PMAX;   // task i   at R_TASKB + 8*i
                                                    // result i at R_TASKB + 8*target + 8*i

static inline unsigned long rmix(unsigned long x)
{
    x ^= x >> 33; x *= 0xff51afd7ed558ccdUL; x ^= x >> 33;
    x *= 0xc4ceb9fe1a85ec53UL; x ^= x >> 33;
    return x;
}

static inline long rld(long* q, long i) { flushCache(&q[i], 64); return *(volatile long*)&q[i]; }

void ret_bench(long* q)
{
    const long lane = mu::getTaskIdx();
    const long P = rld(q, R_P);
    if (lane < 0 || lane >= P) return;

    const long target = rld(q, R_TARGET);
    const long mode   = rld(q, R_MODE);
    const unsigned long salt = (unsigned long)rld(q, R_SALT);
    const uint64_t budget = (uint64_t)rld(q, R_BUDGET);
    const long resb = R_TASKB + 8L * target;
    if (lane == 0) st(q, R_STARTED, 1);

    long i = lane, done = 0, tail = 0;
    uint32_t last = (uint32_t)getClockCycle();
    uint64_t el = 0;

    while (i < target)
    {
        uint32_t n = (uint32_t)getClockCycle();
        el += (uint32_t)(n - last); last = n;
        if (budget > 0 && el > budget) break;

        if (tail <= i)                               // task i not published yet
        {
            tail = rld(q, R_T);
            if (tail <= i) { if (rld(q, R_STOP)) break; continue; }
        }

        unsigned long pay = (unsigned long)rld(q, R_TASKB + 8L * i);   // fresh line
        volatile long* r = (volatile long*)&q[resb + 8L * i];
        for (int j = 0; j < 7; j++) r[j] = (long)rmix(pay + (unsigned long)j);
        r[7] = (long)(salt + (unsigned long)i + 1);                   // seq tag
        flushCache((const void*)r, 64);

        if (mode == 0)
        {
            asm volatile("fence rw, rw" ::: "memory");
            done++;
            *(volatile long*)&q[R_PROG + 8L * lane] = done;
            flushCache(&q[R_PROG + 8L * lane], 64);
        }
        else if (mode == 2)
        {
            asm volatile("fence rw, rw" ::: "memory");
            long m = i;
            asyncMessage(&m, sizeof(m));
        }
        i += P;
    }
    mu::flushCacheAll();
}

MU_KERNEL_ADD(ret_bench);


// ── Burst publish bench (2026-10-08) ──────────────────────────────────────────
// Reproduces the 2026-10-04 failure shape: P lanes each write C chunks of L lines
// and publish each chunk with ONE range flush, many lanes finishing together, while
// host readers chase the counters. Publish protocols:
//   0 counter   : range flush, fence, counter (+cumulative checksum) in one line   [control]
//   1 linetag   : as 0, every line's word 7 = per-(lane,chunk,line) tag; host
//                 re-reads only lines whose tag has not arrived
//   2 checksum  : as 0; host re-reads the range until the line-hash sum matches
//                 the cumulative checksum published with the counter
//   3 atomic    : range flush, fence, counter via atomicSwap58 (does an atomic
//                 order behind the posted write-backs?)
//   4 flushall  : range flush, flushCacheAll, fence, counter
// Layout must match main_spike.cpp (B_*).
enum { B_P = 0, B_C = 8, B_L = 16, B_MODE = 24, B_SALT = 32, B_PROG = 128 };
static const long B_DATA = B_PROG + 8L * 3072;
static const unsigned long B_TAGK = 0x7A6B5C4D3E2F1A0BUL;

void burst_bench(long* q)
{
    const long t = mu::getTaskIdx();
    const long P = rld(q, B_P);
    if (t < 0 || t >= P) return;
    const long C = rld(q, B_C), L = rld(q, B_L), mode = rld(q, B_MODE);
    const unsigned long salt = (unsigned long)rld(q, B_SALT);
    volatile long* prog = (volatile long*)&q[B_PROG + 8L * t];
    unsigned long cum = 0;

    for (long c = 0; c < C; c++)
    {
        long* base = &q[B_DATA + (t * C + c) * L * 8];
        for (long l = 0; l < L; l++)
        {
            const unsigned long key = ((unsigned long)t << 40) | ((unsigned long)c << 20) | (unsigned long)l;
            const unsigned long h = rmix(salt ^ key);
            volatile long* ln = (volatile long*)(base + 8 * l);
            for (int w = 0; w < 7; w++) ln[w] = (long)(h + (unsigned long)w);
            ln[7] = (long)((salt ^ key) ^ B_TAGK);
            cum += h;
        }
        flushCache(base, (uint64_t)L * 64);
        if (mode == 4) mu::flushCacheAll();
        asm volatile("fence rw, rw" ::: "memory");
        if (mode == 3)
            mu::atomicSwap58((int64_t*)&prog[0], (int64_t)(c + 1));
        else
        {
            prog[1] = (long)cum;          // same line as the count: one write-back
            prog[0] = c + 1;
            flushCache((const void*)prog, 64);
        }
    }
    mu::flushCacheAll();
}

MU_KERNEL_ADD(burst_bench);
