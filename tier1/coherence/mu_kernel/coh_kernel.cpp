// MU kernels for the coherence-tax suite (host driver: ../main.cpp, layout: ../coh_common.hpp).
//
// Conventions:
//   - Every launch gets a FRESH config line that the device has never read, so a stale device
//     copy of the configuration is impossible; the kernel still flushes it before reading.
//   - Per-task results go to res[t * 8 ...], one 64 B line per task, flushed by the task so the
//     host can read them after synchronize() + flushHostCache (no DMA anywhere).
//   - mu::flushCache writes a line back and drops it from the MU caches (flush-and-invalidate;
//     ../resident_worker/RESULTS.md "Coherence model"); k6/k8 test exactly that.
#include <mu/mu.hpp>
#include <stdint.h>

#include "coh_common.hpp"

using namespace coh;

namespace
{

inline const Cfg* load_cfg(const Cfg* cfg)
{
    mu::flushCache(cfg, sizeof(Cfg));
    return cfg;
}

inline void slice(uint64_t n, uint64_t par, uint64_t t, uint64_t& lo, uint64_t& hi)
{
    const uint64_t q = n / par, r = n % par;
    lo = t * q + (t < r ? t : r);
    hi = lo + q + (t < r ? 1 : 0);
}

inline void put(uint64_t* res, uint64_t t, uint64_t k, uint64_t v) { res[t * kStride + k] = v; }
inline void publish_res(uint64_t* res, uint64_t t) { mu::flushCache(&res[t * kStride], 64); }

}  // namespace

// k_fill: each task writes pat(i, salt) over its slice of buf.
//   mode: M_ENTRY_INVAL, M_EXIT_FLUSH (flushCache own range), M_EXIT_ALL (flushCacheAll).
//   res: [0] write cycles, [1] exit-flush cycles, [2] entry-invalidate cycles
void k_fill(uint64_t* buf, const Cfg* cfg_in, uint64_t* res)
{
    const Cfg* cfg = load_cfg(cfg_in);
    const uint64_t c_inv0 = mu::getClockCycle();
    if (cfg->mode & M_ENTRY_INVAL) mu::flushCacheAll();
    const uint64_t c_inv1 = mu::getClockCycle();
    const int ti = (int)mu::getTaskIdx();
    if (ti < 0 || (uint64_t)ti >= cfg->par) return;
    const uint64_t t = (uint64_t)ti;
    uint64_t lo, hi;
    slice(cfg->words, cfg->par, t, lo, hi);
    const uint64_t salt = cfg->salt;

    const uint64_t c0 = mu::getClockCycle();
    for (uint64_t i = lo; i < hi; ++i) buf[i] = pat(i, salt);
    const uint64_t c1 = mu::getClockCycle();
    if ((cfg->mode & M_EXIT_FLUSH) && hi > lo) mu::flushCache(&buf[lo], (hi - lo) * 8);
    if (cfg->mode & M_EXIT_ALL) mu::flushCacheAll();
    const uint64_t c2 = mu::getClockCycle();

    put(res, t, 0, c1 - c0);
    put(res, t, 1, c2 - c1);
    put(res, t, 2, c_inv1 - c_inv0);
    publish_res(res, t);
}
MU_KERNEL_ADD(k_fill);

// k_check: each task compares its slice with pat(i, salt) and classifies wrong words.
//   mode: M_ENTRY_INVAL. cfg->a = the previous salt (a word equal to pat(i, a) is "stale").
//   res: [0] bad, [1] first bad index (~0 if none), [2] stale (old salt), [3] poison,
//        [4] entry-invalidate cycles, [5] read cycles, [6] checksum of good words
void k_check(const uint64_t* buf, const Cfg* cfg_in, uint64_t* res)
{
    const Cfg* cfg = load_cfg(cfg_in);
    const uint64_t c_inv0 = mu::getClockCycle();
    if (cfg->mode & M_ENTRY_INVAL) mu::flushCacheAll();
    const uint64_t c_inv1 = mu::getClockCycle();
    const int ti = (int)mu::getTaskIdx();
    if (ti < 0 || (uint64_t)ti >= cfg->par) return;
    const uint64_t t = (uint64_t)ti;
    uint64_t lo, hi;
    slice(cfg->words, cfg->par, t, lo, hi);

    uint64_t bad = 0, first = ~0ull, stale = 0, poison = 0, sum = 0;
    const uint64_t c0 = mu::getClockCycle();
    for (uint64_t i = lo; i < hi; ++i)
    {
        const uint64_t v = buf[i];
        if (v == pat(i, cfg->salt)) { sum += v; continue; }
        if (first == ~0ull) first = i;
        ++bad;
        if (v == pat(i, cfg->a)) ++stale;
        else if (v == kPoison) ++poison;
    }
    const uint64_t c1 = mu::getClockCycle();
    put(res, t, 0, bad);
    put(res, t, 1, first);
    put(res, t, 2, stale);
    put(res, t, 3, poison);
    put(res, t, 4, c_inv1 - c_inv0);
    put(res, t, 5, c1 - c0);
    put(res, t, 6, sum);
    publish_res(res, t);
}
MU_KERNEL_ADD(k_check);

