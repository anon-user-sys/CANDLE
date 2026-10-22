#pragma once
#include <shared_mutex>
#include <thread>
#include <chrono>
#include <string>
#include <cstdlib>
#include "blink_tree.h"
#include "blink_tree_scheduler.h"

namespace db::index::blinktree {

using Clock = std::chrono::high_resolution_clock;
using TimePoint = Clock::time_point;

// ==========================================
// 1. INSERT SEPARATOR TASK
// ==========================================
template <typename K, typename V, class L> 
class InsertSeparatorTask final : public Task<K, V, L> {
public:
    constexpr InsertSeparatorTask(const K key, Node<K, V>* separator, BLinkTree<K, V> *tree, L &listener, Node<K, V>* start_node = nullptr) noexcept
        : Task<K, V, L>(key, listener), _tree(tree), _separator(separator), _current_node(start_node) {}

    void execute([[maybe_unused]] std::uint16_t thread_id) override {
        auto *node = _current_node ? _current_node : _tree->root();
        const std::uint16_t total_workers = BLinkTreeScheduler::get().num_workers();
        const std::uint8_t target_level = _separator->level() + 1;
        bool need_flush = true;

        while (true) {
            if (node == nullptr) {
#ifdef USE_CXL
                cxl_inval_cache_range(_tree, sizeof(BLinkTree<K, V>));
#endif
                node = _tree->root();
                if (node == nullptr) { 
                    std::this_thread::sleep_for(std::chrono::microseconds(10)); continue; }
                need_flush = true;
            }

#ifdef USE_CXL
            if (need_flush) {
                cxl_inval_cache_range(node, sizeof(Node<K, V>));
                need_flush = false;
            }
#endif
            
            std::uint32_t v1 = node->get_version();
            if (v1 & 1) { 
                std::this_thread::sleep_for(std::chrono::microseconds(1)); 
                need_flush = true;
                continue; 
            }

            if (node->level() < target_level) {
                std::this_thread::sleep_for(std::chrono::microseconds(1));
#ifdef USE_CXL
                cxl_inval_cache_range(_tree, sizeof(BLinkTree<K, V>));
#endif
                node = _tree->root();
                need_flush = true;
                continue;
            }

            if (node->level() > target_level) {
                K hk = node->high_key();
                auto* next_sibling = node->right_sibling();
                if (node->is_dirty(v1)) {
                    need_flush = true;
                    continue;
                }

                if (hk <= this->_key && next_sibling != nullptr) {
                    node = next_sibling;
                    need_flush = true;
                    continue;
                }
                
                auto* child_node = node->child(this->_key);
                if (node->is_dirty(v1) || child_node == nullptr) {
                    std::this_thread::sleep_for(std::chrono::microseconds(1));
#ifdef USE_CXL
                    cxl_inval_cache_range(_tree, sizeof(BLinkTree<K, V>));
#endif
                    node = _tree->root();
                    need_flush = true;
                    continue;
                }
                node = child_node;
                need_flush = true;
                continue;
            }

            K hk = node->high_key();
            auto* next_sibling = node->right_sibling();
            if (node->is_dirty(v1)) {
                need_flush = true;
                continue;
            }

            if (hk <= this->_key && next_sibling != nullptr) {
                node = next_sibling;
                need_flush = true;
                continue;
            }

            node->lock();
            // past high_key: never continue; move right or retry from the root
            if (node->high_key() <= this->_key) {
                auto* ns = node->right_sibling();
                node->unlock();
                if (ns != nullptr) {
                    node = ns;
                } else { 
                    std::this_thread::sleep_for(std::chrono::microseconds(1)); 
#ifdef USE_CXL
                    cxl_inval_cache_range(_tree, sizeof(BLinkTree<K, V>));
#endif
                    node = _tree->root(); 
                }
                need_flush = true;
                continue;
            }

            if (!node->full()) {
                std::uint16_t idx = 0;
                while (idx < node->size() && node->leaf_key(idx) < this->_key) {
                    idx++;
                }
                node->insert(idx, _separator, this->_key);
                BLINK_TRACE_LOG("[T] sepinsert node=%p key=%lu sep=%p size=%u high=%lu\n", (void*)node,
                                (unsigned long)this->_key, (void*)_separator, node->size(), (unsigned long)node->high_key());
                
#ifdef USE_CXL
                cxl_inval_cache_range(node, sizeof(Node<K, V>));
                if (_separator) {
                    cxl_inval_cache_range(_separator, sizeof(Node<K, V>));
                }
#endif
                node->unlock();
                
                this->_listener.separator_finished();
                return;
            }

            BLINK_TRACE_LOG("[T] sep-full node=%p key=%lu sep=%p\n", (void*)node, (unsigned long)this->_key, (void*)_separator);
            auto [right, key] = this->_tree->split(node, this->_key, _separator);

            if (node->parent() != nullptr) {
                auto* parent_node = node->parent();
                this->_listener.separator_started();
                BLinkTreeScheduler::get().spawn(
                    std::make_unique<InsertSeparatorTask<K, V, L>>(key, right, _tree, this->_listener, parent_node), 
                    key % total_workers
                );
                this->_listener.separator_finished();
                return;
            }
            
            if (!this->_tree->create_new_root(node, right, key)) {
                auto* retry_start = node->parent() ? node->parent() : this->_tree->root();
                this->_listener.separator_started();
                BLinkTreeScheduler::get().spawn(
                    std::make_unique<InsertSeparatorTask<K, V, L>>(key, right, _tree, this->_listener, retry_start), 
                    key % total_workers
                );
            }
            this->_listener.separator_finished();
            return;
        }
    }

private:
    BLinkTree<K, V> *_tree;
    Node<K, V>* _separator;
    Node<K, V>* _current_node;
};

// ==========================================
// 2. INSERT VALUE TASK
// ==========================================
template <typename K, typename V, class L> 
class InsertValueTask final : public Task<K, V, L> {
public:
    constexpr InsertValueTask(const K key, const V value, BLinkTree<K, V> *tree, L &listener, Node<K, V>* start_node = nullptr
#ifdef MEASURE_LATENCY
                              , TimePoint start_time = TimePoint{}
#endif
    ) noexcept
        : Task<K, V, L>(key, listener), _tree(tree), _value(value), _current_node(start_node)
#ifdef MEASURE_LATENCY
          , _start_time(start_time)
#endif
    {}

