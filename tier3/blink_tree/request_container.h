#pragma once
#include <atomic>
#include <memory>
#include <limits>
#include <algorithm>
#include <utility>
#include "workload.h"
#include "benchmark_config.h"
#include "blink_tree_utils.h"

#ifdef ENABLE_VALIDATION
#include "golden_reference.h"
#endif

namespace application::blinktree_benchmark {

class RequestIndex {
public:
    [[nodiscard]] static constexpr RequestIndex make_finished() noexcept { 
        return {std::numeric_limits<std::uint64_t>::max(), 0}; 
    }
    [[nodiscard]] static constexpr RequestIndex make_no_new() noexcept { 
        return {0, 0}; 
    }

    constexpr RequestIndex() noexcept = default;
    constexpr RequestIndex(std::uint64_t index, std::uint64_t count) noexcept 
        : _index(index), _count(count) {} 
    
    constexpr RequestIndex(std::pair<std::uint64_t, std::uint64_t> p) noexcept
        : _index(p.first), _count(p.second) {}

    [[nodiscard]] constexpr std::uint64_t index() const noexcept { return _index; }
    [[nodiscard]] constexpr std::uint64_t count() const noexcept { return _count; }
    [[nodiscard]] constexpr bool is_finished() const noexcept { return _index == std::numeric_limits<std::uint64_t>::max(); }
    [[nodiscard]] constexpr bool has_new() const noexcept { return _count > 0; }

    constexpr RequestIndex& operator-=(std::uint64_t count) noexcept {
        _count -= count; 
        _index += count; 
        return *this;
    }

private:
    std::uint64_t _index{0};
    std::uint64_t _count{0};
};

class RequestContainer {
public:
#ifdef ENABLE_VALIDATION
    RequestContainer(std::uint16_t /*core_id*/, std::uint64_t max_open_requests, Workload &workload,
                     std::shared_ptr<GoldenReferenceValidator> validator) noexcept
        : _finished_requests(workload.initial_keys()),
          _scheduled_requests(workload.initial_keys()),
          _local_buffer(workload.next(BenchmarkConfig::batch_size())), 
          _max_pending_requests(max_open_requests), 
          _workload(workload),
          _validator(std::move(validator)) {}
#else
    RequestContainer(std::uint16_t /*core_id*/, std::uint64_t max_open_requests, Workload &workload) noexcept
        : _finished_requests(workload.initial_keys()),
          _scheduled_requests(workload.initial_keys()),
          _local_buffer(workload.next(BenchmarkConfig::batch_size())), 
          _max_pending_requests(max_open_requests), 
          _workload(workload) {}
#endif

    [[nodiscard]] RequestIndex next() noexcept {
        const auto sched = _scheduled_requests.load(std::memory_order_relaxed);
        const auto pending = sched - _finished_requests.load(std::memory_order_acquire);
        if (pending >= _max_pending_requests) return RequestIndex::make_no_new();

        if (!_local_buffer.has_new()) {
            _local_buffer = RequestIndex(_workload.next(BenchmarkConfig::batch_size()));
        }

        if (_local_buffer.has_new()) {
            const auto count = std::min(_max_pending_requests - pending, _local_buffer.count());
            _scheduled_requests.fetch_add(count, std::memory_order_release);
            RequestIndex index{_local_buffer.index(), count};
            _local_buffer -= count;
            return index;
        }
        return pending > 0 ? RequestIndex::make_no_new() : RequestIndex::make_finished();
    }

#ifdef MEASURE_LATENCY
    void inserted([[maybe_unused]] std::uint16_t core_id, 
                  [[maybe_unused]] std::uint64_t key, 
                  [[maybe_unused]] std::int64_t val, 
                  db::index::blinktree::TimePoint start_time = {}) noexcept { 
        db::index::blinktree::record_latency(start_time);
#ifdef ENABLE_VALIDATION
        if (_validator) _validator->inserted(core_id, key, val);
#endif
        task_finished(); 
    }

    void updated([[maybe_unused]] std::uint16_t core_id, 
                 [[maybe_unused]] std::uint64_t key, 
                 [[maybe_unused]] std::int64_t val, 
                 db::index::blinktree::TimePoint start_time = {}) noexcept { 
        db::index::blinktree::record_latency(start_time);
#ifdef ENABLE_VALIDATION
        if (_validator) _validator->updated(core_id, key, val);
#endif
        task_finished(); 
    }

