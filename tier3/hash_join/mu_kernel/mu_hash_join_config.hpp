#pragma once
#include <cstdint>
#include <cstddef>

namespace application::hash_join {

// Shared result tuple structure across device kernel and host application
struct JoinTuple {
    std::uint32_t key;
    std::uint32_t val;
};

struct CxlHashJoinConfig {
    std::size_t count_left_keys;    // build relation (-R, default orders.tbl) record count
    std::size_t count_right_keys;   // probe relation (-S, default lineitem.tbl) record count
    std::uint16_t count_cores;      // number of partitions (hash tables), independent of the kernel's numTasks
    std::uint32_t filter_threshold;
    std::uint64_t slot_mask{0};       // slots per partition - 1 (every partition the same size)
    std::uint32_t prefetch_window{0}; // 0 = original per-key probe, 1..32 = batched prefetch probe
    std::uint64_t table_base{0};      // C18: partition p's table = table_base + p * table_stride
    std::uint64_t table_stride{0};    //      (0 = use the hashtable_ptrs array)
    // C19 owner-computes build (--owner-build)
    std::uint64_t own_region_stride{0}; // spacing of the per-task (key,row) regions, in JoinTuples (multiple of 8 = 64 B)
    std::uint32_t own_row_stride{0};    // spacing of the per-task partition-counter rows, in uint32 (multiple of 16 = 64 B)
    // C20 batched probe (--probe-batches): if probe_end != 0, probe only [probe_begin, probe_end)
    std::uint64_t probe_begin{0};
    std::uint64_t probe_end{0};
    std::uint32_t probe_accumulate{0};  // 1 = append results after the existing match_counts
    // C23: address of the completion counter (one 64 B line). If non-zero, every task does flushCacheAll at the end
    // and then atomicAdd(+1) in memory. After synchronize(), the host waits until it equals the task count.
    std::uint64_t done_addr{0};
    // C25a batch result layout: if batch_cap != 0, this batch's results go to one contiguous block.
    // Task t's results = (JoinTuple*)batch_out_base + t * batch_cap, match_counts[t] = this batch's count.
    // The host then invalidates one block per batch (instead of 3,048 small per-task flushes).
    std::uint64_t batch_out_base{0};
    std::uint64_t batch_cap{0};
};

} // namespace application::hash_join