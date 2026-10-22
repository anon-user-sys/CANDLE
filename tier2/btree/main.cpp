// B+tree point-lookup workload: the index counterpart of tier2/scan.
//
//   SELECT o_rownum FROM orders WHERE o_orderkey = ?        (--lookups probes)
//
// A B+tree on TPC-H o_orderkey (N = 1.5M x SF keys, dbgen's sparse key scheme, value = order row
// number) is bulk-loaded on the host. A probe array of lookup keys is generated (uniform or
// scrambled Zipfian over the existing keys, plus --miss-pct keys that are never present), split
// into one slice per thread/task, and looked up. Where the scan is bandwidth-bound, this is
// latency-bound: every lookup is `height` dependent node fetches, each a binary search over
// several cache lines.
//
//   --mode host       tree + probes in host memory, host threads   (bind with numactl)
//   --mode host_cxl   tree + probes on CXL NUMA node --numa-node    (system-ram)
//   --mode host_split restricted host + CXL expander: --dram-frac F of the tree (and of the probe
//                     array) in DRAM node 0, the rest on CXL, mbind per 2 MB chunk. --placement
//                     interleave = F/(1-F) chunk pattern; block = the first F of the array in DRAM,
//                     which with the top-down node layout is the upper levels + the first leaves.
//   --mode device     tree + probes in device memory, MU tasks      (devdax)
//
// --group G walks G lookups in lockstep and prefetches the next node of each (group prefetching);
// G = 1 is the plain loop. Same code on host (__builtin_prefetch) and device (mu::prefetch).
//
// Every iteration is validated against a reference computed from the probe keys alone with the
// dbgen key formula, never through the tree: found count + sum of found positions + sum of values.
// With --materialize 1 the per-lookup output is re-summed (device: on the device and by host loads).
//
// CSV: label,mode,sf,keys,node_bytes,fanout,fill,height,tree_gb,lookups,dist,theta,miss_pct,par,
//      group,materialize,iter,upload_ms,lookup_ms,mops,found,ok

#include <numa.h>
#include <numaif.h>
#include <sys/mman.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <immintrin.h>
#include <string>
#include <thread>
#include <vector>

#include <pxl/pxl.hpp>
#include <pxl/signal_handler.hpp>

#include "btree_common.hpp"

using namespace btree;
using clk = std::chrono::steady_clock;
static double ms_since(clk::time_point t0) {
    return std::chrono::duration<double, std::milli>(clk::now() - t0).count();
}

