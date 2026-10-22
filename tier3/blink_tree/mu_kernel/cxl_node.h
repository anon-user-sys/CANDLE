#pragma once
#include <stdint.h>
#include <limits>
#include <algorithm>
#include <cstddef>
#include <new>
#include <cstring>
#if !defined(__metis) && !defined(__riscv) && !defined(__riscv__)
#include <mutex>
#endif

#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Watomic-alignment"
#endif

#if defined(__x86_64__) || defined(_M_X64)
#include <immintrin.h>
#include <cstdlib>
#endif

// The MU toolchain predefines __metis, not __riscv, and has no A extension: C++ __atomic ops
// become plain lw/sw there, so device code must use the mu:: AMO helpers instead.
#if defined(__metis) || defined(__riscv) || defined(__riscv__)
#define CXL_MU_DEVICE 1
#endif

#if defined(MODE_HOST_CXL) || defined(MODE_CXL_HYBRID) || defined(MODE_CXL_FULL)
#ifndef USE_CXL
#define USE_CXL
#endif
#endif

#ifdef USE_CXL
#include "pxl/pxl.hpp"
#include <pxl/runtime/runtime_api.hpp>
extern pxl::runtime::Context* g_pxl_context;
#endif

// =========================================================================
// Single definition of the cache flush (RISC-V: no-op / host x86: clflush)
// =========================================================================
#if !defined(CXL_MU_DEVICE)
// The host builds the prefill tree alone, so there is no need to flush every insert: main() turns flushing off
// during the prefill and flushes the whole tree once afterwards
inline bool g_cxl_inval_enabled = true;
#endif

inline void cxl_inval_cache_range([[maybe_unused]] const void* addr, [[maybe_unused]] std::size_t len) noexcept {
#if defined(CXL_MU_DEVICE)
    // no-op when building the RISC-V device kernel
#elif defined(MODE_CXL_HYBRID) || defined(MODE_CXL_FULL)
    if (!addr || len == 0 || !g_cxl_inval_enabled) return;

#if defined(__x86_64__) || defined(_M_X64)
    _mm_sfence();
#endif

    std::uintptr_t start = reinterpret_cast<std::uintptr_t>(addr) & ~63ULL;
    std::uintptr_t end = reinterpret_cast<std::uintptr_t>(addr) + len;

#if defined(__CLFLUSHOPT__)
    // clflushopt per line + one mfence (below): the flushes overlap instead of serializing. The
    // serializing clflush loop cost ~380 ns per line on result readback (coherence_tax: plain
    // clflush is 11-93x slower than the optimized flush). BLINK_FLUSH=clflush keeps the old loop.
    static const bool use_opt = [] {
        const char* e = std::getenv("BLINK_FLUSH");
        return !(e && std::strcmp(e, "clflush") == 0);
    }();
    if (use_opt) {
        for (std::uintptr_t p = start; p < end; p += 64) _mm_clflushopt(reinterpret_cast<void*>(p));
    } else
#endif
    for (std::uintptr_t p = start; p < end; p += 64) {
#if defined(__x86_64__) || defined(_M_X64)
        _mm_clflush(reinterpret_cast<const void*>(p));
#endif
    }
#if defined(__x86_64__) || defined(_M_X64)
    _mm_mfence();
#endif
#endif
}

enum NodeType : std::uint8_t {
    Leaf = 1U << 0U,
    Inner = 1U << 1U,
    Branch = 1U << 2U
};

struct alignas(64) CXLNodeHeader {
    NodeType node_type{NodeType::Leaf};
    std::uint8_t level{0};
    std::uint16_t size{0U};
    std::uint16_t channel_id{0U};

    alignas(8) std::uint64_t high_key{0xFFFFFFFFFFFFFFFFULL};
    alignas(8) void* right_sibling{nullptr};
    alignas(8) void* parent{nullptr};
};

struct alignas(64) CXLOptimisticLatch {
    std::uint32_t version{0};

#if defined(CXL_MU_DEVICE)
    // Device: every access to the version goes through a memory-side AMO, so MU threads and
    // the host's flushed writes agree on one value. The latch owns its own cache line.
    inline std::int32_t* word() const noexcept {
        return reinterpret_cast<std::int32_t*>(const_cast<std::uint32_t*>(&version));
    }