    void execute(std::uint16_t thread_id) override {
        auto *node = _current_node ? _current_node : _tree->root();
        const std::uint16_t total_workers = BLinkTreeScheduler::get().num_workers();
        bool need_flush = true;

        while (true) {
            if (node == nullptr) {
#ifdef USE_CXL
                cxl_inval_cache_range(_tree, sizeof(BLinkTree<K, V>));
#endif
                node = _tree->root();
                if (node == nullptr) { 
                    std::this_thread::sleep_for(std::chrono::microseconds(10)); continue; }
                need_flush = true;
            }

#ifdef USE_CXL
            if (need_flush) {
                cxl_inval_cache_range(node, sizeof(Node<K, V>));
                need_flush = false;
            }
#endif

            std::uint32_t v1 = node->get_version();
            if (v1 & 1) { 
                std::this_thread::sleep_for(std::chrono::microseconds(1)); 
                need_flush = true;
                continue; 
            }

            K hk = node->high_key();
            auto* next_sibling = node->right_sibling();
            if (node->is_dirty(v1)) {
                need_flush = true;
                continue;
            }

            if (hk <= this->_key && next_sibling != nullptr) {
#ifdef USE_CXL
                // leaving a node we only read: drop the host copy, or it could later be written back
                // over device updates to it (the Hybrid lost-update mechanism, sec:blink)
                cxl_inval_cache_range(node, sizeof(Node<K, V>));
#endif
                node = next_sibling;
                need_flush = true;
                continue;
            }

            if (node->is_inner()) {
                auto* child_node = node->child(this->_key);
                if (node->is_dirty(v1) || child_node == nullptr) {
                    std::this_thread::sleep_for(std::chrono::microseconds(1));
#ifdef USE_CXL
                    cxl_inval_cache_range(_tree, sizeof(BLinkTree<K, V>));
#endif
                    node = _tree->root();
                    need_flush = true;
                    continue;
                }
                node = child_node;
                need_flush = true;
                continue;
            }

            node->lock();
            // past high_key: never continue; move right or retry from the root
            if (node->high_key() <= this->_key) {
                auto* ns = node->right_sibling();
#ifdef USE_CXL
                cxl_inval_cache_range(node, sizeof(Node<K, V>));   // read but not changed: drop the host copy
#endif
                node->unlock();
                if (ns != nullptr) {
                    node = ns;
                } else { 
                    std::this_thread::sleep_for(std::chrono::microseconds(1)); 
#ifdef USE_CXL
                    cxl_inval_cache_range(_tree, sizeof(BLinkTree<K, V>));
#endif
                    node = _tree->root(); 
                }
                need_flush = true;
                continue;
            }

            const auto index = node->index(this->_key);
            if (index < node->size() && node->leaf_key(index) == this->_key) {
                node->value(index, this->_value);
                _tree->notify_write();

#ifdef USE_CXL
                cxl_inval_cache_range(node, sizeof(Node<K, V>));
#endif
                node->unlock();

                this->_listener.inserted(thread_id, this->_key, this->_value
#ifdef MEASURE_LATENCY
                                         , this->_start_time
#endif
                );
                return;
            }

            if (!node->full()) {
                node->insert(index, this->_value, this->_key);
                _tree->notify_write();

#ifdef USE_CXL
                cxl_inval_cache_range(node, sizeof(Node<K, V>));
#endif
                node->unlock();

                this->_listener.inserted(thread_id, this->_key, this->_value
#ifdef MEASURE_LATENCY
                                         , this->_start_time
#endif
                );
                return;
            }

            auto [right, key] = this->_tree->split(node, this->_key, this->_value);

            if (node->parent() != nullptr) {
                auto* parent_node = node->parent();
                this->_listener.separator_started();
                this->_listener.inserted(thread_id, this->_key, this->_value
#ifdef MEASURE_LATENCY
                                         , this->_start_time
#endif
                );

                BLinkTreeScheduler::get().spawn(
                    std::make_unique<InsertSeparatorTask<K, V, L>>(key, right, _tree, this->_listener, parent_node), 
                    key % total_workers
                );
                return;
            }
            
            if (!this->_tree->create_new_root(node, right, key)) {
                auto* retry_parent = node->parent() ? node->parent() : this->_tree->root();
                this->_listener.separator_started();
                this->_listener.inserted(thread_id, this->_key, this->_value
#ifdef MEASURE_LATENCY
                                         , this->_start_time
#endif
                );

                BLinkTreeScheduler::get().spawn(
                    std::make_unique<InsertSeparatorTask<K, V, L>>(key, right, _tree, this->_listener, retry_parent), 
                    key % total_workers
                );
                return;
            }

            this->_listener.inserted(thread_id, this->_key, this->_value
#ifdef MEASURE_LATENCY
                                     , this->_start_time
#endif
            );
            return;
        }
    }

private:
    BLinkTree<K, V> *_tree;
    const V _value;
    Node<K, V> *_current_node;
#ifdef MEASURE_LATENCY
    TimePoint _start_time{};
#endif
};

// ==========================================
// 3. LOOKUP TASK
// ==========================================
template <typename K, typename V, class L> 
class LookupTask final : public Task<K, V, L> {
public:
    LookupTask(const K key, L &listener, BLinkTree<K, V> *tree, Node<K, V>* start_node = nullptr
#ifdef MEASURE_LATENCY
               , TimePoint start_time = TimePoint{}
#endif
    ) noexcept 
        : Task<K, V, L>(key, listener), _tree(tree), _current_node(start_node)
#ifdef MEASURE_LATENCY
          , _start_time(start_time)
#endif
    {}

