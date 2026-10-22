#include <cstdint>
#include <cstddef>

// POSIX memory allocation stub & C++ runtime symbols
extern "C" {
    void* __dso_handle __attribute__((weak)) = nullptr;
    int __cxa_atexit(void (*destructor)(void*), void* arg, void* dso) {
        (void)destructor; (void)arg; (void)dso;
        return 0;
    }
    int posix_memalign(void** memptr, std::size_t alignment, std::size_t size) {
        (void)memptr; (void)alignment; (void)size;
        return -1;
    }
}

#include <limits>
#include <algorithm>
#include <stdio.h>

#include "mu/mu.hpp"
#include "mu_hash_join_config.hpp"
#include "../inline_hashtable.h"

namespace {
using namespace application::hash_join;

// Probe rows [start_idx, end_idx) and append the results at task_output[count..]. Returns the new count.
// (C20 moved the kernel body into this function, shared by the plain, batched and resident kernels)
std::size_t probe_rows(const std::uint32_t* right_keys, const std::uint32_t* filter_values,
                       const std::uint64_t* hashtable_ptrs, JoinTuple* task_output, std::size_t max_results,
                       std::size_t count, const CxlHashJoinConfig* config,
                       std::size_t start_idx, std::size_t end_idx)
{
    const std::uint16_t num_cores = config->count_cores;
    const std::uint32_t threshold = config->filter_threshold;
    const std::size_t count_left_keys = config->count_left_keys;

    auto emit = [&](std::uint32_t key, std::uint32_t val) {
        if (val != std::numeric_limits<std::uint32_t>::max() && val < count_left_keys) {
            if (task_output && count < max_results) {
                task_output[count] = JoinTuple{key, val};
            }
            count++;
        }
    };

    // C18: back-to-back tables: compute the address instead of using the pointer array
    const std::uint64_t table_base   = config->table_base;
    const std::uint64_t table_stride = config->table_stride;
    auto table_at = [&](std::uint16_t ch) -> const hashtable_type* {
        return table_stride ? reinterpret_cast<const hashtable_type*>(table_base + ch * table_stride)
                            : reinterpret_cast<const hashtable_type*>(hashtable_ptrs[ch]);
    };

    const std::uint32_t window = config->prefetch_window;
    if (window == 0 || config->slot_mask == 0) {
        // original probe: one key at a time, reading the table header on every lookup
        for (std::size_t i = start_idx; i < end_idx; ++i) {
            if (filter_values[i] > threshold) continue;
            const std::uint32_t key = right_keys[i];
            const auto* target_ht = table_at(hashtable_type::partition_hash(key) % num_cores);
            if (target_ht) emit(key, target_ht->get(key));
        }
        return count;
    }

    // Batched prefetch probe (C15). Step 1: gather `window` keys that pass the filter,
    // compute their slots and mu::prefetch the bucket lines. Step 2: look them up. Same results as window=0.
    constexpr std::size_t kMaxWindow = 32;
    const std::size_t w = window < kMaxWindow ? window : kMaxWindow;
    const std::size_t mask = static_cast<std::size_t>(config->slot_mask);
    const hashtable_type::entry_type* base[kMaxWindow];
    std::size_t slot[kMaxWindow];
    std::uint32_t keys[kMaxWindow];

    std::size_t i = start_idx;
    while (i < end_idx) {
        std::size_t n = 0;
        for (; i < end_idx && n < w; ++i) {
            if (filter_values[i] > threshold) continue;
            const std::uint32_t key = right_keys[i];
            const auto* ht = table_at(hashtable_type::partition_hash(key) % num_cores);
            if (!ht) continue;
            keys[n] = key;
            base[n] = ht->entries();          // pointer arithmetic only, no header read
            slot[n] = hashtable_type::bucket_hash(key) & mask;
            const auto line = reinterpret_cast<std::uintptr_t>(&base[n][slot[n]]) & ~static_cast<std::uintptr_t>(63);
            mu::prefetch(reinterpret_cast<const void*>(line), 64);
            ++n;
        }
        for (std::size_t j = 0; j < n; ++j) {
            emit(keys[j], hashtable_type::lookup(base[j], mask, keys[j], slot[j]));
        }
    }
    return count;
}

// C23: task completion signal. Write the results to memory (flushCacheAll), then raise the counter in memory.
inline void signal_done(const CxlHashJoinConfig* config) {
    if (config && config->done_addr) {
        mu::flushCacheAll();
        mu::atomicAdd(reinterpret_cast<std::int32_t*>(config->done_addr), 1);
    }
}

// Split [begin, end) evenly across tasks (same split as the other kernels)
inline void task_share(std::size_t begin, std::size_t end, int taskIdx, int numTasks, std::size_t& s, std::size_t& e) {
    const std::size_t n = end - begin;
    const std::size_t per = (n + numTasks - 1) / numTasks;
    s = begin + std::min<std::size_t>(static_cast<std::size_t>(taskIdx) * per, n);
    e = begin + std::min<std::size_t>(s - begin + per, n);
}
} // namespace

