#pragma once
#include <cassert>
#include <vector>
#include <algorithm>
#include <utility>
#include <memory>
#include <limits>
#include <atomic>
#include "inline_hashtable.h"
#include "morsel_scheduler.h"

namespace application::hash_join {

// Phase 1: 1-Pass Direct Build Task (Host Side)
class HostDirectBuildTask final : public ::Task {
public:
    HostDirectBuildTask(std::size_t start_idx, std::size_t count,
                        const std::uint32_t* left_keys,
                        hashtable_type** hash_tables, std::uint16_t count_cores)
        : _start_idx(start_idx), _count(count), _left_keys(left_keys),
          _hash_tables(hash_tables), _count_cores(count_cores) {}

    void execute(std::uint16_t) override {
        if (!_hash_tables || !_left_keys) return;
        for (std::size_t i = _start_idx; i < _start_idx + _count; ++i) {
            std::uint32_t key = _left_keys[i];
            std::uint16_t ch = hashtable_type::partition_hash(key) % _count_cores;
            _hash_tables[ch]->insert_atomic(key, static_cast<std::uint32_t>(i));
        }
    }
private:
    std::size_t _start_idx, _count;
    const std::uint32_t* _left_keys;
    hashtable_type** _hash_tables;
    std::uint16_t _count_cores;
};

// C29: host owner-computes build (--owner-build in Modes 2/3), same scheme as the device's C19
// kernels: count per partition -> (serial prefix) -> scatter (key,row) partition-major -> each worker
// inserts the partitions it owns with plain stores (insert_plain), no atomics.
struct OwnPair {
    std::uint32_t key;
    std::uint32_t row;
};

class HostOwnCountTask final : public ::Task {
public:
    HostOwnCountTask(std::size_t start, std::size_t count, const std::uint32_t* keys, std::uint32_t partitions,
                     std::uint32_t* counts)
        : _start(start), _count(count), _keys(keys), _partitions(partitions), _counts(counts) {}
    void execute(std::uint16_t) override {
        for (std::uint32_t p = 0; p < _partitions; ++p) _counts[p] = 0;
        for (std::size_t i = _start; i < _start + _count; ++i) {
            _counts[hashtable_type::partition_hash(_keys[i]) % _partitions]++;
        }
    }
private:
    std::size_t _start, _count;
    const std::uint32_t* _keys;
    std::uint32_t _partitions;
    std::uint32_t* _counts;
};

class HostOwnScatterTask final : public ::Task {
public:
    HostOwnScatterTask(std::size_t start, std::size_t count, const std::uint32_t* keys, std::uint32_t partitions,
                       std::uint32_t* offsets, OwnPair* pairs)
        : _start(start), _count(count), _keys(keys), _partitions(partitions), _offsets(offsets), _pairs(pairs) {}
    void execute(std::uint16_t) override {
        for (std::size_t i = _start; i < _start + _count; ++i) {
            const std::uint32_t key = _keys[i];
            _pairs[_offsets[hashtable_type::partition_hash(key) % _partitions]++] = OwnPair{key, static_cast<std::uint32_t>(i)};
        }
    }
private:
    std::size_t _start, _count;
    const std::uint32_t* _keys;
    std::uint32_t _partitions;
    std::uint32_t* _offsets;
    OwnPair* _pairs;
};

class HostOwnInsertTask final : public ::Task {
public:
    static constexpr std::size_t kPrefetchDistance = 16;   // slot lines requested ahead (device: 64-pair batches)
    HostOwnInsertTask(std::uint32_t owner, std::uint32_t owners, std::uint32_t partitions, const OwnPair* pairs,
                      const std::uint32_t* part_begin, hashtable_type** tables, std::atomic<std::size_t>& fails)
        : _owner(owner), _owners(owners), _partitions(partitions), _pairs(pairs), _part_begin(part_begin),
          _tables(tables), _fails(fails) {}
    void execute(std::uint16_t) override {
        std::size_t fails = 0;
        for (std::uint32_t p = _owner; p < _partitions; p += _owners) {
            auto* base = &_tables[p]->at(0);
            const std::size_t mask = _tables[p]->slot_count() - 1U;
            const std::size_t b = _part_begin[p], e = _part_begin[p + 1];
            for (std::size_t j = b; j < e; ++j) {
                if (j + kPrefetchDistance < e) {
                    __builtin_prefetch(&base[hashtable_type::bucket_hash(_pairs[j + kPrefetchDistance].key) & mask], 1);
                }
                if (!hashtable_type::insert_plain(base, mask, _pairs[j].key, _pairs[j].row)) ++fails;
            }
        }
        if (fails) _fails.fetch_add(fails, std::memory_order_relaxed);
    }
private:
    std::uint32_t _owner, _owners, _partitions;
    const OwnPair* _pairs;
    const std::uint32_t* _part_begin;
    hashtable_type** _tables;
    std::atomic<std::size_t>& _fails;
};

class ProbeTask final : public ::Task {
public:
    using hashtable_type = application::hash_join::hashtable_type;

