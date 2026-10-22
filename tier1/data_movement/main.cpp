// Data Movement Tax Benchmark Suite
//
// Characterizes the host↔device data path on XCENA MX1 across the SDK's
// transfer API paths plus direct host-pointer access:
//
//   1. ctx->copyToDevice    (CxlMem, H→D)   — DMA-style host→device
//   2. ctx->copyFromDevice  (CxlMem, D→H)   — DMA-style device→host
//   3. memcpy + flushHostCache  (CxlMem, H→D)  — direct write via host ptr
//      (uses word_copy to avoid SIMD memcpy issues on CXL pages; see notes)
//   4. flushHostCache + memcpy  (CxlMem, D→H)  — direct read via host ptr
//   5. ctx->syncToDevice    (IoMem,  H→D)   — host-mapped IO sync
//   6. ctx->syncFromDevice  (IoMem,  D→H)   — host-mapped IO sync
//
// REQUIRES sudo (or a udev rule granting access to /dev/cxl/mem0 and
// /dev/ndctl0).  Without it, every host-side operation on memAlloc-returned
// pointers — including pxl::flushHostCache — faults with SIGBUS because the
// driver silently fails to map device memory into the user process.  See
// notes/PERMISSIONS.md.
//
// Tests (mirrors the orchestration_tax `tN` convention):
//   D1  bandwidth_sweep     — GB/s vs transfer size for each API path
//   D2  latency_per_call    — small-message round-trip floor (64 B)
//   D3  memtype_compare     — CxlMem vs IoMem at matched sizes
//   D4  direct_vs_copy      — copyToDevice vs memcpy+flush vs store-loop+flush
//   D5  repeat_call_over    — back-to-back same-call burst (batching test)
//   D6  numa_placement      — D1 from NUMA-local vs NUMA-remote host CPU
//   D7  concurrent_bw       — aggregate H→D BW vs host-thread count
//   D8  alloc_cost          — memAlloc / ioMemAlloc vs size (cold + warm)
//   D9  free_cost           — memFree vs size
//   D10 access_pattern      — direct host-pointer reads/writes, varying stride
//   D11 bidirectional       — H→D and D→H simultaneously (duplex link util)
//   D12 rw_ratio            — direct load:store ratio sweep on a CxlMem region
//
// Each test prints a CSV block preceded by a "# TEST: " header for piping
// into per-section parsers.  Standalone cache-flush cost is intentionally
// NOT measured here — see §cache_coherence_tax for those numbers.

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <immintrin.h>
#include <limits>
#include <random>
#include <sched.h>
#include <sys/mman.h>
#include <string>
#include <thread>
#include <vector>

#include "pxl/pxl.hpp"
#include "pxl/signal_handler.hpp"
#include <csignal>
#include "stats.hpp"
#include "timing.hpp"

// ── Direct-access helpers ────────────────────────────────────────────────────
//
// Three alternative ways to write host data into a memAlloc-returned CXL
// pointer, all of which require the user to hold permissions on
// /dev/cxl/mem0 (see notes/PERMISSIONS.md):
//
//   std::memcpy   — glibc's optimized memcpy (rep-movsb / SIMD path)
//   word_copy     — explicit 8-byte MOV loop (no SIMD)
//   nt_store_copy — AVX2 non-temporal stores (bypasses host cache)
//
// We include all three so D4 can A/B them against ctx->copyToDevice.

static inline void word_copy(void* dst, const void* src, size_t n)
{
    auto* d = static_cast<uint8_t*>(dst);
    auto* s = static_cast<const uint8_t*>(src);
    size_t n8 = n / 8;
    auto* d8 = reinterpret_cast<uint64_t*>(d);
    auto* s8 = reinterpret_cast<const uint64_t*>(s);
    for (size_t i = 0; i < n8; i++) d8[i] = s8[i];
    for (size_t i = n8 * 8; i < n; i++) d[i] = s[i];
}

// Write back the cache lines covering [p, p + n): the unaligned head and tail are copied with
// ordinary (cached) stores, which the device would not see without a flush.
static inline void flush_lines(const uint8_t* p, size_t n)
{
    const uintptr_t first = reinterpret_cast<uintptr_t>(p) & ~uintptr_t(63);
    const uintptr_t end   = reinterpret_cast<uintptr_t>(p) + n;
    for (uintptr_t a = first; a < end; a += 64) _mm_clflush(reinterpret_cast<const void*>(a));
}

__attribute__((target("avx2")))
static inline void nt_store_copy(void* dst, const void* src, size_t n)
{
    auto* d = static_cast<uint8_t*>(dst);
    auto* s = static_cast<const uint8_t*>(src);
    bool cached = false;                       // any bytes written with ordinary stores?
    size_t head = (reinterpret_cast<uintptr_t>(d) % 32);
    if (head)
    {
        head = std::min<size_t>(32 - head, n);
        word_copy(d, s, head);
        flush_lines(d, head);
        cached = true;
        d += head; s += head; n -= head;
    }
    size_t body = (n / 32) * 32;
    for (size_t i = 0; i < body; i += 32)
    {
        __m256i v = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(s + i));
        _mm256_stream_si256(reinterpret_cast<__m256i*>(d + i), v);
    }
    if (n > body)
    {
        word_copy(d + body, s + body, n - body);
        flush_lines(d + body, n - body);
        cached = true;
    }
    // Aligned sizes (every size in the sweeps) take neither branch: NT stores + sfence only.
    if (cached) _mm_mfence(); else _mm_sfence();
}

// ── tunables ──────────────────────────────────────────────────────────────────
static const int    WARMUP_REPS = 5;   // direct paths only: flat from call 1
// Copy APIs ramp for ~1,500 calls at 1 MiB (measured 2026-09-25: first 500
// calls 311 us vs 139 us steady, 2.2x). Warm those until the median stabilises.
static const int    WARM_MIN    = 64;      // always at least this many
static const int    WARM_MAX    = 3000;    // cap so large sizes stay bounded
static const int    WARM_WIN    = 200;     // compare medians of adjacent windows
static const double WARM_TOL    = 0.03;    // "stable" = <3% change between windows
// Per-size REPS scales down for larger transfers (see reps_for_size below).
// ─────────────────────────────────────────────────────────────────────────────

static volatile pxl::PxlResult sink;
static uint64_t (*stop_fn)() = timing::stop;
// --no-direct: skip host-pointer paths so the DMA subset runs without root.
static bool g_no_direct = false;
// --keep-iomem: IoMem is capped at 2 GB, unused by our offload code and never
// wins where usable, so its rows are off by default (PLAN.md).
static bool g_keep_iomem = false;
// --fine: D1 also sweeps 128 KiB, 512 KiB and 2 MiB, to pin the DMA/direct
// crossover inside the 4x grid (paper Sec. 4.3 reports it as a band).
static bool g_fine = false;
// --verify-only: D1 runs only the bit-exact round trip per size (no timing) -- a
// few-second device health canary that covers the >=256 KiB sizes that failed
// on 2026-10-09 while small sizes passed.
static bool g_verify_only = false;

static void check(pxl::PxlResult r, const char* msg)
{
    if (r != pxl::PxlResult::Success)
    {
        fprintf(stderr, "FATAL: %s\n", msg);
        std::exit(1);
    }
}

// ── measurement correctness helpers (2026-09-25) ─────────────────────────────
//
// 1. RESULT TALLY. Transfer results used to be assigned to a write-only `sink`.
//    A failed copy returns fast, which in a bandwidth test shows up as a very
//    HIGH GB/s rather than a low one. chkr() counts them; each test prints a
//    COPY_CHECK line.
// 2. PAYLOAD VARIATION. flushHostCache(opt=true) SKIPS ranges whose contents
//    did not change: measured 87.8 us clean vs 142.2 us after a change (1.6x).
//    The suite used to re-copy a constant 0xa5 buffer, so its direct-write
//    flushes took the cheap path. touch_payload() must run before every timed
//    direct write.
// 3. VERIFICATION. Nothing here ever checked that the bytes arrived. This
//    device can produce wrong results while every health check passes, so each
//    size is verified once, outside the timed region, with a position-dependent
//    pattern (a constant cannot catch data landing at the wrong offset).
static long g_calls = 0, g_fails = 0;
static pxl::PxlResult g_first_fail = pxl::PxlResult::Success;

static inline pxl::PxlResult chkr(pxl::PxlResult r)
{
    g_calls++;
    if (r != pxl::PxlResult::Success) { if (!g_fails++) g_first_fail = r; }
    return r;
}
static void copy_check(const char* test)
{
    printf("# COPY_CHECK %s: calls=%ld ok=%ld failed=%ld", test, g_calls, g_calls - g_fails, g_fails);
    if (g_fails) printf("  first_fail=%d  <-- DATA INVALID", (int)g_first_fail);
    printf("\n");
    fflush(stdout);
    g_calls = g_fails = 0; g_first_fail = pxl::PxlResult::Success;
}

// Change one byte per rep so a flush can never be skipped as "unchanged".
static inline void touch_payload(void* buf, size_t n, uint64_t seq)
{
    if (!n) return;
    static_cast<unsigned char*>(buf)[seq % n] = static_cast<unsigned char>(seq);
}

static void fill_pattern(void* buf, size_t n, uint64_t salt)
{
    auto* b = static_cast<unsigned char*>(buf);
    for (size_t i = 0; i < n; i++) b[i] = static_cast<unsigned char>((i * 31 + 7 + salt) & 0xff);
}

// Round-trip verify one size, outside any timed region. Returns true on match.
static bool verify_roundtrip(pxl::runtime::Context* ctx, void* dev, size_t n,
                             const char* tag)
{
    std::vector<unsigned char> src(n), back(n, 0);
    fill_pattern(src.data(), n, 0xA5);
    bool ok = chkr(ctx->copyToDevice(dev, src.data(), n))   == pxl::PxlResult::Success
           && chkr(ctx->copyFromDevice(back.data(), dev, n)) == pxl::PxlResult::Success
           && std::memcmp(src.data(), back.data(), n) == 0;
    if (!ok) printf("# VERIFY FAIL %s size=%zu  <-- DATA INVALID\n", tag, n);
    return ok;
}

// Warm a copy-API path until its median stops moving (or WARM_MAX).
// Returns the number of warm-up calls actually issued, for the record.
template <typename F>
static int warm_until_stable(F&& body, double ghz)
{
    std::vector<double> w;
    double prev = -1;
    int stable = 0;
    for (int i = 0; i < WARM_MAX; i++)
    {
        uint64_t t0 = timing::start(); body(); uint64_t t1 = stop_fn();
        w.push_back(timing::to_us(t1 - t0, ghz));
        if ((int)w.size() >= WARM_MIN && (int)w.size() % WARM_WIN == 0)
        {
            std::vector<double> win(w.end() - WARM_WIN, w.end());
            std::sort(win.begin(), win.end());
            double m = win[win.size() / 2];
            // A slow ramp keeps ADJACENT windows within tolerance while the
            // median is still falling, which exited early at 400 calls. Require
            // the median to stop improving over TWO consecutive windows, and
            // compare against the best median seen, not just the previous one.
            if (prev > 0 && m >= prev * (1.0 - WARM_TOL)) { if (++stable >= 2) return (int)w.size(); }
            else stable = 0;
            if (prev < 0 || m < prev) prev = m;          // track the best (lowest) median
        }
    }
    return WARM_MAX;
}

// Larger transfers take longer per-call; fewer reps are sufficient for stable
// statistics and keep total runtime bounded.
static int reps_for_size(size_t bytes)
{
    if (bytes <=    4 * 1024)        return 500;   // ≤ 4 KB
    if (bytes <=   64 * 1024)        return 300;   // ≤ 64 KB
    if (bytes <=    1 * 1024 * 1024) return 200;   // ≤ 1 MB
    if (bytes <=   16 * 1024 * 1024) return 100;   // ≤ 16 MB
    if (bytes <=  256 * 1024 * 1024) return  40;   // ≤ 256 MB
    return 20;                                     // ≥ 1 GB
}

// Sweep range: powers of 4 from 64 B up to 256 MB.
//   Hits the cache-line (64 B), small-page (4 KB), and huge-page (2 MB)
//   boundaries naturally.  Capped at 256 MB so IoMem (≈ 1.3 GB on this box)
//   has headroom to stage transfers.
static const std::vector<size_t> SIZE_SWEEP = {
    64,                                 //  cache line
    256,
    1024,                               //  1 KB
    4096,                               //  4 KB  (page)
    16   * 1024,
    64   * 1024,                        //  64 KB
    256  * 1024,
    1    * 1024 * 1024,                 //  1 MB
    4    * 1024 * 1024,
    16   * 1024 * 1024,                 //  16 MB
    64   * 1024 * 1024,
    256  * 1024 * 1024,                 //  256 MB
};

