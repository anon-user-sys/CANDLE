#pragma once
// Shared by the host driver and the MU kernel, so host and device run the identical lookup loop.
//
// Workload: point lookups in a B+tree index on TPC-H o_orderkey (value = order row number),
//
//   SELECT o_rownum FROM orders WHERE o_orderkey = ?        (one probe per lookup key)
//
// The tree is bulk-loaded on the host into one array of fixed-size nodes. Nodes refer to each
// other by index, not pointer, so the same bytes work in host DRAM, on a CXL NUMA node and in
// device memory. Nodes are laid out top-down (root = node 0, then each level left to right, leaves
// last), so "the first X% of the array" = the upper levels plus the first leaves.
//
// Node (node_bytes, a multiple of 64):
//   [0,4)  n      entries in use
//   [4,8)  level  0 = leaf
//   [8, 8+8F)          keys[F]
//   [8+8F, 8+16F)      vals[F]: leaf = order row number (1..N), inner = child node index
//   F = fanout = (node_bytes - 8) / 16.  Inner keys[i] = smallest key under child i.
//
// Compact layout (--layout compact, layout = 1): every node is one 64 B line and no node stores
// a child index. Inner node = 8 keys, keys[j] = smallest key under child j (MAX past the end);
// leaf = 4 keys then 4 values. All nodes but the last of each level are full, so the children of
// node p of level h are nodes 8p..8p+7 of level h-1 and a child's index is computed:
// level_off[h-1] + 8p + slot. One line per level instead of 3 (256 B) or 7 (4 KB) - the
// cache-sensitive search tree idea (Rao & Ross) for a read-only, bulk-loaded index.
#include <cstdint>

