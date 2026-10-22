// MU kernels for the device translation-tax experiment (see ../tt_common.hpp and ../README.md).
//
// Coherence: this device has no hardware cache coherence with the host. The host writes the
// config with copyToDevice, which lands in DRAM but can leave a stale line in an MU cache, so
// every kernel flushes the config before reading it (observe) and flushes everything it wrote
// before returning (publish), exactly as tier1/resident_worker does.
#include <cstdint>

#include "mu/mu.hpp"
#include "tt_common.hpp"

namespace {

// getClockCycle() is only meaningful as a 32-bit counter (it wraps about every 4.29 s), so the
// timed loop samples it in chunks and accumulates wrap-safe 32-bit deltas.
constexpr std::uint64_t kClockChunk = 4096;

}  // namespace

// Writes the check value into every target line of the (S, R, L) pattern.
void tt_init(std::uint8_t* buf, tt::Config* cfg)
{
    mu::flushCache(cfg, sizeof(tt::Config));
    const tt::Pattern p = tt::make_pattern(*cfg);
    const std::uint64_t k = tt::lines(*cfg);
    for (std::uint64_t j = 0; j < k; ++j) {
        const std::uint64_t off = tt::offset_of(p, j);
        *static_cast<std::uint64_t*>(static_cast<void*>(buf + off)) = tt::value_at(off);
    }
    cfg->done = 1;
    mu::flushCacheAll();
}

// One untimed warm-up pass (fills caches and any translation structures), then an identical
// timed pass over the same access sequence. The order is chosen before either pass, so the timed
// loop carries no per-access branch on it.
void tt_access(const std::uint8_t* buf, tt::Config* cfg)
{
    mu::flushCache(cfg, sizeof(tt::Config));
    mu::flushCacheAll();   // drop any stale lines this thread may hold for the target region
    const tt::Pattern p = tt::make_pattern(*cfg);
    const std::uint64_t n = cfg->accesses;
    const bool rr = cfg->order == tt::kOrderRoundRobin;

    std::uint64_t s = cfg->seed;
    std::uint64_t warm = 0;
    if (rr) {
        for (std::uint64_t a = 0; a < n; ++a)
            warm += *static_cast<const std::uint64_t*>(static_cast<const void*>(buf + tt::offset_of(p, tt::pick_rr(p, a, s))));
    } else {
        for (std::uint64_t a = 0; a < n; ++a)
            warm += *static_cast<const std::uint64_t*>(static_cast<const void*>(buf + tt::offset_of(p, tt::pick_random(p, s))));
    }
    (void)warm;

    s = cfg->seed;
    std::uint64_t sum = 0;
    std::uint64_t cycles = 0;
    std::uint32_t last = static_cast<std::uint32_t>(mu::getClockCycle());
    if (rr) {
        for (std::uint64_t a = 0; a < n; ++a) {
            sum += *static_cast<const std::uint64_t*>(static_cast<const void*>(buf + tt::offset_of(p, tt::pick_rr(p, a, s))));
            if ((a & (kClockChunk - 1)) == kClockChunk - 1) {
                const std::uint32_t now = static_cast<std::uint32_t>(mu::getClockCycle());
                cycles += static_cast<std::uint32_t>(now - last);
                last = now;
            }
        }
    } else {
        for (std::uint64_t a = 0; a < n; ++a) {
            sum += *static_cast<const std::uint64_t*>(static_cast<const void*>(buf + tt::offset_of(p, tt::pick_random(p, s))));
            if ((a & (kClockChunk - 1)) == kClockChunk - 1) {
                const std::uint32_t now = static_cast<std::uint32_t>(mu::getClockCycle());
                cycles += static_cast<std::uint32_t>(now - last);
                last = now;
            }
        }
    }
    const std::uint32_t now = static_cast<std::uint32_t>(mu::getClockCycle());
    cycles += static_cast<std::uint32_t>(now - last);

    cfg->cycles = cycles;
    cfg->sum = sum;
    cfg->done = 2;
    mu::flushCacheAll();
}

// Spins the PRNG with no memory access, so the host can convert clock cycles to nanoseconds
// (host wall time over a long run divided by the cycles the device counted).
void tt_calibrate(tt::Config* cfg)
{
    mu::flushCache(cfg, sizeof(tt::Config));
    const std::uint64_t n = cfg->accesses;
    std::uint64_t s = cfg->seed;
    std::uint64_t sink = 0;
    std::uint64_t cycles = 0;
    std::uint32_t last = static_cast<std::uint32_t>(mu::getClockCycle());
    for (std::uint64_t a = 0; a < n; ++a) {
        sink += tt::next(s);
        if ((a & (kClockChunk - 1)) == kClockChunk - 1) {
            const std::uint32_t now = static_cast<std::uint32_t>(mu::getClockCycle());
            cycles += static_cast<std::uint32_t>(now - last);
            last = now;
        }
    }
    const std::uint32_t now = static_cast<std::uint32_t>(mu::getClockCycle());
    cycles += static_cast<std::uint32_t>(now - last);
    cfg->cycles = cycles;
    cfg->sum = sink;
    cfg->done = 3;
    mu::flushCacheAll();
}

MU_KERNEL_ADD(tt_init);
MU_KERNEL_ADD(tt_access);
MU_KERNEL_ADD(tt_calibrate);
