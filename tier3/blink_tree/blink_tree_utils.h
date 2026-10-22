#pragma once
#include <mutex>
#include <vector>
#include <cstdint>
#include <chrono>

namespace db::index::blinktree {

using Clock = std::chrono::high_resolution_clock;
using TimePoint = Clock::time_point;

#ifdef MEASURE_LATENCY
inline std::mutex g_latency_registry_mutex;
inline std::vector<std::uint64_t> g_global_latency_samples;

struct alignas(64) ThreadLatencyBuffer {
    std::vector<std::uint64_t> samples;
    ThreadLatencyBuffer() { samples.reserve(100000); }

    ~ThreadLatencyBuffer() {
        if (!samples.empty()) {
            std::lock_guard<std::mutex> lock(g_latency_registry_mutex);
            g_global_latency_samples.insert(
                g_global_latency_samples.end(),
                samples.begin(),
                samples.end()
            );
        }
    }
};

inline thread_local ThreadLatencyBuffer g_tl_latency_buffer;

inline void reset_global_latency_samples() {
    std::lock_guard<std::mutex> lock(g_latency_registry_mutex);
    g_global_latency_samples.clear();
    g_tl_latency_buffer.samples.clear();
}

inline void record_latency(TimePoint start_time) noexcept {
    if (start_time.time_since_epoch().count() == 0) return;
    const auto end_time = Clock::now();
    const std::uint64_t elapsed_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(end_time - start_time).count();
    g_tl_latency_buffer.samples.push_back(elapsed_ns);
}
#endif

} // namespace db::index::blinktree