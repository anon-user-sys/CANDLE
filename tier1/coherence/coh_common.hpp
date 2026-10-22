// Shared between the host driver (main.cpp) and the MU kernels (mu_kernel/coh_kernel.cpp).
// Keep it freestanding: no std library beyond <stdint.h>.
#pragma once
#include <stdint.h>

namespace coh
{

// Value expected at word i for a given salt. Every test verifies against this, so an offset
// error or a stale line from an earlier salt is caught, not just a missing write.
inline uint64_t pat(uint64_t i, uint64_t salt)
{
    uint64_t x = (i + 1) * 0x9E3779B97F4A7C15ull ^ salt;
    x ^= x >> 29; x *= 0xBF58476D1CE4E5B9ull; x ^= x >> 32;
    return x;
}

constexpr uint64_t kPoison = 0xDEADBEEFDEADBEEFull;
constexpr uint64_t kStride = 8;          // one 64 B line per task result slot

// Kernel configuration, written by the host with direct stores + flushHostCache.
struct Cfg
{
    uint64_t words;      // buffer length in 8 B words
    uint64_t par;        // tasks that do work
    uint64_t salt;
    uint64_t mode;       // test-specific bits (see kernel comments)
    uint64_t a, b, c, d; // test-specific parameters
};
static_assert(sizeof(Cfg) == 64, "Cfg must be one cache line");

// Mode bits shared by k_fill / k_check.
enum : uint64_t
{
    M_ENTRY_INVAL = 1,   // flushCacheAll at kernel entry (drop stale device copies)
    M_EXIT_FLUSH  = 2,   // flushCache(own range) at kernel exit
    M_EXIT_ALL    = 4,   // flushCacheAll at kernel exit
};

// Ping-pong layout (k11), in 8 B words; each field on its own line.
enum : uint64_t { PP_H2D = 0, PP_D2H = 8, PP_STOP = 16, PP_DONE = 24, PP_WORDS = 32 };

// Publish layout (k10): counters first (one line per lane), then data.
inline uint64_t pub_counter(uint64_t lane) { return lane * kStride; }
inline uint64_t pub_data(uint64_t P, uint64_t C, uint64_t L, uint64_t lane, uint64_t chunk)
{
    return P * kStride + ((lane * C + chunk) * L) * 8;
}
// Publish modes (k10).
enum : uint64_t
{
    PUB_FLUSH_FENCE = 0,   // flushCache(chunk), fence rw,rw, counter, flushCache(counter)
    PUB_FLUSH       = 1,   // as 0 without the fence
    PUB_FLUSHALL    = 2,   // flushCache(chunk), flushCacheAll, fence, counter
    PUB_NOFLUSH     = 3,   // counter only (data left in the device cache until exit)
    PUB_FLUSHALL_NOFENCE = 4,   // as 2 without the fence: is it flushCacheAll or the fence?
};

}  // namespace coh
