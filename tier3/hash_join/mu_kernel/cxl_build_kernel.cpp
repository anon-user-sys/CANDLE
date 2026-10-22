#include <cstdint>
#include <cstddef>

// POSIX memory-allocation stubs and C++ runtime symbols
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

#include <algorithm>

#include "mu/mu.hpp"
#include "mu_hash_join_config.hpp"
#include "../inline_hashtable.h"

// C23: task completion signal. Push the writes to memory (flushCacheAll), then raise the counter in memory.
static inline void signal_done(const application::hash_join::CxlHashJoinConfig* config) {
    if (config && config->done_addr) {
        mu::flushCacheAll();
        mu::atomicAdd(reinterpret_cast<std::int32_t*>(config->done_addr), 1);
    }
}

// CXL RISC-V Kernel - 1-Pass Concurrent Direct Build Kernel
void cxl_morsel_build_kernel(
    const std::uint32_t* left_keys, 
    const std::uint64_t* hashtable_ptrs, 
    const application::hash_join::CxlHashJoinConfig* config, 
    std::uint64_t* fail_counts, // insert-failure counts
    int numTasks)
{
    using namespace application::hash_join;

    if (!left_keys || !hashtable_ptrs || !config || config->count_cores == 0 || numTasks <= 0) {
        return;
    }

    int taskIdx = mu::getTaskIdx(); 
    if (taskIdx < 0 || taskIdx >= numTasks) {
        return;
    }

    const std::size_t result_stride = 64 / sizeof(std::uint64_t);
    const std::uint16_t num_cores = config->count_cores;

    std::size_t keys_per_task = (config->count_left_keys + numTasks - 1) / numTasks;
    std::size_t start_idx = std::min<std::size_t>(static_cast<std::size_t>(taskIdx) * keys_per_task, config->count_left_keys);
    std::size_t end_idx   = std::min<std::size_t>(start_idx + keys_per_task, config->count_left_keys);

    std::size_t local_fail_count = 0;

#if defined(__riscv)
    asm volatile("fence rw, rw" ::: "memory");
#endif

    // C18: config values hoisted out of the loop (AMOs change memory, so the compiler would reload them each time).
    // With table_stride, compute addresses instead of using the pointer array; with slot_mask, insert without reading the header.
    const std::uint64_t table_base   = config->table_base;
    const std::uint64_t table_stride = config->table_stride;
    const std::size_t   slot_mask    = static_cast<std::size_t>(config->slot_mask);

    for (std::size_t i = start_idx; i < end_idx; ++i) {
        std::uint32_t key = left_keys[i];
        std::uint16_t target_channel = hashtable_type::partition_hash(key) % num_cores;
        auto* target_ht = table_stride
            ? reinterpret_cast<hashtable_type*>(table_base + target_channel * table_stride)
            : reinterpret_cast<hashtable_type*>(hashtable_ptrs[target_channel]);

        if (target_ht) {
            // check the bool result
            bool inserted = (table_stride && slot_mask)
                ? hashtable_type::insert_at(&target_ht->at(0), slot_mask, key, static_cast<std::uint32_t>(i))
                : target_ht->insert_atomic_32(key, static_cast<std::uint32_t>(i));
            if (!inserted) {
                local_fail_count++;
            }
        } else {
            local_fail_count++;
        }
    }

    if (fail_counts) {
        fail_counts[taskIdx * result_stride] = static_cast<std::uint64_t>(local_fail_count);
    }
    signal_done(config);   // C23

#if defined(__riscv)
    asm volatile("fence rw, rw" ::: "memory");
#endif
}

MU_KERNEL_ADD(cxl_morsel_build_kernel)

// ===========================================================================================
// C19 owner-computes build (--owner-build). Split into two kernels to build without atomics.
// Every write goes to a 64 B-aligned region that only one task writes (no line shared between tasks).
// ===========================================================================================

// Kernel A: task t processes only its own slice. Per-partition counts (counts[t][p]) -> prefix offsets ->
// scatter (key,row) into its own region (pairs + t * region_stride) in partition order.
// At the end, local_off[t][p] = end of partition p's range (start = end - counts[t][p]).
void cxl_partition_kernel(
    const std::uint32_t* left_keys,
    application::hash_join::JoinTuple* pairs,
    std::uint32_t* counts,
    std::uint32_t* local_off,
    const application::hash_join::CxlHashJoinConfig* config,
    int numTasks)
{
    using namespace application::hash_join;
    if (!left_keys || !pairs || !counts || !local_off || !config || config->count_cores == 0 || numTasks <= 0) return;
    const int taskIdx = mu::getTaskIdx();
    if (taskIdx < 0 || taskIdx >= numTasks) return;

    const std::uint32_t P = config->count_cores;
    const std::size_t N = config->count_left_keys;
    const std::size_t per_task = (N + numTasks - 1) / numTasks;
    const std::size_t start = std::min<std::size_t>(static_cast<std::size_t>(taskIdx) * per_task, N);
    const std::size_t end = std::min<std::size_t>(start + per_task, N);

    std::uint32_t* cnt = counts + static_cast<std::size_t>(taskIdx) * config->own_row_stride;
    std::uint32_t* off = local_off + static_cast<std::size_t>(taskIdx) * config->own_row_stride;
    JoinTuple* region = pairs + static_cast<std::size_t>(taskIdx) * config->own_region_stride;

    for (std::uint32_t p = 0; p < P; ++p) cnt[p] = 0;
    for (std::size_t i = start; i < end; ++i) cnt[hashtable_type::partition_hash(left_keys[i]) % P]++;
    std::uint32_t run = 0;
    for (std::uint32_t p = 0; p < P; ++p) { off[p] = run; run += cnt[p]; }
    for (std::size_t i = start; i < end; ++i) {
        const std::uint32_t key = left_keys[i];
        region[off[hashtable_type::partition_hash(key) % P]++] = JoinTuple{key, static_cast<std::uint32_t>(i)};
    }
    mu::flushCacheAll();
    signal_done(config);   // C23
}

