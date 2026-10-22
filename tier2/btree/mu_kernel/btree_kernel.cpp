// MU kernels for the B+tree lookup workload (see ../README.md).
//
// Coherence (same protocol as tier2/scan): the host writes the config, tree and probe keys
// with host stores + flushHostCache. The kernel invalidates the config line before reading it and
// runs flushCacheAll at entry and exit, so it never reads a stale line from an earlier iteration
// and its stats/output are written back before synchronize() returns.

#include <cstdint>

#include "mu/mu.hpp"
#include "btree_common.hpp"

// mu::prefetch takes a 64 B-aligned address and 64..512 bytes.
struct DevPrefetch {
    void operator()(const void* p, std::uint64_t bytes) const {
        std::uintptr_t a = reinterpret_cast<std::uintptr_t>(p) & ~std::uintptr_t(63);
        const std::uintptr_t e = reinterpret_cast<std::uintptr_t>(p) + bytes;
        while (a < e) {
            std::uint64_t n = e - a;
            n = n > 512 ? 512 : (n + 63) & ~std::uint64_t(63);
            mu::prefetch(reinterpret_cast<const void*>(a), n);
            a += n;
        }
    }
};

void btree_lookup(const unsigned char* nodes,
                  const btree::Key* lookups,
                  btree::Val* out,
                  std::uint64_t* stats,
                  btree::Config* cfg,
                  const std::uint64_t* bounds)   // per-task ranges when cfg->partitioned
{
    mu::flushCache(cfg, sizeof(btree::Config));
    mu::flushCacheAll();
    const btree::Config c = *cfg;
    const int ti = mu::getTaskIdx();
    if (ti < 0 || static_cast<std::uint64_t>(ti) >= c.par) return;
    const std::uint64_t t = static_cast<std::uint64_t>(ti);

    std::uint64_t lo, hi;
    btree::task_range(c, bounds, t, lo, hi);
    btree::Val* dst = c.materialize ? out : nullptr;
    const btree::SliceResult r = c.layout == 1 ? btree::lookup_range_compact(nodes, c, lookups, dst, lo, hi)
                                               : btree::lookup_range(nodes, c, lookups, dst, lo, hi, DevPrefetch{});

    std::uint64_t* s = stats + t * btree::kStatWords;
    s[0] = r.n;
    s[1] = r.sum_idx;
    s[2] = r.sum_val;
    mu::flushCacheAll();
}

MU_KERNEL_ADD(btree_lookup);

// Re-reads every task's output slice on the device: sum of values and nonzero (found) slots.
void sum_output(const btree::Val* out, std::uint64_t* stats, btree::Config* cfg)
{
    mu::flushCache(cfg, sizeof(btree::Config));
    mu::flushCacheAll();
    const btree::Config c = *cfg;
    const int ti = mu::getTaskIdx();
    if (ti < 0 || static_cast<std::uint64_t>(ti) >= c.par) return;
    const std::uint64_t t = static_cast<std::uint64_t>(ti);
    std::uint64_t lo, hi;
    btree::slice(c, t, lo, hi);
    std::uint64_t sum = 0, nz = 0;
    for (std::uint64_t i = lo; i < hi; ++i) { sum += out[i]; nz += out[i] != 0; }
    stats[t * btree::kStatWords + 3] = sum;
    stats[t * btree::kStatWords + 4] = nz;
    mu::flushCacheAll();
}

MU_KERNEL_ADD(sum_output);

// Overwrites every task's output slice with ~0 (never a valid value), so a slot the lookup
// kernel did not write - or a stale host-cached line - fails validation. Poisoning from the host
// instead (stores + flushHostCache from 16 threads) left poison lines that reached device memory
// AFTER the lookup kernel had written them (2026-10-11).
void fill_output(btree::Val* out, btree::Config* cfg)
{
    mu::flushCache(cfg, sizeof(btree::Config));
    mu::flushCacheAll();
    const btree::Config c = *cfg;
    const int ti = mu::getTaskIdx();
    if (ti < 0 || static_cast<std::uint64_t>(ti) >= c.par) return;
    std::uint64_t lo, hi;
    btree::slice(c, static_cast<std::uint64_t>(ti), lo, hi);
    for (std::uint64_t i = lo; i < hi; ++i) out[i] = ~std::uint64_t(0);
    mu::flushCacheAll();
}

MU_KERNEL_ADD(fill_output);

// Diagnostic: every task only runs flushCacheAll (used after a host-read mismatch, to see whether
// the missing lines were still sitting in an MU cache).
void flush_all(btree::Config* cfg)
{
    (void)cfg;
    mu::flushCacheAll();
}

MU_KERNEL_ADD(flush_all);