    void missing([[maybe_unused]] std::uint16_t core_id, 
                 [[maybe_unused]] std::uint64_t key, 
                 db::index::blinktree::TimePoint start_time = {}) noexcept { 
        db::index::blinktree::record_latency(start_time);
#ifdef ENABLE_VALIDATION
        if (_validator) _validator->missing(core_id, key);
#endif
        task_finished(); 
    }

    void found([[maybe_unused]] std::uint16_t core_id, 
               [[maybe_unused]] std::uint64_t key, 
               [[maybe_unused]] std::int64_t val, 
               db::index::blinktree::TimePoint start_time = {}) noexcept { 
        db::index::blinktree::record_latency(start_time);
#ifdef ENABLE_VALIDATION
        if (_validator) _validator->found(core_id, key, val);
#endif
        task_finished(); 
    }
#else
    void inserted([[maybe_unused]] std::uint16_t core_id, 
                  [[maybe_unused]] std::uint64_t key, 
                  [[maybe_unused]] std::int64_t val) noexcept { 
#ifdef ENABLE_VALIDATION
        if (_validator) _validator->inserted(core_id, key, val);
#endif
        task_finished(); 
    }

    void updated([[maybe_unused]] std::uint16_t core_id, 
                 [[maybe_unused]] std::uint64_t key, 
                 [[maybe_unused]] std::int64_t val) noexcept { 
#ifdef ENABLE_VALIDATION
        if (_validator) _validator->updated(core_id, key, val);
#endif
        task_finished(); 
    }

    void missing([[maybe_unused]] std::uint16_t core_id, 
                 [[maybe_unused]] std::uint64_t key) noexcept { 
#ifdef ENABLE_VALIDATION
        if (_validator) _validator->missing(core_id, key);
#endif
        task_finished(); 
    }

    void found([[maybe_unused]] std::uint16_t core_id, 
               [[maybe_unused]] std::uint64_t key, 
               [[maybe_unused]] std::int64_t val) noexcept { 
#ifdef ENABLE_VALIDATION
        if (_validator) _validator->found(core_id, key, val);
#endif
        task_finished(); 
    }
#endif

    void separator_started() noexcept { _pending_separator_tasks.fetch_add(1, std::memory_order_release); }
    void separator_finished() noexcept { _pending_separator_tasks.fetch_sub(1, std::memory_order_release); }

    // Partitioned host dispatch: workers walk the workload themselves, so mark everything scheduled.
    void take_all() noexcept {
        _local_buffer = RequestIndex::make_no_new();
        _scheduled_requests.store(_workload.size(), std::memory_order_release);
    }

    [[nodiscard]] std::int64_t pending_separators() const noexcept {
        return _pending_separator_tasks.load(std::memory_order_acquire);
    }

    [[nodiscard]] bool is_finished() const noexcept {
        const auto sched = _scheduled_requests.load(std::memory_order_acquire);
        return (sched == _workload.size()) &&
               (_finished_requests.load(std::memory_order_acquire) == sched) &&
               !_local_buffer.has_new() &&
               (_pending_separator_tasks.load(std::memory_order_acquire) == 0);
    }

    [[nodiscard]] const NumericTuple& operator[](std::size_t index) const noexcept { return _workload[index]; }

    [[nodiscard]] std::uint64_t finished_requests() const noexcept {
        return _finished_requests.load(std::memory_order_relaxed);
    }

    [[nodiscard]] std::uint64_t total_requests() const noexcept {
        return _workload.size();
    }
    
private:
    std::atomic<std::uint64_t> _finished_requests{0};
    std::atomic<std::int64_t> _pending_separator_tasks{0};
    std::atomic<std::uint64_t> _scheduled_requests{0};
    RequestIndex _local_buffer;
    const std::uint64_t _max_pending_requests;
    Workload &_workload;

#ifdef ENABLE_VALIDATION
    std::shared_ptr<GoldenReferenceValidator> _validator{nullptr};
#endif

    void task_finished() noexcept { 
        _finished_requests.fetch_add(1, std::memory_order_release); 
    }
};

} // namespace application::blinktree_benchmark