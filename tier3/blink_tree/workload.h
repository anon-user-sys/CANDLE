#pragma once
#include <cstdint>
#include <vector>
#include <random>
#include <cmath>
#include <algorithm>
#include <mutex>
#include <utility>
#include <numeric>

namespace application::blinktree_benchmark {

enum class TupleOp : std::uint8_t { INSERT, LOOKUP, UPDATE };

enum class WorkloadPattern : std::uint8_t { 
    WORKLOAD_A, WORKLOAD_B, WORKLOAD_C, WORKLOAD_D, WORKLOAD_F 
};

struct alignas(8) NumericTuple {
    TupleOp op{TupleOp::LOOKUP};
    std::uint64_t _key{0};
    std::int64_t _value{0};

    [[nodiscard]] constexpr bool operator==(TupleOp target) const noexcept { return op == target; }
    [[nodiscard]] constexpr std::uint64_t key() const noexcept { return _key; }
    [[nodiscard]] constexpr std::int64_t value() const noexcept { return _value; }
};

class ZipfianGenerator {
public:
    explicit ZipfianGenerator(std::uint64_t items, double theta = 0.99)
        : _items(items), _theta(theta),
          _zeta2(zeta(2, _theta)),
          _zetan(zeta(_items, _theta)),
          _alpha(1.0 / (1.0 - _theta)),
          _eta((1.0 - std::pow(2.0 / static_cast<double>(_items), 1.0 - _theta)) / (1.0 - _zeta2 / _zetan)) {}

    [[nodiscard]] std::uint64_t next(std::mt19937_64& gen) {
        std::uniform_real_distribution<double> dis(0.0, 1.0);
        const double u = dis(gen);
        const double uz = u * _zetan;
        if (uz < 1.0) return 1;
        if (uz < 1.0 + std::pow(0.5, _theta)) return 2;
        
        const std::uint64_t val = 1 + static_cast<std::uint64_t>(
            static_cast<double>(_items) * std::pow(_eta * u - _eta + 1.0, _alpha));
        return std::min(val, _items);
    }

private:
    [[nodiscard]] static double zeta(std::uint64_t n, double theta_val) noexcept {
        double sum = 0.0;
        for (std::uint64_t i = 1; i <= n; ++i) {
            sum += 1.0 / std::pow(static_cast<double>(i), theta_val);
        }
        return sum;
    }

    std::uint64_t _items;
    double _theta, _zeta2, _zetan, _alpha, _eta;
};

class Workload {
public:
    // initial_keys == 0: use the previous default, max(1000, total_ops / 10)
    explicit Workload(std::uint64_t total_ops, std::uint64_t seed = 42, 
                      WorkloadPattern pattern = WorkloadPattern::WORKLOAD_A, double skew = 0.99,
                      std::uint64_t initial_keys = 0, bool scramble = false) {
        std::mt19937_64 gen(seed);
        
        _initial_keys = initial_keys ? initial_keys : std::max(1000ULL, total_ops / 10ULL);
        _tuples.reserve(_initial_keys + total_ops);

        for (std::uint64_t i = 1; i <= _initial_keys; ++i) {
            _tuples.push_back({TupleOp::INSERT, i, static_cast<std::int64_t>(i * 10)});
        }

        ZipfianGenerator zipf(_initial_keys, skew);
        // --scramble: map Zipf rank through the bijection key = 1 + ((rank-1) * P) mod N, so hot keys do not
        // pile up in the first few leaves (same purpose as YCSB's hash scattering, no collisions)
        std::uint64_t perm = 2654435761ULL % _initial_keys;
        if (perm == 0) perm = 1;
        while (std::gcd(perm, _initial_keys) != 1) ++perm;
        auto scatter = [&](std::uint64_t rank) {
            if (!scramble) return rank;
            return 1 + static_cast<std::uint64_t>((static_cast<unsigned __int128>(rank - 1) * perm) % _initial_keys);
        };
        std::uniform_int_distribution<std::uint32_t> op_dist(0, 99);
        std::uint64_t max_inserted_key = _initial_keys;

        for (std::uint64_t i = 0; i < total_ops; ++i) {
            const std::uint32_t rand_val = op_dist(gen);
            TupleOp op = TupleOp::LOOKUP;
            std::uint64_t key = scatter(zipf.next(gen));

            switch (pattern) {
                case WorkloadPattern::WORKLOAD_A: op = (rand_val < 50) ? TupleOp::LOOKUP : TupleOp::UPDATE; break;
                case WorkloadPattern::WORKLOAD_B: op = (rand_val < 95) ? TupleOp::LOOKUP : TupleOp::UPDATE; break;
                case WorkloadPattern::WORKLOAD_C: op = TupleOp::LOOKUP; break;
                case WorkloadPattern::WORKLOAD_D:
                    if (rand_val < 95) op = TupleOp::LOOKUP;
                    else { op = TupleOp::INSERT; key = ++max_inserted_key; }
                    break;
                case WorkloadPattern::WORKLOAD_F: op = (rand_val < 50) ? TupleOp::LOOKUP : TupleOp::UPDATE; break;
            }
            // every UPDATE writes a unique value: with a fixed key*10, reordering of updates to the same key
            // or a stale read would be invisible to the validator
            const std::int64_t value = (op == TupleOp::UPDATE)
                ? static_cast<std::int64_t>((i + 1) * 1000003ULL + key)
                : static_cast<std::int64_t>(key * 10);
            _tuples.push_back({op, key, value});
        }
    }

    [[nodiscard]] std::pair<std::uint64_t, std::uint64_t> next(std::uint32_t batch_size) noexcept {
        std::lock_guard<std::mutex> lock(_mutex);
        if (_current_idx >= _tuples.size()) return {0, 0};
        
        const std::uint64_t count = std::min(static_cast<std::uint64_t>(batch_size), _tuples.size() - _current_idx);
        const std::uint64_t idx = _current_idx;
        _current_idx += count;
        return {idx, count};
    }

    // Return a batch starting at start_offset (moves the index on the first call)
    [[nodiscard]] std::pair<std::uint64_t, std::uint64_t> next_from(std::uint64_t start_offset, std::uint32_t batch_size) noexcept {
        std::lock_guard<std::mutex> lock(_mutex);
        if (_current_idx < start_offset) {
            _current_idx = start_offset;
        }
        if (_current_idx >= _tuples.size()) return {0, 0};

        const std::uint64_t count = std::min(static_cast<std::uint64_t>(batch_size), _tuples.size() - _current_idx);
        const std::uint64_t idx = _current_idx;
        _current_idx += count;
        return {idx, count};
    }

    // Skip to an offset
    void skip(std::uint64_t count) noexcept {
        std::lock_guard<std::mutex> lock(_mutex);
        _current_idx = std::min(_current_idx + count, static_cast<std::uint64_t>(_tuples.size()));
    }

    [[nodiscard]] std::uint64_t initial_keys() const noexcept { return _initial_keys; }
    [[nodiscard]] const NumericTuple& operator[](std::size_t index) const noexcept { return _tuples[index]; }
    [[nodiscard]] std::size_t size() const noexcept { return _tuples.size(); }

private:
    std::vector<NumericTuple> _tuples;
    std::uint64_t _current_idx{0};
    std::uint64_t _initial_keys{0};
    std::mutex _mutex;
};

} // namespace application::blinktree_benchmark