// IoMem on this device is ~1.3 GB.  Cap IoMem-path tests at this size so we
// don't fail allocations or saturate the IoMem region during a run.
static const size_t IOMEM_MAX = 256 * 1024 * 1024;  // 256 MB

// GB/s = bytes / (us * 1e-6) / 2^30
static double gb_per_sec(size_t bytes, double us_per_call)
{
    if (us_per_call <= 0) return 0.0;
    const double secs = us_per_call * 1e-6;
    const double gib  = static_cast<double>(bytes) / (1024.0 * 1024.0 * 1024.0);
    return gib / secs;
}

// ── D1: bandwidth_sweep ──────────────────────────────────────────────────────
//
// For each (API path, transfer size) pair, measure the steady-state per-call
// latency and convert to effective bandwidth (GB/s).  Buffers are allocated
// once at maximum size and reused for all smaller sizes, so allocation cost
// does not pollute the timing.

static void d1_bandwidth_sweep(double ghz)
{
    printf("\n# TEST: D1_bandwidth_sweep\n");
    printf("# Copy APIs warm until the median stabilises (<=%d calls); direct paths\n", WARM_MAX);
    printf("# use %d (measured flat from call 1). GB/s from p50, not mean.\n", WARMUP_REPS);
    printf("# Payload changes every rep: flushHostCache skips unchanged ranges.\n");
    printf("api,direction,size_bytes,p50_us,min_us,max_us,mean_us,gb_per_sec,warm,reps\n");

    const size_t maxSize = SIZE_SWEEP.back();

    auto* ctx = pxl::runtime::createContext(0);
    if (!ctx) { fprintf(stderr, "D1: createContext failed\n"); return; }

    void* devCxl = ctx->memAlloc(maxSize);
    void* host   = std::malloc(maxSize);
    if (!devCxl || !host)
    {
        fprintf(stderr, "D1: CxlMem/host allocation failed (cxl=%p host=%p)\n",
                devCxl, host);
        if (devCxl) ctx->memFree(devCxl);
        std::free(host);
        pxl::runtime::destroyContext(ctx);
        return;
    }

    // IoMem may be unavailable for this session — degrade gracefully.
    void* devIo = g_keep_iomem ? ctx->ioMemAlloc(maxSize) : nullptr;
    if (g_keep_iomem && !devIo)
        printf("# D1: IoMem unavailable for this session — sync* rows skipped\n");

    // Pre-touch host buffer so page faults are not in the timing window.
    std::memset(host, 0xa5, maxSize);

    uint64_t seq = 0;
    // `ramping` = a copy API (needs adaptive warm-up); direct paths are flat.
    auto run = [&](const char* api, const char* dir, size_t bytes,
                   std::vector<double>& samples, bool ramping,
                   auto&& body)
    {
        const int reps = reps_for_size(bytes);
        samples.clear();
        samples.reserve(reps);
        auto step = [&] { touch_payload(host, bytes, ++seq); body(); };
        int warm = ramping ? warm_until_stable(step, ghz) : WARMUP_REPS;
        if (!ramping) for (int i = 0; i < WARMUP_REPS; i++) step();
        for (int i = 0; i < reps; i++)
        {
            touch_payload(host, bytes, ++seq);          // outside the timed region
            uint64_t t0 = timing::start();
            body();
            uint64_t t1 = stop_fn();
            samples.push_back(timing::to_us(t1 - t0, ghz));
        }
        auto st = Stats::from(samples);
        printf("%s,%s,%zu,%.4f,%.4f,%.4f,%.4f,%.4f,%d,%d\n",
               api, dir, bytes, st.p50, st.min, st.max, st.mean,
               gb_per_sec(bytes, st.p50), warm, reps);
    };

    std::vector<double> samples;

    std::vector<size_t> sizes = SIZE_SWEEP;
    if (g_fine)
    {
        for (size_t x : {size_t(128) << 10, size_t(512) << 10, size_t(2) << 20}) sizes.push_back(x);
        std::sort(sizes.begin(), sizes.end());
    }

    for (size_t n : sizes)
    {
        // ── CxlMem: DMA-style copy* APIs
        if (!verify_roundtrip(ctx, devCxl, n, "D1")) continue;   // outside timing
        if (g_verify_only) { printf("# VERIFY OK D1 size=%zu\n", n); continue; }

        run("copyToDevice", "H2D", n, samples, true, [&] {
            sink = chkr(ctx->copyToDevice(devCxl, host, n));
        });
        run("copyFromDevice", "D2H", n, samples, true, [&] {
            sink = chkr(ctx->copyFromDevice(host, devCxl, n));
        });
        if (g_no_direct) continue;

        // ── CxlMem: direct host pointer access + explicit cache management
        run("direct_memcpy", "H2D", n, samples, false, [&] {
            std::memcpy(devCxl, host, n);
            pxl::flushHostCache(devCxl, n);
        });
        run("direct_memcpy", "D2H", n, samples, false, [&] {
            pxl::flushHostCache(devCxl, n);
            std::memcpy(host, devCxl, n);
        });
        run("direct_word", "H2D", n, samples, false, [&] {
            word_copy(devCxl, host, n);
            pxl::flushHostCache(devCxl, n);
        });
        run("direct_word", "D2H", n, samples, false, [&] {
            pxl::flushHostCache(devCxl, n);
            word_copy(host, devCxl, n);
        });
        run("direct_ntstore", "H2D", n, samples, false, [&] {
            nt_store_copy(devCxl, host, n);
            pxl::flushHostCache(devCxl, n);
        });
        // NT stores + sfence only (nt_store_copy ends with _mm_sfence). coherence_tax k7/k13
        // (2026-10-10): the device sees NT-written data without a flush (0 bad in every case),
        // so the flush in direct_ntstore above is unnecessary; this arm is the correct path.
        run("direct_ntstore_noflush", "H2D", n, samples, false, [&] {
            nt_store_copy(devCxl, host, n);
        });

        // ── IoMem: sync* APIs (no host-side staging measured here)
        if (devIo && g_keep_iomem)
        {
            run("syncToDevice", "H2D", n, samples, true, [&] {
                sink = chkr(ctx->syncToDevice(devIo, n));
            });
            run("syncFromDevice", "D2H", n, samples, true, [&] {
                sink = chkr(ctx->syncFromDevice(devIo, n));
            });
        }
    }

    ctx->memFree(devCxl);
    if (devIo) ctx->memFree(devIo);
    std::free(host);
    pxl::runtime::destroyContext(ctx);
    copy_check("D1");
}

// ── D2: latency_per_call ─────────────────────────────────────────────────────
//
// The smallest meaningful per-call cost for each transfer API.  D1 already
// shows roughly 50 µs at 64 B; D2 nails down that floor with a much higher
// rep count and reports the full distribution (min/p50/p95/p99/max) so we
// can characterize the tail.
//
// "round_trip" measures one H→D followed by one D→H back-to-back on the same
// 64 B region — i.e. the cost of completing a full ping.  This is the lower
// bound for any device offload pattern that needs to deliver a result back
// to the host per launch.
//
// Smallest size = 64 B (one cache line).  At this size the byte-transfer
// component is sub-microsecond, so the reported number is essentially pure
// API overhead (syscall + DMA setup + command publish + completion wait).

static void d2_latency_per_call(double ghz)
{
    printf("\n# TEST: D2_latency_per_call\n");
    printf("# Single 64-byte transfer per call; high rep count for tight tails.\n");
    printf("# Payload varies per rep; result codes tallied; warm until stable.\n");
    printf("api,direction,reps,mean_us,std_us,min_us,p50_us,p95_us,p99_us,max_us\n");

    const size_t N         = 64;       // one cache line
    const int    LAT_REPS  = 2000;
    const int    LAT_WARMUP = 50;

    auto* ctx = pxl::runtime::createContext(0);
    if (!ctx) { fprintf(stderr, "D2: createContext failed\n"); return; }

    void* devCxl = ctx->memAlloc(N);
    void* host   = std::malloc(N);
    if (!devCxl || !host)
    {
        fprintf(stderr, "D2: CxlMem/host allocation failed\n");
        if (devCxl) ctx->memFree(devCxl);
        std::free(host);
        pxl::runtime::destroyContext(ctx);
        return;
    }
    void* devIo = g_keep_iomem ? ctx->ioMemAlloc(N) : nullptr;
    if (g_keep_iomem && !devIo)
        printf("# D2: IoMem unavailable — sync* rows skipped\n");
    std::memset(host, 0x5a, N);

    uint64_t seq = 0;
    auto run = [&](const char* api, const char* dir, auto&& body)
    {
        auto step = [&] { touch_payload(host, N, ++seq); body(); };
        warm_until_stable(step, ghz);
        (void)LAT_WARMUP;
        std::vector<double> samples;
        samples.reserve(LAT_REPS);
        for (int i = 0; i < LAT_REPS; i++)
        {
            touch_payload(host, N, ++seq);          // outside the timed region
            uint64_t t0 = timing::start();
            body();
            uint64_t t1 = stop_fn();
            samples.push_back(timing::to_us(t1 - t0, ghz));
        }
        auto s = Stats::from(samples);
        printf("%s,%s,%d,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f\n",
               api, dir, LAT_REPS,
               s.mean, s.stddev, s.min, s.p50, s.p95, s.p99, s.max);
    };

    if (!verify_roundtrip(ctx, devCxl, N, "D2"))
        printf("# D2: verification failed — rows below are suspect\n");

    run("copyToDevice",   "H2D",       [&] { sink = chkr(ctx->copyToDevice(devCxl, host, N)); });
    run("copyFromDevice", "D2H",       [&] { sink = chkr(ctx->copyFromDevice(host, devCxl, N)); });
    if (devIo && g_keep_iomem) {
        run("syncToDevice",   "H2D",       [&] { sink = chkr(ctx->syncToDevice(devIo, N)); });
        run("syncFromDevice", "D2H",       [&] { sink = chkr(ctx->syncFromDevice(devIo, N)); });
    }

    // Round-trip: one H→D + one D→H on the CxlMem buffer.
    run("roundtrip_cxl",  "H2D+D2H",   [&] {
        sink = chkr(ctx->copyToDevice(devCxl, host, N));
        sink = chkr(ctx->copyFromDevice(host, devCxl, N));
    });
    if (devIo && g_keep_iomem) {
        run("roundtrip_io",   "H2D+D2H",   [&] {
            sink = ctx->syncToDevice(devIo, N);
            sink = ctx->syncFromDevice(devIo, N);
        });
    }

    ctx->memFree(devCxl);
    if (devIo) ctx->memFree(devIo);
    std::free(host);
    pxl::runtime::destroyContext(ctx);
    copy_check("D2");
}

// ── D3: memtype_compare ──────────────────────────────────────────────────────
//
// Direct A/B between CxlMem (copyTo/FromDevice) and IoMem (syncTo/FromDevice)
// at matched sizes.  Both buffers are allocated once at the maximum size and
// reused.  The interleaving (alternating cxl/io call per iteration) hedges
// against drift in device/host state across the run.
//
// Output columns:
//   size_bytes, direction, cxl_mean_us, io_mean_us, ratio_io_over_cxl
//
//   ratio < 1.0  → IoMem is faster
//   ratio > 1.0  → CxlMem is faster
//   ratio ≈ 1.0  → the SDK treats the two paths equivalently
//
// D1 hinted the two paths are within noise at small sizes and that IoMem may
// be slightly faster at large sizes — this test confirms or refutes with a
// dedicated, interleaved comparison.

