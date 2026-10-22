#pragma once
// Shared by the host driver and the MU kernel, so host and device run the identical loop.
//
// Workload: the probe-side scan of the 260920 Filter hash join, without the join:
//
//   SELECT l_orderkey FROM lineitem WHERE l_quantity <= threshold
//
// For every row, read l_quantity (uint8); if it passes, read l_orderkey (Key) - the key the
// probe would look up - and either count it or write it to this task's output slice.
// l_quantity is uniform in [1,50], so threshold 0/25/50 gives 0%/50%/100% pass, exactly as
// -f 0/25/50 does in hash-join-xcena (see cxl_probe_kernel.cpp).
#include <cstdint>

namespace scanfilter {

// l_orderkey. TPC-H order keys reach ~6e6 x SF, so uint32 overflows above ~sf_700; uint64 keeps
// one format for every scale factor (9 B/row with l_quantity).
using Key = std::uint64_t;

struct Config {
    std::uint64_t rows;           // lineitem row count
    std::uint64_t par;            // tasks (device) or threads (host)
    std::uint64_t threshold;      // predicate: l_quantity <= threshold
    std::uint64_t max_per_task;   // capacity of each task's output slice (= slice length)
    std::uint64_t materialize;    // 0 = count only, 1 = also write passing l_orderkey
    std::uint64_t spare0;         // 1 = flush own output range before the global flush (diagnostic)
    std::uint64_t spare1;         // device prefetch window in 64-row blocks (0 = off)
    std::uint64_t spare2;         // 1 = do not prefetch the key column
    std::uint64_t chunk_rows;     // scan_filter_pub: rows per published chunk (return to host)
    std::uint64_t step;           // scan_filter_step: which part of its slice each task scans now
    std::uint64_t steps;          // (unused: step/steps are passed as a kernel argument)
    std::uint64_t write_mode;     // device materialize: 0 = plain stores, 1 = whole 64 B lines via memcpyGlobal
    std::uint64_t out4;           // materialize as uint32 offsets from the task's base key (keys[slice lo])
};

struct SliceResult {
    std::uint64_t n;        // passing rows
    std::uint64_t sum_idx;  // wrapping sum of passing row indices (which rows passed)
    std::uint64_t sum_key;  // wrapping sum of passing l_orderkey   (the key column was read)
};

// Slice [lo,hi) owned by task t of par - same split as cxl_morsel_probe_kernel.
inline void slice(std::uint64_t rows, std::uint64_t par, std::uint64_t t,
                  std::uint64_t& lo, std::uint64_t& hi) {
    const std::uint64_t per = (rows + par - 1) / par;
    lo = t * per; if (lo > rows) lo = rows;
    hi = lo + per; if (hi > rows) hi = rows;
}

// The scan itself. Branchy on purpose: the hash-join probe skips the key read for filtered
// rows, so this keeps its access pattern (and its branch behaviour at 50%). Out = Key writes the
// key; Out = uint32_t writes key - base (frame of reference: lineitem is in l_orderkey order, so
// every key of a task's slice lies in [keys[lo], keys[lo] + 2^32), checked by the host first).
template <typename Out>
inline SliceResult scan_slice_out(const std::uint8_t* qty, const Key* keys,
                                  Out* dst, std::uint64_t lo, std::uint64_t hi,
                                  std::uint32_t threshold, Key base) {
    SliceResult r{0, 0, 0};
    for (std::uint64_t i = lo; i < hi; ++i) {
        if (qty[i] > threshold) continue;
        const Key key = keys[i];
        if (dst) dst[r.n] = static_cast<Out>(key - base);
        ++r.n;
        r.sum_idx += i;
        r.sum_key += key;
    }
    return r;
}

inline SliceResult scan_slice(const std::uint8_t* qty, const Key* keys,
                              Key* dst, std::uint64_t lo, std::uint64_t hi,
                              std::uint32_t threshold) {
    return scan_slice_out<Key>(qty, keys, dst, lo, hi, threshold, 0);
}

}  // namespace scanfilter
