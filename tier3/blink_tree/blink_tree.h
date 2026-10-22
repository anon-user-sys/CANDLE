#pragma once
#include <atomic>
#include <mutex>
#include <shared_mutex>
#include <utility>
#include <cstdint>
#include <vector>
#include <algorithm>
#include <iostream>
#include <thread>
#include <limits>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include "cxl_node.h"

namespace db::index::blinktree {

inline const bool g_blink_trace = std::getenv("BLINK_TRACE") != nullptr; // debug trace
#define BLINK_TRACE_LOG(...) do { if (::db::index::blinktree::g_blink_trace) std::fprintf(stderr, __VA_ARGS__); } while (0)

using TimePoint = std::chrono::high_resolution_clock::time_point;

template <typename K = std::uint64_t, typename V = std::int64_t>
using Node = CXLNode;

template <typename K, typename V>
class Listener {
public:
    virtual ~Listener() = default;
#ifdef MEASURE_LATENCY
    virtual void inserted([[maybe_unused]] std::uint16_t core_id, [[maybe_unused]] K key, [[maybe_unused]] V value, [[maybe_unused]] TimePoint start_time = {}) {}
    virtual void updated([[maybe_unused]] std::uint16_t core_id, [[maybe_unused]] K key, [[maybe_unused]] V value, [[maybe_unused]] TimePoint start_time = {}) {}
    virtual void removed([[maybe_unused]] std::uint16_t core_id, [[maybe_unused]] K key, [[maybe_unused]] TimePoint start_time = {}) {}
    virtual void found([[maybe_unused]] std::uint16_t core_id, [[maybe_unused]] K key, [[maybe_unused]] V value, [[maybe_unused]] TimePoint start_time = {}) {}
    virtual void missing([[maybe_unused]] std::uint16_t core_id, [[maybe_unused]] K key, [[maybe_unused]] TimePoint start_time = {}) {}
#else
    virtual void inserted([[maybe_unused]] std::uint16_t core_id, [[maybe_unused]] K key, [[maybe_unused]] V value) {}
    virtual void updated([[maybe_unused]] std::uint16_t core_id, [[maybe_unused]] K key, [[maybe_unused]] V value) {}
    virtual void removed([[maybe_unused]] std::uint16_t core_id, [[maybe_unused]] K key) {}
    virtual void found([[maybe_unused]] std::uint16_t core_id, [[maybe_unused]] K key, [[maybe_unused]] V value) {}
    virtual void missing([[maybe_unused]] std::uint16_t core_id, [[maybe_unused]] K key) {}
#endif
    virtual void requests_finished() {}
    virtual void separator_started() {}
    virtual void separator_finished() {}
};

template <typename K, typename V>
class NodeIterator {
public:
    NodeIterator(Node<K, V>* start = nullptr) : _curr(start) {}

    bool operator!=(const NodeIterator& other) const { return _curr != other._curr; }
    Node<K, V>& operator*() const { return *_curr; }
    Node<K, V>* operator->() const { return _curr; }

    NodeIterator& operator++() {
        if (_curr) _curr = _curr->right_sibling();
        return *this;
    }

private:
    Node<K, V>* _curr;
};

template <typename K, typename V>
class BLinkTree {
public:
    explicit BLinkTree(std::uint16_t tree_id = 0) : _tree_id(tree_id) {
        auto* root_node = new Node<K, V>(NodeType::Leaf, nullptr, 0, 0);
#if defined(__x86_64__) || defined(_M_X64)
        _mm_sfence();
#endif
        cxl_inval_cache_range(root_node, sizeof(Node<K, V>));
        _root.store(root_node, std::memory_order_relaxed);
    }
    
    ~BLinkTree() = default;

    std::uint16_t tree_id() const noexcept { return _tree_id; }

    Node<K, V>* root() const noexcept {
        return _root.load(std::memory_order_acquire);
    }

    std::uint64_t write_epoch() const noexcept {
        return _write_epoch.load(std::memory_order_acquire);
    }

    void notify_write() noexcept {
        _write_epoch.fetch_add(1, std::memory_order_release);
    }

    bool create_new_root(Node<K, V>* left, Node<K, V>* right, K split_key) {
        std::lock_guard<std::mutex> lock(_root_mutex);
        if (left->parent() != nullptr) return false;

        auto* new_root = new Node<K, V>(NodeType::Inner, nullptr, _logical_id_counter.fetch_add(1), left->level() + 1);
        new_root->high_key(right->high_key());
    
        new_root->set_key(0, split_key);
        new_root->set_child(0, left);
        new_root->set_child(1, right);
        new_root->size(1);
    
        left->parent(new_root);
        right->parent(new_root);

#if defined(__x86_64__) || defined(_M_X64)
        _mm_sfence();
#endif

        cxl_inval_cache_range(new_root, sizeof(Node<K, V>));
        cxl_inval_cache_range(left, sizeof(Node<K, V>));
        cxl_inval_cache_range(right, sizeof(Node<K, V>));

        _root.store(new_root, std::memory_order_release);
        notify_write();
        return true;
    }