static void d3_memtype_compare(double ghz)
{
    printf("\n# TEST: D3_memtype_compare\n");
    printf("# Interleaved CxlMem (copy*) vs IoMem (sync*) at matched sizes\n");
    printf("size_bytes,direction,cxl_mean_us,cxl_std_us,io_mean_us,io_std_us,ratio_io_over_cxl,reps\n");

    // Smaller, focused sweep; IoMem capped well under available capacity.
    const std::vector<size_t> sizes = {
        64,
        4    * 1024,           // 4 KB
        64   * 1024,           // 64 KB
        1    * 1024 * 1024,    // 1 MB
        16   * 1024 * 1024,    // 16 MB
        64   * 1024 * 1024,    // 64 MB
    };

    auto* ctx = pxl::runtime::createContext(0);
    if (!ctx) { fprintf(stderr, "D3: createContext failed\n"); return; }

    const size_t maxSize = sizes.back();
    void* devCxl = ctx->memAlloc(maxSize);
    void* devIo  = ctx->ioMemAlloc(maxSize);
    void* host   = std::malloc(maxSize);
    if (!devCxl || !host)
    {
        fprintf(stderr, "D3: CxlMem/host allocation failed\n");
        if (devCxl) ctx->memFree(devCxl);
        if (devIo)  ctx->memFree(devIo);
        std::free(host);
        pxl::runtime::destroyContext(ctx);
        return;
    }
    if (!devIo)
    {
        printf("# D3: IoMem unavailable for this session — test SKIPPED\n");
        printf("#     (D3 is a CxlMem-vs-IoMem comparison; both are required.\n");
        printf("#      Try `sudo rmmod mx_dma && sudo modprobe mx_dma` to free leaked IoMem,\n");
        printf("#      then rerun.)\n");
        ctx->memFree(devCxl);
        std::free(host);
        pxl::runtime::destroyContext(ctx);
        return;
    }
    std::memset(host, 0xc3, maxSize);

    for (size_t n : sizes)
    {
        const int reps = reps_for_size(n);

        // Two directions per size.
        for (int dir = 0; dir < 2; dir++)
        {
            const char* dirName = (dir == 0) ? "H2D" : "D2H";

            // Warmup both APIs in alternation.
            for (int i = 0; i < WARMUP_REPS; i++)
            {
                if (dir == 0) { sink = ctx->copyToDevice(devCxl, host, n);
                                sink = ctx->syncToDevice(devIo, n); }
                else          { sink = ctx->copyFromDevice(host, devCxl, n);
                                sink = ctx->syncFromDevice(devIo, n); }
            }

            // Interleaved timed loop: collect both samples per iteration so
            // any system-wide drift affects both equally.
            std::vector<double> cxl_samples, io_samples;
            cxl_samples.reserve(reps);
            io_samples.reserve(reps);

            for (int i = 0; i < reps; i++)
            {
                if (dir == 0)
                {
                    uint64_t t0 = timing::start();
                    sink = ctx->copyToDevice(devCxl, host, n);
                    uint64_t t1 = stop_fn();
                    sink = ctx->syncToDevice(devIo, n);
                    uint64_t t2 = stop_fn();
                    cxl_samples.push_back(timing::to_us(t1 - t0, ghz));
                    io_samples.push_back(timing::to_us(t2 - t1, ghz));
                }
                else
                {
                    uint64_t t0 = timing::start();
                    sink = ctx->copyFromDevice(host, devCxl, n);
                    uint64_t t1 = stop_fn();
                    sink = ctx->syncFromDevice(devIo, n);
                    uint64_t t2 = stop_fn();
                    cxl_samples.push_back(timing::to_us(t1 - t0, ghz));
                    io_samples.push_back(timing::to_us(t2 - t1, ghz));
                }
            }

            auto c = Stats::from(cxl_samples);
            auto o = Stats::from(io_samples);
            double ratio = (c.mean > 0) ? (o.mean / c.mean) : 0.0;
            printf("%zu,%s,%.4f,%.4f,%.4f,%.4f,%.4f,%d\n",
                   n, dirName, c.mean, c.stddev, o.mean, o.stddev, ratio, reps);
        }
    }

    ctx->memFree(devCxl);
    ctx->memFree(devIo);
    std::free(host);
    pxl::runtime::destroyContext(ctx);
}

// ── D4: direct_vs_copy ───────────────────────────────────────────────────────
//
// At several representative sizes, A/B compare every H→D path on the same
// allocation:
//
//   copyToDevice    — SDK's DMA engine (the recommended path)
//   direct_memcpy   — glibc std::memcpy + flushHostCache
//   direct_word     — explicit 8-byte MOV loop + flushHostCache
//   direct_ntstore  — AVX2 non-temporal stores + flushHostCache
//   syncToDevice    — IoMem path (for comparison)
//
// And the D→H counterparts that exist (no NT-load equivalent of nt_store_copy
// makes sense for D→H).
//
// Output expresses each path's mean as a ratio over copyToDevice at the same
// size, so reading the column tells you "is this path faster (<1) or slower
// (>1) than the recommended path?"
//
// Falsifies (or confirms) the hypothesis: "copyToDevice is no faster than a
// hand-rolled memcpy + flushHostCache."  If hand-rolled paths beat the DMA
// API, users have a real reason to bypass it; if not, the SDK is doing
// something (DMA engine, pre-staged descriptors) that can't be matched in
// plain code.

static void d4_direct_vs_copy(double ghz)
{
    printf("\n# TEST: D4_direct_vs_copy\n");
    printf("# Per size, ratio = path_p50 / copyToDevice_p50 (same direction).\n");
    printf("# ratio < 1 → path beats SDK; ratio > 1 → SDK beats path.\n");
    printf("# Payload varies per rep (flushHostCache skips unchanged ranges, so\n");
    printf("# a constant buffer undermeasures every direct-write path).\n");
    printf("size_bytes,direction,api,p50_us,min_us,mean_us,gb_per_sec,ratio_vs_copy\n");

    const std::vector<size_t> sizes = {
        4    * 1024,           // 4 KB
        64   * 1024,           // 64 KB
        1    * 1024 * 1024,    // 1 MB
        16   * 1024 * 1024,    // 16 MB
        64   * 1024 * 1024,    // 64 MB
    };

    uint64_t d4_seq = 0;
    auto* ctx = pxl::runtime::createContext(0);
    if (!ctx) { fprintf(stderr, "D4: createContext failed\n"); return; }

    const size_t maxSize = sizes.back();
    void* devCxl = ctx->memAlloc(maxSize);
    void* host   = std::malloc(maxSize);
    if (!devCxl || !host)
    {
        fprintf(stderr, "D4: CxlMem/host allocation failed\n");
        if (devCxl) ctx->memFree(devCxl);
        std::free(host);
        pxl::runtime::destroyContext(ctx);
        return;
    }
    std::memset(host, 0x4d, maxSize);
    // IoMem is allocated on-demand per size below — a single 64 MB IoMem
    // allocation can fail (fragmentation, etc.) on a session that has already
    // run earlier tests, so we re-allocate per row and gracefully skip the
    // sync* rows when the per-size allocation fails.

    auto measure = [&](size_t n, const char* api, const char* dir, auto&& body)
    {
        const int reps = reps_for_size(n);
        for (int i = 0; i < WARMUP_REPS; i++) body();
        std::vector<double> samples;
        samples.reserve(reps);
        const bool ramping = (std::strncmp(api, "copy", 4) == 0 ||
                              std::strncmp(api, "sync", 4) == 0);
        auto step = [&] { touch_payload(host, n, ++d4_seq); body(); };
        if (ramping) warm_until_stable(step, ghz);
        else for (int i = 0; i < WARMUP_REPS; i++) step();
        for (int i = 0; i < reps; i++)
        {
            touch_payload(host, n, ++d4_seq);       // outside the timed region
            uint64_t t0 = timing::start();
            body();
            uint64_t t1 = stop_fn();
            samples.push_back(timing::to_us(t1 - t0, ghz));
        }
        auto s = Stats::from(samples);
        return std::make_pair(s.p50, s.min);
    };

    auto emit = [&](size_t n, const char* api, const char* dir,
                    double p50, double mn, double base_p50)
    {
        double ratio = (base_p50 > 0) ? (p50 / base_p50) : 0.0;
        printf("%zu,%s,%s,%.4f,%.4f,%.4f,%.4f,%.4f\n",
               n, dir, api, p50, mn, p50, gb_per_sec(n, p50), ratio);
    };

    for (size_t n : sizes)
    {
        // ── H→D direction ────────────────────────────────────────────────────
        // Baseline first: copyToDevice
        auto [base_h2d, base_h2d_std] = measure(n, "copyToDevice", "H2D",
            [&] { sink = chkr(ctx->copyToDevice(devCxl, host, n)); });
        emit(n, "copyToDevice", "H2D", base_h2d, base_h2d_std, base_h2d);

        if (!g_no_direct) {
        auto [mc_h, mc_h_std] = measure(n, "direct_memcpy", "H2D",
            [&] { std::memcpy(devCxl, host, n); pxl::flushHostCache(devCxl, n); });
        emit(n, "direct_memcpy", "H2D", mc_h, mc_h_std, base_h2d);
        }

        if (!g_no_direct) {
        auto [wd_h, wd_h_std] = measure(n, "direct_word", "H2D",
            [&] { word_copy(devCxl, host, n); pxl::flushHostCache(devCxl, n); });
        emit(n, "direct_word", "H2D", wd_h, wd_h_std, base_h2d);
        }

        if (!g_no_direct) {
        auto [nt_h, nt_h_std] = measure(n, "direct_ntstore", "H2D",
            [&] { nt_store_copy(devCxl, host, n); pxl::flushHostCache(devCxl, n); });
        emit(n, "direct_ntstore", "H2D", nt_h, nt_h_std, base_h2d);
        auto [ntn_h, ntn_h_std] = measure(n, "direct_ntstore_noflush", "H2D",
            [&] { nt_store_copy(devCxl, host, n); });
        emit(n, "direct_ntstore_noflush", "H2D", ntn_h, ntn_h_std, base_h2d);
        }

        // IoMem allocated fresh per-size; skip if it fails for this n.
        void* devIo_h = g_keep_iomem ? ctx->ioMemAlloc(n) : nullptr;
        if (devIo_h)
        {
            auto [sy_h, sy_h_std] = measure(n, "syncToDevice", "H2D",
                [&] { sink = chkr(ctx->syncToDevice(devIo_h, n)); });
            emit(n, "syncToDevice", "H2D", sy_h, sy_h_std, base_h2d);
            ctx->memFree(devIo_h);
        }
        else
        {
            printf("# D4: ioMemAlloc(%zu) failed; skipping syncToDevice row\n", n);
        }

        // ── D→H direction ────────────────────────────────────────────────────
        auto [base_d2h, base_d2h_std] = measure(n, "copyFromDevice", "D2H",
            [&] { sink = chkr(ctx->copyFromDevice(host, devCxl, n)); });
        emit(n, "copyFromDevice", "D2H", base_d2h, base_d2h_std, base_d2h);

        if (!g_no_direct) {
        auto [mc_d, mc_d_std] = measure(n, "direct_memcpy", "D2H",
            [&] { pxl::flushHostCache(devCxl, n); std::memcpy(host, devCxl, n); });
        emit(n, "direct_memcpy", "D2H", mc_d, mc_d_std, base_d2h);
        }

        if (!g_no_direct) {
        auto [wd_d, wd_d_std] = measure(n, "direct_word", "D2H",
            [&] { pxl::flushHostCache(devCxl, n); word_copy(host, devCxl, n); });
        emit(n, "direct_word", "D2H", wd_d, wd_d_std, base_d2h);
        }

        void* devIo_d = g_keep_iomem ? ctx->ioMemAlloc(n) : nullptr;
        if (devIo_d)
        {
            auto [sy_d, sy_d_std] = measure(n, "syncFromDevice", "D2H",
                [&] { sink = chkr(ctx->syncFromDevice(devIo_d, n)); });
            emit(n, "syncFromDevice", "D2H", sy_d, sy_d_std, base_d2h);
            ctx->memFree(devIo_d);
        }
        else
        {
            printf("# D4: ioMemAlloc(%zu) failed; skipping syncFromDevice row\n", n);
        }
    }

    ctx->memFree(devCxl);
    std::free(host);
    pxl::runtime::destroyContext(ctx);
}

// ── D5: repeat_call_overhead ─────────────────────────────────────────────────
//
// When you issue N back-to-back transfers of the same size on the same buffer,
// does the SDK / driver batch or amortize anything?
//
//   per_call_us = total_us(N consecutive calls) / N
//
// If per_call_us is flat across N, every call pays the full API overhead
// independently — no batching.  If per_call_us drops as N grows, the SDK is
// fusing or pipelining calls, and a many-small-transfer workload is cheaper
// than D2's per-call floor would suggest.