MU_KERNEL_ADD(cxl_partition_kernel)

// Kernel B: task t owns partitions p = t, t+T, ... It gathers p's (key,row) from every task's region
// and inserts them into its own table with ordinary stores. Counter/offset/pair/slot lines are prefetched in groups.
void cxl_owner_build_kernel(
    const application::hash_join::JoinTuple* pairs,
    const std::uint32_t* counts,
    const std::uint32_t* local_off,
    std::uint64_t* fail_counts,
    const application::hash_join::CxlHashJoinConfig* config,
    int numTasks)
{
    using namespace application::hash_join;
    if (!pairs || !counts || !local_off || !config || config->count_cores == 0 || config->table_stride == 0 || numTasks <= 0) return;
    const int taskIdx = mu::getTaskIdx();
    if (taskIdx < 0 || taskIdx >= numTasks) return;

    const std::uint32_t P = config->count_cores;
    const std::size_t T = static_cast<std::size_t>(numTasks);
    const std::size_t rs = config->own_row_stride;
    const std::size_t gs = config->own_region_stride;
    const std::size_t mask = static_cast<std::size_t>(config->slot_mask);
    const std::uint64_t table_base = config->table_base;
    const std::uint64_t table_stride = config->table_stride;
    const std::size_t result_stride = 64 / sizeof(std::uint64_t);

    constexpr std::size_t kT = 8;      // source tasks looked at per step
    constexpr std::size_t kPairs = 64; // pairs inserted per step
    std::size_t fails = 0;

    for (std::uint32_t p = static_cast<std::uint32_t>(taskIdx); p < P; p += static_cast<std::uint32_t>(T)) {
        auto* ht = reinterpret_cast<hashtable_type*>(table_base + p * table_stride);
        auto* base = &ht->at(0);
        std::uint32_t keys[kPairs];
        std::uint32_t vals[kPairs];
        std::size_t n_batch = 0;
        auto flush_batch = [&]() {
            for (std::size_t j = 0; j < n_batch; ++j) {
                const auto line = reinterpret_cast<std::uintptr_t>(&base[hashtable_type::bucket_hash(keys[j]) & mask]) & ~static_cast<std::uintptr_t>(63);
                mu::prefetch(reinterpret_cast<const void*>(line), 64);
            }
            for (std::size_t j = 0; j < n_batch; ++j) {
                if (!hashtable_type::insert_plain(base, mask, keys[j], vals[j])) ++fails;
            }
            n_batch = 0;
        };
        for (std::size_t t0 = 0; t0 < T; t0 += kT) {
            const std::size_t t1 = std::min(t0 + kT, T);
            for (std::size_t t = t0; t < t1; ++t) {
                mu::prefetch(reinterpret_cast<const void*>(reinterpret_cast<std::uintptr_t>(&counts[t * rs + p]) & ~static_cast<std::uintptr_t>(63)), 64);
                mu::prefetch(reinterpret_cast<const void*>(reinterpret_cast<std::uintptr_t>(&local_off[t * rs + p]) & ~static_cast<std::uintptr_t>(63)), 64);
            }
            std::uint32_t n[kT], s[kT];
            for (std::size_t t = t0; t < t1; ++t) {
                n[t - t0] = counts[t * rs + p];
                s[t - t0] = local_off[t * rs + p] - n[t - t0];
                if (n[t - t0]) {
                    mu::prefetch(reinterpret_cast<const void*>(reinterpret_cast<std::uintptr_t>(pairs + t * gs + s[t - t0]) & ~static_cast<std::uintptr_t>(63)), 64);
                }
            }
            for (std::size_t t = t0; t < t1; ++t) {
                const JoinTuple* src = pairs + t * gs + s[t - t0];
                for (std::uint32_t j = 0; j < n[t - t0]; ++j) {
                    keys[n_batch] = src[j].key;
                    vals[n_batch] = src[j].val;
                    if (++n_batch == kPairs) flush_batch();
                }
            }
        }
        flush_batch();
    }

    if (fail_counts) fail_counts[taskIdx * result_stride] = static_cast<std::uint64_t>(fails);
    mu::flushCacheAll();
    signal_done(config);   // C23
}

MU_KERNEL_ADD(cxl_owner_build_kernel)