#include <iostream>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <cstring>
#include <cstdio>
#include <limits>
#include <stdexcept>
#include <unordered_map>
#include <chrono>
#include <thread>
#include <atomic>
#include <cstdlib>
#include <sys/stat.h>
#include <unistd.h>

#include "benchmark.h"

using namespace application::hash_join;

#ifdef ENABLE_CHECKSUM
inline std::uint64_t mix_pair(std::uint32_t key, std::uint32_t val) {
    std::uint64_t x = (static_cast<std::uint64_t>(key) << 32) | static_cast<std::uint32_t>(val);
    x ^= x >> 30;
    x *= 0xbf58476d1ce4e5b9ULL;
    x ^= x >> 27;
    x *= 0x94d049bb133111ebULL;
    x ^= x >> 31;
    return x;
}
#endif

#if defined(USE_CXL)
template <typename T>
inline auto allocatePxlMemoryAligned(pxl::runtime::Context* context, std::size_t count) {
    constexpr std::size_t ALIGNMENT = 64;
    std::size_t total_bytes = count * sizeof(T);
    std::size_t aligned_bytes = (total_bytes + ALIGNMENT - 1) & ~(ALIGNMENT - 1);
    std::size_t aligned_count = aligned_bytes / sizeof(T);

    auto mem = allocatePxlMemory<T>(context, aligned_count);

    if (reinterpret_cast<std::uintptr_t>(mem.get()) % ALIGNMENT != 0) {
        throw std::runtime_error("[❌ CRITICAL ERROR] CXL Allocated Memory Address is NOT 64-byte aligned!");
    }

    return mem;
}

// Bulk flush/invalidate in parallel on several host workers (C17): the range is cut into 4 MiB pieces,
// spread over the MorselScheduler workers, and we wait for all of them. Each task ends with an sfence.
// A single-thread flush right after waiting for a kernel runs on a low-clocked core and is slow (measured 2026-10-16).
class FlushRangesTask final : public ::Task {
public:
    std::vector<std::pair<void*, std::size_t>> ranges;
    void execute(std::uint16_t) override {
        for (const auto& [p, n] : ranges) pxl::flushHostCache(p, n);
        _mm_sfence();
    }
};

// C23: device kernel completion counter. Every task adds 1 in memory at the end, after flushCacheAll.
static void reset_done(std::uint64_t* done) {
    *reinterpret_cast<volatile std::int32_t*>(done) = 0;
    pxl_helper::safeFlushHostCache(done, 64);
}
static std::int64_t read_done(std::uint64_t* done) {
    pxl::flushHostCache(done, 64);
    _mm_mfence();
    return *reinterpret_cast<volatile std::int32_t*>(done);
}
// Call after synchronize(): wait until every task has signalled completion, and record/print it if synchronize()
// returned first. Resets the counter to 0 for the next launch.
static void wait_done(std::uint64_t* done, std::uint32_t tasks, const char* what, std::size_t& early_events) {
    const std::int64_t first = read_done(done);
    std::int64_t v = first;
    if (v < tasks) {
        ++early_events;
        const auto t0 = std::chrono::steady_clock::now();
        while (v < tasks) {
            if (std::chrono::steady_clock::now() - t0 > std::chrono::seconds(30)) {
                throw std::runtime_error(std::string(what) + ": only " + std::to_string(v) + "/" + std::to_string(tasks) +
                                         " tasks signalled done 30 s after synchronize() (" + std::to_string(first) + " at return)");
            }
            _mm_pause();
            v = read_done(done);
        }
        std::cerr << "[DIAG] " << what << ": synchronize() returned with " << first << "/" << tasks
                  << " tasks done; all done after "
                  << std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count() << " ms\n";
    } else if (v > tasks) {
        std::cerr << "[DIAG] " << what << ": done counter " << v << " > " << tasks << " tasks\n";
    }
    reset_done(done);
}

static void parallel_flush(const std::vector<std::pair<void*, std::size_t>>& ranges) {
    constexpr std::size_t kChunk = std::size_t{4} << 20;
    std::vector<std::pair<void*, std::size_t>> chunks;
    for (const auto& [p, n] : ranges) {
        for (std::size_t off = 0; off < n; off += kChunk) {
            chunks.emplace_back(static_cast<char*>(p) + off, std::min(kChunk, n - off));
        }
    }
    auto& scheduler = MorselScheduler::get();
    const std::size_t workers = std::min<std::size_t>(scheduler.host_thread_count(), chunks.size());
    if (workers <= 1) {
        for (const auto& [p, n] : chunks) pxl::flushHostCache(p, n);
        _mm_sfence();
        return;
    }
    std::vector<std::unique_ptr<FlushRangesTask>> tasks(workers);
    for (auto& t : tasks) t = std::make_unique<FlushRangesTask>();
    for (std::size_t i = 0; i < chunks.size(); ++i) tasks[i % workers]->ranges.push_back(chunks[i]);
    for (std::size_t w = 0; w < workers; ++w) scheduler.spawn(std::move(tasks[w]), static_cast<std::uint16_t>(w));
    scheduler.wait_until_finished();
}
#endif

// C33 (T21): binary column cache. Parsing the .tbl text took ~22 min per sf_100 process (lineitem is read
// twice); the join itself takes seconds. Cache file = <dir>/<basename>.c<col>.<size>.<mtime>.u32, so a changed
// source file never matches. dir = $HJ_COL_CACHE or ./col_cache (run scripts cd to the project root);
// HJ_COL_CACHE=off disables it. Written to a temp name and renamed, so a killed run leaves no partial file.
static std::string column_cache_path(const std::string& file_name, std::uint16_t column_index) {
    const char* env = std::getenv("HJ_COL_CACHE");
    const std::string dir = env ? env : "col_cache";
    if (dir == "off") return {};
    struct stat st{};
    if (::stat(file_name.c_str(), &st) != 0 || ::stat(dir.c_str(), &st) != 0) return {};
    ::stat(file_name.c_str(), &st);
    const auto slash = file_name.find_last_of('/');
    const std::string base = slash == std::string::npos ? file_name : file_name.substr(slash + 1);
    return dir + "/" + base + ".c" + std::to_string(column_index) + "." + std::to_string(st.st_size) + "." +
           std::to_string(st.st_mtime) + ".u32";
}

void TPCHTableReader::read(const std::string &file_name, std::vector<std::uint32_t>& target_keys, std::uint16_t column_index) {
    const std::string cache = column_cache_path(file_name, column_index);
    if (!cache.empty()) {
        std::ifstream in(cache, std::ios::binary | std::ios::ate);
        if (in.is_open()) {
            const auto bytes = static_cast<std::size_t>(in.tellg());
            if (bytes % sizeof(std::uint32_t) == 0) {
                target_keys.resize(bytes / sizeof(std::uint32_t));
                in.seekg(0);
                if (in.read(reinterpret_cast<char*>(target_keys.data()), static_cast<std::streamsize>(bytes))) {
                    std::cerr << "[CACHE] " << cache << " (" << target_keys.size() << " values)\n";
                    return;
                }
            }
            target_keys.clear();
        }
    }
    read_text(file_name, target_keys, column_index);
    if (!cache.empty()) {
        const std::string tmp = cache + ".tmp" + std::to_string(::getpid());
        std::ofstream out(tmp, std::ios::binary);
        if (out.write(reinterpret_cast<const char*>(target_keys.data()),
                      static_cast<std::streamsize>(target_keys.size() * sizeof(std::uint32_t))) && (out.close(), true) &&
            std::rename(tmp.c_str(), cache.c_str()) == 0) {
            std::cerr << "[CACHE] wrote " << cache << "\n";
        } else {
            std::remove(tmp.c_str());
        }
    }
}

