#pragma once
#include <cstdint>
#include <cstddef>
#include <limits>
#include <algorithm>
#include <new>
#include <cstdio>
#if defined(__x86_64__)
#include <cstring>
#include <immintrin.h>
#endif
// The XCENA MU compiler (metisx-dev LLVM fork) defines __metis, not __riscv
#if defined(__metis) || defined(__riscv)
#define HJ_MU_DEVICE 1
#include "mu/atomic/atomic.hpp"
#endif

namespace application::hash_join {
namespace detail {
    static constexpr std::size_t next_power_of_two(std::size_t val) noexcept {
        if (val == 0) return 1;
        val--; val |= val >> 1; val |= val >> 2; val |= val >> 4; val |= val >> 8; val |= val >> 16;
        if constexpr (sizeof(std::size_t) == 8) val |= val >> 32;
        return val + 1;
    }
}

template <typename K, typename V> class InlineHashtable {
private:
    struct alignas(8) Entry {
        K key{EMPTY_KEY};
        V value{0};
    };

public:
    // EMPTY_KEY and BUSY_KEY kept distinct (reserved keys)
    static constexpr K EMPTY_KEY = std::numeric_limits<K>::max();      // 0xFFFFFFFFU
    static constexpr K BUSY_KEY  = std::numeric_limits<K>::max() - 1U; // 0xFFFFFFFEU
    // Cap on waiting for a BUSY slot: if a write is lost and BUSY stays, do not loop forever (device wedge)
    static constexpr std::size_t BUSY_SPIN_LIMIT = std::size_t{1} << 24;
    
    std::size_t byte_size() const noexcept {
        return sizeof(InlineHashtable<K, V>) + sizeof(Entry) * _slots;
    }

    static std::size_t needed_bytes(const std::size_t slots) noexcept {
        std::size_t actual_slots = detail::next_power_of_two(std::max<std::size_t>(slots * 2, 1024));
        return sizeof(InlineHashtable<K, V>) + sizeof(Entry) * actual_slots;
    }

    static std::uint16_t partition_hash(std::uint32_t key) noexcept {
        key ^= key >> 16; key *= 0x85ebca6b;
        key ^= key >> 13; key *= 0xc2b2ae35;
        key ^= key >> 16;
        return static_cast<std::uint16_t>(key);
    }
    
    InlineHashtable(const std::size_t size)
        : _slots((size - sizeof(InlineHashtable<K, V>)) / sizeof(Entry)) {
        char* base_ptr = reinterpret_cast<char*>(this) + sizeof(InlineHashtable<K, V>);
        for (std::size_t i = 0; i < _slots; ++i) {
            ::new (static_cast<void*>(base_ptr + i * sizeof(Entry))) Entry{EMPTY_KEY, 0};
        }
    }

#if defined(__x86_64__)
    // T28: initialize the slots with non-temporal stores (movnti) so no table line stays in the host cache.
    // Tier 1 rule: NT + sfence is visible to the device without a flush. The header (_slots) uses an ordinary
    // store, so the caller must flush its first 64 B line (header_bytes()).
    struct nontemporal_t {};
    InlineHashtable(const std::size_t size, nontemporal_t)
        : _slots((size - sizeof(InlineHashtable<K, V>)) / sizeof(Entry)) {
        static_assert(sizeof(Entry) == sizeof(long long), "NT init assumes 8 B entries");
        const Entry empty{EMPTY_KEY, 0};
        long long pattern;
        std::memcpy(&pattern, &empty, sizeof(pattern));
        auto* p = reinterpret_cast<long long*>(reinterpret_cast<char*>(this) + sizeof(InlineHashtable<K, V>));
        for (std::size_t i = 0; i < _slots; ++i) {
            _mm_stream_si64(p + i, pattern);
        }
        _mm_sfence();
    }
    static constexpr std::size_t header_bytes() noexcept { return 64; }
#endif

    // Spacing when every partition table sits back to back in one allocation (C18). 64 B aligned, a power of two + 64,
    // so the table headers do not crowd into the same cache set.
    static constexpr std::size_t table_stride(const std::size_t needed) noexcept {
        return (needed + 63U) & ~std::size_t{63U};
    }

#if defined(HJ_MU_DEVICE)
    // MU-only insert. The MU toolchain (-mcpu=metisx-d) has no RISC-V A extension, so C++ __atomic
    // compiles to plain loads and stores (CAS is not atomic, so concurrent inserts lose keys; confirmed 2026-10-14).
    // mu::atomicCAS/atomicSwap run in memory, so table lines never enter the MU cache during the build.
    // Build and probe are separate kernels, so publishing the key directly with CAS (no BUSY step) is safe.
    bool insert_atomic_32(const K key, const V value) noexcept {
        return insert_at(&at(0), _slots - 1U, key, value);
    }

    // MU insert without the header (C18): the caller passes base/mask (the kernel uses the config's slot_mask).
    static bool insert_at(Entry* base, const std::size_t mask, const K key, const V value) noexcept {
        static_assert(sizeof(K) == sizeof(std::int32_t) && sizeof(V) == sizeof(std::int32_t), "32-bit key/value");
        if (key == EMPTY_KEY || key == BUSY_KEY) [[unlikely]] {
            return false;
        }
        std::size_t probe_count = 0;
        for (auto index = bucket_hash(key);; ++index) {
            index &= mask;
            auto &entry = base[index];
            const auto old = static_cast<K>(mu::atomicCAS(reinterpret_cast<std::int32_t*>(&entry.key),
                                                          static_cast<std::int32_t>(key),
                                                          static_cast<std::int32_t>(EMPTY_KEY)));
            if (old == EMPTY_KEY) {
                mu::atomicSwap(reinterpret_cast<std::int32_t*>(&entry.value), static_cast<std::int32_t>(value));
                return true;
            }
            // other key or a duplicate key: next slot (keeps duplicate-key tuples, as on the host path)
            if (++probe_count > mask) {
                return false;
            }
        }
    }
#else
    // RV32A 32-bit atomic insert
    bool insert_atomic_32(const K key, const V value) noexcept {
        // guard: never insert a reserved key
        if (key == EMPTY_KEY || key == BUSY_KEY) [[unlikely]] {
            return false; 
        }

        std::size_t probe_count = 0;
        std::size_t spin_count = 0;
        for (auto index = hash(key);; ++index) {
            index &= _slots - 1U;
            auto &entry = at(index);
            std::uint32_t expected_key = EMPTY_KEY;

            // 1. try to claim an EMPTY_KEY slot by setting BUSY_KEY (lock)
            if (__atomic_compare_exchange_n(&entry.key, &expected_key, BUSY_KEY, false, __ATOMIC_ACQ_REL, __ATOMIC_RELAXED)) {
                __atomic_store_n(&entry.value, value, __ATOMIC_RELAXED);
                __atomic_store_n(&entry.key, key, __ATOMIC_RELEASE); // publish the final key
                return true;
            }

            // 2. another thread is writing (BUSY_KEY): spin-wait, then retry this slot
            if (expected_key == BUSY_KEY || expected_key == EMPTY_KEY) {
                if (++spin_count >= BUSY_SPIN_LIMIT) [[unlikely]] {
                    return false;
                }
                #if defined(__riscv)
                asm volatile("pause" ::: "memory");
                #endif
                --index; // cancels the next ++index, so the same slot is checked again
                continue;
            }

            // 3. duplicate key (expected_key == key) or a different key: move on to the next slot
            // (do not overwrite; store in the next EMPTY_KEY slot so duplicate-key tuples are kept)

            if (++probe_count >= _slots) {
                return false;
            }
        }
    }
#endif

    // 64-bit key/value atomic CAS (key and value stored atomically with a single CAS)
    // the 64-bit code was replaced by a wrapper
    bool insert_atomic(const K key, const V value) noexcept {
        return insert_atomic_32(key, value);
    }
    
    // get() under concurrency, aware of BUSY_KEY
    V get(const K key) const noexcept {
        std::size_t probe_count = 0;
        std::size_t spin_count = 0;
        for (auto index = hash(key);; ++index) {
            index &= _slots - 1U;
            const auto &entry = at(index);

            K slot_key = __atomic_load_n(&entry.key, __ATOMIC_ACQUIRE);

            // target key found
            if (slot_key == key) {
                return __atomic_load_n(&entry.value, __ATOMIC_ACQUIRE);
            }

            // another thread is writing this slot: wait until it finishes
            // (past the cap it counts as a miss, which the tuple-count/checksum validation exposes)
            if (slot_key == BUSY_KEY) {
                if (++spin_count >= BUSY_SPIN_LIMIT) [[unlikely]] {
                    return std::numeric_limits<V>::max();
                }
                #if defined(__riscv)
                asm volatile("pause" ::: "memory");
                #endif
                --index;
                continue;
            }

            // stop at an empty slot or after scanning every slot
            if (slot_key == EMPTY_KEY || ++probe_count >= _slots) {
                return std::numeric_limits<V>::max();
            }
        }
    }

    const Entry &at(const std::size_t slot) const noexcept { 
        return reinterpret_cast<const Entry *>(reinterpret_cast<const char*>(this) + sizeof(InlineHashtable<K, V>))[slot]; 
    }
    
    Entry &at(const std::size_t slot) noexcept { 
        return reinterpret_cast<Entry *>(reinterpret_cast<char*>(this) + sizeof(InlineHashtable<K, V>))[slot]; 
    }

    // --- batched probe API ---------------------------------------------------------------------------
    // Compute and prefetch the slots of a group of keys first, then look them up. Every partition has the same
    // size, so the slot mask comes from the config and no key reads the table header (_slots).
    using entry_type = Entry;
    static std::size_t bucket_hash(const K key) noexcept {
        std::uint32_t k = static_cast<std::uint32_t>(key);
        k = (k >> 16) | (k << 16);
        k *= 0x45d9f3b;
        k ^= k >> 16;
        return static_cast<std::size_t>(k);
    }
    std::size_t slot_count() const noexcept { return _slots; }
    const Entry* entries() const noexcept { return &at(0); }
    // Look up from the home slot without the header. Same result as get() (called after the build, so no BUSY;
    // a leftover one counts as another key, so the scan ends at EMPTY or after one full turn).
    static V lookup(const Entry* base, const std::size_t mask, const K key, std::size_t slot) noexcept {
        for (std::size_t probes = 0;; slot = (slot + 1U) & mask) {
            const Entry& e = base[slot];
            if (e.key == key) return e.value;
            if (e.key == EMPTY_KEY || ++probes > mask) return std::numeric_limits<V>::max();
        }
    }

    // C19 owner-computes build: plain (non-atomic) insert when a table has a single writer.
    // No other thread writes the same slot, so no CAS is needed.
    static bool insert_plain(Entry* base, const std::size_t mask, const K key, const V value) noexcept {
        if (key == EMPTY_KEY || key == BUSY_KEY) return false;
        std::size_t index = bucket_hash(key);
        for (std::size_t probes = 0;; ++index) {
            index &= mask;
            Entry& e = base[index];
            if (e.key == EMPTY_KEY) {
                e.value = value;
                e.key = key;
                return true;
            }
            if (++probes > mask) return false;
        }
    }

private:
    const std::uint64_t _slots;
    
    std::size_t hash(K key) const noexcept { return bucket_hash(key); }
};

using hashtable_type = InlineHashtable<std::uint32_t, std::uint32_t>;

} // namespace application::hash_join