    inline std::uint32_t get_version() const noexcept {
        return static_cast<std::uint32_t>(mu::atomicAdd(word(), 0));
    }

    inline bool is_dirty(std::uint32_t old_v) const noexcept {
        std::uint32_t curr = get_version();
        return (curr & 1) || (curr != old_v);
    }

    inline bool try_lock() noexcept {
        std::uint32_t curr = get_version();
        if (curr & 1) return false;
        return static_cast<std::uint32_t>(mu::atomicCAS(word(), static_cast<std::int32_t>(curr + 1),
                                                        static_cast<std::int32_t>(curr))) == curr;
    }

    inline void lock() noexcept {
        while (!try_lock()) {
            __asm__ __volatile__ (".insn i 0x0F, 0, x0, x0, 0x010" ::: "memory");
        }
    }

    inline void unlock() noexcept {
        // Write back this thread's dirty node lines before the version AMO publishes them.
        mu::flushCacheAll();
        mu::atomicAdd(word(), 1);
    }
#else

    inline std::uint32_t get_version() const noexcept {
        cxl_inval_cache_range(&version, sizeof(version));
        return __atomic_load_n(&version, __ATOMIC_ACQUIRE);
    }

    inline bool is_dirty(std::uint32_t old_v) const noexcept {
        cxl_inval_cache_range(&version, sizeof(version));
        std::uint32_t curr = __atomic_load_n(&version, __ATOMIC_ACQUIRE);
        return (curr & 1) || (curr != old_v);
    }

