#pragma once
#include <map>
#include <unordered_map>
#include <vector>
#include <mutex>
#include <atomic>
#include <iostream>
#include <memory>
#include <cstdint>
#include <set>
#include "workload.h"
#include "blink_tree.h"
#include "blink_tree_tasks.h"
#include "blink_tree_utils.h"

namespace application::blinktree_benchmark {

struct ValidationReport {
    std::uint64_t total_operations{0};
    std::atomic<std::uint64_t> success_count{0};
    std::atomic<std::uint64_t> mismatch_count{0};
    std::atomic<std::uint64_t> final_state_mismatch_count{0};
    std::atomic<bool> passed{true};

    void print() const {
        std::cout << "\n========================================\n";
        std::cout << "   GOLDEN REFERENCE VALIDATION REPORT   \n";
        std::cout << "========================================\n";
        std::cout << " * Total Ops Validated     : " << total_operations << "\n";
        std::cout << " * Real-time Callback Match: " << success_count.load() << "\n";
        std::cout << " * Real-time Mismatches    : " << mismatch_count.load() << "\n";
        std::cout << " * Tree-Map Final State Diff: " << final_state_mismatch_count.load() << "\n";
        std::cout << " * Final Integrity Status  : " << (passed.load() ? "🟢 PASSED (1:1 Exact Match)" : "🔴 FAILED (Corruption Detected)") << "\n";
        std::cout << "========================================\n";
    }
};

class GoldenReferenceValidator : public db::index::blinktree::Listener<std::uint64_t, std::int64_t> {
public:
    enum class OpType { INSERT, UPDATE, REMOVE, FOUND, MISSING };

    struct ExpectedEvent {
        OpType op;
        std::uint64_t key;
        std::int64_t value;
    };

    GoldenReferenceValidator(std::uint16_t num_channels, const Workload& workload)
        : _num_channels(num_channels), _finished_channels(0), _finished(false) {
        build_golden_reference(workload);
    }

    void inserted([[maybe_unused]] std::uint16_t core_id, std::uint64_t key, std::int64_t value) override {
        verify_event(key, {OpType::INSERT, key, value});
    }

    void updated([[maybe_unused]] std::uint16_t core_id, std::uint64_t key, std::int64_t value) override {
        verify_event(key, {OpType::UPDATE, key, value});
    }

    void removed([[maybe_unused]] std::uint16_t core_id, std::uint64_t key) override {
        verify_event(key, {OpType::REMOVE, key, 0});
    }

    void found([[maybe_unused]] std::uint16_t core_id, std::uint64_t key, std::int64_t value) override {
        verify_event(key, {OpType::FOUND, key, value});
    }

    void missing([[maybe_unused]] std::uint16_t core_id, std::uint64_t key) override {
        verify_event(key, {OpType::MISSING, key, 0});
    }

    void requests_finished() override {
        if (_finished_channels.fetch_add(1, std::memory_order_acq_rel) + 1 == _num_channels) {
            _finished.store(true, std::memory_order_release);
        }
    }

    void separator_started() override {}
    void separator_finished() override {}

    bool wait_until_finish() const {
        return _finished.load(std::memory_order_acquire);
    }

    template <typename TreePtrContainer>
    bool verify_final_state(const TreePtrContainer& trees) {
        std::lock_guard<std::mutex> lock(_log_mutex);
        
        std::map<std::uint64_t, std::int64_t> actual_tree_map;
        std::set<void*> visited_roots;

        for (auto* tree : trees) {
            if (!tree || !tree->root() || visited_roots.count(tree) > 0) continue;
            visited_roots.insert(tree);

            // descend from the root to the leftmost leaf
            // invalidate so device-written nodes are read from memory, not from stale host-cache copies
            cxl_inval_cache_range(tree, sizeof(*tree));
            auto* curr = tree->root();
            if (curr) cxl_inval_cache_range(curr, sizeof(*curr));
            while (curr && !curr->is_leaf()) {
                curr = curr->child(0);
                if (curr) cxl_inval_cache_range(curr, sizeof(*curr));
            }

            // walk every leaf along right_sibling links, starting from the leftmost
            db::index::blinktree::NodeIterator<std::uint64_t, std::int64_t> it(curr);
            db::index::blinktree::NodeIterator<std::uint64_t, std::int64_t> end_it;
            while (it != end_it) {
                cxl_inval_cache_range(&*it, sizeof(*it));
                if (it->is_leaf()) {
                    for (std::uint16_t i = 0; i < it->size(); ++i) {
                        actual_tree_map[it->leaf_key(i)] = it->value(i);
                    }
                }
                ++it;
            }
        }

        bool state_matched = true;
        if (actual_tree_map.size() != _final_golden_map.size()) {
            std::cerr << "[❌ Final State Error] Total Key Count Mismatch! Golden Map: " 
                      << _final_golden_map.size() << ", B-Link Tree: " 
                      << actual_tree_map.size() << "\n";
            state_matched = false;
            _report.final_state_mismatch_count.fetch_add(1, std::memory_order_relaxed);
        }

        for (const auto& [key, golden_val] : _final_golden_map) {
            auto it = actual_tree_map.find(key);
            if (it == actual_tree_map.end()) {
                std::cerr << "[❌ Final State Missing] Key: " << key << " exists in Golden Map but missing in B-Link Tree!\n";
                state_matched = false;
                _report.final_state_mismatch_count.fetch_add(1, std::memory_order_relaxed);
            } else if (it->second != golden_val) {
                std::cerr << "[❌ Final State Value Mismatch] Key: " << key 
                          << " Expected: " << golden_val 
                          << ", Actual Tree Value: " << it->second << "\n";
                state_matched = false;
                _report.final_state_mismatch_count.fetch_add(1, std::memory_order_relaxed);
            }
        }

        if (!state_matched) {
            _report.passed.store(false, std::memory_order_relaxed);
        }
        return state_matched;
    }

