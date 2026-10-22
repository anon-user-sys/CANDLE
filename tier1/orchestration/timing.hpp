#pragma once
#include <cstdint>
#include <ctime>

// rdtsc-based timing.
//
// Ideal: start = lfence+rdtsc, stop = rdtscp+lfence (Intel white paper).
// rdtscp requires CPUID.80000001H:EDX[27]; QEMU does not expose it and
// traps it as an illegal instruction.
//
// Portable alternative: lfence+rdtsc for both start and stop.
// The leading lfence on stop() drains the OoO window before reading the
// counter, giving the same one-sided ordering guarantee as rdtscp.
// For measurements in the microsecond range the difference is < 1 ns.
//
// On real silicon, upgrade stop() to rdtscp+lfence for tighter bounds.

namespace timing
{

// Probe whether the CPU supports rdtscp (CPUID.80000001H:EDX[27]).
// Call once at startup; result is stable for the lifetime of the process.
inline bool has_rdtscp()
{
    uint32_t edx = 0;
    __asm__ volatile (
        "cpuid"
        : "=d"(edx)
        : "a"(0x80000001u)
        : "ebx", "ecx"
    );
    return (edx >> 27) & 1u;
}

static inline uint64_t start()
{
    uint32_t lo, hi;
    __asm__ volatile (
        "lfence\n\t"
        "rdtsc\n\t"
        : "=a"(lo), "=d"(hi)
        :
        : "memory"
    );
    return (uint64_t)hi << 32 | lo;
}

// Portable stop: lfence+rdtsc.  Works on all x86 including QEMU.
static inline uint64_t stop()
{
    uint32_t lo, hi;
    __asm__ volatile (
        "lfence\n\t"
        "rdtsc\n\t"
        : "=a"(lo), "=d"(hi)
        :
        : "memory"
    );
    return (uint64_t)hi << 32 | lo;
}

// Precision stop for real silicon: rdtscp+lfence.
// Only call this after confirming has_rdtscp() == true.
static inline uint64_t stop_precise()
{
    uint32_t lo, hi;
    __asm__ volatile (
        "rdtscp\n\t"
        "lfence\n\t"
        : "=a"(lo), "=d"(hi)
        :
        : "rcx", "memory"
    );
    return (uint64_t)hi << 32 | lo;
}

// Calibrate TSC frequency against CLOCK_MONOTONIC over a 100 ms window.
// Returns cycles per nanosecond (= GHz).
inline double tsc_ghz()
{
    const uint64_t window_ns = 100'000'000ULL;
    struct timespec ts0, ts1;

    clock_gettime(CLOCK_MONOTONIC, &ts0);
    uint64_t t0 = start();

    struct timespec tsc;
    do { clock_gettime(CLOCK_MONOTONIC, &tsc); }
    while (static_cast<uint64_t>(tsc.tv_sec  - ts0.tv_sec)  * 1'000'000'000ULL +
           static_cast<uint64_t>(tsc.tv_nsec - ts0.tv_nsec) < window_ns);

    uint64_t t1 = stop();
    clock_gettime(CLOCK_MONOTONIC, &ts1);

    uint64_t elapsed_ns = static_cast<uint64_t>(ts1.tv_sec  - ts0.tv_sec)  * 1'000'000'000ULL
                        + static_cast<uint64_t>(ts1.tv_nsec - ts0.tv_nsec);
    return static_cast<double>(t1 - t0) / static_cast<double>(elapsed_ns);
}

inline double to_us(uint64_t cycles, double ghz)
{
    return static_cast<double>(cycles) / ghz / 1000.0;
}

} // namespace timing