static void d5_repeat_call_overhead(double ghz)
{
    printf("\n# TEST: D5_repeat_call_overhead\n");
    printf("# Issues N back-to-back copyToDevice / syncToDevice calls in one bracket.\n");
    printf("# per_call_us = total / N.  Flat curve → no batching; falling curve → batching.\n");
    printf("api,size_bytes,burst_n,total_us_mean,total_us_std,per_call_us,reps\n");

    const std::vector<size_t> sizes = {
        64,                    // latency-bound
        4    * 1024,           // page
        64   * 1024,           // 64 KB
    };
    const std::vector<int> bursts = {1, 2, 4, 8, 16, 32, 64, 128};

    auto* ctx = pxl::runtime::createContext(0);
    if (!ctx) { fprintf(stderr, "D5: createContext failed\n"); return; }

    const size_t maxSize = sizes.back();
    void* devCxl = ctx->memAlloc(maxSize);
    void* host   = std::malloc(maxSize);
    if (!devCxl || !host)
    {
        fprintf(stderr, "D5: CxlMem/host allocation failed\n");
        if (devCxl) ctx->memFree(devCxl);
        std::free(host);
        pxl::runtime::destroyContext(ctx);
        return;
    }
    std::memset(host, 0x77, maxSize);

    // IoMem may be unavailable (zero free reported at session start in some
    // runs).  Try once at maxSize; if it succeeds we test syncToDevice too,
    // otherwise we just report copyToDevice burst behavior.
    void* devIo = ctx->ioMemAlloc(maxSize);
    if (!devIo)
        printf("# D5: IoMem unavailable for this session — copyToDevice rows only\n");

    auto run = [&](const char* api, size_t n, int burst,
                   std::vector<double>& samples,
                   auto&& single_call)
    {
        const int reps = std::max(20, reps_for_size(n) / std::max(1, burst / 4));
        samples.clear();
        samples.reserve(reps);
        for (int i = 0; i < WARMUP_REPS; i++)
            for (int j = 0; j < burst; j++) single_call();
        for (int i = 0; i < reps; i++)
        {
            uint64_t t0 = timing::start();
            for (int j = 0; j < burst; j++) single_call();
            uint64_t t1 = stop_fn();
            samples.push_back(timing::to_us(t1 - t0, ghz));
        }
        auto s = Stats::from(samples);
        printf("%s,%zu,%d,%.4f,%.4f,%.4f,%d\n",
               api, n, burst, s.mean, s.stddev, s.mean / burst, reps);
    };

    std::vector<double> samples;
    for (size_t n : sizes)
    {
        for (int burst : bursts)
        {
            run("copyToDevice", n, burst, samples,
                [&] { sink = ctx->copyToDevice(devCxl, host, n); });
            if (devIo)
                run("syncToDevice", n, burst, samples,
                    [&] { sink = ctx->syncToDevice(devIo, n); });
        }
    }

    ctx->memFree(devCxl);
    if (devIo) ctx->memFree(devIo);
    std::free(host);
    pxl::runtime::destroyContext(ctx);
}

// ── D6: numa_placement ───────────────────────────────────────────────────────
//
// The CXL device sits on NUMA node 0 (`lspci -v` reports `Physical Slot 1`,
// which is in node 0's IIO domain).  This box has two NUMA nodes:
//
//   node 0  CPU 0..127     ← device-local
//   node 1  CPU 128..255   ← remote via UPI (lscpu distance 21:10 ≈ 2.1×)
//
// We measure the same set of (api, direction, size) tuples twice: once with
// the running thread pinned to CPU 0 (local) and once to CPU 128 (remote).
// The delta is the UPI cost on the data path.
//
// Note on memory placement: the host buffer is allocated once in main() while
// the process is still pinned to CPU 0, so its pages live on node 0 under
// Linux's first-touch policy.  The "remote" test therefore traverses UPI for
// every host-buffer access in addition to whatever device-path UPI cost
// exists.  A future variant (D5b) could allocate a node-1-local host buffer
// via libnuma and compare; for now this measures the common case (thread
// migrates, memory stays).

static const int NUMA0_CPU = 0;
static const int NUMA1_CPU = 128;

static bool pin_cpu(int cpu)
{
    cpu_set_t cs;
    CPU_ZERO(&cs);
    CPU_SET(cpu, &cs);
    return sched_setaffinity(0, sizeof(cs), &cs) == 0;
}

static void d6_numa_placement(double ghz)
{
    printf("\n# TEST: D6_numa_placement\n");
    printf("# Device on NUMA 0 (CPU %d-127); remote test pins to CPU %d (NUMA 1).\n",
           NUMA0_CPU, NUMA1_CPU);
    printf("# Host buffer first-touched on NUMA 0, so remote calls also traverse UPI\n");
    printf("# for host-buffer accesses (typical 'thread migrates, memory stays' case).\n");
    printf("api,direction,size_bytes,local_us_mean,local_us_std,remote_us_mean,remote_us_std,ratio_remote_over_local,reps\n");

    const std::vector<size_t> sizes = {
        64,
        4    * 1024,
        64   * 1024,
        1    * 1024 * 1024,
        16   * 1024 * 1024,
        64   * 1024 * 1024,
    };

    auto* ctx = pxl::runtime::createContext(0);
    if (!ctx) { fprintf(stderr, "D5: createContext failed\n"); return; }

    const size_t maxSize = sizes.back();
    void* devCxl = ctx->memAlloc(maxSize);
    void* host   = std::malloc(maxSize);
    if (!devCxl || !host)
    {
        fprintf(stderr, "D5: allocation failed\n");
        if (devCxl) ctx->memFree(devCxl);
        std::free(host);
        pxl::runtime::destroyContext(ctx);
        return;
    }
    // First-touch on NUMA 0 (main is currently pinned there).
    std::memset(host, 0x33, maxSize);

    auto sweep_on_cpu = [&](int cpu, const char* api, const char* dir,
                            size_t n, std::vector<double>& samples,
                            auto&& body)
    {
        if (!pin_cpu(cpu))
        {
            fprintf(stderr, "D5: failed to pin to CPU %d\n", cpu);
            samples.clear();
            return;
        }
        const int reps = reps_for_size(n);
        samples.clear();
        samples.reserve(reps);
        for (int i = 0; i < WARMUP_REPS; i++) body();
        for (int i = 0; i < reps; i++)
        {
            uint64_t t0 = timing::start();
            body();
            uint64_t t1 = stop_fn();
            samples.push_back(timing::to_us(t1 - t0, ghz));
        }
    };

    std::vector<double> local_samples, remote_samples;

    for (size_t n : sizes)
    {
        // H→D via copyToDevice
        sweep_on_cpu(NUMA0_CPU, "copyToDevice", "H2D", n, local_samples,
                     [&] { sink = ctx->copyToDevice(devCxl, host, n); });
        sweep_on_cpu(NUMA1_CPU, "copyToDevice", "H2D", n, remote_samples,
                     [&] { sink = ctx->copyToDevice(devCxl, host, n); });
        if (!local_samples.empty() && !remote_samples.empty())
        {
            auto l = Stats::from(local_samples);
            auto r = Stats::from(remote_samples);
            double ratio = (l.mean > 0) ? (r.mean / l.mean) : 0.0;
            printf("copyToDevice,H2D,%zu,%.4f,%.4f,%.4f,%.4f,%.4f,%d\n",
                   n, l.mean, l.stddev, r.mean, r.stddev, ratio,
                   reps_for_size(n));
        }

        // D→H via copyFromDevice
        sweep_on_cpu(NUMA0_CPU, "copyFromDevice", "D2H", n, local_samples,
                     [&] { sink = ctx->copyFromDevice(host, devCxl, n); });
        sweep_on_cpu(NUMA1_CPU, "copyFromDevice", "D2H", n, remote_samples,
                     [&] { sink = ctx->copyFromDevice(host, devCxl, n); });
        if (!local_samples.empty() && !remote_samples.empty())
        {
            auto l = Stats::from(local_samples);
            auto r = Stats::from(remote_samples);
            double ratio = (l.mean > 0) ? (r.mean / l.mean) : 0.0;
            printf("copyFromDevice,D2H,%zu,%.4f,%.4f,%.4f,%.4f,%.4f,%d\n",
                   n, l.mean, l.stddev, r.mean, r.stddev, ratio,
                   reps_for_size(n));
        }
    }

    // Restore main thread to CPU 0 so any subsequent tests run device-local.
    pin_cpu(NUMA0_CPU);

    ctx->memFree(devCxl);
    std::free(host);
    pxl::runtime::destroyContext(ctx);
}

// ── D7: concurrent_bw ────────────────────────────────────────────────────────
//
// N parallel host threads each run a steady-state H→D copyToDevice loop on
// their own context and their own host+device buffers (no contention by
// construction).  All threads pin to distinct CPUs on NUMA 0 (CPUs 0..N-1)
// so the device's host-side path is local for every thread.
//
// Aggregate GB/s = (N × size × reps) / max(per-thread wall time).
// Per-thread GB/s reveals when threads start fighting for the same device
// resource: at the saturation knee, per-thread GB/s falls off but aggregate
// GB/s plateaus.
//
// A hand-rolled start barrier (acquire/release on std::atomic) ensures we
// don't measure thread-spawn skew.  Each thread does its own warmup, then
// signals readiness; the main thread releases the barrier once all are in.

static void d7_concurrent_bw(double ghz)
{
    printf("\n# TEST: D7_concurrent_bw\n");
    printf("# N host threads each run steady-state copyToDevice (H2D).\n");
    printf("# Per-thread context + per-thread host/device buffers, pinned to\n");
    printf("# CPUs 0..N-1 on NUMA 0.  aggregate_gb_per_sec = total_bytes / max(wall).\n");
    printf("api,direction,size_bytes,n_threads,per_thread_us_mean,per_thread_gb_per_sec,aggregate_gb_per_sec,reps_per_thread\n");

    const std::vector<size_t> sizes = {
        64   * 1024,
        1    * 1024 * 1024,
        16   * 1024 * 1024,
    };
    const std::vector<int> thread_counts = { 1, 2, 4, 8, 16, 32 };

    for (size_t n : sizes)
    {
        for (int N : thread_counts)
        {
            const int reps = reps_for_size(n);

            std::vector<pxl::runtime::Context*> ctxs(N, nullptr);
            std::vector<void*> dev(N, nullptr), host(N, nullptr);

            auto cleanup = [&]()
            {
                for (int i = 0; i < N; i++)
                {
                    if (host[i]) std::free(host[i]);
                    if (dev[i] && ctxs[i]) ctxs[i]->memFree(dev[i]);
                    if (ctxs[i]) pxl::runtime::destroyContext(ctxs[i]);
                }
            };

            bool ok = true;
            for (int i = 0; i < N; i++)
            {
                ctxs[i] = pxl::runtime::createContext(0);
                if (!ctxs[i]) { ok = false; break; }
                dev[i]  = ctxs[i]->memAlloc(n);
                host[i] = std::malloc(n);
                if (!dev[i] || !host[i]) { ok = false; break; }
                // First-touch on the spawning thread (currently CPU 0, NUMA 0).
                // The worker will re-pin to CPU=tid before timing, so per-thread
                // host buffer pages already live on NUMA 0 by construction.
                std::memset(host[i], 0x33 + (i & 0x7F), n);
            }
            if (!ok)
            {
                fprintf(stderr, "# D7: allocation failed at size=%zu, N=%d\n", n, N);
                cleanup();
                continue;
            }

            std::atomic<int>  ready{0};
            std::atomic<bool> go{false};
            std::vector<double> per_thread_total_us(N, 0.0);
            std::vector<double> per_thread_mean_us(N, 0.0);

            auto worker = [&](int tid)
            {
                pin_cpu(tid);  // CPUs 0..N-1 are all on NUMA 0 (NUMA 0 = CPU 0..127)

                for (int r = 0; r < WARMUP_REPS; r++)
                    sink = ctxs[tid]->copyToDevice(dev[tid], host[tid], n);

                ready.fetch_add(1, std::memory_order_acq_rel);
                while (!go.load(std::memory_order_acquire)) { /* spin */ }

                uint64_t t0 = timing::start();
                for (int r = 0; r < reps; r++)
                    sink = ctxs[tid]->copyToDevice(dev[tid], host[tid], n);
                uint64_t t1 = stop_fn();

                double total = timing::to_us(t1 - t0, ghz);
                per_thread_total_us[tid] = total;
                per_thread_mean_us[tid]  = total / reps;
            };

            std::vector<std::thread> threads;
            threads.reserve(N);
            for (int i = 0; i < N; i++) threads.emplace_back(worker, i);

            while (ready.load(std::memory_order_acquire) < N) { /* spin */ }
            go.store(true, std::memory_order_release);

            for (auto& t : threads) t.join();

            double max_total_us = 0.0;
            double sum_mean_us  = 0.0;
            for (int i = 0; i < N; i++)
            {
                if (per_thread_total_us[i] > max_total_us)
                    max_total_us = per_thread_total_us[i];
                sum_mean_us += per_thread_mean_us[i];
            }
            const double mean_mean_us     = sum_mean_us / N;
            const double per_thread_gbps  = gb_per_sec(n, mean_mean_us);
            const double total_gib        = static_cast<double>(n) * reps * N
                                            / (1024.0 * 1024.0 * 1024.0);
            const double aggregate_gbps   = (max_total_us > 0)
                                            ? total_gib / (max_total_us * 1e-6)
                                            : 0.0;

            printf("copyToDevice,H2D,%zu,%d,%.4f,%.4f,%.4f,%d\n",
                   n, N, mean_mean_us, per_thread_gbps, aggregate_gbps, reps);

            cleanup();
        }
    }

    pin_cpu(NUMA0_CPU);  // restore main thread to CPU 0
}