namespace btree {

using Key = std::uint64_t;
using Val = std::uint64_t;   // order row number 1..N; 0 in the materialized output = not found

// dbgen's sparse order keys (mk_sparse, SPARSE_BITS 2, SPARSE_KEEP 3, seq 0): of every 32 keys
// only the first 8 exist, so i = 1..N maps to 1..7, 32..39, 64..71, ...  Checked against
// sf_1 orders.tbl (row 8 -> 32, row 1500000 -> 6000000).
inline Key order_key(std::uint64_t i) { return ((i >> 3) << 5) | (i & 7); }
inline bool is_order_key(Key k) { return (k & 24) == 0; }
inline std::uint64_t order_index(Key k) { return ((k >> 5) << 3) | (k & 7); }

constexpr std::uint64_t kNodeHeader = 8;
constexpr std::uint64_t kMaxGroup = 64;
constexpr std::uint64_t kStatWords = 8;   // per-task stats: one 64 B line each
constexpr std::uint64_t kMaxLevels = 24;
constexpr std::uint64_t kCompactFan = 8, kCompactLeaf = 4;
constexpr Key kKeyMax = ~Key(0);

struct Config {
    std::uint64_t lookups;      // lookup keys in the probe array
    std::uint64_t par;          // tasks (device) or threads (host)
    std::uint64_t per;          // lookups per task slice, a multiple of 8 (64 B output lines)
    std::uint64_t root;         // root node index (0 with the top-down layout)
    std::uint64_t height;       // levels, 1 = the root is a leaf
    std::uint64_t node_bytes;
    std::uint64_t fanout;
    std::uint64_t group;        // lookups walked in lockstep with prefetch (1 = plain loop)
    std::uint64_t materialize;  // 1 = write each lookup's value (0 = miss) to out[i]
    std::uint64_t layout;       // 0 = classic (header + keys + vals), 1 = compact (64 B, implicit children)
    std::uint64_t partitioned;  // 1 = per-task bounds array instead of uniform slices
    std::uint64_t spare[5];
    std::uint64_t level_off[kMaxLevels];   // first node index of each level (0 = leaves), top-down layout
};

// Stats line of task t: [0] found, [1] sum of found lookup positions, [2] sum of found values,
// [3] sum_output: sum of out[] over the slice, [4] sum_output: nonzero slots.
struct SliceResult {
    std::uint64_t n;
    std::uint64_t sum_idx;
    std::uint64_t sum_val;
};

inline void slice(const Config& c, std::uint64_t t, std::uint64_t& lo, std::uint64_t& hi) {
    lo = t * c.per; if (lo > c.lookups) lo = c.lookups;
    hi = lo + c.per; if (hi > c.lookups) hi = c.lookups;
}

// Task t's lookups: bounds[t]..bounds[t+1] when the probes were range-partitioned, else slice().
inline void task_range(const Config& c, const std::uint64_t* bounds, std::uint64_t t, std::uint64_t& lo, std::uint64_t& hi) {
    if (c.partitioned && bounds) { lo = bounds[t]; hi = bounds[t + 1]; }
    else slice(c, t, lo, hi);
}

// Compact layout: lookups [lo, hi), one 64 B line per level.
inline SliceResult lookup_range_compact(const unsigned char* nodes, const Config& c, const Key* q, Val* out,
                                        std::uint64_t lo, std::uint64_t hi) {
    SliceResult r{0, 0, 0};
    for (std::uint64_t i = lo; i < hi; ++i) {
        const Key k = q[i];
        std::uint64_t cur = c.root;                       // absolute node index
        for (std::uint64_t h = c.height - 1; h > 0; --h) {
            const Key* keys = reinterpret_cast<const Key*>(nodes + cur * 64);
            std::uint64_t slot = 0;
            for (int j = 1; j < 8; ++j) slot += keys[j] <= k;
            cur = c.level_off[h - 1] + kCompactFan * (cur - c.level_off[h]) + slot;
        }
        const Key* lk = reinterpret_cast<const Key*>(nodes + cur * 64);
        Val v = 0;
        for (int j = 0; j < 4; ++j) if (lk[j] == k) v = lk[4 + j];
        if (out) out[i] = v;
        if (v) { ++r.n; r.sum_idx += i; r.sum_val += v; }
    }
    return r;
}

inline std::uint32_t node_n(const unsigned char* node) { return *reinterpret_cast<const std::uint32_t*>(node); }
inline const Key* node_keys(const unsigned char* node) { return reinterpret_cast<const Key*>(node + kNodeHeader); }
inline const std::uint64_t* node_vals(const unsigned char* node, std::uint64_t fanout) {
    return reinterpret_cast<const std::uint64_t*>(node + kNodeHeader + 8 * fanout);
}

// Child to descend into: the last i with keys[i] <= k (child 0 when k is below keys[1]).
inline std::uint64_t inner_child(const unsigned char* node, std::uint64_t fanout, Key k) {
    const Key* keys = node_keys(node);
    std::uint32_t lo = 1, hi = node_n(node);
    while (lo < hi) {
        const std::uint32_t mid = (lo + hi) >> 1;
        if (keys[mid] <= k) lo = mid + 1; else hi = mid;
    }
    return node_vals(node, fanout)[lo - 1];
}

// Value of k in a leaf, 0 if absent.
inline Val leaf_find(const unsigned char* node, std::uint64_t fanout, Key k) {
    const Key* keys = node_keys(node);
    const std::uint32_t n = node_n(node);
    std::uint32_t lo = 0, hi = n;
    while (lo < hi) {
        const std::uint32_t mid = (lo + hi) >> 1;
        if (keys[mid] < k) lo = mid + 1; else hi = mid;
    }
    return (lo < n && keys[lo] == k) ? node_vals(node, fanout)[lo] : 0;
}

// Prefetch for the next visit of `node`: nodes up to 512 B whole (one mu::prefetch); bigger ones
// only the header line and the line of the first binary-search probe (keys[F/2] when full) - the
// search touches ~log2(F) lines, so fetching a whole 4 KiB node wastes bandwidth.
template <typename Prefetch>
inline void prefetch_node(const unsigned char* node, std::uint64_t nb, std::uint64_t F, Prefetch pf) {
    if (nb <= 512) { pf(node, nb); return; }
    pf(node, 64);
    pf(node + kNodeHeader + 8 * (F / 2), 64);
}

// Lookups [lo, hi) of the probe array. With group G > 1 the lookups go in groups of G walked
// level by level in lockstep; each one prefetches the node it descends into, so a group has G
// node fetches in flight instead of 1 (group prefetching). G = 1 is the plain one-at-a-time loop
// with no prefetch. `pf(addr, bytes)` is the platform prefetch.
template <typename Prefetch>
inline SliceResult lookup_range(const unsigned char* nodes, const Config& c, const Key* q, Val* out,
                                std::uint64_t lo, std::uint64_t hi, Prefetch pf) {
    const std::uint64_t nb = c.node_bytes, F = c.fanout;
    const std::uint64_t G = c.group < 1 ? 1 : (c.group > kMaxGroup ? kMaxGroup : c.group);
    SliceResult r{0, 0, 0};
    std::uint64_t cur[kMaxGroup];
    Key key[kMaxGroup];
    for (std::uint64_t b = lo; b < hi; b += G) {
        const std::uint64_t m = hi - b < G ? hi - b : G;
        if (G > 1 && b + G < hi) pf(q + b + G, (hi - b - G < G ? hi - b - G : G) * sizeof(Key));
        for (std::uint64_t j = 0; j < m; ++j) { key[j] = q[b + j]; cur[j] = c.root; }
        for (std::uint64_t lvl = c.height; lvl > 1; --lvl)
            for (std::uint64_t j = 0; j < m; ++j) {
                cur[j] = inner_child(nodes + cur[j] * nb, F, key[j]);
                if (G > 1) prefetch_node(nodes + cur[j] * nb, nb, F, pf);
            }
        for (std::uint64_t j = 0; j < m; ++j) {
            const Val v = leaf_find(nodes + cur[j] * nb, F, key[j]);
            if (out) out[b + j] = v;
            if (v) { ++r.n; r.sum_idx += b + j; r.sum_val += v; }
        }
    }
    return r;
}

}  // namespace btree
