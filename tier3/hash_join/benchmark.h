#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <tuple>
#include <vector>
#include <chrono>
#include <limits>

#include "join_task.h"
#include "tiered_alloc.h"

#if defined(USE_CXL)
#include "pxl_helper.hpp"
#include "mu_kernel/mu_hash_join_config.hpp"
#endif

namespace application::hash_join {

class MergeTask;

struct TPCHTableReader {
    static void read(const std::string &file_name, std::vector<std::uint32_t>& target_keys, std::uint16_t column_index);
    static void read_text(const std::string &file_name, std::vector<std::uint32_t>& target_keys, std::uint16_t column_index);
};

class Benchmark {
public:
    Benchmark(
        std::vector<std::uint16_t> &&core_counts, std::uint16_t iterations, std::vector<std::uint32_t> &&batches,
        std::tuple<std::pair<std::string, std::uint16_t>, std::pair<std::string, std::uint16_t>> &&join_table_files,
        std::string &&result_file_name, double selectivity_pct = 100.0, std::uint16_t partitions = 0U,
        const tiered::Options &tier_options = {}, std::uint32_t prefetch_window = 0U, bool owner_build = false,
        std::uint32_t probe_batches = 0U, bool resident = false, std::uint32_t mu_batch = 0U,
        bool host_direct_probe = false, bool build_input_cxl = false);

    ~Benchmark();

    std::uint16_t next_core_count();
    
    void run_build(); // Phase 1: Build Phase
    void run_probe(); // Phase 3: Probe Phase
    void run_merge(); // Phase 4: Merge & Reporting Phase

private:
    void clear_hash_tables();

    // number of hash tables (partitions). Without -p (0), the core count, as before
    std::uint16_t partition_count() const noexcept;
    // device kernel task count (capped at the usable MU threads)
    std::uint32_t device_task_count() const noexcept;

    const std::uint16_t _partitions;

    std::vector<std::uint16_t> _core_counts;
    std::size_t _current_core_index{0U};

    const std::uint16_t _iterations;
    std::uint16_t _current_iteration = std::numeric_limits<std::uint16_t>::max();

    const std::vector<std::uint32_t> _batches;
    std::uint16_t _current_batch_index{0U};

    const std::string _result_file_name;
    std::tuple<std::vector<std::uint32_t>, std::vector<std::uint32_t>> _join_keys;
    std::vector<std::uint32_t> _filter_values; 
    std::uint32_t _filter_threshold;
    
    std::vector<InlineHashtable<std::uint32_t, std::uint32_t>*> _hash_tables;
    std::unique_ptr<MergeTask> _merge_task;

    std::chrono::high_resolution_clock::time_point _start_time;
    std::chrono::high_resolution_clock::time_point _pure_build_end_time;
    std::chrono::high_resolution_clock::time_point _build_end_time;
    std::chrono::high_resolution_clock::time_point _probe_start_time;
    std::chrono::high_resolution_clock::time_point _pure_probe_end_time;
    std::chrono::high_resolution_clock::time_point _probe_end_time;

#if defined(MODE_HOST_DRAM)
    std::vector<std::uint8_t> _host_table_arena;   // C18: every partition back to back
#endif

    const tiered::Options _tier_options;
    const std::uint32_t _prefetch_window;   // device batched-probe window (0 = off)
    const bool _owner_build;                // C19: Mode 4 owner-computes build (no atomics); C29: host version for Modes 2/3
    const bool _build_input_cxl;            // C30: the Modes 2/3 host build reads the orders keys from the CXL copy
    bool _golden_ready{false};              // C34: reference join result (once per process / cached)
    std::size_t _golden_tuples{0};
    std::uint64_t _golden_checksum{0};
    double _own_partition_ms{0.0};          // C19 diagnostics: kernel A run time
    double _own_insert_ms{0.0};             // C19 diagnostics: kernel B run time
    const std::uint32_t _probe_batches;     // C20: probe in K batches (0 = all at once)
    const bool _resident;                   // C20: NDP@CXL resident probe kernel (Mode 3/4, batched)
    const std::uint32_t _mu_batch;          // C21: tasks per MU thread (0 = runtime default 16)
    const bool _host_direct_probe;          // C26: host probe without routing (Modes 1, 2, 5)
    std::size_t _max_results_per_task{0};   // device result area size (used by the merge)
    std::size_t _batch_cap{0};              // C25a: result slots per task in batch mode (0 = not the batch layout)
    std::vector<std::uint64_t> _batch_counts; // C25a: [batch k][task t] result counts (read by the host at handoff)
#if defined(MODE_HOST_TIERED)
    // Mode 5: input columns and the hash tables (one arena) placed in DRAM/CXL at the cxl_pct ratio
    tiered::PlacedBuffer _tier_left_keys;
    tiered::PlacedBuffer _tier_right_keys;
    tiered::PlacedBuffer _tier_filter_values;
    tiered::PlacedBuffer _tier_hashtable_arena;
#endif

#if defined(USE_CXL)
    std::unique_ptr<PxlResources> _pxlResources;
    PxlUniquePtr<application::hash_join::CxlHashJoinConfig> _cxl_config;
    PxlUniquePtr<std::uint32_t> _cxl_left_keys;
    PxlUniquePtr<std::uint32_t> _cxl_right_keys;
    PxlUniquePtr<std::uint32_t> _cxl_filter_values;
    PxlUniquePtr<std::uint64_t> _cxl_ht_ptrs_mem;
    PxlUniquePtr<std::uint64_t> _cxl_done;          // C23: kernel completion counter (one 64 B line)
    std::size_t _early_sync_events{0};              // C23: times synchronize() returned with only some tasks done
    PxlUniquePtr<std::uint8_t> _cxl_table_arena;   // C18: every partition back to back in one memAlloc
    std::size_t _table_stride{0};
#endif

#if defined(MODE_CXL_EXPANDER) || defined(MODE_CXL_HYBRID)
    // C29 host owner-computes build: partition-ordered (key,row) buffers live in CXL, like the device version (C19).
    // Kept across iterations (allocation and first touch outside the timer). Count/offset rows are in host DRAM (small).
    PxlUniquePtr<std::uint64_t> _host_own_pairs;
    std::size_t _host_own_pairs_count{0};
    std::vector<std::uint32_t> _host_own_counts;    // [task][partition], row stride own_row_stride
    std::vector<std::uint32_t> _host_own_offsets;
    std::vector<std::uint32_t> _host_own_part_begin; // partition p range = [begin[p], begin[p+1])
#endif

#if defined(MODE_CXL_HYBRID) || defined(MODE_CXL_FULL_NMC)
    PxlUniquePtr<JoinTuple> _cxl_result_buffers;
    PxlUniquePtr<std::uint64_t> _cxl_match_counts;
    PxlUniquePtr<std::uint64_t> _cxl_resident_ctrl;   // C20: the resident kernel's two control lines
#endif

#if defined(MODE_CXL_FULL_NMC)
    PxlUniquePtr<std::uint64_t> _cxl_build_fail_counts;
    // C19 owner-computes build buffers (per-task regions/rows, 64 B aligned)
    PxlUniquePtr<JoinTuple> _cxl_own_pairs;
    PxlUniquePtr<std::uint32_t> _cxl_own_counts;
    PxlUniquePtr<std::uint32_t> _cxl_own_offsets;
#endif
};

} // namespace application::hash_join