// ── D8: alloc_cost ───────────────────────────────────────────────────────────
//
// Background — what each allocator actually backs (see the paper's
// CxlMem-vs-IoMem section for the full derivation):
//
//   memAlloc    → device DRAM exposed via the CXL.mem coherent protocol
//                 (cxl_pci + devdax, /dev/cxl/mem0).  Host load/store is
//                 hardware-coherent with the device; pxl::flushHostCache
//                 orders the visibility window.  Capacity on this server
//                 is ≈ 233 GiB — the full installed device DRAM.
//
//   ioMemAlloc  → HOST RAM kernel-pinned for DMA, allocated by mx_dma
//                 via the Linux dma_pool_* / dmam_alloc_attrs APIs
//                 (confirmed by nm/strings on mx_dma.ko).  The region is
//                 host-mapped (CPU stores directly) AND DMA-accessible
//                 (device reads/writes through PCIe); syncToDevice and
//                 syncFromDevice flush/invalidate the non-coherent
//                 mapping.  Capacity (≈ 2.2 GiB) is a driver-imposed
//                 pinned-host-RAM budget, not a hardware constraint
//                 (host has 503 GiB total RAM, < 2 MiB of PCIe BAR).
//
//   Bottom line: the two pools aren't different slices of the same
//   DRAM.  CxlMem allocations land on the device; ioMemAlloc carves out
//   pre-pinned host RAM whose physical address is shared with the
//   device.  This explains every behavioral asymmetry we measure here:
//   pool size, mapping eagerness, sync semantics, and the 60-second
//   zombie_grace_ms reclamation window (mx_dma must drain in-flight DMA
//   descriptors before returning pinned host pages to the kernel).
//
// What this test measures, per (api, size):
//
//   alloc_first_us    one-shot cost of the first alloc(size) this
//                     context has ever served.  Cold-by-size for later
//                     rows; truly cold only for the first row of the
//                     api sweep.
//
//   alloc_steady_us   mean cost of subsequent alloc→free→alloc pairs.
//                     Measures the allocator's free-list fast path.
//                     If steady ≪ first, the allocator is amortizing a
//                     real per-call setup.
//
//   first_touch_us    after a fresh alloc, the cost of writing 1 byte
//                     per page (page-stride) and per cache line
//                     (line-stride), measured on two distinct
//                     allocations so the page-stride touch can't
//                     pre-fault pages the line-stride measurement would
//                     have caught.
//
//                     Predictions, from the backing-storage analysis
//                     above:
//                       memAlloc   — page-stride exposes CXL.mem first-
//                                    access cost (TLB fill + line
//                                    install + any lazy-mapping page
//                                    fault).
//                       ioMemAlloc — should be ≈ 0; the dma_pool
//                                    region is fully resident and host-
//                                    mapped at alloc time, so no page
//                                    faults and only an L1/L2 store
//                                    cost per access.
//
// One context is created per api (memAlloc, ioMemAlloc), reused across
// the size sweep.  This keeps daemon lifecycle churn minimal — context
// creation/destroy proved fragile on this server — at the cost that
// only the first size of each sweep sees a truly cold allocator.
//
// IoMem is capped at IOMEM_MAX (256 MB) because every outstanding
// ioMemAlloc pins host RAM that mx_dma only releases after the 60 s
// zombie_grace_ms window.  reps × size near the 2.2 GiB pool ceiling
// would exhaust the pool inside one row; we budget reps to keep the
// product ≲ ½ pool, and before each first_touch alloc we additionally
// gate on availableIoMemSize() so a near-empty pool is reported via
// a skip comment rather than a SIGBUS.

static const std::vector<size_t> D8_SIZES = {
    4    * 1024,                // 4 KB  (page)
    16   * 1024,
    64   * 1024,
    256  * 1024,
    1    * 1024 * 1024,         // 1 MB
    4    * 1024 * 1024,
    16   * 1024 * 1024,         // 16 MB
    64   * 1024 * 1024,
    256  * 1024 * 1024,         // 256 MB  (IoMem cap)
    1024UL * 1024 * 1024,       // 1 GB  (CxlMem only)
};

// Number of alloc-free pairs for the steady-state mean.  The CxlMem pool is
// 250 GiB so we can afford generous reps even at multi-hundred-MiB sizes.
static int alloc_reps_mem(size_t bytes)
{
    if (bytes <=    64 * 1024)        return 500;
    if (bytes <=    1 * 1024 * 1024)  return 200;
    if (bytes <=   16 * 1024 * 1024)  return 100;
    if (bytes <=  256 * 1024 * 1024)  return  30;
    return 10;                                       // ≥ 1 GB
}

// IoMem reps are tighter because the driver applies a ~60 s zombie-grace
// period (mx_dma module parameter zombie_grace_ms=60000): memFree returns
// immediately but the underlying allocation is not reclaimed to the pool
// until the grace timer elapses.  With a ~2.2 GiB IoMem pool, a naive
// 100-reps × 256 MiB sweep would queue ~25 GiB of outstanding allocations
// and exhaust the pool inside one row.  We budget reps so reps × size ≲
// ½ pool ≈ 1 GiB, keeping the test self-contained without relying on a
// daemon restart between sizes.
static int alloc_reps_io(size_t bytes)
{
    if (bytes <=    64 * 1024)        return 100;
    if (bytes <=  256 * 1024)         return 100;
    if (bytes <=    4 * 1024 * 1024)  return  50;
    if (bytes <=   16 * 1024 * 1024)  return  30;
    if (bytes <=   64 * 1024 * 1024)  return  10;
    return  4;                                       // up to IOMEM_MAX (256 MB)
}

// Touch one byte per `stride` bytes across the buffer; return microseconds.
static double touch_strided(void* p, size_t n, size_t stride, double ghz)
{
    volatile uint8_t* b = static_cast<volatile uint8_t*>(p);
    uint64_t t0 = timing::start();
    for (size_t off = 0; off < n; off += stride)
        b[off] = static_cast<uint8_t>(off);
    uint64_t t1 = stop_fn();
    return timing::to_us(t1 - t0, ghz);
}

static void d8_alloc_cost(double ghz)
{
    printf("\n# TEST: D8_alloc_cost\n");
    printf("# Per-api fresh context (one for memAlloc, one for ioMemAlloc).\n");
    printf("# alloc_first_us is truly cold only for the first size in each sweep;\n");
    printf("# subsequent rows reflect allocator-warm but size-cold state.\n");
    printf("# first_touch_* measured on a separate alloc per stride (no contamination).\n");
    printf("# IoMem sweep capped at %zu bytes; see file header for rationale.\n", IOMEM_MAX);
    printf("api,size_bytes,alloc_first_us,alloc_steady_mean_us,alloc_steady_std_us,"
           "first_touch_page_us,first_touch_page_us_per_page,"
           "first_touch_line_us,first_touch_line_us_per_line,reps\n");

    struct ApiSpec
    {
        const char* name;
        bool        is_io;
        size_t      max_size;
    };
    const ApiSpec apis[] = {
        { "memAlloc",   false, std::numeric_limits<size_t>::max() },
        { "ioMemAlloc", true,  IOMEM_MAX                          },
    };

    for (const auto& api : apis)
    {
        auto* ctx = pxl::runtime::createContext(0);
        if (!ctx)
        {
            fprintf(stderr, "# D8: createContext failed for %s sweep\n", api.name);
            continue;
        }

        auto do_alloc = [&](size_t n) -> void* {
            return api.is_io ? ctx->ioMemAlloc(n) : ctx->memAlloc(n);
        };

        for (size_t n : D8_SIZES)
        {
            if (n > api.max_size)
            {
                printf("# D8: %s skipping size=%zu (above api cap)\n", api.name, n);
                fflush(stdout);
                continue;
            }

            // (1) alloc_first: one cold-for-this-size alloc.
            uint64_t t0 = timing::start();
            void* p = do_alloc(n);
            uint64_t t1 = stop_fn();
            if (!p)
            {
                printf("# D8: %s(%zu) failed; skipping row\n", api.name, n);
                fflush(stdout);
                continue;
            }
            const double alloc_first_us = timing::to_us(t1 - t0, ghz);
            ctx->memFree(p);

            // (2) alloc_steady: alloc → free pairs, timing only the alloc.
            const int reps = api.is_io ? alloc_reps_io(n) : alloc_reps_mem(n);
            std::vector<double> samples;
            samples.reserve(reps);
            for (int r = 0; r < reps; r++)
            {
                uint64_t a0 = timing::start();
                void* q = do_alloc(n);
                uint64_t a1 = stop_fn();
                if (!q) break;
                samples.push_back(timing::to_us(a1 - a0, ghz));
                ctx->memFree(q);
            }
            if (samples.empty())
            {
                printf("# D8: %s(%zu) steady-state alloc failed; skipping row\n",
                       api.name, n);
                fflush(stdout);
                continue;
            }
            auto st = Stats::from(samples);

            // (3) first_touch: measured for both apis.
            //   memAlloc   — exposes CXL.mem first-access cost.
            //   ioMemAlloc — expected ≈ 0 (host-pinned region, already
            //                resident and mapped at alloc time).
            // Use two distinct allocations so the page-stride touch can't
            // pre-fault pages the line-stride measurement would have caught.
            // For IoMem, gate on availableIoMemSize() to avoid the silent-
            // mmap-fail-then-SIGBUS-on-touch failure mode when the pinned
            // pool is near exhaustion (zombie_grace_ms = 60 s keeps freed
            // allocations outstanding from the pool's perspective).
            double ft_page_us = std::nan("");
            double ft_line_us = std::nan("");
            double ft_page_per = std::nan("");
            double ft_line_per = std::nan("");

            bool can_first_touch = true;
            if (api.is_io)
            {
                const uint64_t avail = ctx->availableIoMemSize();
                if (avail < 2 * static_cast<uint64_t>(n))
                {
                    printf("# D8: %s(%zu) skipping first_touch — availableIoMem=%lu "
                           "< 2*size (pool draining through zombie_grace_ms)\n",
                           api.name, n, avail);
                    fflush(stdout);
                    can_first_touch = false;
                }
            }

            if (can_first_touch)
            {
                void* p_pg = do_alloc(n);
                if (p_pg)
                {
                    ft_page_us = touch_strided(p_pg, n, 4096, ghz);
                    ctx->memFree(p_pg);
                    const size_t n_pages = (n + 4095) / 4096;
                    if (n_pages) ft_page_per = ft_page_us / n_pages;
                }
                void* p_ln = do_alloc(n);
                if (p_ln)
                {
                    ft_line_us = touch_strided(p_ln, n, 64, ghz);
                    ctx->memFree(p_ln);
                    const size_t n_lines = (n + 63) / 64;
                    if (n_lines) ft_line_per = ft_line_us / n_lines;
                }
            }

            printf("%s,%zu,%.4f,%.4f,%.4f,%.4f,%.6f,%.4f,%.6f,%d\n",
                   api.name, n,
                   alloc_first_us, st.mean, st.stddev,
                   ft_page_us, ft_page_per,
                   ft_line_us, ft_line_per,
                   static_cast<int>(samples.size()));
            fflush(stdout);
        }

        pxl::runtime::destroyContext(ctx);
    }
}

// ── D9: free_cost ────────────────────────────────────────────────────────────
//
// Times ctx->memFree() in two regimes per (api, size):
//
//   free_untouched   alloc → free immediately, no host access between.
//                    Measures the pure "release-to-allocator" cost.
//
//   free_touched     alloc → 1-byte write per 4 KiB page (page-stride
//                    touch) → free.  The page-touch installs mappings
//                    the free may have to tear down; if free_touched >
//                    free_untouched, the SDK is doing per-page cleanup
//                    on free.
//
// Background on the IoMem rows (see D8 commentary for the storage
// derivation): user-visible free on IoMem is expected to be near-
// instant in both regimes because mx_dma defers the actual page
// reclamation to a kernel thread (zombie_grace_ms = 60 s).  D9
// therefore measures the synchronous tail of free, not the total
// resource-release cost.  Reporting both columns makes this concrete:
// IoMem free_touched ≈ free_untouched ≈ a few µs is the expected
// signature; CxlMem may show free_touched > free_untouched if
// devdax-backed mappings are torn down synchronously on free.
//
// Pre-flight: before the IoMem sweep, we query availableIoMemSize().
// If the pool is below half capacity we skip the IoMem half entirely
// rather than running degraded measurements or crashing on a silent
// alloc failure.  The skip line tells the user exactly which command
// to run (systemctl restart pxl_resourced.service) to recover.
//
// Note on cumulative pool consumption: D9 issues 2 × reps allocations
// per IoMem row (one rep set per regime), each held until completion.
// Combined with D8 running before it in the same invocation, the pool
// will typically be exhausted by the time D9 starts.  This is the
// design intent — the test is meant to be run after a fresh daemon
// restart, and the pre-flight guard makes the failure mode loud and
// actionable rather than crashy.

