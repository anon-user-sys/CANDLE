// MU kernel for the standalone TPC-H scan filter (see ../README.md).
//
// Coherence: the host writes the config with copyToDevice, which can leave a stale line in an
// MU cache, so the kernel flushes the config before reading it (observe) and flushes its own
// writes before returning (publish) - the protocol from tier1/resident_worker. The input
// columns are re-uploaded by the host every iteration, so stale column lines are dropped too.

#include <cstdint>

#include "mu/mu.hpp"
#include "scan_common.hpp"

// Output writer for write_mode 1: once the next slot is 64 B-aligned, keys are collected 8 at a
// time and stored as one whole line with memcpyGlobal, so the cache has no reason to fetch the
// line's old contents first (the suspected read-for-ownership that makes materialize ~1.6x slower
// than count). Slots before the first aligned one and the final partial line use plain stores.
struct LineOut {
    scanfilter::Key* dst;
    scanfilter::Key buf[8];
    unsigned k = 0;
    void put(scanfilter::Key v) {
        if (k == 0 && (reinterpret_cast<std::uintptr_t>(dst) & 63)) { *dst++ = v; return; }
        buf[k++] = v;
        if (k == 8) { mu::memcpyGlobal(dst, buf, 64); dst += 8; k = 0; }
    }
    void finish() { for (unsigned j = 0; j < k; ++j) dst[j] = buf[j]; dst += k; k = 0; }
};

// scan_slice with the output going through a LineOut.
static scanfilter::SliceResult scan_slice_lines(const std::uint8_t* qty, const scanfilter::Key* keys, LineOut& w,
                                                std::uint64_t lo, std::uint64_t hi, std::uint32_t threshold)
{
    scanfilter::SliceResult r{0, 0, 0};
    for (std::uint64_t i = lo; i < hi; ++i) {
        if (qty[i] > threshold) continue;
        const scanfilter::Key key = keys[i];
        w.put(key);
        ++r.n;
        r.sum_idx += i;
        r.sum_key += key;
    }
    return r;
}

// Software lookahead prefetch (the MU cores have no hardware prefetcher). Rows go in blocks of
// 64: one 64 B line of l_quantity and 512 B (8 lines) of l_orderkey, each fetched with a single
// mu::prefetch (one `aspc` instruction) `window` blocks ahead. The scan of each block is the same
// shared scan_slice, so window 0 is exactly the original loop. Same idea as the hash join's
// batched probe prefetch (HASHJOIN_PREFETCH, -38% probe time, 2026-09-15).
template <typename Out = scanfilter::Key>
static scanfilter::SliceResult scan_slice_prefetch(const std::uint8_t* qty, const scanfilter::Key* keys,
                                                   Out* dst, std::uint64_t lo, std::uint64_t hi,
                                                   std::uint32_t threshold, std::uint64_t window,
                                                   bool prefetch_keys, LineOut* w = nullptr,
                                                   scanfilter::Key base = 0)
{
    constexpr std::uint64_t kBlock = 64;                       // rows per l_quantity cache line
    const std::uint64_t ahead = window * kBlock;
    scanfilter::SliceResult r{0, 0, 0};
    for (std::uint64_t b = lo; b < hi; b += kBlock) {
        const std::uint64_t pf = (b + ahead) & ~(kBlock - 1);   // line-aligned (buffers are 1 GiB-aligned)
        if (pf < hi) {
            mu::prefetch(qty + pf, 64);
            if (prefetch_keys) mu::prefetch(keys + pf, kBlock * sizeof(scanfilter::Key));
        }
        const std::uint64_t e = b + kBlock < hi ? b + kBlock : hi;
        const scanfilter::SliceResult s = w ? scan_slice_lines(qty, keys, *w, b, e, threshold)
                                            : scanfilter::scan_slice_out<Out>(qty, keys, dst ? dst + r.n : nullptr, b, e, threshold, base);
        r.n += s.n; r.sum_idx += s.sum_idx; r.sum_key += s.sum_key;
    }
    return r;
}

// out4: the task's output slice holds uint32 offsets from base = keys[slice lo]; max_per_task is in
// uint32 elements then. Returns the scan of [a, b) appended at slot `cur` of task t's slice.
static scanfilter::SliceResult scan_out4(const std::uint8_t* qty, const scanfilter::Key* keys, void* out,
                                         const scanfilter::Config* cfg, std::uint64_t t, std::uint64_t lo,
                                         std::uint64_t a, std::uint64_t b, std::uint64_t cur)
{
    auto* dst = static_cast<std::uint32_t*>(out) + t * cfg->max_per_task + cur;
    const scanfilter::Key base = keys[lo];
    const std::uint32_t thr = static_cast<std::uint32_t>(cfg->threshold);
    return cfg->spare1 ? scan_slice_prefetch<std::uint32_t>(qty, keys, dst, a, b, thr, cfg->spare1, cfg->spare2 == 0, nullptr, base)
                       : scanfilter::scan_slice_out<std::uint32_t>(qty, keys, dst, a, b, thr, base);
}