// k_devflush: device-side flush cost. Each task owns cfg->a bytes (multiple of 64) at
// buf + t * a. Sequence: write slice -> flushCache (dirty) -> read slice -> flushCache (clean)
// -> write slice -> flushCacheAll (dirty, whole hierarchy of this thread).
//   res: [0] flush dirty, [1] flush clean, [2] flushCacheAll dirty, [3] write cycles, [4] sum
void k_devflush(uint64_t* buf, const Cfg* cfg_in, uint64_t* res)
{
    const Cfg* cfg = load_cfg(cfg_in);
    const int ti = (int)mu::getTaskIdx();
    if (ti < 0 || (uint64_t)ti >= cfg->par) return;
    const uint64_t t = (uint64_t)ti;
    const uint64_t w = cfg->a / 8;
    uint64_t* s = buf + t * w;
    const uint64_t salt = cfg->salt;

    const uint64_t c0 = mu::getClockCycle();
    for (uint64_t i = 0; i < w; ++i) s[i] = pat(t * w + i, salt);
    const uint64_t c1 = mu::getClockCycle();
    mu::flushCache(s, cfg->a);
    const uint64_t c2 = mu::getClockCycle();

    uint64_t sum = 0;
    for (uint64_t i = 0; i < w; ++i) sum += ((volatile uint64_t*)s)[i];
    const uint64_t c3 = mu::getClockCycle();
    mu::flushCache(s, cfg->a);
    const uint64_t c4 = mu::getClockCycle();

    for (uint64_t i = 0; i < w; ++i) s[i] = pat(t * w + i, salt ^ 1);
    const uint64_t c5 = mu::getClockCycle();
    mu::flushCacheAll();
    const uint64_t c6 = mu::getClockCycle();

    put(res, t, 0, c2 - c1);
    put(res, t, 1, c4 - c3);
    put(res, t, 2, c6 - c5);
    put(res, t, 3, c1 - c0);
    put(res, t, 4, sum);
    publish_res(res, t);
}
MU_KERNEL_ADD(k_devflush);

// k_publish: in-kernel publish race (k10). P = cfg->par lanes, C = cfg->a chunks, L = cfg->b
// lines per chunk, mode = PUB_*. After each chunk the lane publishes counter = chunk + 1.
// Host readers chase the counters while the kernel runs and verify every line they consume.
//   res: [0] write cycles, [1] publish cycles
void k_publish(uint64_t* buf, const Cfg* cfg_in, uint64_t* res)
{
    const Cfg* cfg = load_cfg(cfg_in);
    const int ti = (int)mu::getTaskIdx();
    if (ti < 0 || (uint64_t)ti >= cfg->par) return;
    const uint64_t t = (uint64_t)ti, P = cfg->par, C = cfg->a, L = cfg->b, mode = cfg->mode;
    const uint64_t salt = cfg->salt;
    volatile uint64_t* ctr = &buf[pub_counter(t)];
    uint64_t cw = 0, cp = 0;

    for (uint64_t c = 0; c < C; ++c)
    {
        const uint64_t base = pub_data(P, C, L, t, c);
        const uint64_t c0 = mu::getClockCycle();
        for (uint64_t i = 0; i < L * 8; ++i) buf[base + i] = pat(base + i, salt);
        const uint64_t c1 = mu::getClockCycle();
        if (mode != PUB_NOFLUSH) mu::flushCache(&buf[base], L * 64);
        if (mode == PUB_FLUSHALL || mode == PUB_FLUSHALL_NOFENCE) mu::flushCacheAll();
        if (mode == PUB_FLUSH_FENCE || mode == PUB_FLUSHALL) asm volatile("fence rw, rw" ::: "memory");
        *ctr = c + 1;
        mu::flushCache((const void*)ctr, 64);
        const uint64_t c2 = mu::getClockCycle();
        cw += c1 - c0;
        cp += c2 - c1;
    }
    mu::flushCacheAll();
    put(res, t, 0, cw);
    put(res, t, 1, cp);
    publish_res(res, t);
}
MU_KERNEL_ADD(k_publish);

// k_pingpong: one resident task answers cfg->a host messages. Host writes line PP_H2D
// (word 0 = sequence k, words 1..7 = pat(8k + j, salt)) and flushes it; the task polls that
// line (flushCache drops its copy, then reads), checks the payload, and answers on PP_D2H.
// Exits on PP_STOP or after cfg->b cycles, so a lost message cannot hang the device.
//   q[PP_DONE] = rounds answered, q[PP_DONE + 1] = payloads that arrived wrong
void k_pingpong(uint64_t* q, const Cfg* cfg_in)
{
    const Cfg* cfg = load_cfg(cfg_in);
    if (mu::getTaskIdx() != 0) return;
    const uint64_t n = cfg->a, budget = cfg->b, salt = cfg->salt;
    const uint64_t start = mu::getClockCycle();
    volatile uint64_t* h2d = &q[PP_H2D];
    volatile uint64_t* d2h = &q[PP_D2H];
    uint64_t done = 0, bad = 0, spins = 0;

    for (uint64_t k = 1; k <= n; ++k)
    {
        for (;;)
        {
            mu::flushCache((const void*)h2d, 64);
            if (h2d[0] == k) break;
            if ((++spins & 1023) == 0)
            {
                mu::flushCache(&q[PP_STOP], 64);
                if (((volatile uint64_t*)q)[PP_STOP] || mu::getClockCycle() - start > budget) goto out;
            }
        }
        for (uint64_t j = 1; j < 8; ++j)
            if (h2d[j] != pat(8 * k + j, salt)) ++bad;
        for (uint64_t j = 1; j < 8; ++j) d2h[j] = pat(8 * k + j, salt ^ 1);
        d2h[0] = k;
        mu::flushCache((const void*)d2h, 64);
        done = k;
    }
out:
    ((volatile uint64_t*)q)[PP_DONE] = done;
    ((volatile uint64_t*)q)[PP_DONE + 1] = bad;
    mu::flushCache(&q[PP_DONE], 64);
    mu::flushCacheAll();
}
MU_KERNEL_ADD(k_pingpong);