namespace {

struct Opt {
    std::string mode = "host", label, dist = "uniform", placement = "interleave", layout = "classic";
    double sf = 1.0, fill = 1.0, theta = 0.99, miss_pct = 0.0, dram_frac = 1.0 / 3.0;
    std::uint64_t keys = 0;            // 0 = 1.5M x sf
    std::uint64_t node_bytes = 512, lookups = 10000000, par = 128, group = 1, seed = 42;
    std::uint64_t partition = 0;       // 0 = uniform slices; P = probes range-partitioned into P key ranges
    int materialize = 0, iters = 3, verify = 1, numa_node = 2, thp = 0;
};

struct Expected { std::uint64_t n = 0, sum_idx = 0, sum_val = 0; };

constexpr unsigned kGen = 64;   // threads for build / generate / reference (fixed: output is deterministic)

template <typename F>
void parallel(unsigned n, F&& f) {
    std::vector<std::thread> th;
    for (unsigned t = 0; t < n; ++t) th.emplace_back(f, t);
    for (auto& x : th) x.join();
}

// floor(total * p / parts) without overflow.
std::uint64_t part(std::uint64_t total, std::uint64_t parts, std::uint64_t p) {
    return static_cast<std::uint64_t>(static_cast<unsigned __int128>(total) * p / parts);
}

// ---------------------------------------------------------------- tree

struct Tree {
    std::uint64_t keys = 0, node_bytes = 0, fanout = 0, per_leaf = 0, per_inner = 0, height = 0, nodes = 0;
    std::vector<std::uint64_t> cnt, off;   // per level (0 = leaves): node count, first node index
    std::size_t bytes() const { return nodes * node_bytes; }
};

Tree plan_tree(std::uint64_t n, std::uint64_t node_bytes, double fill) {
    Tree t;
    t.keys = n; t.node_bytes = node_bytes;
    t.fanout = (node_bytes - kNodeHeader) / 16;
    const auto cap = [&](double f) { return std::max<std::uint64_t>(2, std::min<std::uint64_t>(t.fanout, static_cast<std::uint64_t>(t.fanout * f))); };
    t.per_leaf = cap(fill); t.per_inner = cap(fill);
    t.cnt.push_back((n + t.per_leaf - 1) / t.per_leaf);
    while (t.cnt.back() > 1) t.cnt.push_back((t.cnt.back() + t.per_inner - 1) / t.per_inner);
    t.height = t.cnt.size();
    t.off.assign(t.height, 0);   // top-down: root = node 0, leaves last
    for (std::size_t h = t.height - 1; h-- > 0;) t.off[h] = t.off[h + 1] + t.cnt[h + 1];
    t.nodes = t.off[0] + t.cnt[0];
    return t;
}

// Compact layout: 64 B nodes, 4 entries per leaf, 8 children per inner node, all full but the last.
Tree plan_compact(std::uint64_t n) {
    Tree t;
    t.keys = n; t.node_bytes = 64; t.fanout = kCompactFan; t.per_leaf = kCompactLeaf; t.per_inner = kCompactFan;
    t.cnt.push_back((n + kCompactLeaf - 1) / kCompactLeaf);
    while (t.cnt.back() > 1) t.cnt.push_back((t.cnt.back() + kCompactFan - 1) / kCompactFan);
    t.height = t.cnt.size();
    t.off.assign(t.height, 0);
    for (std::size_t h = t.height - 1; h-- > 0;) t.off[h] = t.off[h + 1] + t.cnt[h + 1];
    t.nodes = t.off[0] + t.cnt[0];
    return t;
}

void build_compact(const Tree& t, unsigned char* dst) {
    for (std::uint64_t h = 0; h < t.height; ++h) {
        const std::uint64_t cnt = t.cnt[h];
        parallel(kGen, [&, h](unsigned w) {
            for (std::uint64_t p = part(cnt, kGen, w); p < part(cnt, kGen, w + 1); ++p) {
                auto* k = reinterpret_cast<Key*>(dst + (t.off[h] + p) * 64);
                if (h == 0) {
                    for (std::uint64_t j = 0; j < kCompactLeaf; ++j) {
                        const std::uint64_t i = p * kCompactLeaf + j;   // 0-based key index
                        k[j] = i < t.keys ? order_key(i + 1) : kKeyMax;
                        k[kCompactLeaf + j] = i < t.keys ? i + 1 : 0;
                    }
                } else {
                    for (std::uint64_t j = 0; j < kCompactFan; ++j) {
                        std::uint64_t c = p * kCompactFan + j;           // child position at level h-1
                        if (c >= t.cnt[h - 1]) { k[j] = kKeyMax; continue; }
                        for (std::uint64_t l = h - 1; l > 0; --l) c *= kCompactFan;   // its first leaf
                        k[j] = order_key(c * kCompactLeaf + 1);
                    }
                }
            }
        });
    }
}

// Smallest key under node `pos` of `level`: follow first children down to a leaf.
Key first_key(const Tree& t, std::uint64_t level, std::uint64_t pos) {
    for (; level > 0; --level) pos = part(t.cnt[level - 1], t.cnt[level], pos);
    return order_key(part(t.keys, t.cnt[0], pos) + 1);
}

// Writes every node into dst (t.bytes() bytes). Entries are spread evenly over each level's nodes.
void build_tree(const Tree& t, unsigned char* dst) {
    const std::uint64_t nb = t.node_bytes, F = t.fanout;
    for (std::uint64_t h = 0; h < t.height; ++h) {
        const std::uint64_t cnt = t.cnt[h];
        const std::uint64_t below = h ? t.cnt[h - 1] : t.keys;
        parallel(kGen, [&, h](unsigned w) {
            for (std::uint64_t p = part(cnt, kGen, w); p < part(cnt, kGen, w + 1); ++p) {
                unsigned char* node = dst + (t.off[h] + p) * nb;
                std::memset(node, 0, nb);
                const std::uint64_t lo = part(below, cnt, p), hi = part(below, cnt, p + 1);
                auto* hdr = reinterpret_cast<std::uint32_t*>(node);
                hdr[0] = static_cast<std::uint32_t>(hi - lo);
                hdr[1] = static_cast<std::uint32_t>(h);
                auto* keys = reinterpret_cast<Key*>(node + kNodeHeader);
                auto* vals = reinterpret_cast<std::uint64_t*>(node + kNodeHeader + 8 * F);
                for (std::uint64_t j = lo; j < hi; ++j) {
                    if (h == 0) { keys[j - lo] = order_key(j + 1); vals[j - lo] = j + 1; }
                    else        { keys[j - lo] = first_key(t, h - 1, j); vals[j - lo] = t.off[h - 1] + j; }
                }
            }
        });
    }
}

// ---------------------------------------------------------------- probe keys

std::uint64_t splitmix(std::uint64_t& s) {
    std::uint64_t z = (s += 0x9e3779b97f4a7c15ULL);
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
    return z ^ (z >> 31);
}
double u01(std::uint64_t& s) { return (splitmix(s) >> 11) * 0x1.0p-53; }

std::uint64_t fnv64(std::uint64_t v) {   // YCSB's scrambled-Zipfian hash
    std::uint64_t h = 0xcbf29ce484222325ULL;
    for (int i = 0; i < 8; ++i) { h ^= v & 0xff; h *= 1099511628211ULL; v >>= 8; }
    return h;
}

// Zipf(n, s) ranks 1..n in O(1) per sample: rejection-inversion (Hormann & Derflinger 1996),
// as in Apache Commons RNG. No zeta(n) sum, so n = 1e10 is fine.
struct Zipf {
    double n, s, hx1, hn, sv;
    Zipf(std::uint64_t n_, double s_) : n(static_cast<double>(n_)), s(s_) {
        hx1 = hint(1.5) - 1.0; hn = hint(n + 0.5); sv = 2.0 - hinv(hint(2.5) - h(2.0));
    }
    static double helper1(double x) { return std::fabs(x) > 1e-8 ? std::log1p(x) / x : 1.0 - x * (0.5 - x * (1.0 / 3.0 - 0.25 * x)); }
    static double helper2(double x) { return std::fabs(x) > 1e-8 ? std::expm1(x) / x : 1.0 + x * 0.5 * (1.0 + x * (1.0 / 3.0) * (1.0 + 0.25 * x)); }
    double h(double x) const { return std::exp(-s * std::log(x)); }
    double hint(double x) const { const double lx = std::log(x); return helper2((1.0 - s) * lx) * lx; }
    double hinv(double x) const { double t = x * (1.0 - s); if (t < -1.0) t = -1.0; return std::exp(helper1(t) * x); }
    std::uint64_t sample(std::uint64_t& rng) const {
        for (;;) {
            const double u = hn + u01(rng) * (hx1 - hn);
            const double x = hinv(u);
            double k = std::floor(x + 0.5);
            if (k < 1) k = 1; else if (k > n) k = n;
            if (k - x <= sv || u >= hint(k + 0.5) - h(k)) return static_cast<std::uint64_t>(k);
        }
    }
};

std::vector<Key> gen_lookups(const Opt& o, std::uint64_t n) {
    std::vector<Key> q(o.lookups);
    const bool zipf = o.dist == "zipf";
    const Zipf z(n, o.theta);
    const double miss = o.miss_pct / 100.0;
    parallel(kGen, [&](unsigned w) {
        std::uint64_t rng = o.seed * 0x9e3779b97f4a7c15ULL + w;
        for (std::uint64_t i = part(o.lookups, kGen, w); i < part(o.lookups, kGen, w + 1); ++i) {
            if (miss > 0 && u01(rng) < miss) {   // never a key: bit 3 or 4 of the low 5 bits is set
                q[i] = (splitmix(rng) % (n / 8 + 1)) * 32 + 8 + splitmix(rng) % 24;
                continue;
            }
            const std::uint64_t idx = zipf ? 1 + fnv64(z.sample(rng)) % n : 1 + splitmix(rng) % n;
            q[i] = order_key(idx);
        }
    });
    return q;
}

// From the key formula only, never through the tree.
Expected expected(const std::vector<Key>& q, std::uint64_t n) {
    std::vector<Expected> e(kGen);
    parallel(kGen, [&](unsigned w) {
        Expected x;
        for (std::uint64_t i = part(q.size(), kGen, w); i < part(q.size(), kGen, w + 1); ++i) {
            const Key k = q[i];
            const std::uint64_t idx = order_index(k);
            if (k != 0 && is_order_key(k) && idx >= 1 && idx <= n) { ++x.n; x.sum_idx += i; x.sum_val += idx; }
        }
        e[w] = x;
    });
    Expected r;
    for (const auto& x : e) { r.n += x.n; r.sum_idx += x.sum_idx; r.sum_val += x.sum_val; }
    return r;
}

// ---------------------------------------------------------------- host memory placement

// 2 MB-aligned anonymous memory; THP on or off. Nothing is touched here.
void* host_map(std::size_t bytes, bool thp, std::size_t& map_len, void*& map_base) {
    constexpr std::size_t kChunk = std::size_t(2) << 20;
    const std::size_t len = (bytes + kChunk - 1) / kChunk * kChunk;
    map_len = len + kChunk;
    map_base = ::mmap(nullptr, map_len, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (map_base == MAP_FAILED) return nullptr;
    auto* base = reinterpret_cast<char*>((reinterpret_cast<std::uintptr_t>(map_base) + kChunk - 1) & ~(kChunk - 1));
    ::madvise(base, len, thp ? MADV_HUGEPAGE : MADV_NOHUGEPAGE);
    return base;
}

// Binds each 2 MB chunk to DRAM (node 0) or cxl_node before first touch (same rule as
// tier2/scan): interleave puts chunk c in DRAM when floor((c+1)F) > floor(cF); block puts
// the first round(nchunks*F) chunks in DRAM. frac = 0 puts everything on cxl_node.
bool bind_chunks(void* base, std::size_t bytes, double frac, bool interleave, int cxl_node) {
    constexpr std::size_t kChunk = std::size_t(2) << 20;
    const std::size_t n = (bytes + kChunk - 1) / kChunk, ndram = static_cast<std::size_t>(n * frac + 0.5);
    auto in_dram = [&](std::size_t c) {
        return interleave ? std::uint64_t((c + 1) * frac) > std::uint64_t(c * frac) : c < ndram;
    };
    for (std::size_t c = 0; c < n;) {
        const bool d = in_dram(c);
        std::size_t e = c + 1;
        while (e < n && in_dram(e) == d) ++e;
        unsigned long mask = 1UL << (d ? 0 : cxl_node);
        if (::mbind(static_cast<char*>(base) + c * kChunk, (e - c) * kChunk, MPOL_BIND, &mask, sizeof(mask) * 8, 0) != 0) {
            std::perror("mbind"); return false;
        }
        c = e;
    }
    return true;
}

void report_placement(const char* name, const void* p, std::size_t bytes, int cxl_node) {
    constexpr std::size_t kChunk = std::size_t(2) << 20;
    std::vector<void*> pages;
    for (std::size_t off = 0; off < bytes; off += kChunk) pages.push_back(const_cast<char*>(static_cast<const char*>(p)) + off);
    std::vector<int> status(pages.size(), -1);
    if (::move_pages(0, pages.size(), pages.data(), nullptr, status.data(), 0) != 0) { std::perror("move_pages"); return; }
    std::size_t n0 = 0, ncxl = 0, other = 0;
    for (int st : status) { if (st == 0) ++n0; else if (st == cxl_node) ++ncxl; else ++other; }
    const double gb = bytes / 1e9, tot = static_cast<double>(pages.size());
    std::fprintf(stderr, "# placement %-8s %7.2f GB: DRAM %5.1f%%  CXL %5.1f%%%s\n", name, gb,
                 100.0 * n0 / tot, 100.0 * ncxl / tot, other ? "  WARNING: pages on other nodes" : "");
}

std::uint64_t anon_huge_kb() {
    FILE* f = std::fopen("/proc/self/smaps_rollup", "r");
    if (!f) return 0;
    char line[256]; unsigned long long v = 0;
    while (std::fgets(line, sizeof line, f)) if (std::sscanf(line, "AnonHugePages: %llu", &v) == 1) break;
    std::fclose(f);
    return v;
}

// ---------------------------------------------------------------- reporting

double g_part_ms = 0.0;   // time to range-partition the probes (host, once), reported on every row

void emit(const Opt& o, const Tree& t, int iter, double upload_ms, double lookup_ms, std::uint64_t found, bool ok) {
    std::printf("%s,%s,%g,%llu,%llu,%llu,%.2f,%llu,%.3f,%llu,%s,%.2f,%.1f,%llu,%llu,%d,%d,%.3f,%.3f,%.3f,%llu,%d,%s,%llu,%.3f\n",
                o.label.c_str(), o.mode.c_str(), o.sf, (unsigned long long)t.keys, (unsigned long long)t.node_bytes,
                (unsigned long long)t.fanout, o.fill, (unsigned long long)t.height, t.bytes() / 1e9,
                (unsigned long long)o.lookups, o.dist.c_str(), o.dist == "zipf" ? o.theta : 0.0, o.miss_pct,
                (unsigned long long)o.par, (unsigned long long)o.group, o.materialize, iter, upload_ms, lookup_ms,
                o.lookups / (lookup_ms * 1e3), (unsigned long long)found, ok, o.layout.c_str(),
                (unsigned long long)o.partition, g_part_ms);
    std::fflush(stdout);
}

bool same(const Expected& a, const Expected& b) { return a.n == b.n && a.sum_idx == b.sum_idx && a.sum_val == b.sum_val; }

// Sum + nonzero count of the materialized output (host loads).
Expected sum_out(const Val* out, std::uint64_t n) {
    std::vector<Expected> e(16);
    parallel(16, [&](unsigned w) {
        Expected x;
        for (std::uint64_t i = part(n, 16, w); i < part(n, 16, w + 1); ++i) { x.sum_val += out[i]; x.n += out[i] != 0; }
        e[w] = x;
    });
    Expected r;
    for (const auto& x : e) { r.n += x.n; r.sum_val += x.sum_val; }
    return r;
}

std::uint64_t count_bad(const Val* out, const std::vector<Key>& q, std::uint64_t n) {
    std::vector<std::uint64_t> bad(16, 0);
    parallel(16, [&](unsigned w) {
        for (std::uint64_t i = part(q.size(), 16, w); i < part(q.size(), 16, w + 1); ++i) {
            const Key k = q[i];
            const std::uint64_t idx = order_index(k);
            bad[w] += out[i] != ((k != 0 && is_order_key(k) && idx >= 1 && idx <= n) ? idx : 0);
        }
    });
    std::uint64_t b = 0;
    for (auto x : bad) b += x;
    return b;
}

// On an output mismatch: which slots are wrong and how (still the poison, zero, other value).
void diagnose_output(const Val* out, const std::vector<Key>& q, std::uint64_t n, std::uint64_t per) {
    std::uint64_t bad = 0, poison = 0, zero = 0, shown = 0, lines = 0, last_line = ~0ULL;
    std::vector<std::uint64_t> bad_tasks;
    for (std::uint64_t i = 0; i < q.size(); ++i) {
        const Key k = q[i];
        const std::uint64_t idx = order_index(k);
        const Val want = (k != 0 && is_order_key(k) && idx >= 1 && idx <= n) ? idx : 0;
        const Val got = out[i];
        if (got == want) continue;
        ++bad; poison += got == ~0ULL; zero += got == 0;
        if (i / 8 != last_line) { ++lines; last_line = i / 8; }
        if (bad_tasks.empty() || bad_tasks.back() != i / per) bad_tasks.push_back(i / per);
        if (shown++ < 8)
            std::fprintf(stderr, "  slot %llu (task %llu, +%llu): expected %llu got 0x%016llx\n", (unsigned long long)i,
                         (unsigned long long)(i / per), (unsigned long long)(i % per), (unsigned long long)want, (unsigned long long)got);
    }
    if (const char* f = std::getenv("BTREE_BADLINES")) {   // offsets of the wrong 64 B lines, for comparing runs
        if (FILE* fp = std::fopen(f, "a")) {
            std::fprintf(fp, "# out=%p\n", static_cast<const void*>(out));
            std::uint64_t prev = ~0ULL;
            for (std::uint64_t i = 0; i < q.size(); ++i) {
                const Key k = q[i];
                const std::uint64_t idx = order_index(k);
                if (out[i] != ((k != 0 && is_order_key(k) && idx >= 1 && idx <= n) ? idx : 0) && i / 8 != prev) {
                    prev = i / 8; std::fprintf(fp, "%llu\n", (unsigned long long)prev);
                }
            }
            std::fclose(fp);
        }
    }
    std::fprintf(stderr, "  summary: %llu wrong slots in %llu lines, %zu tasks (%llu still poison, %llu zero)\n",
                 (unsigned long long)bad, (unsigned long long)lines, bad_tasks.size(), (unsigned long long)poison, (unsigned long long)zero);
}

Config make_config(const Opt& o, const Tree& t) {
    Config c{};
    c.lookups = o.lookups; c.par = o.par;
    c.per = ((o.lookups + o.par - 1) / o.par + 7) / 8 * 8;
    c.root = 0; c.height = t.height; c.node_bytes = t.node_bytes; c.fanout = t.fanout;
    c.group = o.group; c.materialize = static_cast<std::uint64_t>(o.materialize);
    c.layout = o.layout == "compact" ? 1 : 0;
    c.partitioned = o.partition ? 1 : 0;
    for (std::size_t h = 0; h < t.height; ++h) c.level_off[h] = t.off[h];
    return c;
}

// Range-partitions the probes into P key ranges (stable counting sort, 64 threads, timed) and
// returns per-task bounds (equal counts). With P = par every task's lookups stay in about 1/par
// of the leaves.
std::vector<std::uint64_t> partition_probes(std::vector<Key>& q, std::uint64_t n, std::uint64_t P, std::uint64_t par, double& ms) {
    const auto t0 = clk::now();
    const std::uint64_t N = q.size();
    auto bucket = [&](Key k) { std::uint64_t i = order_index(k); if (i > n) i = n; return part(P, n + 1, i); };
    std::vector<std::vector<std::uint64_t>> cnt(kGen, std::vector<std::uint64_t>(P, 0));
    parallel(kGen, [&](unsigned w) {
        for (std::uint64_t i = part(N, kGen, w); i < part(N, kGen, w + 1); ++i) ++cnt[w][bucket(q[i])];
    });
    std::vector<std::uint64_t> start(P + 1, 0);
    for (std::uint64_t b = 0, acc = 0; b < P; ++b) {
        start[b] = acc;
        for (unsigned w = 0; w < kGen; ++w) { const std::uint64_t c = cnt[w][b]; cnt[w][b] = acc; acc += c; }
    }
    start[P] = N;
    std::vector<Key> out(N);
    parallel(kGen, [&](unsigned w) {
        for (std::uint64_t i = part(N, kGen, w); i < part(N, kGen, w + 1); ++i) out[cnt[w][bucket(q[i])]++] = q[i];
    });
    q.swap(out);
    // Equal-count slices of the partitioned array: each task still covers a narrow, contiguous key
    // range (about P/par ranges), and skewed probes cannot pile up on a few tasks.
    std::vector<std::uint64_t> bounds(par + 1, 0);
    for (std::uint64_t t = 0; t <= par; ++t) bounds[t] = part(N, par, t);
    ms = ms_since(t0);
    return bounds;
}

// ---------------------------------------------------------------- host / host_cxl / host_split

struct HostPrefetch {
    void operator()(const void* p, std::uint64_t bytes) const {
        const auto* a = reinterpret_cast<const char*>(reinterpret_cast<std::uintptr_t>(p) & ~std::uintptr_t(63));
        for (const char* e = static_cast<const char*>(p) + bytes; a < e; a += 64) __builtin_prefetch(a, 0, 3);
    }
};

int run_host(const Opt& o, const Tree& t, const std::vector<Key>& q_dram, const std::vector<std::uint64_t>& bounds, const Expected& exp) {
    const bool cxl = o.mode == "host_cxl", split = o.mode == "host_split";
    if ((cxl || split) && (numa_available() < 0 || numa_node_size64(o.numa_node, nullptr) <= 0)) {
        std::fprintf(stderr, "node %d reports no memory - put the dax device in system-ram mode first\n", o.numa_node);
        return 1;
    }
    const double frac = cxl ? 0.0 : o.dram_frac;
    const bool il = o.placement == "interleave";
    std::size_t len[3] = {0, 0, 0};
    void* map[3] = {nullptr, nullptr, nullptr};
    const std::size_t qbytes = o.lookups * sizeof(Key), obytes = o.materialize ? o.lookups * sizeof(Val) : 0;
    auto* nodes = static_cast<unsigned char*>(host_map(t.bytes(), o.thp, len[0], map[0]));
    auto* q = static_cast<Key*>(host_map(qbytes, o.thp, len[1], map[1]));
    auto* out = obytes ? static_cast<Val*>(host_map(obytes, o.thp, len[2], map[2])) : nullptr;
    if (!nodes || !q || (obytes && !out)) { std::fprintf(stderr, "mmap failed\n"); return 1; }
    if (cxl || split) {
        if (!bind_chunks(nodes, t.bytes(), frac, il, o.numa_node) || !bind_chunks(q, qbytes, frac, il, o.numa_node)) return 1;
        // Output: DRAM for the restricted host, CXL for host_cxl (as in tier2/scan).
        if (out && !bind_chunks(out, obytes, cxl ? 0.0 : 1.0, false, o.numa_node)) return 1;
    }
    const auto tb = clk::now();
    if (o.layout == "compact") build_compact(t, nodes); else build_tree(t, nodes);
    parallel(kGen, [&](unsigned w) {
        const std::uint64_t lo = part(o.lookups, kGen, w), hi = part(o.lookups, kGen, w + 1);
        std::memcpy(q + lo, q_dram.data() + lo, (hi - lo) * sizeof(Key));
    });
    if (out) parallel(kGen, [&](unsigned w) {
        const std::uint64_t lo = part(o.lookups, kGen, w), hi = part(o.lookups, kGen, w + 1);
        std::memset(out + lo, 0, (hi - lo) * sizeof(Val));
    });
    std::fprintf(stderr, "# built tree + copied probes in place in %.0f ms (untimed); AnonHugePages %.1f GB\n",
                 ms_since(tb), anon_huge_kb() / 1e6);
    if (cxl || split) {
        report_placement("tree", nodes, t.bytes(), o.numa_node);
        report_placement("probes", q, qbytes, o.numa_node);
    }

    const Config c = make_config(o, t);
    std::vector<SliceResult> res(o.par);
    int rc = 0;
    for (int it = 0; it < o.iters; ++it) {
        if (out) std::memset(out, 0xff, obytes);   // poison: every slot must be rewritten
        const auto t0 = clk::now();
        parallel(static_cast<unsigned>(o.par), [&](unsigned w) {
            std::uint64_t lo, hi; task_range(c, bounds.data(), w, lo, hi);
            res[w] = c.layout == 1 ? lookup_range_compact(nodes, c, q, out, lo, hi)
                                   : lookup_range(nodes, c, q, out, lo, hi, HostPrefetch{});
        });
        const double lookup_ms = ms_since(t0);
        Expected got;
        for (const auto& r : res) { got.n += r.n; got.sum_idx += r.sum_idx; got.sum_val += r.sum_val; }
        bool ok = !o.verify || same(got, exp);
        if (o.verify && out && ok) {
            const Expected s = sum_out(out, o.lookups);
            ok = s.n == exp.n && s.sum_val == exp.sum_val;
        }
        emit(o, t, it, 0.0, lookup_ms, got.n, ok);
        if (!ok) { std::fprintf(stderr, "MISMATCH: host result does not match the reference (found %llu, expected %llu)\n",
                                (unsigned long long)got.n, (unsigned long long)exp.n); rc = 1; break; }
    }
    if (cxl || split) report_placement("tree(after)", nodes, t.bytes(), o.numa_node);
    for (int i = 0; i < 3; ++i) if (map[i]) ::munmap(map[i], len[i]);
    return rc;
}

// ---------------------------------------------------------------- device

// MX1 DMA is unsafe above a buffer's first GiB (see tier2/scan README), so this mode uses
// no DMA at all: the host writes device memory with stores + flushHostCache and reads results
// after invalidating. Buffers are still whole-GiB sized and 1 GiB-aligned, like the scan's.
constexpr std::size_t kGiB = std::size_t(1) << 30;

void* dev_alloc(pxl::runtime::Context* ctx, std::size_t bytes) {
    const std::size_t rounded = (bytes + kGiB - 1) / kGiB * kGiB;
    void* p = ctx->memAlloc(rounded);
    if (p && reinterpret_cast<std::uintptr_t>(p) % kGiB != 0) {
        std::fprintf(stderr, "device buffer %p is not 1 GiB-aligned - refusing\n", p);
        ctx->memFree(p);
        return nullptr;
    }
    return p;
}

// Host stores into device memory, then write back + drop the host's copies.
void put(void* dst, const void* src, std::size_t bytes, unsigned threads) {
    parallel(threads, [&](unsigned w) {
        const std::size_t lo = part(bytes, threads, w) & ~std::size_t(63), hi = w + 1 == threads ? bytes : part(bytes, threads, w + 1) & ~std::size_t(63);
        std::memcpy(static_cast<char*>(dst) + lo, static_cast<const char*>(src) + lo, hi - lo);
        pxl::flushHostCache(static_cast<char*>(dst) + lo, hi - lo);
    });
    _mm_mfence();
}

int run_device(const Opt& o, const Tree& t, const std::vector<Key>& q, const std::vector<std::uint64_t>& bounds, const Expected& exp) {
    pxl::registerSignalHandler({SIGINT, SIGTERM, SIGSEGV, SIGABRT, SIGBUS, SIGFPE, SIGILL});
    if (pxl::runtime::getNumDevice() == 0) { std::fprintf(stderr, "no PXL device\n"); return 1; }
    auto* ctx = pxl::runtime::createContext(0);
    if (!ctx) { std::fprintf(stderr, "createContext failed\n"); return 1; }
    auto module = pxl::kernel::createModule("mu_kernel/btree_kernel.mubin");
    if (!module) { std::fprintf(stderr, "cannot load mu_kernel/btree_kernel.mubin\n"); return 1; }
    auto* job = ctx->createJob();
    if (!job) { std::fprintf(stderr, "createJob failed\n"); return 1; }
    job->load(module);
    auto* fn = module->createFunction("btree_lookup");
    auto* fn_sum = module->createFunction("sum_output");
    auto* fn_fill = module->createFunction("fill_output");
    if (!fn || !fn_sum || !fn_fill) { std::fprintf(stderr, "createFunction failed\n"); return 1; }
    auto* map = job->buildMap(fn, static_cast<std::uint32_t>(o.par));
    auto* map_sum = o.materialize ? job->buildMap(fn_sum, static_cast<std::uint32_t>(o.par)) : nullptr;
    auto* map_fill = o.materialize ? job->buildMap(fn_fill, static_cast<std::uint32_t>(o.par)) : nullptr;
    if (!map || (o.materialize && (!map_sum || !map_fill))) { std::fprintf(stderr, "buildMap failed\n"); return 1; }

    const std::size_t qbytes = o.lookups * sizeof(Key), obytes = o.materialize ? o.lookups * sizeof(Val) : 0;
    const std::size_t stat_off = 4096, stat_bytes = o.par * kStatWords * sizeof(std::uint64_t);
    auto* d_nodes = static_cast<unsigned char*>(dev_alloc(ctx, t.bytes()));
    auto* d_q = static_cast<Key*>(dev_alloc(ctx, qbytes));
    auto* d_out = obytes ? static_cast<Val*>(dev_alloc(ctx, obytes)) : nullptr;
    auto* d_small = static_cast<unsigned char*>(dev_alloc(ctx, (std::size_t(4) << 20) + (o.par + 1) * sizeof(std::uint64_t)));
    if (!d_nodes || !d_q || !d_small || (obytes && !d_out)) { std::fprintf(stderr, "device allocation failed\n"); return 1; }
    auto* d_cfg = reinterpret_cast<Config*>(d_small);
    auto* d_stats = reinterpret_cast<std::uint64_t*>(d_small + stat_off);
    const std::size_t bounds_off = std::size_t(4) << 20;   // after the stats (<= 64 B x 64K tasks)
    auto* d_bounds = reinterpret_cast<std::uint64_t*>(d_small + bounds_off);

    // Build in host DRAM, then upload (timed once, reported on every row as upload_ms).
    std::vector<unsigned char> h_nodes(t.bytes());
    const auto tb = clk::now();
    if (o.layout == "compact") build_compact(t, h_nodes.data()); else build_tree(t, h_nodes.data());
    std::fprintf(stderr, "# built tree in host DRAM in %.0f ms (untimed)\n", ms_since(tb));
    const auto tu = clk::now();
    put(d_nodes, h_nodes.data(), t.bytes(), 16);
    put(d_q, q.data(), qbytes, 16);
    const double upload_ms = ms_since(tu);
    std::fprintf(stderr, "# upload (16 threads memcpy + flushHostCache): %.0f ms, %.2f GB/s\n", upload_ms,
                 (t.bytes() + qbytes) / 1e6 / upload_ms);
    std::vector<unsigned char>().swap(h_nodes);
    const Config c = make_config(o, t);
    put(d_cfg, &c, sizeof c, 1);
    put(d_bounds, bounds.data(), bounds.size() * sizeof(std::uint64_t), 1);

    std::vector<std::uint64_t> poison(stat_bytes / 8, ~0ULL);
    int rc = 0;
    for (int it = 0; it < o.iters; ++it) {
        // Poison stats (and output) so a launch that does not run cannot validate on old values.
        // The output is poisoned on the device (see fill_output) and the host only invalidates.
        put(d_stats, poison.data(), stat_bytes, 1);
        if (d_out && (map_fill->execute(d_out, d_cfg) != pxl::PxlResult::Success ||
                      map_fill->synchronize() != pxl::PxlResult::Success)) { std::fprintf(stderr, "fill_output kernel failed\n"); rc = 1; break; }
        const auto t0 = clk::now();
        if (map->execute(d_nodes, d_q, d_out, d_stats, d_cfg, d_bounds) != pxl::PxlResult::Success ||
            map->synchronize() != pxl::PxlResult::Success) { std::fprintf(stderr, "lookup kernel failed\n"); rc = 1; break; }
        const double lookup_ms = ms_since(t0);

        pxl::flushHostCache(d_stats, stat_bytes);
        _mm_mfence();
        Expected got;
        for (std::uint64_t w = 0; w < o.par; ++w) {
            const std::uint64_t* s = d_stats + w * kStatWords;
            got.n += s[0]; got.sum_idx += s[1]; got.sum_val += s[2];
        }
        bool ok = !o.verify || same(got, exp);
        if (o.verify && d_out && ok) {
            // (a) the device re-reads its output; (b) the host reads it through CXL.mem.
            if (map_sum->execute(d_out, d_stats, d_cfg) != pxl::PxlResult::Success ||
                map_sum->synchronize() != pxl::PxlResult::Success) { std::fprintf(stderr, "sum_output kernel failed\n"); rc = 1; break; }
            pxl::flushHostCache(d_stats, stat_bytes);
            _mm_mfence();
            Expected dev;
            for (std::uint64_t w = 0; w < o.par; ++w) { dev.sum_val += d_stats[w * kStatWords + 3]; dev.n += d_stats[w * kStatWords + 4]; }
            if (dev.n != exp.n || dev.sum_val != exp.sum_val) { std::fprintf(stderr, "device re-read of the output disagrees\n"); ok = false; }
            pxl::flushHostCache(d_out, obytes);
            _mm_mfence();
            const Expected h = sum_out(d_out, o.lookups);
            if (h.n != exp.n || h.sum_val != exp.sum_val) {
                std::fprintf(stderr, "host read of the output disagrees\n");
                diagnose_output(d_out, q, t.keys, c.per);
                // Do the lines become correct later? (still in an MU cache vs. lost)
                auto reread = [&](const char* when) {
                    pxl::flushHostCache(d_out, obytes);
                    _mm_mfence();
                    std::fprintf(stderr, "  re-read %-26s: %llu wrong slots\n", when, (unsigned long long)count_bad(d_out, q, t.keys));
                };
                reread("immediately");
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
                reread("after 200 ms");
                auto* fn_fl = module->createFunction("flush_all");
                auto* map_fl = fn_fl ? job->buildMap(fn_fl, 3048) : nullptr;
                if (map_fl && map_fl->execute(d_cfg) == pxl::PxlResult::Success && map_fl->synchronize() == pxl::PxlResult::Success)
                    reread("after flushCacheAll kernel");
                else std::fprintf(stderr, "  flush_all kernel failed\n");
                {   // host reads ~400 MB of other device memory (tree + probes), then re-read
                    pxl::flushHostCache(d_nodes, t.bytes());
                    pxl::flushHostCache(d_q, qbytes);
                    _mm_mfence();
                    std::uint64_t sink = 0;
                    for (std::size_t i = 0; i < t.bytes() / 8; i += 8) sink += reinterpret_cast<const std::uint64_t*>(d_nodes)[i];
                    for (std::size_t i = 0; i < qbytes / 8; i += 8) sink += d_q[i];
                    std::fprintf(stderr, "  (host read %.0f MB of other device memory, sink %llu)\n", (t.bytes() + qbytes) / 1e6, (unsigned long long)(sink & 1));
                    reread("after host reads elsewhere");
                }
                // MU reads 160+ MB of other memory (sum_output over the tree array) to evict device caches.
                if (map_sum->execute(reinterpret_cast<Val*>(d_nodes), d_stats, d_cfg) == pxl::PxlResult::Success &&
                    map_sum->synchronize() == pxl::PxlResult::Success)
                    reread("after MU reads elsewhere");
                // And does the device itself still see the right values?
                if (map_sum->execute(d_out, d_stats, d_cfg) == pxl::PxlResult::Success && map_sum->synchronize() == pxl::PxlResult::Success) {
                    pxl::flushHostCache(d_stats, stat_bytes);
                    _mm_mfence();
                    Expected dv;
                    for (std::uint64_t w = 0; w < o.par; ++w) { dv.sum_val += d_stats[w * kStatWords + 3]; dv.n += d_stats[w * kStatWords + 4]; }
                    std::fprintf(stderr, "  device re-read now: %s\n", dv.n == exp.n && dv.sum_val == exp.sum_val ? "correct" : "WRONG");
                }
                if (map_fl) job->destroyMap(map_fl);
                if (fn_fl) module->destroyFunction(fn_fl);
                ok = false;
            }
        }
        emit(o, t, it, upload_ms, lookup_ms, got.n, ok);
        if (!ok) { std::fprintf(stderr, "MISMATCH: device result does not match the reference (found %llu, expected %llu)\n",
                                (unsigned long long)got.n, (unsigned long long)exp.n); rc = 1; break; }
    }

    ctx->memFree(d_small);
    if (d_out) ctx->memFree(d_out);
    ctx->memFree(d_q);
    ctx->memFree(d_nodes);
    if (map_fill) job->destroyMap(map_fill);
    if (map_sum) job->destroyMap(map_sum);
    job->destroyMap(map);
    ctx->destroyJob(job);
    module->destroyFunction(fn_fill);
    module->destroyFunction(fn_sum);
    module->destroyFunction(fn);
    pxl::kernel::destroyModule(module);
    pxl::runtime::destroyContext(ctx);
    return rc;
}

constexpr const char* kHeader =
    "label,mode,sf,keys,node_bytes,fanout,fill,height,tree_gb,lookups,dist,theta,miss_pct,par,group,materialize,iter,upload_ms,lookup_ms,mops,found,ok,layout,partition,part_ms";

std::uint64_t parse_count(const std::string& s) {   // 100M, 1G, 1e8
    if (s.empty()) return 0;
    const char u = static_cast<char>(std::tolower(s.back()));
    const double m = u == 'k' ? 1e3 : u == 'm' ? 1e6 : u == 'g' ? 1e9 : 1.0;
    return static_cast<std::uint64_t>(std::stod(m == 1.0 ? s : s.substr(0, s.size() - 1)) * m);
}

}  // namespace

int main(int argc, char** argv) {
    Opt o;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto v = [&]() -> std::string { if (i + 1 >= argc) { std::fprintf(stderr, "%s needs a value\n", a.c_str()); std::exit(2); } return argv[++i]; };
        if (a == "--mode") o.mode = v();
        else if (a == "--label") o.label = v();
        else if (a == "--sf") o.sf = std::stod(v());
        else if (a == "--keys") o.keys = parse_count(v());
        else if (a == "--node-bytes") o.node_bytes = std::stoull(v());
        else if (a == "--fill") o.fill = std::stod(v());
        else if (a == "--lookups") o.lookups = parse_count(v());
        else if (a == "--dist") o.dist = v();
        else if (a == "--theta") o.theta = std::stod(v());
        else if (a == "--miss-pct") o.miss_pct = std::stod(v());
        else if (a == "--seed") o.seed = std::stoull(v());
        else if (a == "--par") o.par = std::stoull(v());
        else if (a == "--group") o.group = std::stoull(v());
        else if (a == "--materialize") o.materialize = std::stoi(v());
        else if (a == "--iters") o.iters = std::stoi(v());
        else if (a == "--verify") o.verify = std::stoi(v());
        else if (a == "--numa-node") o.numa_node = std::stoi(v());
        else if (a == "--dram-frac") o.dram_frac = std::stod(v());
        else if (a == "--placement") o.placement = v();
        else if (a == "--thp") o.thp = std::stoi(v());
        else if (a == "--layout") o.layout = v();
        else if (a == "--partition") o.partition = std::stoull(v());
        else if (a == "--header") { std::printf("%s\n", kHeader); return 0; }
        else { std::fprintf(stderr, "unknown option %s\n", a.c_str()); return 2; }
    }
    if (o.label.empty()) o.label = o.mode;
    if (o.mode != "host" && o.mode != "host_cxl" && o.mode != "host_split" && o.mode != "device") { std::fprintf(stderr, "--mode host|host_cxl|host_split|device\n"); return 2; }
    if (o.node_bytes < 64 || o.node_bytes % 64) { std::fprintf(stderr, "--node-bytes must be a multiple of 64\n"); return 2; }
    if (o.fill <= 0 || o.fill > 1) { std::fprintf(stderr, "--fill must be in (0,1]\n"); return 2; }
    if (o.dist != "uniform" && o.dist != "zipf") { std::fprintf(stderr, "--dist uniform|zipf\n"); return 2; }
    if (o.miss_pct < 0 || o.miss_pct > 100) { std::fprintf(stderr, "--miss-pct must be 0..100\n"); return 2; }
    if (o.placement != "interleave" && o.placement != "block") { std::fprintf(stderr, "--placement interleave|block\n"); return 2; }
    if (o.dram_frac < 0 || o.dram_frac > 1) { std::fprintf(stderr, "--dram-frac must be 0..1\n"); return 2; }
    if (o.par == 0 || o.lookups == 0) { std::fprintf(stderr, "--par and --lookups must be > 0\n"); return 2; }
    if (o.layout != "classic" && o.layout != "compact") { std::fprintf(stderr, "--layout classic|compact\n"); return 2; }
    if (o.partition && o.materialize) { std::fprintf(stderr, "--partition does not support --materialize\n"); return 2; }
    if (o.partition > o.par) { std::fprintf(stderr, "--partition must be <= --par\n"); return 2; }
    if (o.group < 1 || o.group > kMaxGroup) { std::fprintf(stderr, "--group must be 1..%llu\n", (unsigned long long)kMaxGroup); return 2; }