static void d9_free_cost(double ghz)
{
    printf("\n# TEST: D9_free_cost\n");
    printf("# free_untouched: alloc -> immediate free, no host access between.\n");
    printf("# free_touched:   alloc -> 1-byte write per 4 KiB page -> free.\n");
    printf("# IoMem free is deferred by mx_dma (zombie_grace_ms=60s), so user-\n");
    printf("# visible IoMem free time is the synchronous tail only; the actual\n");
    printf("# page reclamation runs asynchronously in mx_zombie_cleanup_thd.\n");
    printf("# IoMem sweep capped at %zu bytes (see file header).\n", IOMEM_MAX);
    printf("api,size_bytes,free_untouched_mean_us,free_untouched_std_us,"
           "free_touched_mean_us,free_touched_std_us,reps\n");

    struct ApiSpec
    {
        const char* name;
        bool        is_io;
        size_t      max_size;
    };
    const ApiSpec apis[] = {
        { "memAlloc",   false, std::numeric_limits<size_t>::max() },
        { "ioMemAlloc", true,  IOMEM_MAX                          },
    };

    for (const auto& api : apis)
    {
        auto* ctx = pxl::runtime::createContext(0);
        if (!ctx)
        {
            fprintf(stderr, "# D9: createContext failed for %s sweep\n", api.name);
            continue;
        }

        // Pre-flight: refuse the IoMem half if the pool is unhealthy.
        if (api.is_io)
        {
            uint64_t capacity = 0;
            ctx->getAttribute(pxl::DeviceAttr::IoMemoryCapacity, capacity);
            const uint64_t avail = ctx->availableIoMemSize();
            if (capacity > 0 && avail < capacity / 2)
            {
                printf("# D9: ioMemAlloc available=%lu (capacity=%lu) below 50%% threshold;\n",
                       avail, capacity);
                printf("#     skipping IoMem sweep. Run:\n");
                printf("#       sudo systemctl restart pxl_resourced.service\n");
                printf("#     and re-run d9 to get IoMem rows.\n");
                fflush(stdout);
                pxl::runtime::destroyContext(ctx);
                continue;
            }
        }

        auto do_alloc = [&](size_t n) -> void* {
            return api.is_io ? ctx->ioMemAlloc(n) : ctx->memAlloc(n);
        };

        for (size_t n : D8_SIZES)
        {
            if (n > api.max_size)
            {
                printf("# D9: %s skipping size=%zu (above api cap)\n", api.name, n);
                fflush(stdout);
                continue;
            }

            const int reps = api.is_io ? alloc_reps_io(n) : alloc_reps_mem(n);

            // (1) free_untouched
            std::vector<double> u_samples;
            u_samples.reserve(reps);
            for (int r = 0; r < reps; r++)
            {
                void* p = do_alloc(n);
                if (!p) break;
                uint64_t t0 = timing::start();
                ctx->memFree(p);
                uint64_t t1 = stop_fn();
                u_samples.push_back(timing::to_us(t1 - t0, ghz));
            }

            // (2) free_touched — same alloc, page-stride 1-byte writes, then free.
            std::vector<double> t_samples;
            t_samples.reserve(reps);
            for (int r = 0; r < reps; r++)
            {
                void* p = do_alloc(n);
                if (!p) break;
                // Touch every page; discard the touch timing.
                (void)touch_strided(p, n, 4096, ghz);
                uint64_t t0 = timing::start();
                ctx->memFree(p);
                uint64_t t1 = stop_fn();
                t_samples.push_back(timing::to_us(t1 - t0, ghz));
            }

            if (u_samples.empty() || t_samples.empty())
            {
                printf("# D9: %s(%zu) measurement failed (alloc returned null); skipping\n",
                       api.name, n);
                fflush(stdout);
                continue;
            }

            const auto us = Stats::from(u_samples);
            const auto ts = Stats::from(t_samples);

            printf("%s,%zu,%.4f,%.4f,%.4f,%.4f,%d\n",
                   api.name, n,
                   us.mean, us.stddev,
                   ts.mean, ts.stddev,
                   static_cast<int>(std::min(u_samples.size(), t_samples.size())));
            fflush(stdout);
        }

        pxl::runtime::destroyContext(ctx);
    }
}

// ── D10: access_pattern ──────────────────────────────────────────────────────
//
// Stride sensitivity of direct host-pointer access to device memory.
// For each api (memAlloc, ioMemAlloc), and for each stride pattern,
// we walk a 64 MiB buffer doing one access per stride point, repeating
// walks until total time exceeds D10_MIN_WALK_US for stable timing.
//
// Stride patterns:
//   64        every cache line  → measures sequential bandwidth; the
//                                 hardware prefetcher will help.
//   4096      every 4 KiB page  → keeps working-set tiny (1 MiB at our
//                                 buffer size) but stresses the TLB
//                                 differently on the two paths:
//                                   CxlMem (2 MiB devdax mapping) — same
//                                     huge-page covers many strides, so
//                                     TLB stays cool.
//                                   IoMem (4 KiB dma_pool mapping) — each
//                                     access crosses a fresh TLB entry.
//   2097152   every 2 MiB        → forces a fresh huge-page TLB entry
//                                 (CxlMem) or 4 KiB entry (IoMem) per
//                                 access; defeats most prefetching.
//   random    cache-line-aligned permutation → defeats every prefetcher
//                                 and hides per-line latency; the
//                                 difference between random and 64-stride
//                                 is the prefetcher's contribution.
//
// Directions: read and write per stride.  Reads accumulate into a
// volatile sink to prevent dead-code elimination; writes store the
// loop index so the compiler cannot fold them.
//
// IoMem rows here serve as a *host-RAM baseline*: the dma_pool region
// is plain host DRAM (confirmed in D8) accessed through a 4 KiB-page
// mapping.  The interesting contrast is CxlMem's CXL.mem-coherent path
// (device DRAM, huge pages) vs IoMem's host DRAM through 4 KiB pages.
// Where the two diverge tells the data-movement-tax story most directly.
//
// Buffer size note: 64 MiB fits within LLC on this 256-core Xeon, so
// the small-stride (64, 4096) numbers characterize CACHED throughput.
// The 2 MiB-stride and random numbers naturally defeat caching because
// each access lands on a distinct cache line that has not been recently
// touched.  We document this rather than trying to size around it,
// because IOMEM_MAX caps the IoMem buffer at the same size we use for
// CxlMem for apples-to-apples comparison.
//
// Pre-flight for IoMem: require availableIoMemSize ≥ 2 × buffer; on
// failure skip with the canonical pxl_resourced restart instruction.

// Must exceed this host's 96 MiB L3 per socket, or a pointer walk measures
// cache instead of memory.  256 MiB = 2.7x L3.  Used by D10 and D12.
static const size_t   D10_BUFFER_BYTES = 256 * 1024 * 1024;  // 256 MiB
static const uint64_t D10_MIN_WALK_US  = 20000;              // 20 ms
static volatile uint64_t d10_sink = 0;
// D10 issues no copy calls, so copy_check cannot cover it.  A write walk on CXL
// memory can still land wrong bytes silently (see PLAN.md 3), so each write walk
// spot-checks what it stored.
static long g_d10_verified = 0;
static long g_d10_bad      = 0;

struct D10Result
{
    size_t ops_per_walk;
    size_t walks;
    double ns_per_op;
    double line_gb_per_sec;
};

// One inlined walk-and-time loop, parameterized at compile time on
// direction + access-pattern style to keep the inner loop tight.
template <bool IS_WRITE, bool IS_RANDOM>
static D10Result d10_walk(void* p, size_t n, size_t stride,
                          const size_t* offsets, size_t n_offsets,
                          double ghz)
{
    const size_t ops_per_walk = IS_RANDOM ? n_offsets
                                          : (n + stride - 1) / stride;
    volatile uint8_t* b = static_cast<volatile uint8_t*>(p);
    uint64_t sink_local = 0;
    size_t walks = 0;

    uint64_t t0 = timing::start();
    do
    {
        if constexpr (IS_RANDOM)
        {
            if constexpr (IS_WRITE)
                for (size_t i = 0; i < n_offsets; i++)
                    b[offsets[i]] = static_cast<uint8_t>(i);
            else
                for (size_t i = 0; i < n_offsets; i++)
                    sink_local += b[offsets[i]];
        }
        else
        {
            if constexpr (IS_WRITE)
                for (size_t off = 0; off < n; off += stride)
                    b[off] = static_cast<uint8_t>(off);
            else
                for (size_t off = 0; off < n; off += stride)
                    sink_local += b[off];
        }
        walks++;
    }
    while (timing::to_us(stop_fn() - t0, ghz) < D10_MIN_WALK_US);
    uint64_t t1 = stop_fn();

    d10_sink += sink_local;

    // Spot-check the writes OUTSIDE the timed region so verification costs
    // nothing.  The walk stored a value derived from the offset; re-derive it
    // and compare.  Sampled, not exhaustive: a full scan of a 1 GiB buffer
    // would dominate the test's runtime.
    if constexpr (IS_WRITE)
    {
        const size_t step = IS_RANDOM ? (n_offsets / 4096 + 1)
                                      : (ops_per_walk / 4096 + 1) * stride;
        if constexpr (IS_RANDOM)
        {
            for (size_t i = 0; i < n_offsets; i += step)
            {
                g_d10_verified++;
                if (b[offsets[i]] != static_cast<uint8_t>(i)) g_d10_bad++;
            }
        }
        else
        {
            for (size_t off = 0; off < n; off += step)
            {
                g_d10_verified++;
                if (b[off] != static_cast<uint8_t>(off)) g_d10_bad++;
            }
        }
    }

    const double total_us = timing::to_us(t1 - t0, ghz);
    const double total_ops = static_cast<double>(ops_per_walk) * walks;
    D10Result r;
    r.ops_per_walk     = ops_per_walk;
    r.walks            = walks;
    r.ns_per_op        = (total_us * 1000.0) / total_ops;
    r.line_gb_per_sec  = (total_ops * 64.0) / (total_us * 1e-6)
                         / (1024.0 * 1024.0 * 1024.0);
    return r;
}