    const ValidationReport& report() const { return _report; }

private:
    struct PerKeyQueue {
        std::mutex mutex;
        std::vector<ExpectedEvent> events;
        std::size_t current_idx{0};
    };

    void build_golden_reference(const Workload& workload) {
        _report.total_operations = workload.size();
        
        for (std::size_t i = 0; i < workload.size(); ++i) {
            const auto& tuple = workload[i];
            std::uint64_t k = tuple.key();
            std::int64_t v = tuple.value();

            auto& queue_ptr = _per_key_events[k];
            if (!queue_ptr) {
                queue_ptr = std::make_unique<PerKeyQueue>();
            }

            auto map_it = _final_golden_map.find(k);

            if (tuple.op == TupleOp::INSERT) {
                _final_golden_map[k] = v;
                queue_ptr->events.push_back({OpType::INSERT, k, v});
            } else if (tuple.op == TupleOp::LOOKUP) {
                if (map_it != _final_golden_map.end()) {
                    queue_ptr->events.push_back({OpType::FOUND, k, map_it->second});
                } else {
                    queue_ptr->events.push_back({OpType::MISSING, k, 0});
                }
            } else if (tuple.op == TupleOp::UPDATE) {
                if (map_it != _final_golden_map.end()) {
                    _final_golden_map[k] = v;
                    queue_ptr->events.push_back({OpType::UPDATE, k, v});
                } else {
                    queue_ptr->events.push_back({OpType::MISSING, k, 0});
                }
            }
        }
    }

    void verify_event(std::uint64_t key, const ExpectedEvent& actual) {
        auto it = _per_key_events.find(key);
        if (it == _per_key_events.end() || !it->second) {
            std::lock_guard<std::mutex> lock(_log_mutex);
            std::cerr << "[❌ Unexpected Key] Received event for unknown key: " << key << "\n";
            _report.mismatch_count.fetch_add(1, std::memory_order_relaxed);
            _report.passed.store(false, std::memory_order_relaxed);
            return;
        }

        auto& key_queue = *(it->second);
        
        ExpectedEvent expected{};
        bool has_extra_event = false;

        {
            std::lock_guard<std::mutex> lock(key_queue.mutex);
            if (key_queue.current_idx >= key_queue.events.size()) {
                has_extra_event = true;
            } else {
                expected = key_queue.events[key_queue.current_idx++];
            }
        }

        if (has_extra_event) {
            std::lock_guard<std::mutex> log_lock(_log_mutex);
            std::cerr << "[❌ Extra Event] Unexpected extra event for key: " << key 
                      << " (Op: " << op_to_string(actual.op) << ")\n";
            _report.mismatch_count.fetch_add(1, std::memory_order_relaxed);
            _report.passed.store(false, std::memory_order_relaxed);
            return;
        }

        if (expected.op != actual.op || (expected.op != OpType::MISSING && expected.value != actual.value)) {
            std::lock_guard<std::mutex> log_lock(_log_mutex);
            std::cerr << "[❌ Event Mismatch] Key: " << key << "\n"
                      << "   Expected: Op=" << op_to_string(expected.op) << ", Val=" << expected.value << "\n"
                      << "   Actual  : Op=" << op_to_string(actual.op) << ", Val=" << actual.value << "\n";
            _report.mismatch_count.fetch_add(1, std::memory_order_relaxed);
            _report.passed.store(false, std::memory_order_relaxed);
        } else {
            _report.success_count.fetch_add(1, std::memory_order_relaxed);
        }
    }

    static const char* op_to_string(OpType op) {
        switch (op) {
            case OpType::INSERT:  return "INSERT";
            case OpType::UPDATE:  return "UPDATE";
            case OpType::REMOVE:  return "REMOVE";
            case OpType::FOUND:   return "FOUND";
            case OpType::MISSING: return "MISSING";
        }
        return "UNKNOWN";
    }

    std::uint16_t _num_channels;
    std::atomic<std::uint16_t> _finished_channels{0};
    std::atomic<bool> _finished{false};

    std::map<std::uint64_t, std::int64_t> _final_golden_map;
    std::unordered_map<std::uint64_t, std::unique_ptr<PerKeyQueue>> _per_key_events;
    
    ValidationReport _report;
    std::mutex _log_mutex;
};

} // namespace application::blinktree_benchmark