    void execute(std::uint16_t thread_id) override {
        auto *node = _current_node ? _current_node : _tree->root();
        bool need_flush = true;

        while (true) {
            if (node == nullptr) {
#ifdef USE_CXL
                cxl_inval_cache_range(_tree, sizeof(BLinkTree<K, V>));
#endif
                node = _tree->root();
                if (node == nullptr) { 
                    std::this_thread::sleep_for(std::chrono::microseconds(10)); continue; }
                need_flush = true;
            }

#ifdef USE_CXL
            if (need_flush) {
                cxl_inval_cache_range(node, sizeof(Node<K, V>));
                need_flush = false;
            }
#endif

            std::uint32_t v1 = node->get_version(); 
            if (v1 & 1) { 
                std::this_thread::sleep_for(std::chrono::microseconds(1)); 
                need_flush = true;
                continue; 
            }

            K hk = node->high_key();
            auto* next_sibling = node->right_sibling();
            if (node->is_dirty(v1)) {
                need_flush = true;
                continue;
            }

            if (hk <= this->_key && next_sibling != nullptr) {
                node = next_sibling;
                need_flush = true;
                continue;
            }

            if (node->is_inner()) {
                auto* next_node = node->child(this->_key); 
                if (node->is_dirty(v1) || next_node == nullptr) {
                    std::this_thread::sleep_for(std::chrono::microseconds(1));                    
#ifdef USE_CXL
                    cxl_inval_cache_range(_tree, sizeof(BLinkTree<K, V>));
#endif
                    node = _tree->root();
                    need_flush = true;
                    continue; 
                }
                node = next_node;
                need_flush = true;
                continue;
            }
            
            const auto index = node->index(this->_key);
            bool is_found = (index < node->size() && node->leaf_key(index) == this->_key);
            V found_val = is_found ? node->value(index) : V{};
            
            if (node->is_dirty(v1)) {
                need_flush = true;
                continue; 
            }
            
            if (is_found) {
                this->_listener.found(thread_id, this->_key, found_val
#ifdef MEASURE_LATENCY
                                      , this->_start_time
#endif
                );
            } else {
                this->_listener.missing(thread_id, this->_key
#ifdef MEASURE_LATENCY
                                        , this->_start_time
#endif
                );
            }
            break;
        }
    }
    
private:
    BLinkTree<K, V> *_tree;
    Node<K, V>* _current_node;
    std::uint64_t _host_local_epoch{0};
#ifdef MEASURE_LATENCY
    TimePoint _start_time{};
#endif
};

// ==========================================
// 4. UPDATE TASK
// ==========================================
template <typename K, typename V, class L> 
class UpdateTask final : public Task<K, V, L> {
public:
    constexpr UpdateTask(const K key, const V value, L &listener, BLinkTree<K, V> *tree, Node<K, V>* start_node = nullptr
#ifdef MEASURE_LATENCY
                         , TimePoint start_time = TimePoint{}
#endif
    ) noexcept 
        : Task<K, V, L>(key, listener), _value(value), _tree(tree), _current_node(start_node)
#ifdef MEASURE_LATENCY
          , _start_time(start_time)
#endif
    {}