static void d10_access_pattern(double ghz)
{
    printf("\n# TEST: D10_access_pattern\n");
    printf("# Direct host-pointer access at varying strides.\n");
    printf("# Baseline is host malloc + MADV_HUGEPAGE, NOT IoMem: the CXL mapping\n");
    printf("# reports KernelPageSize 2048 kB while IoMem maps host RAM in 4 KiB\n");
    printf("# pages, so the old IoMem comparison confounded memory type with page\n");
    printf("# size and understated the gap on random access.\n");
    printf("# Buffer sizes are chosen to EXCEED last-level cache: this CPU\n");
    printf("# (Xeon 6756E) has 96 MiB of L3 per socket, so an 8 or 64 MiB walk\n");
    printf("# never leaves cache and measures L3, not memory -- an earlier version\n");
    printf("# of this test did exactly that and found CXL indistinguishable from\n");
    printf("# DRAM (within 4%%), which was a cache artifact.  Translation cost is\n");
    printf("# measured separately (device TLB: 8 entries x 1 GB, ~590 ns/miss), not\n");
    printf("# here: a TLB-resident buffer would have to be small enough to sit\n");
    printf("# inside L3, so the two effects cannot be separated by buffer size.\n");
    printf("api,buffer_bytes,l3_multiple,direction,stride_label,ops_per_walk,walks,ns_per_op,line_gb_per_sec\n");

    // Both sizes must exceed the 96 MiB L3 or the walk measures cache.
    // 256 MiB = 2.7x L3; 1 GiB = 10.7x L3 (also confirms the result is not
    // an artifact of partial cache residency at the smaller size).
    const size_t sizes[] = { 256u * 1024 * 1024, 1024ull * 1024 * 1024 };
    // The permutation must be rebuilt per buffer size: offsets are absolute
    // byte positions, so a 64 MiB permutation walked against an 8 MiB buffer
    // reads far past its end (this was a SIGSEGV).
    auto make_perm = [](size_t buf_bytes) {
        const size_t n = buf_bytes / 64;
        std::vector<size_t> off(n);
        for (size_t i = 0; i < n; i++) off[i] = i * 64;
        std::mt19937_64 rng(42);
        for (size_t i = n - 1; i > 0; i--)
        {
            std::uniform_int_distribution<size_t> dist(0, i);
            std::swap(off[i], off[dist(rng)]);
        }
        return off;
    };

    // host_hugepage = malloc + MADV_HUGEPAGE, matched to CXL's 2 MiB pages.
    struct ApiSpec { const char* name; bool is_io; bool is_host; };
    const ApiSpec apis[] = {
        { "memAlloc",       false, false },
        { "host_hugepage",  false, true  },
        { "ioMemAlloc",     true,  false },   // only with --keep-iomem
    };

    // Strides must touch enough DISTINCT cache lines to exceed L3, not merely
    // span a large buffer.  A 2 MiB stride touches one line per page, so its
    // working set is buffer/2MiB*64B -- 8 KiB at 256 MiB, 32 KiB at 1 GiB --
    // which is L1-resident after the first walk.  That arm measured cache on
    // BOTH memories and reported CXL/DRAM = 1.00x, a repeat of the bug this
    // test was just fixed for.  The retained strides touch 4.2M lines (64 B)
    // and 65K lines (4 KiB) at 256 MiB: 256 MiB and 4 MiB of distinct lines.
    const struct { const char* label; size_t stride; } seq_strides[] = {
        { "64",      64                     },
        { "4096",    4096                   },
    };

    for (const auto& api : apis)
    {
      if (api.is_io && !g_keep_iomem) continue;
      // Walking memAlloc/ioMemAlloc memory is a DIRECT host access: without
      // root the mapping silently fails and the first touch SIGBUSes. Only the
      // host_hugepage baseline is safe unprivileged.
      if (!api.is_host && g_no_direct) { printf("# D10: %s skipped (--no-direct)\n", api.name); continue; }
      for (size_t BUF : sizes)
      {
        // 96 MiB L3 per socket on this host.
        char tlb[16];
        snprintf(tlb, sizeof(tlb), "%.1fx",
                 (double)BUF / (96.0 * 1024 * 1024));
        const std::vector<size_t> random_offsets = make_perm(BUF);
        auto* ctx = pxl::runtime::createContext(0);
        if (!ctx)
        {
            fprintf(stderr, "# D10: createContext failed for %s sweep\n", api.name);
            continue;
        }

        if (api.is_io)
        {
            const uint64_t avail = ctx->availableIoMemSize();
            if (avail < 2 * BUF)
            {
                printf("# D10: ioMemAlloc available=%lu < 2*buffer (%zu); skipping IoMem.\n",
                       avail, 2 * BUF);
                printf("#       sudo systemctl restart pxl_resourced.service\n");
                fflush(stdout);
                pxl::runtime::destroyContext(ctx);
                continue;
            }
        }

        void* buf = nullptr;
        if (api.is_host)
        {
            if (posix_memalign(&buf, 2u * 1024 * 1024, BUF) != 0) buf = nullptr;
#ifdef MADV_HUGEPAGE
            if (buf) madvise(buf, BUF, MADV_HUGEPAGE);   // match CXL's 2 MiB pages
#endif
        }
        else buf = api.is_io ? ctx->ioMemAlloc(BUF) : ctx->memAlloc(BUF);
        if (!buf)
        {
            printf("# D10: %s(%zu) alloc failed; skipping\n", api.name, BUF);
            fflush(stdout);
            pxl::runtime::destroyContext(ctx);
            continue;
        }

        // Pre-touch every 4 KiB page so first-touch cost is amortized
        // out of the steady-state walk measurements.
        // Pre-touch on 2 MiB boundaries: that is the real page size on both
        // the CXL mapping and the hugepage baseline.
        for (size_t off = 0; off < BUF; off += 2u * 1024 * 1024)
            static_cast<volatile uint8_t*>(buf)[off] = 0xa5;

        auto emit = [&](const char* dir, const char* label, const D10Result& r) {
            printf("%s,%zu,%s,%s,%s,%zu,%zu,%.4f,%.4f\n",
                   api.name, BUF, tlb, dir, label,
                   r.ops_per_walk, r.walks, r.ns_per_op, r.line_gb_per_sec);
            fflush(stdout);
        };

        for (const auto& s : seq_strides)
        {
            emit("read",  s.label,
                 d10_walk<false, false>(buf, BUF, s.stride,
                                        nullptr, 0, ghz));
            emit("write", s.label,
                 d10_walk<true,  false>(buf, BUF, s.stride,
                                        nullptr, 0, ghz));
        }
        emit("read",  "random",
             d10_walk<false, true>(buf, BUF, 0,
                                   random_offsets.data(),
                                   random_offsets.size(), ghz));
        emit("write", "random",
             d10_walk<true,  true>(buf, BUF, 0,
                                   random_offsets.data(),
                                   random_offsets.size(), ghz));

        if (api.is_host) std::free(buf); else ctx->memFree(buf);
        pxl::runtime::destroyContext(ctx);
      }
    }
    // No copy_check here: D10 issues no copy calls, so it would always print
    // calls=0 and read like a failure.  What D10 *can* get wrong silently is
    // the data itself -- see the write-verify in the walk loop below.
    printf("# WALK_CHECK D10: %ld writes verified, %ld mismatched%s\n",
           g_d10_verified, g_d10_bad,
           g_d10_bad ? "  <-- DATA INVALID" : "");
    fflush(stdout);
    g_d10_verified = g_d10_bad = 0;
}

// ── D11: bidirectional ───────────────────────────────────────────────────────
//
// Quantifies duplex link utilization on CxlMem.  For each size we run
// three phases:
//
//   (1) H→D alone on CPU 0
//   (2) D→H alone on CPU 0
//   (3) Both directions concurrently — H→D on CPU 0, D→H on CPU 1.
//       Two threads, two contexts, two device buffers; barrier-
//       synchronized start so we measure steady-state duplex, not
//       thread-spawn skew.  Each thread reports its own sustained
//       throughput while the other is running.
//
// Headline metric: duplex_efficiency = (h2d_concurrent + d2h_concurrent)
//                                    / (h2d_alone + d2h_alone)
//
//   ≈ 1.0  → full-duplex (the two directions don't contend)
//   ≈ 0.5  → half-duplex (a single shared channel; concurrent run
//            costs both directions about half their solo throughput)
//   < 0.5  → active interference (concurrent allocations or DMA
//            queues fighting beyond a simple half-duplex split)
//
// CxlMem only — IoMem is omitted per the earlier scoping decision
// (IoMem here would measure host-RAM duplex, which is well-known
// territory and doesn't characterize the CXL link).

static void d11_bidirectional(double ghz)
{
    printf("\n# TEST: D11_bidirectional\n");
    printf("# CxlMem, two contexts.  Phase 1: copyToDevice alone (CPU 0).\n");
    printf("# Phase 2: copyFromDevice alone (CPU 0).  Phase 3: both threads\n");
    printf("# concurrent, H→D pinned to CPU 0 and D→H pinned to CPU 1, barrier-\n");
    printf("# synchronized start.  duplex_efficiency = (h2d_conc + d2h_conc)\n");
    printf("# / (h2d_alone + d2h_alone); ≈ 1.0 is full duplex.\n");
    printf("size_bytes,h2d_alone_gbps,d2h_alone_gbps,"
           "h2d_concurrent_gbps,d2h_concurrent_gbps,"
           "aggregate_gbps,duplex_efficiency,reps\n");

    const std::vector<size_t> sizes = {
        64   * 1024,
        1    * 1024 * 1024,
        16   * 1024 * 1024,
        64   * 1024 * 1024,
    };

    auto bytes_to_gbps = [](size_t bytes, double us) -> double {
        if (us <= 0) return 0.0;
        return static_cast<double>(bytes)
             / (1024.0 * 1024.0 * 1024.0)
             / (us * 1e-6);
    };

    for (size_t n : sizes)
    {
        const int reps = reps_for_size(n);

        auto* ctx_h = pxl::runtime::createContext(0);
        auto* ctx_d = pxl::runtime::createContext(0);
        if (!ctx_h || !ctx_d)
        {
            fprintf(stderr, "# D11: createContext failed at size=%zu\n", n);
            if (ctx_h) pxl::runtime::destroyContext(ctx_h);
            if (ctx_d) pxl::runtime::destroyContext(ctx_d);
            continue;
        }

        void* dev_h  = ctx_h->memAlloc(n);
        void* dev_d  = ctx_d->memAlloc(n);
        void* host_h = std::malloc(n);
        void* host_d = std::malloc(n);
        if (!dev_h || !dev_d || !host_h || !host_d)
        {
            fprintf(stderr, "# D11: allocation failed at size=%zu\n", n);
            if (dev_h)  ctx_h->memFree(dev_h);
            if (dev_d)  ctx_d->memFree(dev_d);
            std::free(host_h);
            std::free(host_d);
            pxl::runtime::destroyContext(ctx_h);
            pxl::runtime::destroyContext(ctx_d);
            continue;
        }
        std::memset(host_h, 0x55, n);
        std::memset(host_d, 0xaa, n);

        // ── Phase 1: H→D alone (CPU 0) ────────────────────────────────────────
        pin_cpu(NUMA0_CPU);
        for (int i = 0; i < WARMUP_REPS; i++)
            sink = ctx_h->copyToDevice(dev_h, host_h, n);
        uint64_t t0 = timing::start();
        for (int i = 0; i < reps; i++)
            sink = ctx_h->copyToDevice(dev_h, host_h, n);
        const double h2d_alone_us = timing::to_us(stop_fn() - t0, ghz);
        const double h2d_alone_gbps = bytes_to_gbps(static_cast<size_t>(n) * reps, h2d_alone_us);

        // ── Phase 2: D→H alone (CPU 0) ────────────────────────────────────────
        for (int i = 0; i < WARMUP_REPS; i++)
            sink = ctx_d->copyFromDevice(host_d, dev_d, n);
        t0 = timing::start();
        for (int i = 0; i < reps; i++)
            sink = ctx_d->copyFromDevice(host_d, dev_d, n);
        const double d2h_alone_us = timing::to_us(stop_fn() - t0, ghz);
        const double d2h_alone_gbps = bytes_to_gbps(static_cast<size_t>(n) * reps, d2h_alone_us);

        // ── Phase 3: both concurrent (CPU 0 + CPU 1, barrier-synchronized) ────
        std::atomic<int>  ready{0};
        std::atomic<bool> go{false};
        double h2d_conc_us = 0.0;
        double d2h_conc_us = 0.0;

        auto h_worker = [&]() {
            pin_cpu(0);
            for (int i = 0; i < WARMUP_REPS; i++)
                sink = ctx_h->copyToDevice(dev_h, host_h, n);
            ready.fetch_add(1, std::memory_order_acq_rel);
            while (!go.load(std::memory_order_acquire)) { /* spin */ }
            uint64_t s = timing::start();
            for (int i = 0; i < reps; i++)
                sink = ctx_h->copyToDevice(dev_h, host_h, n);
            h2d_conc_us = timing::to_us(stop_fn() - s, ghz);
        };
        auto d_worker = [&]() {
            pin_cpu(1);
            for (int i = 0; i < WARMUP_REPS; i++)
                sink = ctx_d->copyFromDevice(host_d, dev_d, n);
            ready.fetch_add(1, std::memory_order_acq_rel);
            while (!go.load(std::memory_order_acquire)) { /* spin */ }
            uint64_t s = timing::start();
            for (int i = 0; i < reps; i++)
                sink = ctx_d->copyFromDevice(host_d, dev_d, n);
            d2h_conc_us = timing::to_us(stop_fn() - s, ghz);
        };

        std::thread th1(h_worker);
        std::thread th2(d_worker);
        while (ready.load(std::memory_order_acquire) < 2) { /* spin */ }
        go.store(true, std::memory_order_release);
        th1.join();
        th2.join();

        const double h2d_conc_gbps = bytes_to_gbps(static_cast<size_t>(n) * reps, h2d_conc_us);
        const double d2h_conc_gbps = bytes_to_gbps(static_cast<size_t>(n) * reps, d2h_conc_us);
        const double aggregate_gbps = h2d_conc_gbps + d2h_conc_gbps;
        const double sum_alones     = h2d_alone_gbps + d2h_alone_gbps;
        const double duplex_eff     = (sum_alones > 0) ? aggregate_gbps / sum_alones : 0.0;

        printf("%zu,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%d\n",
               n, h2d_alone_gbps, d2h_alone_gbps,
               h2d_conc_gbps, d2h_conc_gbps,
               aggregate_gbps, duplex_eff, reps);
        fflush(stdout);

        ctx_h->memFree(dev_h);
        ctx_d->memFree(dev_d);
        std::free(host_h);
        std::free(host_d);
        pxl::runtime::destroyContext(ctx_h);
        pxl::runtime::destroyContext(ctx_d);
    }

    pin_cpu(NUMA0_CPU);  // restore main thread
}