    const std::uint64_t n = o.keys ? o.keys : static_cast<std::uint64_t>(1500000.0 * o.sf + 0.5);
    if (n == 0) { std::fprintf(stderr, "no keys\n"); return 2; }
    const Tree t = o.layout == "compact" ? plan_compact(n) : plan_tree(n, o.node_bytes, o.fill);
    if (t.height > kMaxLevels) { std::fprintf(stderr, "tree too tall\n"); return 2; }
    std::string levels;
    for (std::size_t h = t.height; h-- > 0;) levels += std::to_string(t.cnt[h]) + (h ? " / " : "");
    std::fprintf(stderr, "# tree: %llu keys, node %llu B, fanout %llu (%llu per leaf, %llu per inner), height %llu, %.3f GB; nodes per level root->leaf: %s\n",
                 (unsigned long long)n, (unsigned long long)t.node_bytes, (unsigned long long)t.fanout,
                 (unsigned long long)t.per_leaf, (unsigned long long)t.per_inner, (unsigned long long)t.height,
                 t.bytes() / 1e9, levels.c_str());

    const auto tg = clk::now();
    std::vector<Key> q = gen_lookups(o, n);
    std::vector<std::uint64_t> bounds;
    if (o.partition) {
        bounds = partition_probes(q, n, o.partition, o.par, g_part_ms);
        std::fprintf(stderr, "# probes range-partitioned into %llu key ranges in %.0f ms (64 threads)\n",
                     (unsigned long long)o.partition, g_part_ms);
    }
    const Expected exp = expected(q, n);
    std::fprintf(stderr, "# %llu %s probes (theta %.2f, miss %.1f%%) generated in %.0f ms; %llu present\n",
                 (unsigned long long)o.lookups, o.dist.c_str(), o.theta, o.miss_pct, ms_since(tg), (unsigned long long)exp.n);
    return o.mode == "device" ? run_device(o, t, q, bounds, exp) : run_host(o, t, q, bounds, exp);
}