// counts/sum_idx/sum_key[t] are 64-byte strided to avoid false sharing between tasks.
void scan_filter(const std::uint8_t* qty,
                 const scanfilter::Key* keys,
                 scanfilter::Key* out,
                 std::uint64_t* counts,
                 std::uint64_t* sum_idx,
                 std::uint64_t* sum_key,
                 scanfilter::Config* cfg)
{
    mu::flushCache(cfg, sizeof(scanfilter::Config));
    mu::flushCacheAll();
    const int ti = mu::getTaskIdx();
    if (ti < 0 || static_cast<std::uint64_t>(ti) >= cfg->par) return;
    const std::uint64_t t = static_cast<std::uint64_t>(ti);

    std::uint64_t lo, hi;
    scanfilter::slice(cfg->rows, cfg->par, t, lo, hi);
    scanfilter::Key* dst = (cfg->materialize && out) ? out + t * cfg->max_per_task : nullptr;

    // spare1 = prefetch window in 64-row blocks (0 = off), spare2 = 1 disables the key prefetch.
    const std::uint32_t thr = static_cast<std::uint32_t>(cfg->threshold);
    LineOut lw{dst};
    LineOut* w = (dst && cfg->write_mode == 1) ? &lw : nullptr;
    const scanfilter::SliceResult r = (dst && cfg->out4) ? scan_out4(qty, keys, out, cfg, t, lo, lo, hi, 0)
        : cfg->spare1
        ? scan_slice_prefetch(qty, keys, dst, lo, hi, thr, cfg->spare1, cfg->spare2 == 0, w)
        : w ? scan_slice_lines(qty, keys, *w, lo, hi, thr)
            : scanfilter::scan_slice(qty, keys, dst, lo, hi, thr);
    if (w) w->finish();

    // spare0 = 1: explicitly write back this task's output range before the global flush
    // (diagnostic for whether flushCacheAll alone publishes every line).
    if (dst && cfg->spare0 && r.n) mu::flushCache(dst, r.n * sizeof(scanfilter::Key));

    const std::uint64_t stride = 64 / sizeof(std::uint64_t);
    counts[t * stride] = r.n;
    sum_idx[t * stride] = r.sum_idx;
    sum_key[t * stride] = r.sum_key;
    mu::flushCacheAll();
}

MU_KERNEL_ADD(scan_filter);


// Launch-per-chunk variant (return to host). The host launches this `steps` times; launch
// `step` scans part `step` of every task's slice and appends to the task's output slice, so the
// final layout equals scan_filter's. progress[t] is the task's cursor (keys written so far),
// carried across launches in device memory; counts/sums accumulate the same way. Nothing is
// published while a launch runs: the host reads the cursors only after synchronize(), when
// the closing flushCacheAll has completed. (An earlier in-kernel publishing variant raced: the counter reached memory before the
// data, fence or not; it was removed.)
void scan_filter_step(const std::uint8_t* qty,
                      const scanfilter::Key* keys,
                      scanfilter::Key* out,
                      std::uint64_t* counts,
                      std::uint64_t* sum_idx,
                      std::uint64_t* sum_key,
                      std::uint64_t* progress,
                      scanfilter::Config* cfg,
                      std::uint64_t step_steps)   // (step << 32) | steps; kernels take at most 9 args
{
    const std::uint64_t step = step_steps >> 32, steps = step_steps & 0xffffffffu;
    mu::flushCache(cfg, sizeof(scanfilter::Config));
    mu::flushCacheAll();
    const int ti = mu::getTaskIdx();
    if (ti < 0 || static_cast<std::uint64_t>(ti) >= cfg->par) return;
    const std::uint64_t t = static_cast<std::uint64_t>(ti);
    const std::uint64_t stride = 64 / sizeof(std::uint64_t);

    std::uint64_t lo, hi;
    scanfilter::slice(cfg->rows, cfg->par, t, lo, hi);
    std::uint64_t a, b;                                  // this launch's part of [lo, hi)
    // step/steps come as a kernel argument rather than by re-uploading cfg between launches,
    // so no per-launch config copy has to reach the MU caches.
    if (steps == 0 || step >= steps) return;
    scanfilter::slice(hi - lo, steps, step, a, b);
    a += lo; b += lo;

    // The previous launch may have run this task on another MU core: drop any stale copy.
    mu::flushCache(&progress[t * stride], 64);
    mu::flushCache(&counts[t * stride], 64);
    mu::flushCache(&sum_idx[t * stride], 64);
    mu::flushCache(&sum_key[t * stride], 64);
    const bool first = step == 0;
    const std::uint64_t cur = first ? 0 : progress[t * stride];
    std::uint64_t si = first ? 0 : sum_idx[t * stride];
    std::uint64_t sk = first ? 0 : sum_key[t * stride];

    const std::uint32_t thr = static_cast<std::uint32_t>(cfg->threshold);
    scanfilter::Key* dst = out + t * cfg->max_per_task + cur;
    LineOut lw{dst};
    LineOut* w = (cfg->write_mode == 1 && !cfg->out4) ? &lw : nullptr;
    const scanfilter::SliceResult r = cfg->out4 ? scan_out4(qty, keys, out, cfg, t, lo, a, b, cur)
        : cfg->spare1
        ? scan_slice_prefetch(qty, keys, dst, a, b, thr, cfg->spare1, cfg->spare2 == 0, w)
        : w ? scan_slice_lines(qty, keys, *w, a, b, thr)
            : scanfilter::scan_slice(qty, keys, dst, a, b, thr);
    if (w) w->finish();

    progress[t * stride] = cur + r.n;
    counts[t * stride] = cur + r.n;
    sum_idx[t * stride] = si + r.sum_idx;
    sum_key[t * stride] = sk + r.sum_key;
    mu::flushCacheAll();
}