// CXL RISC-V Kernel - Parallel Lock-Free Materializing Probe.
// C20: if config->probe_end != 0, probe only [probe_begin, probe_end) (batched NDP-sync);
//      if probe_accumulate != 0, append results after the existing match_counts.
void cxl_morsel_probe_kernel(
    const std::uint32_t* right_keys, 
    const std::uint32_t* filter_values,
    std::uint64_t* hashtable_ptrs, 
    application::hash_join::JoinTuple* result_buffers,
    std::size_t max_results_per_task,
    std::uint64_t* match_counts,
    const application::hash_join::CxlHashJoinConfig* config, 
    int numTasks)
{
    if (!right_keys || !filter_values || !hashtable_ptrs || !match_counts || !config || config->count_cores == 0 || config->count_right_keys == 0 || numTasks <= 0) {
        return;
    }
    const int taskIdx = mu::getTaskIdx(); 
    if (taskIdx < 0 || taskIdx >= numTasks) {
        return;
    }
    const std::size_t result_stride = 64 / sizeof(std::uint64_t);
    const std::size_t begin = config->probe_end ? config->probe_begin : 0;
    const std::size_t end   = config->probe_end ? config->probe_end : config->count_right_keys;
    std::size_t s, e;
    task_share(begin, end, taskIdx, numTasks, s, e);

    JoinTuple* task_output = result_buffers ? (result_buffers + (static_cast<std::size_t>(taskIdx) * max_results_per_task)) : nullptr;
    std::size_t prev = config->probe_accumulate ? static_cast<std::size_t>(match_counts[taskIdx * result_stride]) : 0;
    std::size_t cap = max_results_per_task;
    if (config->batch_cap) {   // C25a: this batch's contiguous result block
        cap = static_cast<std::size_t>(config->batch_cap);
        task_output = reinterpret_cast<JoinTuple*>(config->batch_out_base) + static_cast<std::size_t>(taskIdx) * cap;
        prev = 0;
    }
    const std::size_t count = probe_rows(right_keys, filter_values, hashtable_ptrs, task_output, cap,
                                         prev, config, s, e);
    match_counts[taskIdx * result_stride] = static_cast<std::uint64_t>(count);
    signal_done(config);   // C23
}

MU_KERNEL_ADD(cxl_morsel_probe_kernel)

// C20 NDP@CXL: a kernel launched once that stays resident and processes the probe batches the host publishes.
// ctrl (CXL memory, two 64 B lines, published by the host with NT stores):
//   line 0: [0] seq (batch number, from 1), [1] begin, [2] end, [3] stop, [4] result block address (C25a)
//   line 1: [8] done (int32, atomicAdd in memory each time a task finishes a batch)
// A task writes its results and match_count, then flushCacheAll (the rule for publishing inside a kernel, §4.4.4),
// then raises done. The host publishes the next batch once done == T x number of batches.
void cxl_resident_probe_kernel(
    const std::uint32_t* right_keys,
    const std::uint32_t* filter_values,
    std::uint64_t* hashtable_ptrs,
    application::hash_join::JoinTuple* result_buffers,
    std::size_t max_results_per_task,
    std::uint64_t* match_counts,
    const application::hash_join::CxlHashJoinConfig* config,
    std::uint64_t* ctrl,
    int numTasks)
{
    if (!right_keys || !filter_values || !match_counts || !config || !ctrl || config->count_cores == 0 || numTasks <= 0) {
        return;
    }
    const int taskIdx = mu::getTaskIdx();
    if (taskIdx < 0 || taskIdx >= numTasks) {
        return;
    }
    const std::size_t result_stride = 64 / sizeof(std::uint64_t);
    JoinTuple* task_output = result_buffers ? (result_buffers + (static_cast<std::size_t>(taskIdx) * max_results_per_task)) : nullptr;
    std::size_t count = 0;
    std::uint64_t last = 0;
    volatile std::uint64_t* line0 = ctrl;

    for (;;) {
        mu::flushCache(ctrl, 64);                 // drop the previously read ctrl line and re-read it from memory
        const std::uint64_t seq  = line0[0];
        const std::uint64_t stop = line0[3];
        if (stop) break;
        if (seq == last) {
            for (volatile int d = 0; d < 256; ++d) {}   // back off: less memory traffic on the ctrl line while waiting
            continue;
        }
        last = seq;
        std::size_t s, e;
        task_share(static_cast<std::size_t>(line0[1]), static_cast<std::size_t>(line0[2]), taskIdx, numTasks, s, e);
        if (config->batch_cap) {   // C25a: write to the contiguous block the host published in [4] for this batch
            const std::size_t cap = static_cast<std::size_t>(config->batch_cap);
            JoinTuple* out = reinterpret_cast<JoinTuple*>(line0[4]) + static_cast<std::size_t>(taskIdx) * cap;
            count = probe_rows(right_keys, filter_values, hashtable_ptrs, out, cap, 0, config, s, e);
        } else {
            count = probe_rows(right_keys, filter_values, hashtable_ptrs, task_output, max_results_per_task,
                               count, config, s, e);
        }
        match_counts[taskIdx * result_stride] = static_cast<std::uint64_t>(count);
        mu::flushCacheAll();                      // after writing the results and the count to memory
        mu::atomicAdd(reinterpret_cast<std::int32_t*>(ctrl + 8), 1);   // completion signal (performed in memory)
    }
    mu::flushCacheAll();
}

MU_KERNEL_ADD(cxl_resident_probe_kernel)
