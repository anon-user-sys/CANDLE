// application/blinktree_benchmark/benchmark_config.h
#pragma once
#include <cstdint>

namespace application::blinktree_benchmark {

// Run-time settings (set by main() from CLI options such as --batch)
class BenchmarkConfig {
public:
    [[nodiscard]] static std::uint32_t batch_size() noexcept { return _batch_size; }
    [[nodiscard]] static std::uint64_t max_parallel_requests() noexcept { return _max_parallel_requests; }

    static void set_batch_size(std::uint32_t n) noexcept {
        _batch_size = n;
        if (_max_parallel_requests < n) _max_parallel_requests = n;
    }

private:
    static inline std::uint32_t _batch_size{32U};
    static inline std::uint64_t _max_parallel_requests{512U};
};

} // namespace application::blinktree_benchmark