    // Leaf Split (Value)
    std::pair<Node<K, V>*, K> split(Node<K, V>* node, K key, V val) {
        std::unique_lock<std::mutex> lock(_split_mutex);
        std::uint16_t next_logical_id = _logical_id_counter.fetch_add(1, std::memory_order_relaxed);

        auto* right_node = new Node<K, V>(NodeType::Leaf, node->parent(), next_logical_id, node->level());
        right_node->lock();

        const std::uint16_t current_size = node->size();
        const std::uint16_t move_count = current_size / 2;
        const std::uint16_t remain_count = current_size - move_count;

        node->move(right_node, remain_count, move_count);
        right_node->size(move_count);
        right_node->right_sibling(node->right_sibling());
        right_node->high_key(node->high_key());

        K split_key = right_node->leaf_key(0);
        node->size(remain_count);

        if (key < split_key) {
            node->insert(node->index(key), val, key);
        } else {
            right_node->insert(right_node->index(key), val, key);
        }

        node->high_key(split_key);
        std::atomic_thread_fence(std::memory_order_release);
        node->right_sibling(right_node);
        std::atomic_thread_fence(std::memory_order_release);

        BLINK_TRACE_LOG("[T] leafsplit node=%p right=%p split=%lu node_size=%u right_size=%u right_high=%lu parent=%p\n",
                        (void*)node, (void*)right_node, (unsigned long)split_key, node->size(), right_node->size(),
                        (unsigned long)right_node->high_key(), (void*)node->parent());
        // 1. write the node data to CXL DRAM first (flush)
#if defined(__x86_64__) || defined(_M_X64)
        _mm_sfence();
#endif
        cxl_inval_cache_range(right_node, sizeof(Node<K, V>));
        cxl_inval_cache_range(node, sizeof(Node<K, V>));

        // 2. unlock only after the flush
        right_node->unlock();
        node->unlock();

        notify_write();
        return {right_node, split_key};    
    }

    // Inner Split (Separator Pointer)
    std::pair<Node<K, V>*, K> split(Node<K, V>* node, K key, Node<K, V>* separator) {
        std::unique_lock<std::mutex> lock(_split_mutex);
        std::uint16_t next_logical_id = _logical_id_counter.fetch_add(1, std::memory_order_relaxed);

        auto* right_node = new Node<K, V>(NodeType::Inner, node->parent(), next_logical_id, node->level());
        right_node->lock();

        const std::uint16_t current_size = node->size();
        const std::uint16_t remain_count = current_size / 2;
        const std::uint16_t move_count = current_size - remain_count - 1;

        K split_key = node->leaf_key(remain_count);

        // children whose parent pointer changed
        std::vector<Node<K, V>*> modified_children;

        for (std::uint16_t i = 0; i < move_count; ++i) {
            right_node->set_key(i, node->leaf_key(remain_count + 1 + i));
            right_node->set_child(i, node->child_ptr(remain_count + 1 + i));
            if (right_node->child_ptr(i) != nullptr) {
                right_node->child_ptr(i)->parent(right_node);
                modified_children.push_back(right_node->child_ptr(i));
            }
        }
        right_node->set_child(move_count, node->child_ptr(current_size));
        if (right_node->child_ptr(move_count) != nullptr) {
            right_node->child_ptr(move_count)->parent(right_node);
            modified_children.push_back(right_node->child_ptr(move_count));
        }
        
        right_node->size(move_count);
        node->size(remain_count);

        right_node->right_sibling(node->right_sibling());
        right_node->high_key(node->high_key());

        if (key < split_key) {
            std::uint16_t idx = 0;
            while (idx < node->size() && node->leaf_key(idx) < key) idx++;
            node->insert(idx, separator, key);
        } else {
            std::uint16_t idx = 0;
            while (idx < right_node->size() && right_node->leaf_key(idx) < key) idx++;
            right_node->insert(idx, separator, key);
        }
        // insert() updates separator->parent, so this node must be flushed too. Otherwise a dirty header line
        // stays in the host cache, and a later clflush overwrites the size the device wrote with the old value
        // (wl=d 1M: half a leaf (123 keys) lost at every inner split)
        if (separator != nullptr) modified_children.push_back(separator);

        node->high_key(split_key);
        std::atomic_thread_fence(std::memory_order_release);
        node->right_sibling(right_node);
        std::atomic_thread_fence(std::memory_order_release);

        BLINK_TRACE_LOG("[T] innersplit node=%p right=%p split=%lu sep_key=%lu sep=%p node_size=%u right_size=%u\n",
                        (void*)node, (void*)right_node, (unsigned long)split_key, (unsigned long)key, (void*)separator,
                        node->size(), right_node->size());
        // 1. write the node and its changed children to CXL DRAM first (flush)
#if defined(__x86_64__) || defined(_M_X64)
        _mm_sfence();
#endif
        cxl_inval_cache_range(right_node, sizeof(Node<K, V>));
        cxl_inval_cache_range(node, sizeof(Node<K, V>));

        for (auto* child : modified_children) {
            cxl_inval_cache_range(child, sizeof(Node<K, V>));
        }

        // 2. unlock only after the flush
        right_node->unlock();
        node->unlock();

        notify_write();
        return {right_node, split_key};
    }

private:
    std::uint16_t _tree_id{0};
    std::atomic<Node<K, V>*> _root{nullptr};
    std::mutex _root_mutex;
    std::mutex _split_mutex;
    std::atomic<std::uint16_t> _logical_id_counter{1};
    std::atomic<std::uint64_t> _write_epoch{0};
};

} // namespace db::index::blinktree