    ProbeTask(std::vector<std::pair<std::size_t, std::size_t>> &result_set, const std::size_t size, hashtable_type* hashtable)
        : _result_set(result_set), _hashtable(hashtable) { _keys.reserve(size); }
        
    void execute(const std::uint16_t) override {
        for (const auto key : _keys) {
            const auto row = _hashtable->get(key);
            if (row != std::numeric_limits<std::uint32_t>::max()) {
                _result_set.emplace_back(key, static_cast<std::size_t>(row));
            }
        }
    }
    
    void emplace_back(const std::size_t /*row_id*/, const std::uint32_t key) noexcept { _keys.emplace_back(key); }
    std::uint64_t size() const noexcept { return _keys.size(); }
    bool empty() const noexcept { return _keys.empty(); }
private:
    std::vector<std::uint32_t> _keys;
    std::vector<std::pair<std::size_t, std::size_t>> &_result_set;
    hashtable_type* _hashtable;
};

// C26: probe this task's own slice directly, without routing (as the device probe does). Small batches then
// do not create tens of thousands of per-partition routing tasks. Results go to this task's own result set.
class HostDirectProbeTask final : public ::Task {
public:
    HostDirectProbeTask(std::size_t start, std::size_t count, const std::uint32_t* keys, const std::uint32_t* filter,
                        std::uint32_t threshold, hashtable_type** tables, std::uint16_t partitions,
                        std::vector<std::pair<std::size_t, std::size_t>>& out)
        : _start(start), _count(count), _keys(keys), _filter(filter), _threshold(threshold),
          _tables(tables), _partitions(partitions), _out(out) {}
    void execute(std::uint16_t) override {
        for (std::size_t i = _start; i < _start + _count; ++i) {
            if (_filter[i] > _threshold) continue;
            const std::uint32_t key = _keys[i];
            const auto row = _tables[hashtable_type::partition_hash(key) % _partitions]->get(key);
            if (row != std::numeric_limits<std::uint32_t>::max()) _out.emplace_back(key, static_cast<std::size_t>(row));
        }
    }
private:
    std::size_t _start, _count;
    const std::uint32_t* _keys;
    const std::uint32_t* _filter;
    std::uint32_t _threshold;
    hashtable_type** _tables;
    std::uint16_t _partitions;
    std::vector<std::pair<std::size_t, std::size_t>>& _out;
};

template <typename T>
struct AlignedValue {
    alignas(64) T data;
    T& value() noexcept { return data; }
    const T& value() const noexcept { return data; }
};

class MergeTask final : public ::Task {
public:
    // C31: reserve only the first `reserved_sets` sets (default all). The host-direct probe (C26) writes
    // one set per worker, not per partition, so it reserves rows / workers for the workers' sets only.
    MergeTask(const std::uint16_t count_cores, const std::uint64_t output_per_core, std::uint16_t reserved_sets = 0xFFFF)
        : _count_cores(count_cores) {
        _result_sets = new AlignedValue<std::vector<std::pair<std::size_t, std::size_t>>>[count_cores];
        reserved_sets = std::min(reserved_sets, count_cores);
        for (std::uint16_t channel_id = 0U; channel_id < reserved_sets; ++channel_id) {
            _result_sets[channel_id].value().reserve(output_per_core);
        }
    }

