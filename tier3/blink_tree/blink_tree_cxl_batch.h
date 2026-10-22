#pragma once
#include <vector>
#include <cstdint>
#include <iostream>
#include <cstdlib>
#include <mutex>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <type_traits>
#include <thread>
#include <cstring>
#include <string>
#include <immintrin.h>
#include "pxl_helper.hpp"
#include "blink_tree.h"

namespace db::index::blinktree {

// ---- optional launch profiler (BLINK_PROFILE=1) ----
struct LaunchProfile {
    bool on = std::getenv("BLINK_PROFILE") != nullptr;
    std::uint64_t n = 0, ns = 0;
    void add(std::uint64_t launch_ns) {
        if (!on) return;
        ++n; ns += launch_ns;
        if (n % 1000 == 0) {
            std::cerr << "[prof] launches=" << n << " us/launch=" << ns / 1000.0 / 1000 << "\n";
            ns = 0;
        }
    }
};
inline LaunchProfile g_launch_prof;
inline std::uint64_t prof_now_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

// Counters for the run summary (printed by main() at the end)
struct CxlBatchStats {
    std::atomic<std::uint64_t> launches{0};
    std::atomic<std::uint64_t> device_lookups{0};
    std::atomic<std::uint64_t> device_writes{0};
    std::atomic<std::uint64_t> host_locates{0};
    std::atomic<std::uint64_t> host_split_inserts{0};
    std::atomic<std::uint64_t> launch_ns{0};
    // per-phase host time (ns), one scheduler thread
    std::atomic<std::uint64_t> stage_ns{0};     // copy requests into the PXL buffer + clflush
    std::atomic<std::uint64_t> readback_ns{0};  // clflush + copy results back
    std::atomic<std::uint64_t> readback_inval_ns{0}; // the clflush part of readback (SDBI cost)
    std::atomic<std::uint64_t> dedup_ns{0}, drain_ns{0}, locate_host_ns{0}, replay_ns{0},
                               write_prep_ns{0}, split_ns{0}, report_ns{0}, batch_ns{0},
                               host_write_ns{0},      // --host-dispatch=batched: parallel host writes
                               post_inval_ns{0};      // hybrid: post-locate leaf invalidation (inside locate_host_ns)
};
inline CxlBatchStats g_cxl_stats;

inline std::mutex g_pxl_batch_mutex;

// --no-combine: the device does one traversal per YCSB op (same work as a host thread): locate
// sends one request per op and every write op becomes its own device write, in batch order.
// Answers still come from the in-order replay. Default (false): one locate per distinct key and
// one write per changed key (request combining).
inline bool g_cxl_no_combine = false;

// Request staging: non-temporal 32 B stores + sfence write straight to CXL memory and need no
// clflush (coherence_tax suite: 0 bad lines with NT+sfence). BLINK_STAGE=flush selects the old
// copy + clflush path for comparison.
inline const bool g_stage_nt = [] {
    const char* s = std::getenv("BLINK_STAGE");
    return !(s && std::string(s) == "flush");
}();

inline void cxl_stream_store(void* dst, const void* src, std::size_t bytes) noexcept {
#if defined(__AVX2__)
    auto* d = static_cast<__m256i*>(dst);
    const auto* s = static_cast<const __m256i*>(src);
    for (std::size_t i = 0; i < bytes / 32; ++i) _mm256_stream_si256(d + i, _mm256_loadu_si256(s + i));
    _mm_sfence();
#else
    std::memcpy(dst, src, bytes);
    cxl_inval_cache_range(dst, bytes);
#endif
}

#ifdef USE_CXL
inline PxlUniquePtr<CXLWriteRequest> g_pxl_write_buf;
inline std::size_t g_pxl_write_capacity = 0;

inline PxlUniquePtr<CXLLookupRequest> g_pxl_lookup_buf;
inline std::size_t g_pxl_lookup_capacity = 0;
#endif

#ifdef USE_CXL
// --resident (NDP@CXL): one launch of cxl_resident_worker for the whole run; each batch's lookup and
// write jobs are posted into a ctrl block instead of launching a kernel (removes per-launch dispatch).
struct ResidentWorker {
    bool on = false;
    PxlUniquePtr<std::uint64_t> ctrl;
    pxl::runtime::Map* map = nullptr;
    std::thread waiter;
    std::atomic<int> state{0};          // 0 running, 1 exited ok, 2 failed
    std::uint64_t seq = 0;
    std::uint32_t tasks = 0;