void TPCHTableReader::read_text(const std::string &file_name, std::vector<std::uint32_t>& target_keys, std::uint16_t column_index) {
    std::ifstream tpc_file(file_name);
    if (!tpc_file.is_open()) {
        std::cerr << "[❌ ERROR] cannot open the TPC-H data file: " << file_name << std::endl;
        std::exit(1);
    }
    std::string line;
    while (std::getline(tpc_file, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        auto line_stream = std::stringstream{line};
        std::string column; std::uint16_t index = 0U;
        while (std::getline(line_stream, column, '|')) {
            if (index == column_index && !column.empty()) {
                target_keys.emplace_back(std::stoul(column));
            }
            index++;
        }
    }
}

Benchmark::Benchmark(
    std::vector<std::uint16_t> &&core_counts, const std::uint16_t iterations, std::vector<std::uint32_t> &&batches,
    std::tuple<std::pair<std::string, std::uint16_t>, std::pair<std::string, std::uint16_t>> &&join_table_files,
    std::string &&result_file_name, double selectivity_pct, std::uint16_t partitions,
    const tiered::Options &tier_options, std::uint32_t prefetch_window, bool owner_build,
    std::uint32_t probe_batches, bool resident, std::uint32_t mu_batch, bool host_direct_probe, bool build_input_cxl)
    : _core_counts(std::move(core_counts)), _partitions(partitions), _iterations(iterations), _batches(std::move(batches)), _result_file_name(std::move(result_file_name)),
      _tier_options(tier_options), _prefetch_window(prefetch_window), _owner_build(owner_build), _build_input_cxl(build_input_cxl),
      _probe_batches(probe_batches), _resident(resident), _mu_batch(mu_batch), _host_direct_probe(host_direct_probe) {
    
    std::vector<std::uint32_t> left_keys, right_keys;
    TPCHTableReader::read(std::get<0>(std::get<0>(join_table_files)), left_keys, std::get<1>(std::get<0>(join_table_files)));
    TPCHTableReader::read(std::get<0>(std::get<1>(join_table_files)), right_keys, std::get<1>(std::get<1>(join_table_files)));
    _join_keys = std::make_tuple(std::move(left_keys), std::move(right_keys));

    TPCHTableReader::read(std::get<0>(std::get<1>(join_table_files)), _filter_values, 4);
    
    // compute the integer threshold once, for an l_quantity maximum of 50
    std::uint32_t max_val = 50; 
    _filter_threshold = static_cast<std::uint32_t>(max_val * (std::clamp(selectivity_pct, 0.0, 100.0) / 100.0));

#if defined(MODE_HOST_TIERED)
    // the input is assumed to already live in its placement: place and fill once (outside the timer)
    tiered::check_nodes(_tier_options);
    auto place_copy = [this](tiered::PlacedBuffer& dst, const std::vector<std::uint32_t>& src, const char* name) {
        const std::size_t bytes = src.size() * sizeof(std::uint32_t);
        dst = tiered::PlacedBuffer(std::max<std::size_t>(bytes, 1), _tier_options);
        std::memcpy(dst.data(), src.data(), bytes);
        tiered::report_placement(name, dst.data(), bytes, _tier_options);
    };
    place_copy(_tier_left_keys, std::get<0>(_join_keys), "build_keys");
    place_copy(_tier_right_keys, std::get<1>(_join_keys), "probe_keys");
    place_copy(_tier_filter_values, _filter_values, "filter_values");
#endif
}

Benchmark::~Benchmark() {
    clear_hash_tables();

#if defined(USE_CXL)
    _cxl_left_keys.reset();
    _cxl_right_keys.reset();
    _cxl_filter_values.reset();
#if defined(MODE_CXL_EXPANDER) || defined(MODE_CXL_HYBRID)
    _host_own_pairs.reset();   // C29: free before the context
#endif
    _pxlResources.reset();
#endif
}

std::uint16_t Benchmark::partition_count() const noexcept {
    return _partitions != 0U ? _partitions : _core_counts[_current_core_index];
}

std::uint32_t Benchmark::device_task_count() const noexcept {
#if defined(USE_CXL)
    return std::min<std::uint32_t>(_core_counts[_current_core_index], kMaxDeviceTasks);
#else
    return _core_counts[_current_core_index];
#endif
}

void Benchmark::clear_hash_tables() {
    _hash_tables.clear();
    _merge_task.reset();

#if defined(MODE_HOST_DRAM)
    _host_table_arena.clear();
    _host_table_arena.shrink_to_fit();
#endif

#if defined(MODE_HOST_TIERED)
    _tier_hashtable_arena.reset();
#endif

#if defined(USE_CXL)
    _cxl_config.reset();
    _cxl_done.reset();
    _early_sync_events = 0;
    _cxl_table_arena.reset();
    _cxl_ht_ptrs_mem.reset();
#endif

#if defined(MODE_CXL_HYBRID) || defined(MODE_CXL_FULL_NMC)
    _cxl_result_buffers.reset();
    _cxl_match_counts.reset();
    _cxl_resident_ctrl.reset();
#endif

#if defined(MODE_CXL_FULL_NMC)
    _cxl_build_fail_counts.reset(); // reset when initializing MODE_CXL_FULL_NMC
    _cxl_own_pairs.reset();
    _cxl_own_counts.reset();
    _cxl_own_offsets.reset();
#endif
}

// Phase 1: Build Phase
void Benchmark::run_build() {
    clear_hash_tables();
    
    const auto count_cores = _core_counts[_current_core_index];
    const auto count_partitions = partition_count();
    const auto count_left_keys = std::get<0>(_join_keys).size();
    const auto count_left_keys_per_core = count_left_keys / count_cores;
    const auto count_left_keys_per_partition = count_left_keys / count_partitions;

#if defined(MODE_HOST_DRAM) || defined(MODE_HOST_TIERED)
    // Mode 1: Host CPU + Host DRAM / Mode 5: Host CPU + DRAM/CXL tiered placement
    _hash_tables.resize(count_partitions, nullptr);
    const auto estimated_keys = std::max<std::size_t>(static_cast<std::size_t>(count_left_keys_per_partition), 2048);
    const auto needed_bytes = hashtable_type::needed_bytes(estimated_keys);

    const std::size_t table_stride = hashtable_type::table_stride(needed_bytes);
#if defined(MODE_HOST_DRAM)
    // C18: same layout as the other modes (one allocation, 64 B aligned, spacing table_stride)
    _host_table_arena.assign(table_stride * count_partitions + 64U, 0U);
    char* host_base = reinterpret_cast<char*>((reinterpret_cast<std::uintptr_t>(_host_table_arena.data()) + 63U) & ~std::uintptr_t{63U});
    for (std::uint16_t channel_id = 0U; channel_id < count_partitions; ++channel_id) {
        _hash_tables[channel_id] = new (host_base + channel_id * table_stride) hashtable_type(needed_bytes);
    }
    const auto* left_base_ptr = std::get<0>(_join_keys).data();
#else
    // every partition table back to back in one arena: the CXL share stays exact even for tables under 2 MB
    _tier_hashtable_arena = tiered::PlacedBuffer(table_stride * count_partitions, _tier_options);
    for (std::uint16_t channel_id = 0U; channel_id < count_partitions; ++channel_id) {
        _hash_tables[channel_id] = new (_tier_hashtable_arena.data() + channel_id * table_stride) hashtable_type(needed_bytes);
    }
    if (_current_iteration == 0U) {
        tiered::report_placement("hash_tables", _tier_hashtable_arena.data(), _tier_hashtable_arena.size(), _tier_options);
    }
    const auto* left_base_ptr = reinterpret_cast<const std::uint32_t*>(_tier_left_keys.data());
#endif
    _start_time = std::chrono::high_resolution_clock::now();

    for (std::uint16_t i = 0U; i < count_cores; ++i) {
        const auto left_offset = i * count_left_keys_per_core;
        const auto left_count = (i == count_cores - 1U) ? (count_left_keys - left_offset) : count_left_keys_per_core;

        auto build_task = std::make_unique<HostDirectBuildTask>(
            left_offset, left_count, left_base_ptr, _hash_tables.data(), count_partitions);
        MorselScheduler::get().spawn(std::move(build_task), i);
    }
    MorselScheduler::get().wait_until_finished();

    _pure_build_end_time = std::chrono::high_resolution_clock::now();
    _build_end_time = _pure_build_end_time;

#elif defined(USE_CXL)
    if (!_pxlResources) {
        _pxlResources = std::make_unique<PxlResources>();
        _pxlResources->context = pxl::runtime::createContext(0);
        if (!_pxlResources->context) throw std::runtime_error("Cannot create PXL context");
    }
    
    _hash_tables.resize(count_partitions, nullptr);
    std::vector<std::uintptr_t> raw_ptrs(count_partitions);

    // C18: every partition table back to back in one memAlloc (instead of 3,048 per-partition allocations).
    // The device kernel computes addresses as table_base + p * table_stride instead of using a pointer array.
    const auto estimated_keys = std::max<std::size_t>(static_cast<std::size_t>(count_left_keys_per_partition), 2048);
    const auto needed_bytes = hashtable_type::needed_bytes(estimated_keys);
    _table_stride = hashtable_type::table_stride(needed_bytes);
    _cxl_table_arena = allocatePxlMemoryAligned<std::uint8_t>(_pxlResources->context, _table_stride * count_partitions);

    for (std::uint16_t channel_id = 0U; channel_id < count_partitions; ++channel_id) {
        char* table_mem = reinterpret_cast<char*>(_cxl_table_arena.get()) + channel_id * _table_stride;
#if defined(MODE_CXL_FULL_NMC)
        // T28: the device builds, so initialize with NT stores that leave the host cache clean (only the header line needs a flush)
        _hash_tables[channel_id] = new (table_mem) hashtable_type(needed_bytes, hashtable_type::nontemporal_t{});
#else
        _hash_tables[channel_id] = new (table_mem) hashtable_type(needed_bytes);
#endif
        raw_ptrs[channel_id] = reinterpret_cast<std::uintptr_t>(_hash_tables[channel_id]);
    }

#if defined(MODE_CXL_EXPANDER) || defined(MODE_CXL_HYBRID)
#if defined(MODE_CXL_HYBRID)
    _cxl_ht_ptrs_mem = allocatePxlMemoryAligned<std::uint64_t>(_pxlResources->context, count_partitions);
    std::memcpy(_cxl_ht_ptrs_mem.get(), raw_ptrs.data(), count_partitions * sizeof(std::uintptr_t));

    _cxl_config = allocatePxlMemoryAligned<CxlHashJoinConfig>(_pxlResources->context, 1);
    // C24: memAlloc memory is not zeroed. Construct the struct so its defaults ({0}) apply
    // (C20's probe_begin/end kept old contents, and a one-pass probe processed only some rows).
    new (_cxl_config.get()) CxlHashJoinConfig{};
    _cxl_config->count_left_keys  = count_left_keys;
    _cxl_config->count_right_keys = std::get<1>(_join_keys).size();
    _cxl_config->count_cores      = count_partitions;
    _cxl_config->filter_threshold = _filter_threshold;
    _cxl_config->slot_mask        = _hash_tables[0]->slot_count() - 1U;   // every partition the same size
    _cxl_config->prefetch_window  = _prefetch_window;
    _cxl_config->table_base       = reinterpret_cast<std::uint64_t>(_cxl_table_arena.get());
    _cxl_config->table_stride     = _table_stride;
    _cxl_done = allocatePxlMemoryAligned<std::uint64_t>(_pxlResources->context, 8);
    reset_done(_cxl_done.get());
    _cxl_config->done_addr        = reinterpret_cast<std::uint64_t>(_cxl_done.get());
#endif

    const std::uint32_t* left_base_ptr = std::get<0>(_join_keys).data();
    if (_build_input_cxl) {
        // C30: read the orders keys from CXL, like the device build. Empty the host cache outside the timer in every
        // iteration so each starts from cold input (the device does not see the host cache).
        if (!_cxl_left_keys) {
            _cxl_left_keys = allocatePxlMemoryAligned<std::uint32_t>(_pxlResources->context, count_left_keys);
            std::memcpy(_cxl_left_keys.get(), left_base_ptr, count_left_keys * sizeof(std::uint32_t));
        }
        pxl_helper::safeFlushHostCache(_cxl_left_keys.get(), count_left_keys * sizeof(std::uint32_t));
        left_base_ptr = _cxl_left_keys.get();
    }

    // C29: owner-computes host build. Buffer allocation and first touch (page mapping) outside the timer, once.
    const std::uint32_t own_row_stride = (static_cast<std::uint32_t>(count_partitions) + 15U) & ~15U;   // 64 B rows
    // C32: the owner build is split by host workers. For Hybrid, count_cores = device task count (3,048), so a serial
    // prefix over the 3,048 x P count table took the partition step at sf_10 from 12 to 150 ms (same cause as T41).
    const std::uint16_t own_workers = static_cast<std::uint16_t>(
        std::min<std::size_t>(count_cores, std::max<std::size_t>(MorselScheduler::get().host_thread_count(), 1)));
    if (_owner_build) {
        if (_host_own_pairs_count != count_left_keys) {
            _host_own_pairs = allocatePxlMemoryAligned<std::uint64_t>(_pxlResources->context, count_left_keys);
            std::memset(_host_own_pairs.get(), 0, count_left_keys * sizeof(std::uint64_t));
            pxl_helper::safeFlushHostCache(_host_own_pairs.get(), count_left_keys * sizeof(std::uint64_t));
            _host_own_pairs_count = count_left_keys;
        }
        _host_own_counts.assign(static_cast<std::size_t>(own_workers) * own_row_stride, 0U);
        _host_own_offsets.assign(static_cast<std::size_t>(own_workers) * own_row_stride, 0U);
        _host_own_part_begin.assign(static_cast<std::size_t>(count_partitions) + 1U, 0U);
    }

    _start_time = std::chrono::high_resolution_clock::now();

    if (!_owner_build) {
        for (std::uint16_t i = 0U; i < count_cores; ++i) {
            const auto left_offset = i * count_left_keys_per_core;
            const auto left_count = (i == count_cores - 1U) ? (count_left_keys - left_offset) : count_left_keys_per_core;

            auto build_task = std::make_unique<HostDirectBuildTask>(
                left_offset, left_count, left_base_ptr, _hash_tables.data(), count_partitions);
            MorselScheduler::get().spawn(std::move(build_task), i);
        }
        MorselScheduler::get().wait_until_finished();
    } else {
        auto& scheduler = MorselScheduler::get();
        auto* pairs = reinterpret_cast<OwnPair*>(_host_own_pairs.get());
        const std::size_t per_worker = count_left_keys / own_workers;
        auto slice = [&](std::uint16_t i) {
            const std::size_t off = static_cast<std::size_t>(i) * per_worker;
            return std::make_pair(off, (i == own_workers - 1U) ? (count_left_keys - off) : per_worker);
        };
        // 1) per-worker partition counts
        for (std::uint16_t i = 0U; i < own_workers; ++i) {
            const auto [off, cnt] = slice(i);
            scheduler.spawn(std::make_unique<HostOwnCountTask>(off, cnt, left_base_ptr, count_partitions,
                                                               _host_own_counts.data() + static_cast<std::size_t>(i) * own_row_stride), i);
        }
        scheduler.wait_until_finished();
        // 2) partition-ordered prefix offsets: partition p's pairs are contiguous (worker 0, 1, ... in order)
        std::uint32_t pos = 0;
        for (std::uint16_t p = 0U; p < count_partitions; ++p) {
            _host_own_part_begin[p] = pos;
            for (std::uint16_t i = 0U; i < own_workers; ++i) {
                const std::size_t at = static_cast<std::size_t>(i) * own_row_stride + p;
                _host_own_offsets[at] = pos;
                pos += _host_own_counts[at];
            }
        }
        _host_own_part_begin[count_partitions] = pos;
        // 3) scatter
        for (std::uint16_t i = 0U; i < own_workers; ++i) {
            const auto [off, cnt] = slice(i);
            scheduler.spawn(std::make_unique<HostOwnScatterTask>(off, cnt, left_base_ptr, count_partitions,
                                                                 _host_own_offsets.data() + static_cast<std::size_t>(i) * own_row_stride, pairs), i);
        }
        scheduler.wait_until_finished();
        const auto part_end = std::chrono::high_resolution_clock::now();
        // 4) owner insert: worker i owns partitions i, i + T, ..., plain stores
        std::atomic<std::size_t> fails{0};
        for (std::uint16_t i = 0U; i < own_workers; ++i) {
            scheduler.spawn(std::make_unique<HostOwnInsertTask>(i, own_workers, count_partitions, pairs,
                                                                _host_own_part_begin.data(), _hash_tables.data(), fails), i);
        }
        scheduler.wait_until_finished();
        const auto own_end = std::chrono::high_resolution_clock::now();
        _own_partition_ms = std::chrono::duration<double, std::milli>(part_end - _start_time).count();
        _own_insert_ms = std::chrono::duration<double, std::milli>(own_end - part_end).count();
        if (fails.load()) std::cerr << "[❌ ERROR] host owner build: " << fails.load() << " inserts failed\n";
    }

    // host CPU pure build finishes here
    _pure_build_end_time = std::chrono::high_resolution_clock::now();

#if defined(MODE_CXL_HYBRID)
    // Mode 3 coherency (SDBI) flush -> coherency tax (\gamma) interval (C17: parallel on host workers)
    {
        parallel_flush({{_cxl_table_arena.get(), _table_stride * count_partitions}});
    }
    pxl_helper::safeFlushHostCache(_cxl_ht_ptrs_mem.get(), count_partitions * sizeof(std::uintptr_t));
    pxl_helper::safeFlushHostCache(_cxl_config.get(), sizeof(CxlHashJoinConfig));
#endif

    _build_end_time = std::chrono::high_resolution_clock::now();

#elif defined(MODE_CXL_FULL_NMC)
    // Mode 4: CXL RISC-V Full Build Kernel Offloading
    if (!_cxl_left_keys) {
        _cxl_left_keys = allocatePxlMemoryAligned<std::uint32_t>(_pxlResources->context, count_left_keys);
        std::memcpy(_cxl_left_keys.get(), std::get<0>(_join_keys).data(), count_left_keys * sizeof(std::uint32_t));
        
        // flush the left_keys the host copied with memcpy from the host cache
        pxl_helper::safeFlushHostCache(_cxl_left_keys.get(), count_left_keys * sizeof(std::uint32_t));
    }

    const std::size_t result_stride = 64 / sizeof(std::uint64_t);
    const std::uint32_t device_tasks = device_task_count();

    // allocate fail_counts in CXL memory and zero it (outside the timer, like the probe's result-buffer setup)
    if (!_cxl_build_fail_counts) {
        _cxl_build_fail_counts = allocatePxlMemoryAligned<std::uint64_t>(_pxlResources->context, device_tasks * result_stride);
    }
    std::memset(_cxl_build_fail_counts.get(), 0, device_tasks * result_stride * sizeof(std::uint64_t));
    pxl_helper::safeFlushHostCache(_cxl_build_fail_counts.get(), device_tasks * result_stride * sizeof(std::uint64_t));

    _cxl_ht_ptrs_mem = allocatePxlMemoryAligned<std::uint64_t>(_pxlResources->context, count_partitions);
    std::memcpy(_cxl_ht_ptrs_mem.get(), raw_ptrs.data(), count_partitions * sizeof(std::uintptr_t));

    _cxl_config = allocatePxlMemoryAligned<CxlHashJoinConfig>(_pxlResources->context, 1);
    // C24: memAlloc memory is not zeroed. Construct the struct so its defaults ({0}) apply
    // (C20's probe_begin/end kept old contents, and a one-pass probe processed only some rows).
    new (_cxl_config.get()) CxlHashJoinConfig{};
    _cxl_config->count_left_keys  = count_left_keys;
    _cxl_config->count_right_keys = std::get<1>(_join_keys).size();
    _cxl_config->count_cores      = count_partitions;
    _cxl_config->filter_threshold = _filter_threshold;
    _cxl_config->slot_mask        = _hash_tables[0]->slot_count() - 1U;   // every partition the same size
    _cxl_config->prefetch_window  = _prefetch_window;
    _cxl_config->table_base       = reinterpret_cast<std::uint64_t>(_cxl_table_arena.get());
    _cxl_config->table_stride     = _table_stride;
    _cxl_done = allocatePxlMemoryAligned<std::uint64_t>(_pxlResources->context, 8);
    reset_done(_cxl_done.get());
    _cxl_config->done_addr        = reinterpret_cast<std::uint64_t>(_cxl_done.get());

    // load the kernel outside the timer, as for the probe
    // C19 owner-computes build: per-task regions and row spacing in multiples of 64 B (no line shared between tasks)
    std::size_t own_region_stride = 0, own_row_stride = 0;
    if (_owner_build) {
        own_region_stride = (((count_left_keys + device_tasks - 1) / device_tasks) + 7U) & ~std::size_t{7U};
        own_row_stride = (static_cast<std::size_t>(count_partitions) + 15U) & ~std::size_t{15U};
        _cxl_own_pairs   = allocatePxlMemoryAligned<JoinTuple>(_pxlResources->context, device_tasks * own_region_stride);
        _cxl_own_counts  = allocatePxlMemoryAligned<std::uint32_t>(_pxlResources->context, device_tasks * own_row_stride);
        _cxl_own_offsets = allocatePxlMemoryAligned<std::uint32_t>(_pxlResources->context, device_tasks * own_row_stride);
        _cxl_config->own_region_stride = own_region_stride;
        _cxl_config->own_row_stride    = static_cast<std::uint32_t>(own_row_stride);
    }

    if (_owner_build) {
        loadPxlKernel(_pxlResources.get(), "mu_kernel/mu_kernel_build.mubin", "cxl_partition_kernel", device_tasks, _mu_batch);
    } else {
        loadPxlKernel(_pxlResources.get(), "mu_kernel/mu_kernel_build.mubin", "cxl_morsel_build_kernel", device_tasks, _mu_batch);
    }

    // Coherency tax (γ): the table slots were initialized with NT stores, so no flush is needed (T28). Only the
    // table header lines, the pointer array and the config, written with ordinary stores, are flushed to CXL DRAM.
    // Same cost as Mode 3's build flush, so it counts toward the build time (like the probe's pre-flush)
    auto pre_flush_start = std::chrono::high_resolution_clock::now();
    for (std::uint16_t channel_id = 0U; channel_id < count_partitions; ++channel_id) {
        pxl_helper::safeFlushHostCache(reinterpret_cast<char*>(_cxl_table_arena.get()) + channel_id * _table_stride, hashtable_type::header_bytes());
    }
    pxl_helper::safeFlushHostCache(_cxl_ht_ptrs_mem.get(), count_partitions * sizeof(std::uintptr_t));
    pxl_helper::safeFlushHostCache(_cxl_config.get(), sizeof(CxlHashJoinConfig));
    auto pre_flush_duration = std::chrono::high_resolution_clock::now() - pre_flush_start;

    _start_time = std::chrono::high_resolution_clock::now();

    if (!_owner_build) {
        if (_pxlResources->executor->execute(_cxl_left_keys.get(), 
            _cxl_ht_ptrs_mem.get(), 
            _cxl_config.get(), 
            _cxl_build_fail_counts.get(),
            static_cast<int>(device_tasks)) != pxl::PxlResult::Success)
            throw std::runtime_error("CXL 1-Pass Build Execute Failed");
        synchronizeOrThrow(_pxlResources.get(), "CXL 1-Pass Build");
        wait_done(_cxl_done.get(), device_tasks, "CXL 1-Pass Build", _early_sync_events);
        _pure_build_end_time = std::chrono::high_resolution_clock::now();
    } else {
        // Kernel A (partition) -> kernel boundary -> kernel B (owner build). Loading the second kernel is outside
        // the timer like every other kernel load: time = A run + B run.
        if (_pxlResources->executor->execute(_cxl_left_keys.get(), _cxl_own_pairs.get(), _cxl_own_counts.get(),
                _cxl_own_offsets.get(), _cxl_config.get(), static_cast<int>(device_tasks)) != pxl::PxlResult::Success)
            throw std::runtime_error("CXL Partition Execute Failed");
        synchronizeOrThrow(_pxlResources.get(), "CXL Partition");
        wait_done(_cxl_done.get(), device_tasks, "CXL Partition", _early_sync_events);
        const auto part_end = std::chrono::high_resolution_clock::now();

        loadPxlKernel(_pxlResources.get(), "mu_kernel/mu_kernel_build.mubin", "cxl_owner_build_kernel", device_tasks, _mu_batch);

        const auto own_start = std::chrono::high_resolution_clock::now();
        if (_pxlResources->executor->execute(_cxl_own_pairs.get(), _cxl_own_counts.get(), _cxl_own_offsets.get(),
                _cxl_build_fail_counts.get(), _cxl_config.get(), static_cast<int>(device_tasks)) != pxl::PxlResult::Success)
            throw std::runtime_error("CXL Owner Build Execute Failed");
        synchronizeOrThrow(_pxlResources.get(), "CXL Owner Build");
        wait_done(_cxl_done.get(), device_tasks, "CXL Owner Build", _early_sync_events);
        const auto own_end = std::chrono::high_resolution_clock::now();
        _pure_build_end_time = part_end + (own_end - own_start);
        _own_partition_ms = std::chrono::duration<double, std::milli>(part_end - _start_time).count();
        _own_insert_ms = std::chrono::duration<double, std::milli>(own_end - own_start).count();
    }
    _build_end_time = _pure_build_end_time + pre_flush_duration;

    // invalidate the host cache before reading the device-written fail_counts (outside the timer, diagnostics)
    pxl_helper::safeFlushHostCache(_cxl_build_fail_counts.get(), device_tasks * result_stride * sizeof(std::uint64_t));

    // check the failure counts
    std::size_t total_build_failures = 0;
    for (std::uint32_t i = 0; i < device_tasks; ++i) {
        total_build_failures += _cxl_build_fail_counts.get()[i * result_stride];
    }

    if (total_build_failures > 0) {
        std::cerr << "[❌ ERROR] build phase: " << total_build_failures 
                  << " tuple inserts failed!" << std::endl;
    }
#endif
#endif
}

// Phase 2: Probe Phase
void Benchmark::run_probe() {
    const auto count_cores = _core_counts[_current_core_index];
    const auto count_partitions = partition_count();
    const auto count_right_keys = std::get<1>(_join_keys).size();

#if defined(USE_CXL)
    // =============================================================
    // [all of Modes 2, 3, 4] allocate the probe input in CXL shared memory and copy it
    // =============================================================
    bool need_flush_probe_inputs = false;

    if (!_cxl_right_keys) {
        _cxl_right_keys = allocatePxlMemoryAligned<std::uint32_t>(_pxlResources->context, count_right_keys);
        std::memcpy(_cxl_right_keys.get(), std::get<1>(_join_keys).data(), count_right_keys * sizeof(std::uint32_t));
        need_flush_probe_inputs = true;
    }

    if (!_cxl_filter_values) {
        _cxl_filter_values = allocatePxlMemoryAligned<std::uint32_t>(_pxlResources->context, count_right_keys);
        std::memcpy(_cxl_filter_values.get(), _filter_values.data(), count_right_keys * sizeof(std::uint32_t));
        need_flush_probe_inputs = true;
    }

#if defined(MODE_CXL_HYBRID) || defined(MODE_CXL_FULL_NMC)
    // flush the host CPU cache before the CXL RISC-V cores read (before the timer starts)
    if (need_flush_probe_inputs) {
        pxl_helper::safeFlushHostCache(_cxl_right_keys.get(), count_right_keys * sizeof(std::uint32_t));
        pxl_helper::safeFlushHostCache(_cxl_filter_values.get(), count_right_keys * sizeof(std::uint32_t));
    }
#endif
#endif

#if defined(MODE_HOST_DRAM) || defined(MODE_CXL_EXPANDER) || defined(MODE_HOST_TIERED)
    // =============================================================
    // [Host CPU Probe] Mode 1 (Host DRAM) & Mode 2 (CXL DRAM) & Mode 5 (Tiered)
    // =============================================================
    const auto count_right_keys_per_core = count_right_keys / count_cores;
    // C31: host-direct probe writes result set i per worker i (~rows/workers each); sized per partition the
    // sets re-grew inside the timer when partitions > workers (sf_100, P = 1,024: probe 870 vs 585 ms).
    _merge_task = _host_direct_probe
        ? std::make_unique<MergeTask>(count_partitions, count_right_keys / count_cores + 1U, count_cores)
        : std::make_unique<MergeTask>(count_partitions, count_right_keys / count_partitions);

#if defined(MODE_HOST_DRAM)
    // Mode 1: host DRAM pointers
    const auto* right_base_ptr = std::get<1>(_join_keys).data();
    const auto* filter_base_ptr = _filter_values.data();
#elif defined(MODE_HOST_TIERED)
    // Mode 5: DRAM/CXL placement buffers
    const auto* right_base_ptr = reinterpret_cast<const std::uint32_t*>(_tier_right_keys.data());
    const auto* filter_base_ptr = reinterpret_cast<const std::uint32_t*>(_tier_filter_values.data());
#else
    // Mode 2: CXL DRAM pointers
    const auto* right_base_ptr = _cxl_right_keys.get();
    const auto* filter_base_ptr = _cxl_filter_values.get();
#endif

    // C20: with --probe-batches K, probe lineitem in K consecutive batches (together, all of it once).
    // K = 1 (default) is the same as before.
    (void)count_right_keys_per_core;
    const std::size_t K = std::max<std::uint32_t>(_probe_batches, 1U);
    const std::size_t batch_rows = (count_right_keys + K - 1) / K;

    _probe_start_time = std::chrono::high_resolution_clock::now();

    for (std::size_t k = 0; k < K; ++k) {
        const std::size_t b = std::min(k * batch_rows, count_right_keys);
        const std::size_t e = std::min(b + batch_rows, count_right_keys);
        if (b >= e) break;
        const std::size_t per_core = (e - b) / count_cores;
        if (_host_direct_probe) {
            // C26: task i probes its own slice directly and writes only to result_set(i) (tasks <= partitions)
            if (count_cores > count_partitions) throw std::runtime_error("--host-direct-probe needs cores <= partitions");
            for (std::uint16_t i = 0U; i < count_cores; ++i) {
                const auto right_offset = b + i * per_core;
                const auto right_count = (i == count_cores - 1U) ? (e - right_offset) : per_core;
                if (right_count == 0) continue;
                MorselScheduler::get().spawn(std::make_unique<HostDirectProbeTask>(right_offset, right_count, right_base_ptr,
                    filter_base_ptr, _filter_threshold, _hash_tables.data(), count_partitions, _merge_task->result_set(i)), i);
            }
            MorselScheduler::get().wait_until_finished();
            continue;
        }
        for (std::uint16_t i = 0U; i < count_cores; ++i) {
            const auto right_offset = b + i * per_core;
            const auto right_count = (i == count_cores - 1U) ? (e - right_offset) : per_core;
            if (right_count == 0) continue;

            auto probe_task = std::make_unique<PartitionTask<ProbeTask>>(
                count_partitions, _batches[_current_batch_index], right_offset, right_count,
                _hash_tables.data(), right_base_ptr + right_offset, filter_base_ptr + right_offset,
                _merge_task.get(), _filter_threshold);

            MorselScheduler::get().spawn(std::move(probe_task), i);
        }
        MorselScheduler::get().wait_until_finished();
    }

    _pure_probe_end_time = std::chrono::high_resolution_clock::now();
    _probe_end_time = _pure_probe_end_time;

#elif defined(MODE_CXL_HYBRID) || defined(MODE_CXL_FULL_NMC)
    // =============================================================
    // [CXL RISC-V NDP Probe] Mode 3 & Mode 4
    // =============================================================
    const std::uint32_t device_tasks = device_task_count();
    const bool batched = _probe_batches > 0;
    const std::size_t K = std::max<std::uint32_t>(_probe_batches, 1U);
    const std::size_t batch_rows = (count_right_keys + K - 1) / K;
    // C25a: batch mode writes one contiguous result block per batch (batch_cap slots per task). One pass uses the per-task areas.
    const std::size_t batch_cap = batched ? (batch_rows + device_tasks - 1) / device_tasks : 0;
    std::size_t max_results_per_task = batched ? batch_cap : (count_right_keys + device_tasks - 1) / device_tasks;
    _max_results_per_task = max_results_per_task;
    _batch_cap = batch_cap;
    _batch_counts.assign(batched ? K * device_tasks : 0, 0);
    const std::size_t result_slots = batched ? K * device_tasks * batch_cap : device_tasks * max_results_per_task;
    if (!_cxl_result_buffers) {
        _cxl_result_buffers = allocatePxlMemoryAligned<JoinTuple>(_pxlResources->context, result_slots);
    }
    auto batch_block = [&](std::size_t k) { return _cxl_result_buffers.get() + k * device_tasks * batch_cap; };
    // C25a: handoff of batch k: invalidate and read the count line, then invalidate one result block
    auto hand_off_batch = [&](std::size_t k) {
        const auto t0 = std::chrono::high_resolution_clock::now();
        pxl_helper::safeFlushHostCache(_cxl_match_counts.get(), device_tasks * 8 * sizeof(std::uint64_t));
        for (std::uint32_t t = 0; t < device_tasks; ++t) {
            _batch_counts[k * device_tasks + t] = std::min<std::uint64_t>(_cxl_match_counts.get()[t * 8], batch_cap);
        }
        const std::size_t bytes = device_tasks * batch_cap * sizeof(JoinTuple);
        // C27: waking host workers (~0.9 ms) costs more than a single-thread flush (~4 ns per line),
        // so below 32 MiB flush serially (with a 4 MiB limit, sf_10 K=100's 4.8 MB blocks lost ~0.9 ms per batch).
        if (bytes < (std::size_t{32} << 20)) {
            pxl_helper::safeFlushHostCache(batch_block(k), bytes);
        } else {
            parallel_flush({{batch_block(k), bytes}});
        }
        return std::chrono::high_resolution_clock::now() - t0;
    };

    const std::size_t result_stride = 64 / sizeof(std::uint64_t);
    if (!_cxl_match_counts) {
        _cxl_match_counts = allocatePxlMemoryAligned<std::uint64_t>(_pxlResources->context, device_tasks * result_stride);
    }
    
    // T27: the result buffer is not zero-filled (match_counts tells how many slots are valid). Zero-filling
    // dirtied 4.8 GB (sf_100) that then needed a flush.
    std::memset(_cxl_match_counts.get(), 0, device_tasks * result_stride * sizeof(std::uint64_t));

    // time the pre-launch host cache flush (coherency tax: \gamma)
    auto pre_flush_start = std::chrono::high_resolution_clock::now();
    pxl_helper::safeFlushHostCache(_cxl_match_counts.get(), device_tasks * result_stride * sizeof(std::uint64_t));
    auto pre_flush_duration = std::chrono::high_resolution_clock::now() - pre_flush_start;

    // Device→host handoff (T27/C17): invalidate only match_counts and the newly written result ranges.
    // In batch mode, only the range each batch appended. Small ranges serially, large ones on the host workers.
    std::vector<std::uint64_t> handed_off(device_tasks, 0);
    auto hand_off_new_results = [&]() {
        const auto t0 = std::chrono::high_resolution_clock::now();
        pxl_helper::safeFlushHostCache(_cxl_match_counts.get(), device_tasks * result_stride * sizeof(std::uint64_t));
        std::vector<std::pair<void*, std::size_t>> ranges;
        std::size_t bytes = 0;
        for (std::uint32_t t = 0; t < device_tasks; ++t) {
            const std::uint64_t n = std::min<std::uint64_t>(_cxl_match_counts.get()[t * result_stride], max_results_per_task);
            if (n > handed_off[t]) {
                const std::size_t len = (n - handed_off[t]) * sizeof(JoinTuple);
                ranges.emplace_back(_cxl_result_buffers.get() + static_cast<std::size_t>(t) * max_results_per_task + handed_off[t], len);
                bytes += len;
                handed_off[t] = n;
            }
        }
        if (bytes < (std::size_t{4} << 20)) {
            for (const auto& [ptr, len] : ranges) pxl::flushHostCache(ptr, len);
            _mm_sfence();
        } else {
            parallel_flush(ranges);
        }
        return std::chrono::high_resolution_clock::now() - t0;
    };

    std::chrono::high_resolution_clock::duration handoff_total{0};

    if (!batched) {
        loadPxlKernel(_pxlResources.get(), "mu_kernel/mu_kernel_probe.mubin", "cxl_morsel_probe_kernel", device_tasks, _mu_batch);

        // pure probe time (kernel launch to synchronize)
        _probe_start_time = std::chrono::high_resolution_clock::now();
        auto ret = _pxlResources->executor->execute(
            _cxl_right_keys.get(), _cxl_filter_values.get(), _cxl_ht_ptrs_mem.get(), _cxl_result_buffers.get(),
            max_results_per_task, _cxl_match_counts.get(), _cxl_config.get(), static_cast<int>(device_tasks));
        if (ret != pxl::PxlResult::Success) throw std::runtime_error("CXL Probe Execute Failed");
        synchronizeOrThrow(_pxlResources.get(), "CXL Probe");
        wait_done(_cxl_done.get(), device_tasks, "CXL Probe", _early_sync_events);
        _pure_probe_end_time = std::chrono::high_resolution_clock::now();
        handoff_total = hand_off_new_results();
    } else if (!_resident) {
        // C20 batched NDP-sync: one launch per batch. The range goes into the config and only that line is flushed.
        // The handle is rebuilt every 200 dispatches, per the §4.1.4 rule (that cost is timed too).
        loadPxlKernel(_pxlResources.get(), "mu_kernel/mu_kernel_probe.mubin", "cxl_morsel_probe_kernel", device_tasks, _mu_batch);
        _cxl_config->probe_accumulate = 0;
        _cxl_config->batch_cap = batch_cap;
        _probe_start_time = std::chrono::high_resolution_clock::now();
        for (std::size_t k = 0; k < K; ++k) {
            const std::size_t b = std::min(k * batch_rows, count_right_keys);
            const std::size_t e = std::min(b + batch_rows, count_right_keys);
            if (b >= e) break;
            const auto tc = std::chrono::high_resolution_clock::now();
            _cxl_config->probe_begin = b;
            _cxl_config->probe_end = e;
            _cxl_config->batch_out_base = reinterpret_cast<std::uint64_t>(batch_block(k));
            pxl_helper::safeFlushHostCache(_cxl_config.get(), sizeof(CxlHashJoinConfig));
            handoff_total += std::chrono::high_resolution_clock::now() - tc;
            if (k > 0 && k % 200 == 0) {
                loadPxlKernel(_pxlResources.get(), "mu_kernel/mu_kernel_probe.mubin", "cxl_morsel_probe_kernel", device_tasks, _mu_batch);
            }
            auto ret = _pxlResources->executor->execute(
                _cxl_right_keys.get(), _cxl_filter_values.get(), _cxl_ht_ptrs_mem.get(), _cxl_result_buffers.get(),
                max_results_per_task, _cxl_match_counts.get(), _cxl_config.get(), static_cast<int>(device_tasks));
            if (ret != pxl::PxlResult::Success) throw std::runtime_error("CXL Batched Probe Execute Failed");
            synchronizeOrThrow(_pxlResources.get(), "CXL Batched Probe");
            wait_done(_cxl_done.get(), device_tasks, "CXL Batched Probe", _early_sync_events);
            handoff_total += hand_off_batch(k);
        }
        _pure_probe_end_time = std::chrono::high_resolution_clock::now() - handoff_total;
    } else {
        if (device_tasks > kUserMuThreads) {
            throw std::runtime_error("--resident needs at most " + std::to_string(kUserMuThreads) +
                                     " device tasks (all tasks must be resident at once)");
        }
        // C20 NDP@CXL: launch the resident kernel once. The host publishes each batch range with NT stores (no flush,
        // Tier 1 rule) and waits, invalidating the done counter (atomicAdd in memory).
        if (!_cxl_resident_ctrl) {
            _cxl_resident_ctrl = allocatePxlMemoryAligned<std::uint64_t>(_pxlResources->context, 16);
        }
        std::uint64_t* ctrl = _cxl_resident_ctrl.get();
        std::memset(ctrl, 0, 16 * sizeof(std::uint64_t));
        pxl_helper::safeFlushHostCache(ctrl, 16 * sizeof(std::uint64_t));
        auto nt_store = [](std::uint64_t* p, std::uint64_t v) { _mm_stream_si64(reinterpret_cast<long long*>(p), static_cast<long long>(v)); };
        auto read_done = [ctrl]() {
            pxl::flushHostCache(ctrl + 8, 64);
            _mm_mfence();
            return static_cast<std::int64_t>(*reinterpret_cast<volatile std::int32_t*>(ctrl + 8));
        };

        _cxl_config->batch_cap = batch_cap;   // C25a
        pxl_helper::safeFlushHostCache(_cxl_config.get(), sizeof(CxlHashJoinConfig));
        loadPxlKernel(_pxlResources.get(), "mu_kernel/mu_kernel_probe.mubin", "cxl_resident_probe_kernel", device_tasks, 1U);

        // a thread that waits for the kernel to finish. Whatever the exit path, stop is published and the thread joined.
        std::atomic<int> sync_state{0};   // 0 = running, 1 = finished, 2 = failed
        std::thread waiter;
        struct StopGuard {
            std::uint64_t* ctrl; std::thread* waiter;
            ~StopGuard() {
                _mm_stream_si64(reinterpret_cast<long long*>(ctrl + 3), 1LL);
                _mm_sfence();
                if (waiter->joinable()) waiter->join();
            }
        } guard{ctrl, &waiter};

        _probe_start_time = std::chrono::high_resolution_clock::now();
        auto ret = _pxlResources->executor->execute(
            _cxl_right_keys.get(), _cxl_filter_values.get(), _cxl_ht_ptrs_mem.get(), _cxl_result_buffers.get(),
            max_results_per_task, _cxl_match_counts.get(), _cxl_config.get(), ctrl, static_cast<int>(device_tasks));
        if (ret != pxl::PxlResult::Success) throw std::runtime_error("CXL Resident Probe Execute Failed");
        waiter = std::thread([&]() {
            sync_state = (_pxlResources->executor->synchronize() == pxl::PxlResult::Success) ? 1 : 2;
        });

        for (std::size_t k = 0; k < K; ++k) {
            const std::size_t b = std::min(k * batch_rows, count_right_keys);
            const std::size_t e = std::min(b + batch_rows, count_right_keys);
            if (b >= e) break;
            nt_store(ctrl + 1, b);
            nt_store(ctrl + 2, e);
            nt_store(ctrl + 4, reinterpret_cast<std::uint64_t>(batch_block(k)));
            _mm_sfence();
            nt_store(ctrl + 0, k + 1);
            _mm_sfence();
            const std::int64_t target = static_cast<std::int64_t>(device_tasks) * static_cast<std::int64_t>(k + 1);
            const auto wait_start = std::chrono::steady_clock::now();
            while (read_done() < target) {
                if (sync_state.load() != 0) throw std::runtime_error("Resident probe kernel exited before finishing batch " + std::to_string(k));
                if (std::chrono::steady_clock::now() - wait_start > std::chrono::seconds(60))
                    throw std::runtime_error("Resident probe: batch " + std::to_string(k) + " timed out (done=" + std::to_string(read_done()) + ")");
                _mm_pause();
            }
            handoff_total += hand_off_batch(k);
        }
        _pure_probe_end_time = std::chrono::high_resolution_clock::now() - handoff_total;
        // the guard's destructor publishes stop and waits for the kernel to exit (outside the timer)
    }

    // Probe flush (γ) = pre-launch flush + result handoff (summed per batch in batch mode)
    _probe_end_time = _pure_probe_end_time + pre_flush_duration + handoff_total;
#endif
}

// Phase 3: Merge & Benchmark Result Output
void Benchmark::run_merge() {
    std::size_t total_tuples = 0;
    std::uint64_t total_checksum = 0;
    const auto count_partitions = partition_count();
    const std::uint32_t device_tasks = device_task_count();

#if defined(MODE_HOST_DRAM) || defined(MODE_CXL_EXPANDER) || defined(MODE_HOST_TIERED)
    if (_merge_task) {
        _merge_task->execute(0);
        total_tuples = _merge_task->count_tuples();
        
#ifdef ENABLE_CHECKSUM
        for (std::uint16_t ch = 0U; ch < count_partitions; ++ch) {
            for (const auto& [r_key, l_idx] : _merge_task->result_set(ch)) {
                total_checksum ^= mix_pair(r_key, static_cast<std::uint32_t>(l_idx));
            }
        }
#endif
    }
#elif defined(MODE_CXL_HYBRID) || defined(MODE_CXL_FULL_NMC)
    const std::size_t result_stride = 64 / sizeof(std::uint64_t);

    if (_batch_cap) {   // C25a: batch layout
        for (const auto n : _batch_counts) total_tuples += n;
    } else {
        for (std::uint32_t i = 0; i < device_tasks; ++i) {
            total_tuples += _cxl_match_counts.get()[i * result_stride];
        }
    }

#ifdef ENABLE_CHECKSUM
    const auto count_right_keys = std::get<1>(_join_keys).size();
    const std::size_t max_results_per_task = _max_results_per_task;
    (void)count_right_keys;

    if (_cxl_result_buffers && _batch_cap) {   // C25a: block for batch k, task t
        const std::size_t K = _batch_counts.size() / device_tasks;
        for (std::size_t k = 0; k < K; ++k) {
            for (std::uint32_t i = 0; i < device_tasks; ++i) {
                const JoinTuple* t = _cxl_result_buffers.get() + (k * device_tasks + i) * _batch_cap;
                for (std::uint64_t j = 0; j < _batch_counts[k * device_tasks + i]; ++j) {
                    total_checksum ^= mix_pair(t[j].key, t[j].val);
                }
            }
        }
    } else if (_cxl_result_buffers) {
        for (std::uint32_t i = 0; i < device_tasks; ++i) {
            std::size_t matches = _cxl_match_counts.get()[i * result_stride];
            const JoinTuple* task_tuples = _cxl_result_buffers.get() + (i * max_results_per_task);

            for (std::size_t j = 0; j < matches; ++j) {
                total_checksum ^= mix_pair(task_tuples[j].key, task_tuples[j].val);
            }
        }
    }
#endif
#endif

    double pure_build_ms  = std::chrono::duration_cast<std::chrono::microseconds>(_pure_build_end_time - _start_time).count() / 1000.0;
    double build_flush_ms = std::chrono::duration_cast<std::chrono::microseconds>(_build_end_time - _pure_build_end_time).count() / 1000.0;
    double total_build_ms = pure_build_ms + build_flush_ms;

    double pure_probe_ms  = std::chrono::duration_cast<std::chrono::microseconds>(_pure_probe_end_time - _probe_start_time).count() / 1000.0;
    double probe_flush_ms = std::chrono::duration_cast<std::chrono::microseconds>(_probe_end_time - _pure_probe_end_time).count() / 1000.0;
    double total_probe_ms = pure_probe_ms + probe_flush_ms;

    double total_ms       = total_build_ms + total_probe_ms;

    // ground-truth generation and validation live entirely under ENABLE_CHECKSUM
#ifdef ENABLE_CHECKSUM
    // C34: the reference join depends only on the data and the threshold, not on the plan, partition count
    // or iteration. It took ~2 min per iteration at sf_100 (single-threaded unordered_map over 150 M keys,
    // probed with 600 M rows), outside the timer. Compute it once per process, and keep it in the column
    // cache dir keyed by a hash of the three loaded columns + threshold. Every iteration still compares.
    if (!_golden_ready) {
        const auto& [left_keys, right_keys] = _join_keys;
        std::uint64_t h = 1469598103934665603ULL;
        auto fold = [&h](const std::vector<std::uint32_t>& v) {
            for (const auto x : v) { h ^= x; h *= 1099511628211ULL; }
            h ^= v.size(); h *= 1099511628211ULL;
        };
        fold(left_keys); fold(right_keys); fold(_filter_values);
        const char* env = std::getenv("HJ_COL_CACHE");
        const std::string dir = env ? env : "col_cache";
        char name[96];
        std::snprintf(name, sizeof(name), "/truth_%016llx_t%u.txt", static_cast<unsigned long long>(h), _filter_threshold);
        struct stat st{};
        const std::string truth_file = (dir != "off" && ::stat(dir.c_str(), &st) == 0) ? dir + name : std::string{};
        bool loaded = false;
        if (!truth_file.empty()) {
            std::ifstream in(truth_file);
            loaded = static_cast<bool>(in >> _golden_tuples >> std::hex >> _golden_checksum);
        }
        if (!loaded) {
            _golden_tuples = 0; _golden_checksum = 0;
            std::unordered_map<std::uint32_t, std::size_t> ht;
            ht.reserve(left_keys.size());
            for (std::size_t i = 0; i < left_keys.size(); ++i) ht[left_keys[i]] = i;
            for (std::size_t i = 0; i < right_keys.size(); ++i) {
                if (_filter_values[i] > _filter_threshold) continue;
                const auto it = ht.find(right_keys[i]);
                if (it != ht.end() && it->second < left_keys.size()) {
                    _golden_tuples++;
                    _golden_checksum ^= mix_pair(right_keys[i], static_cast<std::uint32_t>(it->second));
                }
            }
            if (!truth_file.empty()) {
                const std::string tmp = truth_file + ".tmp" + std::to_string(::getpid());
                { std::ofstream out(tmp); out << _golden_tuples << " " << std::hex << _golden_checksum << "\n"; }
                if (std::rename(tmp.c_str(), truth_file.c_str()) != 0) std::remove(tmp.c_str());
            }
        }
        std::cerr << "[TRUTH] " << (loaded ? "loaded " : "computed ") << (truth_file.empty() ? "(no cache)" : truth_file) << "\n";
        _golden_ready = true;
    }
    const std::size_t golden_tuples = _golden_tuples;
    const std::uint64_t golden_checksum = _golden_checksum;

    if (total_tuples != golden_tuples || total_checksum != golden_checksum) {
        std::cerr << "\n[❌ CRITICAL ERROR] Ground-Truth Verification Failed!\n"
                  << "  Expected Tuples=" << golden_tuples << ", Checksum=0x" << std::hex << golden_checksum << std::dec
                  << "\n  Actual   Tuples=" << total_tuples << ", Checksum=0x" << std::hex << total_checksum << std::dec
                  << "\n" << std::endl;
        throw std::runtime_error("Verification Mismatch Detected!");
    } else {
        std::cout << " [✅ C++ VALIDATION PASSED] Output Matched with Host Ground-Truth!" << std::endl;
    }
#endif

#if defined(MODE_HOST_DRAM)
    const char* mode_str = "HOST_DRAM";
#elif defined(MODE_CXL_EXPANDER)
    const char* mode_str = "CXL_EXPANDER";
#elif defined(MODE_CXL_HYBRID)
    const char* mode_str = "CXL_HYBRID_NMC";
#elif defined(MODE_CXL_FULL_NMC)
    const char* mode_str = "CXL_FULL_NMC";
#elif defined(MODE_HOST_TIERED)
    const char* mode_str = "HOST_TIERED";
#endif

std::cout << ">>> [XCENA RESULT] Mode: " << mode_str
              << " | Cores: " << _core_counts[_current_core_index] 
              << " | Partitions: " << count_partitions
              << " | Device Tasks: " << device_tasks
              << " | Prefetch: " << _prefetch_window
              << " | OwnerBuild: " << (_owner_build ? 1 : 0)
              << " | BuildInputCxl: " << (_build_input_cxl ? 1 : 0)
              << " | Batches: " << _probe_batches << (_resident ? " resident" : "")
              << " | MuBatch: " << _mu_batch
              << " | HostDirect: " << (_host_direct_probe ? 1 : 0)
#if defined(USE_CXL)
              << " | EarlySync: " << _early_sync_events
#endif
              ;
    if (_owner_build) std::cout << " (partition " << _own_partition_ms << " ms + insert " << _own_insert_ms << " ms)";
    std::cout
#if defined(MODE_HOST_TIERED)
              << " | CXL%: " << _tier_options.cxl_pct
              << " | Placement: " << (_tier_options.interleave ? "interleave" : "block")
#endif
              << " | Pure Build: " << pure_build_ms << " ms"
              << " | Build Flush(γ): " << build_flush_ms << " ms"
              << " | Pure Probe: " << pure_probe_ms << " ms"
              << " | Probe Flush(γ): " << probe_flush_ms << " ms"
              << " | Total: " << total_ms << " ms"
              << " | Tuples: " << total_tuples;
#ifdef ENABLE_CHECKSUM
    char checksum_hex[32];
    std::snprintf(checksum_hex, sizeof(checksum_hex), "0x%016lx", total_checksum);
    std::cout << " | Checksum: " << checksum_hex;
#endif
    std::cout << std::endl;

    // 3. update the JSON output
    if (!_result_file_name.empty()) {
        std::ofstream json_file(_result_file_name, std::ios::app);
        if (json_file.is_open()) {
            json_file << "{\"mode\":\"" << mode_str << "\","
                      << "\"cores\":" << _core_counts[_current_core_index] << ","
                      << "\"partitions\":" << count_partitions << ","
                      << "\"device_tasks\":" << device_tasks << ","
                      << "\"prefetch_window\":" << _prefetch_window << ","
                      << "\"owner_build\":" << (_owner_build ? 1 : 0) << ","
                      << "\"build_input_cxl\":" << (_build_input_cxl ? 1 : 0) << ","
                      << "\"probe_batches\":" << _probe_batches << ","
                      << "\"resident\":" << (_resident ? 1 : 0) << ","
                      << "\"mu_batch\":" << _mu_batch << ","
                      << "\"host_direct_probe\":" << (_host_direct_probe ? 1 : 0) << ","
#if defined(USE_CXL)
                      << "\"early_sync_events\":" << _early_sync_events << ","
#endif
                      << "\"own_partition_ms\":" << _own_partition_ms << ","
                      << "\"own_insert_ms\":" << _own_insert_ms << ","
#if defined(MODE_HOST_TIERED)
                      << "\"cxl_pct\":" << _tier_options.cxl_pct << ","
                      << "\"cxl_node\":" << _tier_options.cxl_node << ","
                      << "\"placement\":\"" << (_tier_options.interleave ? "interleave" : "block") << "\","
#endif
                      << "\"batch_size\":" << _batches[_current_batch_index] << ","
                      << "\"iteration\":" << _current_iteration << ","
                      << "\"pure_build_ms\":" << pure_build_ms << ","
                      << "\"build_flush_ms\":" << build_flush_ms << ","
                      << "\"build_ms\":" << total_build_ms << ","
                      << "\"pure_probe_ms\":" << pure_probe_ms << ","
                      << "\"probe_flush_ms\":" << probe_flush_ms << ","
                      << "\"probe_ms\":" << total_probe_ms << ","
                      << "\"total_ms\":" << total_ms << ","
                      << "\"output_tuples\":" << total_tuples;
#ifdef ENABLE_CHECKSUM
            json_file << ",\"checksum\":\"" << checksum_hex << "\"";
#endif
            json_file << "}\n";
            json_file.flush();
        }
    }
}

std::uint16_t Benchmark::next_core_count() {
    if (_current_iteration == std::numeric_limits<std::uint16_t>::max()) {
        _current_iteration = 0U;
        _current_batch_index = 0U;
        _current_core_index = 0U;
        return _core_counts[_current_core_index];
    }

    clear_hash_tables();

    if (++_current_iteration < _iterations) {
        return _core_counts[_current_core_index];
    }
    _current_iteration = 0U;

    if (++_current_batch_index < _batches.size()) {
        return _core_counts[_current_core_index];
    }
    _current_batch_index = 0U;

    if (++_current_core_index < _core_counts.size()) {
        return _core_counts[_current_core_index];
    }

    return 0U;
}