    inline bool try_lock() noexcept {
        cxl_inval_cache_range(&version, sizeof(version));
        std::uint32_t curr = __atomic_load_n(&version, __ATOMIC_RELAXED);
        if ((curr & 1) == 0) {
            if (__atomic_compare_exchange_n(&version, &curr, curr + 1, true, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
                cxl_inval_cache_range(&version, sizeof(version));
                return true;
            }
        }
        return false;
    }
    
    inline void lock() noexcept {
        while (true) {
            cxl_inval_cache_range(&version, sizeof(version));
            std::uint32_t curr = __atomic_load_n(&version, __ATOMIC_RELAXED);
            if ((curr & 1) == 0) {
                if (__atomic_compare_exchange_n(&version, &curr, curr + 1, true, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
                    cxl_inval_cache_range(&version, sizeof(version));
                    break;
                }
            }
#if defined(CXL_MU_DEVICE)
            __asm__ __volatile__ (".insn i 0x0F, 0, x0, x0, 0x010" ::: "memory");
#elif defined(__x86_64__) || defined(_M_X64)
            _mm_pause();
#endif
        }
    }

    inline void unlock() noexcept {
#if defined(CXL_MU_DEVICE)
        __asm__ __volatile__ ("fence rw, rw" ::: "memory");
#else
        __atomic_thread_fence(__ATOMIC_SEQ_CST);
#endif
        __atomic_fetch_add(&version, 1, __ATOMIC_RELEASE);
#if defined(CXL_MU_DEVICE)
        mu::flushCache(&version, sizeof(version));
        __asm__ __volatile__ ("fence rw, rw" ::: "memory");
#else
        __atomic_thread_fence(__ATOMIC_SEQ_CST);
        cxl_inval_cache_range(&version, sizeof(version));
#endif
    }
#endif
};

static constexpr std::uint16_t usable_node_payload_size = 4096 - sizeof(CXLNodeHeader) - sizeof(CXLOptimisticLatch);

struct CXLInnerNode {
    static constexpr std::uint16_t alignment_margin = 16;
    static constexpr std::uint16_t max_keys = (usable_node_payload_size - sizeof(void*) - alignment_margin) / (sizeof(std::uint64_t) + sizeof(void*));
    static constexpr std::uint16_t max_separators = max_keys + 1;

    std::uint64_t keys[max_keys];
    void* separators[max_separators];
};

struct CXLLeafNode {
    static constexpr std::uint16_t alignment_margin = 16;
    static constexpr std::uint16_t max_items = (usable_node_payload_size - alignment_margin) / (sizeof(std::uint64_t) + sizeof(std::int64_t));

    std::uint64_t keys[max_items];
    std::int64_t values[max_items];
};

class alignas(64) CXLNode {
public:
    CXLNodeHeader header;
    CXLOptimisticLatch latch;
    union {
        CXLInnerNode inner;
        CXLLeafNode leaf;
    };

#ifdef USE_CXL
    // memAlloc allocates in 64 KiB units, so one call per 4 KiB node would waste 16x the memory.
    // Cut nodes from large chunks instead (nodes are never freed while running; they go with the context).
    static constexpr std::size_t pool_chunk_bytes = 64ULL << 20;
    static inline std::mutex pool_mutex;
    static inline char* pool_cursor = nullptr;
    static inline char* pool_end = nullptr;
    static inline std::size_t pool_nodes = 0;

    static void* pool_alloc(std::size_t size) {
        const std::size_t step = (size + 4095) & ~static_cast<std::size_t>(4095);
        std::lock_guard<std::mutex> lock(pool_mutex);
        if (!pool_cursor || pool_cursor + step > pool_end) {
            pool_cursor = static_cast<char*>(g_pxl_context->memAlloc(pool_chunk_bytes));
            if (!pool_cursor) return nullptr;
            pool_end = pool_cursor + pool_chunk_bytes;
        }
        void* p = pool_cursor;
        pool_cursor += step;
        ++pool_nodes;
        return p;
    }
#endif

    void* operator new(std::size_t size) {
#ifdef USE_CXL
        if (g_pxl_context) {
            if (void* ptr = pool_alloc(size)) return ptr;
            throw std::bad_alloc();
        }
#endif
        return ::operator new(size);
    }

    void operator delete([[maybe_unused]] void* ptr) noexcept {
#ifdef USE_CXL
        if (g_pxl_context) return; // pool memory: released with the context
#endif
        ::operator delete(ptr);
    }

    inline CXLNode(NodeType type = NodeType::Leaf, CXLNode* parent_ptr = nullptr, std::uint16_t channel_id = 0, std::uint8_t lvl = 0) noexcept {
        header.node_type = type;
        header.level = lvl;
        header.size = 0;
        header.channel_id = channel_id;
        header.high_key = std::numeric_limits<std::uint64_t>::max();
        header.right_sibling = nullptr;
        header.parent = static_cast<void*>(parent_ptr);
        latch.version = 0;

        std::memset(&inner, 0, sizeof(inner));
        cxl_inval_cache_range(this, sizeof(CXLNode));
    }

    inline bool is_leaf() const noexcept { return (header.node_type & NodeType::Leaf) != 0; }
    inline bool is_inner() const noexcept { return (header.node_type & NodeType::Inner) != 0; }

    inline std::uint8_t level() const noexcept {
        return __atomic_load_n(&header.level, __ATOMIC_ACQUIRE);
    }
    inline void level(std::uint8_t lvl) noexcept {
        __atomic_store_n(&header.level, lvl, __ATOMIC_RELEASE);
    }

    inline std::uint16_t size() const noexcept {
        return __atomic_load_n(&header.size, __ATOMIC_ACQUIRE);
    }
    inline void size(std::uint16_t sz) noexcept {
        __atomic_store_n(&header.size, sz, __ATOMIC_RELEASE);
    }

    inline std::uint64_t high_key() const noexcept {
        return __atomic_load_n(&header.high_key, __ATOMIC_ACQUIRE);
    }
    inline void high_key(std::uint64_t k) noexcept {
        __atomic_store_n(&header.high_key, k, __ATOMIC_RELEASE);
    }

    inline CXLNode* right_sibling() const noexcept {
        return static_cast<CXLNode*>(__atomic_load_n(&header.right_sibling, __ATOMIC_ACQUIRE));
    }
    inline void right_sibling(CXLNode* sibling) noexcept {
        __atomic_store_n(&header.right_sibling, static_cast<void*>(sibling), __ATOMIC_RELEASE);
    }

    inline CXLNode* parent() const noexcept {
        return static_cast<CXLNode*>(__atomic_load_n(&header.parent, __ATOMIC_ACQUIRE));
    }
    inline void parent(CXLNode* p) noexcept {
        __atomic_store_n(&header.parent, static_cast<void*>(p), __ATOMIC_RELEASE);
    }

    inline std::uint32_t get_version() const noexcept { return latch.get_version(); }
    inline bool is_dirty(std::uint32_t old_v) const noexcept { return latch.is_dirty(old_v); }
    inline void lock() noexcept { latch.lock(); }
    inline void unlock() noexcept { latch.unlock(); }

    inline bool full() const noexcept {
        std::uint16_t max_sz = is_leaf() ? CXLLeafNode::max_items : CXLInnerNode::max_keys;
        return size() >= max_sz;
    }

    inline CXLNode* child(std::uint64_t key) const noexcept {
        std::uint16_t sz = size();
        if (sz > CXLInnerNode::max_keys) sz = CXLInnerNode::max_keys;
        
        if (sz == 0) return nullptr;

        std::int16_t low = 0;
        std::int16_t high = static_cast<std::int16_t>(sz) - 1;
        while (low <= high) {
            auto mid = (low + high) >> 1;
            if (mid < 0 || mid >= CXLInnerNode::max_keys) break;
            std::uint64_t k = __atomic_load_n(&inner.keys[mid], __ATOMIC_ACQUIRE);
            if (k <= key) low = mid + 1;
            else high = mid - 1;
        }
        std::uint16_t target_idx = static_cast<std::uint16_t>(high + 1);
        if (target_idx > sz || target_idx >= CXLInnerNode::max_separators) return nullptr;
        return static_cast<CXLNode*>(__atomic_load_n(&inner.separators[target_idx], __ATOMIC_ACQUIRE));
    }

    inline std::uint16_t index(std::uint64_t key) const noexcept {
        std::uint16_t low = 0;
        std::uint16_t high = size();
        std::uint16_t max_sz = is_leaf() ? CXLLeafNode::max_items : CXLInnerNode::max_keys;
        if (high > max_sz) high = max_sz;

        while (low < high) {
            std::uint16_t mid = low + (high - low) / 2;
            std::uint64_t k = is_leaf() ? __atomic_load_n(&leaf.keys[mid], __ATOMIC_ACQUIRE)
                                        : __atomic_load_n(&inner.keys[mid], __ATOMIC_ACQUIRE);
            if (k < key) low = mid + 1;
            else high = mid;
        }
        return low;
    }

    inline std::uint64_t leaf_key(std::uint16_t idx) const noexcept {
        return is_leaf() ? __atomic_load_n(&leaf.keys[idx], __ATOMIC_ACQUIRE)
                         : __atomic_load_n(&inner.keys[idx], __ATOMIC_ACQUIRE);
    }

    inline std::int64_t value(std::uint16_t idx) const noexcept {
        return __atomic_load_n(&leaf.values[idx], __ATOMIC_ACQUIRE);
    }

    inline void value(std::uint16_t idx, std::int64_t val) noexcept {
        __atomic_store_n(&leaf.values[idx], val, __ATOMIC_RELEASE);
#if defined(CXL_MU_DEVICE)
        __asm__ __volatile__ ("fence rw, rw" ::: "memory");
#else
        __atomic_thread_fence(__ATOMIC_SEQ_CST);
#endif
    }

    inline CXLNode* child_ptr(std::uint16_t idx) const noexcept {
        return static_cast<CXLNode*>(__atomic_load_n(&inner.separators[idx], __ATOMIC_ACQUIRE));
    }

    inline void set_child(std::uint16_t idx, CXLNode* child) noexcept {
        __atomic_store_n(&inner.separators[idx], static_cast<void*>(child), __ATOMIC_RELEASE);
    }

    inline void set_key(std::uint16_t idx, std::uint64_t key) noexcept {
        if (is_leaf()) {
            __atomic_store_n(&leaf.keys[idx], key, __ATOMIC_RELEASE);
        } else {
            __atomic_store_n(&inner.keys[idx], key, __ATOMIC_RELEASE);
        }
    }

    inline void insert_leaf(std::uint16_t idx, std::uint64_t key, std::int64_t val) noexcept {
        std::uint16_t curr_sz = size();
        if (idx < curr_sz) {
            for (std::uint16_t i = curr_sz; i > idx; --i) {
                __atomic_store_n(&leaf.keys[i], __atomic_load_n(&leaf.keys[i - 1], __ATOMIC_RELAXED), __ATOMIC_RELAXED);
                __atomic_store_n(&leaf.values[i], __atomic_load_n(&leaf.values[i - 1], __ATOMIC_RELAXED), __ATOMIC_RELAXED);
            }
        }
        __atomic_store_n(&leaf.keys[idx], key, __ATOMIC_RELEASE);
        __atomic_store_n(&leaf.values[idx], val, __ATOMIC_RELEASE);
#if defined(CXL_MU_DEVICE)
        __asm__ __volatile__ ("fence rw, rw" ::: "memory");
#else
        __atomic_thread_fence(__ATOMIC_SEQ_CST);
#endif
        size(curr_sz + 1);
    }

    inline void insert(std::uint16_t idx, std::int64_t val, std::uint64_t key) noexcept {
        insert_leaf(idx, key, val);
    }

    inline void insert(std::uint16_t idx, CXLNode* child_ptr, std::uint64_t key) noexcept {
        std::uint16_t curr_sz = size();
        if (idx < curr_sz) {
            for (std::uint16_t i = curr_sz; i > idx; --i) {
                __atomic_store_n(&inner.keys[i], __atomic_load_n(&inner.keys[i - 1], __ATOMIC_RELAXED), __ATOMIC_RELAXED);
                __atomic_store_n(&inner.separators[i + 1], __atomic_load_n(&inner.separators[i], __ATOMIC_RELAXED), __ATOMIC_RELAXED);
            }
        }
        __atomic_store_n(&inner.keys[idx], key, __ATOMIC_RELEASE);
        __atomic_store_n(&inner.separators[idx + 1], static_cast<void*>(child_ptr), __ATOMIC_RELEASE);
        if (child_ptr) child_ptr->parent(this);
        size(curr_sz + 1);
    }

    inline void move(CXLNode* target, std::uint16_t start_idx, std::uint16_t count) noexcept {
        for (std::uint16_t i = 0; i < count; ++i) {
            if (is_leaf()) {
                target->leaf.keys[i] = __atomic_load_n(&leaf.keys[start_idx + i], __ATOMIC_RELAXED);
                target->leaf.values[i] = __atomic_load_n(&leaf.values[start_idx + i], __ATOMIC_RELAXED);
            } else {
                target->inner.keys[i] = __atomic_load_n(&inner.keys[start_idx + i], __ATOMIC_RELAXED);
                target->inner.separators[i] = __atomic_load_n(&inner.separators[start_idx + i], __ATOMIC_RELAXED);
            }
        }
    }
};

// Host ↔ kernel request records, one definition for both sides. 32 B each, two per cache line:
// a line must never be written by two MU tasks (lost updates), so every task's slice starts on an
// even index (lookup chunks are rounded up to even; the host pads write slices with skip records).
struct alignas(32) CXLWriteRequest {
    std::uint64_t key;
    std::int64_t value;
    std::uint64_t leaf;   // leaf found by the locate pass; the kernel starts there (0 = root)
    std::uint8_t op;      // 0: Insert, 1: Update, 0xFF: padding record (skipped)
    std::uint8_t status;  // 0: Pending, 1: Success, 2: Missing, 3: Need host split, 4: skip
    std::uint16_t owner;  // device task that processes it (all writes to one leaf share an owner)
    std::uint32_t _pad;
};

struct alignas(32) CXLLookupRequest {
    std::uint64_t key;
    std::int64_t value;
    std::uint64_t leaf;   // out: leaf that holds (or would hold) the key
    std::uint8_t status;  // 0: Pending, 1: Found, 2: Missing
    std::uint8_t _pad[7];
};
static_assert(sizeof(CXLWriteRequest) == 32 && sizeof(CXLLookupRequest) == 32, "two records per cache line");

#if defined(__clang__)
#pragma clang diagnostic pop
#endif