    static void nt(std::uint64_t* p, std::uint64_t v) { _mm_stream_si64(reinterpret_cast<long long*>(p), static_cast<long long>(v)); }
    std::int64_t done() const {
        _mm_clflushopt(ctrl.get() + 8);
        _mm_mfence();
        return *reinterpret_cast<volatile std::int32_t*>(ctrl.get() + 8);
    }
    void start(std::uint32_t num_tasks) {
        tasks = std::max<std::uint32_t>(1, std::min<std::uint32_t>(num_tasks, 3048));
        ctrl = allocatePxlMemory<std::uint64_t>(g_pxl_context, 16);
        for (int i = 0; i < 16; ++i) nt(ctrl.get() + i, 0);
        _mm_sfence();
        map = g_pxl_job->buildMap(g_pxl_func_resident, tasks);
        if (!map) throw std::runtime_error("resident: buildMap failed");
        map->setBatchSize(1);                           // every task resident on its own MU thread
        if (map->execute(ctrl.get(), static_cast<int>(tasks)) != pxl::PxlResult::Success)
            throw std::runtime_error("resident: execute failed");
        waiter = std::thread([this] { state = (map->synchronize() == pxl::PxlResult::Success) ? 1 : 2; });
    }
    void run(std::uint64_t kind, CXLNode* root, void* reqs, std::size_t n) {
        std::uint64_t* c = ctrl.get();
        nt(c + 1, kind); nt(c + 2, reinterpret_cast<std::uint64_t>(reqs)); nt(c + 3, n);
        nt(c + 5, reinterpret_cast<std::uint64_t>(root));
        _mm_sfence();
        nt(c + 0, ++seq);
        _mm_sfence();
        const std::int64_t target = static_cast<std::int64_t>(tasks) * static_cast<std::int64_t>(seq);
        const auto t0 = std::chrono::steady_clock::now();
        while (done() < target) {
            if (state.load() != 0) throw std::runtime_error("resident worker exited early");
            if (std::chrono::steady_clock::now() - t0 > std::chrono::seconds(60)) throw std::runtime_error("resident worker: job timed out");
            _mm_pause();
        }
    }
    void stop() {
        if (!map) return;
        nt(ctrl.get() + 4, 1);
        _mm_sfence();
        if (waiter.joinable()) waiter.join();
        g_pxl_job->destroyMap(map);
        map = nullptr;
        ctrl.reset();
    }
};
inline ResidentWorker g_resident;
#endif

// Free the buffers safely just before main() returns, while the PXL context is still alive
inline void reset_cxl_batch_buffers() {
#ifdef USE_CXL
    std::lock_guard<std::mutex> lock(g_pxl_batch_mutex);
    g_resident.stop();
    g_pxl_write_buf.reset();
    g_pxl_write_capacity = 0;
    g_pxl_lookup_buf.reset();
    g_pxl_lookup_capacity = 0;
#endif
}


#ifdef USE_CXL
// Stages reqs in a PXL buffer, runs one launch over them, and copies the results back.
template <typename Req>
inline void run_cxl_batch(std::vector<Req>& reqs, CXLNode* root, pxl::kernel::Function* func,
                          std::uint32_t num_tasks, PxlUniquePtr<Req>& buf, std::size_t& capacity) {
    if (!func || !g_pxl_context) {
        std::cerr << "❌ [FATAL ERROR] CXL kernel not initialized!\n";
        std::exit(1);
    }
    if (reqs.empty()) return;

    std::lock_guard<std::mutex> lock(g_pxl_batch_mutex);
    if (!buf || capacity < reqs.size()) {
        capacity = std::max<std::size_t>(1024, reqs.size() * 2);
        buf = allocatePxlMemory<Req>(g_pxl_context, capacity);
    }

    static_assert(sizeof(Req) % 32 == 0, "stream staging writes whole 32 B records");
    const auto ts = prof_now_ns();
    Req* dev = buf.get();
    const std::size_t bytes = reqs.size() * sizeof(Req);
    if (g_stage_nt) {
        cxl_stream_store(dev, reqs.data(), bytes);
    } else {
        std::copy(reqs.begin(), reqs.end(), dev);
        cxl_inval_cache_range(dev, bytes);
    }
    const auto t0 = prof_now_ns();
    g_cxl_stats.stage_ns.fetch_add(t0 - ts, std::memory_order_relaxed);
    if (g_resident.on && g_pxl_func_resident) {
        if (!g_resident.map) g_resident.start(g_pxl_num_tasks);
        g_resident.run(std::is_same_v<Req, CXLLookupRequest> ? 1 : 2, root, dev, reqs.size());
    } else {
        pxl_launch_sync(func, std::max<std::uint32_t>(1, num_tasks), root, dev, static_cast<int>(bytes));
    }
    const auto dt = prof_now_ns() - t0;
    g_launch_prof.add(dt);
    g_cxl_stats.launches.fetch_add(1, std::memory_order_relaxed);
    g_cxl_stats.launch_ns.fetch_add(dt, std::memory_order_relaxed);
    const auto tr = prof_now_ns();
    cxl_inval_cache_range(dev, bytes);
    g_cxl_stats.readback_inval_ns.fetch_add(prof_now_ns() - tr, std::memory_order_relaxed);

    std::copy(dev, dev + reqs.size(), reqs.begin());
    g_cxl_stats.readback_ns.fetch_add(prof_now_ns() - tr, std::memory_order_relaxed);
}

inline void process_cxl_write_batch(std::vector<CXLWriteRequest>& writes, CXLNode* root, std::uint32_t num_tasks) {
    g_cxl_stats.device_writes.fetch_add(
        std::count_if(writes.begin(), writes.end(), [](const CXLWriteRequest& w) { return w.op != 0xFF; }),
        std::memory_order_relaxed);
    run_cxl_batch(writes, root, g_pxl_func_write, num_tasks, g_pxl_write_buf, g_pxl_write_capacity);
}

inline void process_cxl_lookup_batch(std::vector<CXLLookupRequest>& lookups, CXLNode* root, std::uint32_t num_tasks) {
    g_cxl_stats.device_lookups.fetch_add(lookups.size(), std::memory_order_relaxed);
    run_cxl_batch(lookups, root, g_pxl_func_lookup, num_tasks, g_pxl_lookup_buf, g_pxl_lookup_capacity);
}
#endif

} // namespace db::index::blinktree