// ── D12: rw_ratio ────────────────────────────────────────────────────────────
//
// Direct-pointer load:store ratio sweep on a CxlMem region.  Reframed
// from the original CXL-Bench "rw_ratio at the API level" — since the
// SDK's copy* APIs already split cleanly into H→D and D→H (covered by
// D11 with the duplex framing), the more revealing measurement is what
// happens to *direct host-pointer access* when the read/write mix
// changes.  That's what this test does.
//
// Pattern: same 64-byte-aligned random permutation as D10 (so cold-
// line access on every step), with the read/write decision at each
// step driven by a rhythmic (i & 3) < writes_per_4 pattern.  Five
// ratios sweep the spectrum:
//
//   100R / 0W   → should reproduce D10 random-read CxlMem (~32 ns/op)
//   75R / 25W
//   50R / 50W   → linear interpolation predicts ~50 ns/op; deviation
//                 from linear is the headline finding (positive or
//                 negative interaction between concurrent reads and
//                 writes on the same CXL.mem region)
//   25R / 75W
//   0R / 100W  → should reproduce D10 random-write CxlMem (~67 ns/op)
//
// The rhythmic pattern is deliberate: the branch predictor handles a
// fixed mix in steady state, so we measure mix cost rather than branch
// mispredict noise.  Documented in the file header.  CxlMem only —
// IoMem here would measure host-RAM rw asymmetry, which isn't the CXL
// data-movement story.

template <int WRITES_PER_4>
static D10Result d12_loop(void* p, const size_t* offsets, size_t n_offsets, double ghz)
{
    volatile uint8_t* b = static_cast<volatile uint8_t*>(p);
    uint64_t sink_local = 0;
    size_t walks = 0;

    uint64_t t0 = timing::start();
    do
    {
        if constexpr (WRITES_PER_4 == 0)
        {
            for (size_t i = 0; i < n_offsets; i++)
                sink_local += b[offsets[i]];
        }
        else if constexpr (WRITES_PER_4 == 4)
        {
            for (size_t i = 0; i < n_offsets; i++)
                b[offsets[i]] = static_cast<uint8_t>(i);
        }
        else
        {
            for (size_t i = 0; i < n_offsets; i++)
            {
                if ((i & 3) < WRITES_PER_4)
                    b[offsets[i]] = static_cast<uint8_t>(i);
                else
                    sink_local += b[offsets[i]];
            }
        }
        walks++;
    }
    while (timing::to_us(stop_fn() - t0, ghz) < D10_MIN_WALK_US);
    uint64_t t1 = stop_fn();

    d10_sink += sink_local;

    const double total_us  = timing::to_us(t1 - t0, ghz);
    const double total_ops = static_cast<double>(n_offsets) * walks;
    D10Result r;
    r.ops_per_walk    = n_offsets;
    r.walks           = walks;
    r.ns_per_op       = (total_us * 1000.0) / total_ops;
    r.line_gb_per_sec = (total_ops * 64.0) / (total_us * 1e-6)
                        / (1024.0 * 1024.0 * 1024.0);
    return r;
}

static void d12_rw_ratio(double ghz)
{
    printf("\n# TEST: D12_rw_ratio\n");
    printf("# Direct-pointer load:store ratio sweep on CxlMem (random offset).\n");
    printf("# Buffer = %zu bytes, pre-touched.  Same random permutation as D10.\n",
           D10_BUFFER_BYTES);
    printf("# Access type at step i: write iff (i & 3) < writes_per_4 (rhythmic).\n");
    printf("read_pct,write_pct,n_ops,walks,ns_per_op,line_gb_per_sec\n");

    // Same permutation as D10 (cache-line-aligned, fixed seed).
    const size_t n_offsets = D10_BUFFER_BYTES / 64;
    std::vector<size_t> offsets(n_offsets);
    for (size_t i = 0; i < n_offsets; i++) offsets[i] = i * 64;
    std::mt19937_64 rng(42);
    for (size_t i = n_offsets - 1; i > 0; i--)
    {
        std::uniform_int_distribution<size_t> dist(0, i);
        std::swap(offsets[i], offsets[dist(rng)]);
    }

    auto* ctx = pxl::runtime::createContext(0);
    if (!ctx) { fprintf(stderr, "# D12: createContext failed\n"); return; }

    void* buf = ctx->memAlloc(D10_BUFFER_BYTES);
    if (!buf)
    {
        fprintf(stderr, "# D12: memAlloc(%zu) failed\n", D10_BUFFER_BYTES);
        pxl::runtime::destroyContext(ctx);
        return;
    }

    for (size_t off = 0; off < D10_BUFFER_BYTES; off += 4096)
        static_cast<volatile uint8_t*>(buf)[off] = 0xa5;

    auto emit = [&](int read_pct, int write_pct, const D10Result& r) {
        printf("%d,%d,%zu,%zu,%.4f,%.4f\n",
               read_pct, write_pct,
               r.ops_per_walk, r.walks,
               r.ns_per_op, r.line_gb_per_sec);
        fflush(stdout);
    };

    emit(100,   0, d12_loop<0>(buf, offsets.data(), n_offsets, ghz));
    emit( 75,  25, d12_loop<1>(buf, offsets.data(), n_offsets, ghz));
    emit( 50,  50, d12_loop<2>(buf, offsets.data(), n_offsets, ghz));
    emit( 25,  75, d12_loop<3>(buf, offsets.data(), n_offsets, ghz));
    emit(  0, 100, d12_loop<4>(buf, offsets.data(), n_offsets, ghz));

    ctx->memFree(buf);
    pxl::runtime::destroyContext(ctx);
}

// ── main ──────────────────────────────────────────────────────────────────────

static const int NUM_TESTS = 12;

static void usage(const char* prog)
{
    fprintf(stderr,
        "Usage: %s [tests...]\n"
        "  Tests: d1 d2 ... d11  (default: all implemented)\n"
        "  d1  bandwidth_sweep    — GB/s vs size for each API path\n"
        "  d2  latency_per_call   — small-message floor (64 B per call, high reps)\n"
        "  d3  memtype_compare    — interleaved CxlMem vs IoMem A/B\n"
        "  d4  direct_vs_copy     — copyToDevice vs memcpy/word/ntstore direct paths\n"
        "  d5  repeat_call_over   — back-to-back same-call burst (batching test)\n"
        "  d6  numa_placement     — local (CPU 0) vs remote (CPU 128) NUMA host placement\n"
        "  d7  concurrent_bw      — host-thread count vs aggregate H→D BW\n"
        "  d8  alloc_cost         — memAlloc/ioMemAlloc vs size (first, steady, first-touch)\n"
        "  d9  free_cost          — memFree vs size (untouched + touched regimes)\n"
        "  d10 access_pattern     — direct-pointer reads/writes at varying strides\n"
        "  d11 bidirectional      — CxlMem H→D + D→H concurrent, duplex efficiency\n"
        "  d12 rw_ratio           — direct-pointer load:store ratio sweep on CxlMem\n",
        prog);
}

static int parse_test_arg(const char* a)
{
    if (!a || a[0] != 'd') return -1;
    const char* p = a + 1;
    if (*p == '\0') return -1;
    int n = 0;
    for (; *p; p++)
    {
        if (*p < '0' || *p > '9') return -1;
        n = n * 10 + (*p - '0');
        if (n > NUM_TESTS) return -1;
    }
    return (n >= 1 && n <= NUM_TESTS) ? n : -1;
}

// -1 = unpinned (default). --cpu=N pins the measuring thread to core N.
// Default is unpinned because pinning to a non-zero core segfaulted once in
// the orchestration suite (B3, unresolved).
static int g_pin_cpu = -1;

static void pin_after_context()
{
    if (g_pin_cpu < 0) { printf("# cpu_affinity: unpinned (runtime free to schedule)\n"); return; }
    cpu_set_t cs; CPU_ZERO(&cs); CPU_SET(g_pin_cpu, &cs);
    if (sched_setaffinity(0, sizeof(cs), &cs) != 0)
        fprintf(stderr, "# Warning: sched_setaffinity(%d) failed\n", g_pin_cpu);
    else
        printf("# cpu_affinity: measuring thread pinned to CPU %d (after context init)\n", g_pin_cpu);
}

__attribute__((unused)) static void pin_to_cpu0()
{
    cpu_set_t cs;
    CPU_ZERO(&cs);
    CPU_SET(0, &cs);
    if (sched_setaffinity(0, sizeof(cs), &cs) != 0)
        fprintf(stderr, "# Warning: sched_setaffinity failed — measurements may have jitter\n");
    else
        printf("# cpu_affinity: pinned to CPU 0\n");
}

static void print_device_info()
{
    printf("# PLATFORM\n");
    printf("# devices: %u\n", pxl::runtime::getNumDevice());

    auto* ctx = pxl::runtime::createContext(0);
    if (!ctx) return;

    struct { pxl::DeviceAttr attr; const char* name; } attrs[] = {
        { pxl::DeviceAttr::CxlMemoryCapacity, "CxlMemoryCapacity" },
        { pxl::DeviceAttr::IoMemoryCapacity,  "IoMemoryCapacity"  },
    };
    for (auto& a : attrs)
    {
        uint64_t v = 0;
        if (ctx->getAttribute(a.attr, v) == pxl::PxlResult::Success)
            printf("# %s: %lu\n", a.name, v);
    }
    printf("# available_mem_gb: %.1f\n",   ctx->availableMemSize()   / (1024.0 * 1024 * 1024));
    printf("# available_io_gb:  %.1f\n",   ctx->availableIoMemSize() / (1024.0 * 1024 * 1024));

    pxl::runtime::destroyContext(ctx);
}

int main(int argc, char* argv[])
{
    bool run[NUM_TESTS + 1] = {};
    bool any_test = false;
    for (int i = 1; i < argc; i++) if (parse_test_arg(argv[i]) > 0) any_test = true;

    if (!any_test)
    {
        for (int i = 1; i <= NUM_TESTS; i++) run[i] = true;
    }
    if (argc > 1)
    {
        for (int i = 1; i < argc; i++)
        {
            if (std::strcmp(argv[i], "-h") == 0 || std::strcmp(argv[i], "--help") == 0)
            {
                usage(argv[0]); return 0;
            }
            if (std::strcmp(argv[i], "--no-direct")  == 0) { g_no_direct  = true; continue; }
            if (std::strcmp(argv[i], "--keep-iomem")  == 0) { g_keep_iomem = true; continue; }
            if (std::strcmp(argv[i], "--fine")        == 0) { g_fine       = true; continue; }
            if (std::strcmp(argv[i], "--verify-only") == 0) { g_verify_only = true; continue; }
            if (std::strncmp(argv[i], "--cpu=", 6)    == 0) { g_pin_cpu = atoi(argv[i]+6); continue; }
            int n = parse_test_arg(argv[i]);
            if (n > 0) { run[n] = true; continue; }
            fprintf(stderr, "Unknown argument: %s\n", argv[i]);
            usage(argv[0]); return 1;
        }
    }

    printf("# Data Movement Tax Benchmark Suite\n");
    pxl::registerSignalHandler({SIGINT, SIGTERM, SIGSEGV, SIGBUS});
    printf("# direct_paths: %s   iomem_rows: %s\n",
           g_no_direct ? "SKIPPED (--no-direct)" : "on",
           g_keep_iomem ? "on (--keep-iomem)" : "off");
    printf("# d1_sizes: %s\n", g_fine ? "4x grid + 128K/512K/2M (--fine)" : "4x grid");

    // Pinning is deferred until after print_device_info() creates the first
    // context: threads inherit the creator's affinity, so pinning here would
    // confine the runtime's own workers too (Part 1, orchestration tax). The
    // device is on NUMA 0, so a NUMA-0 core is the right place for the
    // measuring thread -- but not CPU 0, which is this host's interrupt sink.
    if (timing::has_rdtscp())
    {
        stop_fn = timing::stop_precise;
        printf("# tsc_stop: rdtscp+lfence (precise)\n");
    }
    else
    {
        stop_fn = timing::stop;
        printf("# tsc_stop: lfence+rdtsc (portable, emulator-safe)\n");
    }

    print_device_info();
    pin_after_context();

    printf("# Calibrating TSC frequency...\n");
    double ghz = timing::tsc_ghz();
    printf("# tsc_freq_ghz: %.4f\n", ghz);

    if (run[1]) d1_bandwidth_sweep(ghz);
    if (run[2]) d2_latency_per_call(ghz);
    if (run[3]) d3_memtype_compare(ghz);
    if (run[4]) d4_direct_vs_copy(ghz);
    if (run[5]) d5_repeat_call_overhead(ghz);
    if (run[6]) d6_numa_placement(ghz);
    if (run[7]) d7_concurrent_bw(ghz);
    if (run[8]) d8_alloc_cost(ghz);
    if (run[9]) d9_free_cost(ghz);
    if (run[10]) d10_access_pattern(ghz);
    if (run[11]) d11_bidirectional(ghz);
    if (run[12]) d12_rw_ratio(ghz);

    printf("\n# Done.\n");
    return 0;
}