MU_KERNEL_ADD(scan_filter_step);

// Diagnostic: re-reads each task's materialized output on the device and sums it, so a host
// readback mismatch can be split into "writes never landed" vs "copyFromDevice returned wrong
// bytes". counts[] must hold the scan kernel's per-task counts.
void sum_output(const scanfilter::Key* out,
                const std::uint64_t* counts,
                std::uint64_t* out_sum,
                scanfilter::Config* cfg)
{
    mu::flushCache(cfg, sizeof(scanfilter::Config));
    mu::flushCacheAll();
    const int ti = mu::getTaskIdx();
    if (ti < 0 || static_cast<std::uint64_t>(ti) >= cfg->par) return;
    const std::uint64_t t = static_cast<std::uint64_t>(ti);
    const std::uint64_t stride = 64 / sizeof(std::uint64_t);
    const scanfilter::Key* src = out + t * cfg->max_per_task;
    const std::uint64_t n = counts[t * stride];
    std::uint64_t s = 0;
    for (std::uint64_t j = 0; j < n; ++j) s += src[j];
    out_sum[t * stride] = s;
    mu::flushCacheAll();
}

MU_KERNEL_ADD(sum_output);

// ---- Read-bandwidth probe (bw tool). Same memory and allocator as the scan, no predicate:
// each task sums its slice of uint64 words (8 independent accumulators for ILP), optionally
// prefetching `window` 512 B blocks ahead. cfg: rows = words, par = tasks, spare1 = window.
void bw_fill(std::uint64_t* buf, scanfilter::Config* cfg)
{
    mu::flushCache(cfg, sizeof(scanfilter::Config));
    const int ti = mu::getTaskIdx();
    if (ti < 0 || static_cast<std::uint64_t>(ti) >= cfg->par) return;
    std::uint64_t lo, hi;
    scanfilter::slice(cfg->rows, cfg->par, static_cast<std::uint64_t>(ti), lo, hi);
    for (std::uint64_t i = lo; i < hi; ++i) buf[i] = i;
    mu::flushCacheAll();
}

void bw_read(const std::uint64_t* buf, std::uint64_t* sums, scanfilter::Config* cfg)
{
    mu::flushCache(cfg, sizeof(scanfilter::Config));
    mu::flushCacheAll();
    const int ti = mu::getTaskIdx();
    if (ti < 0 || static_cast<std::uint64_t>(ti) >= cfg->par) return;
    const std::uint64_t t = static_cast<std::uint64_t>(ti);
    std::uint64_t lo, hi;
    scanfilter::slice(cfg->rows, cfg->par, t, lo, hi);
    const std::uint64_t ahead = cfg->spare1 * 64;             // words (64 words = 512 B)
    std::uint64_t s[8] = {0, 0, 0, 0, 0, 0, 0, 0};
    std::uint64_t i = lo;
    for (; i + 64 <= hi; i += 64) {
        if (ahead) {
            const std::uint64_t pf = (i + ahead) & ~std::uint64_t(63);
            if (pf < hi) mu::prefetch(buf + pf, 512);
        }
        for (std::uint64_t j = 0; j < 64; j += 8)
            for (int k = 0; k < 8; ++k) s[k] += buf[i + j + k];
    }
    for (; i < hi; ++i) s[0] += buf[i];
    const std::uint64_t stride = 64 / sizeof(std::uint64_t);
    sums[t * stride] = s[0] + s[1] + s[2] + s[3] + s[4] + s[5] + s[6] + s[7];
    mu::flushCacheAll();
}

MU_KERNEL_ADD(bw_fill);
MU_KERNEL_ADD(bw_read);