    ~MergeTask() override {
        delete[] _result_sets;
    }

    void execute(const std::uint16_t) override {
        _count_output_tuples = 0;
        for (std::uint16_t channel = 0U; channel < _count_cores; ++channel) {
            _count_output_tuples += result_set(channel).size();
        }
    }

    std::vector<std::pair<std::size_t, std::size_t>> &result_set(const std::uint16_t channel_id) {
        return _result_sets[channel_id].value();
    }

    [[nodiscard]] const std::vector<std::pair<std::size_t, std::size_t>> &result_set(const std::uint16_t channel_id) const {
        return _result_sets[channel_id].value();
    }

    [[nodiscard]] std::size_t count_tuples() const noexcept { return _count_output_tuples; }

private:
    const std::uint16_t _count_cores;
    std::size_t _count_output_tuples{0U};
    AlignedValue<std::vector<std::pair<std::size_t, std::size_t>>> *_result_sets;
};

template <typename T> class PartitionTask final : public ::Task {
public:
    using hashtable_ptr = typename T::hashtable_type*;

    constexpr PartitionTask(const std::uint16_t count_cores, const std::uint32_t batch_size,
                            const std::size_t offset, const std::size_t count,
                            const hashtable_ptr *hash_tables, const std::uint32_t *data, 
                            const std::uint32_t *filter_data, MergeTask *merge_task = nullptr,
                            std::uint32_t filter_threshold = std::numeric_limits<std::uint32_t>::max()) noexcept
        : _count_cores(count_cores), _batch_size(batch_size), _offset(offset), _count(count), _hash_tables(hash_tables), 
          _data(data), _filter_data(filter_data), _merge_task(merge_task), _filter_threshold(filter_threshold) {}

    void execute(std::uint16_t) override {
        assert(_data != nullptr && "PartitionTask: _data pointer cannot be null!");
        assert(_filter_data != nullptr && "PartitionTask: _filter_data pointer cannot be null!");
        
        if (_count == 0) return;
        
        std::vector<std::unique_ptr<T>> build_probe_tasks(_count_cores);

        // C25b: create a partition's task when its first key arrives and reserve for the expected key count (before:
        // partitions x _batch_size allocated up front, i.e. tens of thousands of allocations per small batch).
        const std::size_t expected = _count / _count_cores + 16U;
        const std::size_t reserve_keys = std::min<std::size_t>(_batch_size, 2U * expected);
        auto create_task = [&](std::size_t channel) {
            return std::make_unique<T>(_merge_task->result_set(channel), reserve_keys, _hash_tables[channel]);
        };

        for (std::size_t data_index = 0U; data_index < _count; ++data_index) {
            const auto filter_val = _filter_data[data_index];
            if (filter_val > _filter_threshold) continue;

            const auto key = _data[data_index];
            const auto target_channel_id = hashtable_type::partition_hash(key) % _count_cores;
            auto& task = build_probe_tasks[target_channel_id];
            if (!task) task = create_task(target_channel_id);
            task->emplace_back(_offset + data_index, key);

            if (task->size() == _batch_size) {
                MorselScheduler::get().spawn(std::move(task), target_channel_id);
            }
        }

        for (std::uint16_t target_channel_id = 0U; target_channel_id < _count_cores; ++target_channel_id) {
            if (build_probe_tasks[target_channel_id] && !build_probe_tasks[target_channel_id]->empty()) {
                MorselScheduler::get().spawn(std::move(build_probe_tasks[target_channel_id]), target_channel_id);
            }
        }
    }
private:
    const std::uint16_t _count_cores;
    const std::uint32_t _batch_size;
    const std::size_t _offset;
    const std::size_t _count;
    const hashtable_ptr *_hash_tables;
    const std::uint32_t *_data;
    const std::uint32_t *_filter_data;
    MergeTask *_merge_task;
    std::uint32_t _filter_threshold;
};

} // namespace application::hash_join