    void execute(std::uint16_t thread_id) override {
        auto *node = _current_node ? _current_node : _tree->root();
        bool need_flush = true;

        while (true) {
            if (node == nullptr) {
#ifdef USE_CXL
                cxl_inval_cache_range(_tree, sizeof(BLinkTree<K, V>));
#endif
                node = _tree->root();
                if (node == nullptr) { 
                    std::this_thread::sleep_for(std::chrono::microseconds(10)); continue; }
                need_flush = true;
            }

#ifdef USE_CXL
            if (need_flush) {
                cxl_inval_cache_range(node, sizeof(Node<K, V>));
                need_flush = false;
            }
#endif

            std::uint32_t v1 = node->get_version();
            if (v1 & 1) { 
                std::this_thread::sleep_for(std::chrono::microseconds(1)); 
                need_flush = true;
                continue; 
            }

            K hk = node->high_key();
            auto* next_sibling = node->right_sibling();
            if (node->is_dirty(v1)) {
                need_flush = true;
                continue;
            }

            if (hk <= this->_key && next_sibling != nullptr) {
                node = next_sibling;
                need_flush = true;
                continue;
            }

            if (node->is_inner()) {
                auto* child_node = node->child(this->_key);
                if (node->is_dirty(v1) || child_node == nullptr) {
                    std::this_thread::sleep_for(std::chrono::microseconds(1));
#ifdef USE_CXL
                    cxl_inval_cache_range(_tree, sizeof(BLinkTree<K, V>));
#endif
                    node = _tree->root();
                    need_flush = true;
                    continue;
                }
                node = child_node;
                need_flush = true;
                continue;
            }

            node->lock();
            // past high_key: never continue; move right or retry from the root
            if (node->high_key() <= this->_key) {
                auto* ns = node->right_sibling();
                node->unlock();
                if (ns != nullptr) {
                    node = ns;
                } else { 
                    std::this_thread::sleep_for(std::chrono::microseconds(1)); 
#ifdef USE_CXL
                    cxl_inval_cache_range(_tree, sizeof(BLinkTree<K, V>));
#endif
                    node = _tree->root(); 
                }
                need_flush = true;
                continue;
            }

            const auto index = node->index(this->_key);
            if (index < node->size() && node->leaf_key(index) == this->_key) {
                node->value(index, this->_value);
                _tree->notify_write();

#ifdef USE_CXL
                cxl_inval_cache_range(node, sizeof(Node<K, V>));
#endif
                node->unlock();

                this->_listener.updated(thread_id, this->_key, this->_value
#ifdef MEASURE_LATENCY
                                        , this->_start_time
#endif
                );
            } else {
                node->unlock();
                this->_listener.missing(thread_id, this->_key
#ifdef MEASURE_LATENCY
                                        , this->_start_time
#endif
                );
            }
            break;
        }
    }

private:
    const V _value;
    BLinkTree<K, V> *_tree;
    Node<K, V>* _current_node;
#ifdef MEASURE_LATENCY
    TimePoint _start_time{};
#endif
};

// Read-only descent used by the batched CXL path (hybrid locate): returns the leaf that holds
// or would hold key, and whether/what value it holds. Same optimistic protocol as LookupTask.
// BLINK_LOCATE_INVAL picks how much of each visited node is invalidated BEFORE reading it:
//   none (default): nothing. Safe because the batch path invalidates every visited leaf AFTER the
//         locate (process_cxl_batch), so the host never holds copies of device-written lines;
//   node: the whole 4 KiB node; lines: only the lines this descent reads (header, keys, value).
// (Earlier "faster reads lose device updates" notes were wrong: the cause was host copies of tree
// lines written back over device updates; see the post-locate invalidation in process_cxl_batch.)
enum class LocateInval : std::uint8_t { Lines, Node, None };
inline LocateInval locate_inval_mode() {
    static const LocateInval mode = [] {
        const char* e = std::getenv("BLINK_LOCATE_INVAL");
        if (e && std::string(e) == "lines") return LocateInval::Lines;
        if (e && std::string(e) == "node") return LocateInval::Node;
        return LocateInval::None;
    }();
    return mode;
}

template <typename K, typename V>
inline Node<K, V>* locate_leaf(BLinkTree<K, V>* tree, K key, bool& found, V& value) {
    const LocateInval mode = locate_inval_mode();
    // invalidate one line (Lines mode); Node/None handle the whole node up front
    auto line = [mode](const void* p) {
#ifdef USE_CXL
        if (mode == LocateInval::Lines) cxl_inval_cache_range(p, 1);
#else
        (void)p; (void)mode;
#endif
    };
    auto* node = tree->root();
    while (true) {
        if (node == nullptr) {
#ifdef USE_CXL
            cxl_inval_cache_range(tree, sizeof(BLinkTree<K, V>));
#endif
            node = tree->root();
            continue;
        }
#ifdef USE_CXL
        if (mode == LocateInval::Node) cxl_inval_cache_range(node, sizeof(Node<K, V>));
        else if (mode == LocateInval::Lines) cxl_inval_cache_range(node, 64); // header line
#endif
        const std::uint32_t v1 = mode == LocateInval::None ? node->latch.version : node->get_version();
        if (v1 & 1) { std::this_thread::yield(); continue; }
        auto dirty = [&] { return mode != LocateInval::None && node->is_dirty(v1); };

        const K hk = node->high_key();
        auto* next_sibling = node->right_sibling();
        if (dirty()) continue;
        if (hk <= key && next_sibling != nullptr) { node = next_sibling; continue; }

        const std::uint16_t sz = node->size();
        if (node->is_inner()) {
            // child = separators[upper_bound(keys, key)]
            std::int32_t lo = 0, hi = static_cast<std::int32_t>(std::min<std::uint16_t>(sz, CXLInnerNode::max_keys)) - 1;
            while (lo <= hi) {
                const std::int32_t mid = (lo + hi) / 2;
                line(&node->inner.keys[mid]);
                if (node->inner.keys[mid] <= key) lo = mid + 1; else hi = mid - 1;
            }
            const std::int32_t target = hi + 1;
            Node<K, V>* child = nullptr;
            if (sz > 0 && target <= sz && target < CXLInnerNode::max_separators) {
                line(&node->inner.separators[target]);
                child = static_cast<Node<K, V>*>(node->inner.separators[target]);
            }
            if (dirty() || child == nullptr) { node = tree->root(); continue; }
            node = child;
            continue;
        }

        // leaf: lower_bound(keys, key)
        std::uint16_t lo = 0, hi = std::min<std::uint16_t>(sz, CXLLeafNode::max_items);
        while (lo < hi) {
            const std::uint16_t mid = lo + (hi - lo) / 2;
            line(&node->leaf.keys[mid]);
            if (node->leaf.keys[mid] < key) lo = mid + 1; else hi = mid;
        }
        bool is_found = false;
        V val{};
        if (lo < sz) {
            line(&node->leaf.keys[lo]);
            if (node->leaf.keys[lo] == key) {
                line(&node->leaf.values[lo]);
                is_found = true;
                val = node->leaf.values[lo];
            }
        }
        if (dirty()) continue;
        found = is_found;
        value = val;
        return node;
    }
}

} // namespace db::index::blinktree
