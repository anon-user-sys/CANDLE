// Coherence-tax (gamma) microbenchmark suite for the XCENA MX1P. See PLAN.md.
//
//   sudo ./coh <test> [--quick]     test = k1 ... k11, or "all"
//
// Needs root (host load/store on memAlloc memory) and devdax. run_suite.sh adds the pinned host
// configuration of Sec. 4.3, a canary before and after every test, and state capture on failure.
// Output: CSV rows "kN,..." with a "#kN header ..." line before each block; '#' lines are notes.
#include <immintrin.h>
#include <sched.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <climits>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include <pxl/pxl.hpp>

#include "coh_common.hpp"
#include "timing.hpp"

using namespace coh;

namespace
{

constexpr size_t KiB = 1024, MiB = KiB * 1024, GiB = MiB * 1024;
constexpr size_t kLine = 64;
constexpr uint32_t kMaxTasks = 3048;
constexpr size_t kL3 = 96 * MiB;            // per socket; cache state is only "held" below this
bool g_quick = false;
double g_ghz = 1.0;

// ── timing / stats ────────────────────────────────────────────────────────────
inline uint64_t t0() { return timing::start(); }
inline uint64_t t1() { return timing::stop_precise(); }
inline double ns(uint64_t c) { return double(c) / g_ghz; }

struct Sum { double p50, min, max, mean; size_t n; };
Sum summarize(std::vector<double> v)
{
    Sum s{0, 0, 0, 0, v.size()};
    if (v.empty()) return s;
    std::sort(v.begin(), v.end());
    s.p50 = v[v.size() / 2]; s.min = v.front(); s.max = v.back();
    double a = 0; for (double x : v) a += x; s.mean = a / v.size();
    return s;
}
double median(std::vector<double> v) { return summarize(std::move(v)).p50; }

// Run body(t) on n threads released together; returns ns from release to the last finish.
double parallel(int n, const std::function<void(int)>& body)
{
    std::atomic<int> ready{0};
    std::atomic<bool> go{false};
    std::vector<std::thread> th;
    for (int t = 0; t < n; ++t)
        th.emplace_back([&, t] { ready.fetch_add(1); while (!go.load(std::memory_order_acquire)) {} body(t); });
    while (ready.load() < n) {}
    const auto a = std::chrono::steady_clock::now();
    go.store(true, std::memory_order_release);
    for (auto& x : th) x.join();
    return std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - a).count();
}
void split(size_t n, int par, int t, size_t& lo, size_t& hi)
{
    const size_t q = n / par, r = n % par;
    lo = t * q + (size_t(t) < r ? t : r);
    hi = lo + q + (size_t(t) < r ? 1 : 0);
}

// ── host cache primitives ─────────────────────────────────────────────────────
// Library flush: pxl::flushHostCache = clflushopt loop + mfence (opt=true) or clflush (opt=false).
inline void flush_lib(const void* p, size_t n, bool opt = true) { pxl::flushHostCache(const_cast<void*>(p), n, opt); }
// Raw loop, to separate the library's own per-call overhead from the instruction cost.
inline void flush_raw(const void* p, size_t n)
{
    const uintptr_t a = reinterpret_cast<uintptr_t>(p) & ~uintptr_t(kLine - 1);
    const uintptr_t e = reinterpret_cast<uintptr_t>(p) + n;
    for (uintptr_t x = a; x < e; x += kLine) _mm_clflushopt(reinterpret_cast<void*>(x));
    _mm_mfence();
}
inline void flush_raw_clflush(const void* p, size_t n)
{
    const uintptr_t a = reinterpret_cast<uintptr_t>(p) & ~uintptr_t(kLine - 1);
    const uintptr_t e = reinterpret_cast<uintptr_t>(p) + n;
    for (uintptr_t x = a; x < e; x += kLine) _mm_clflush(reinterpret_cast<void*>(x));
    _mm_mfence();
}
inline void write_pat(uint64_t* b, size_t lo, size_t hi, uint64_t salt)
{
    for (size_t i = lo; i < hi; ++i) b[i] = pat(i, salt);
}
inline void write_pat_nt(uint64_t* b, size_t lo, size_t hi, uint64_t salt)   // lo, hi multiples of 4
{
    for (size_t i = lo; i < hi; i += 4)
    {
        const __m256i v = _mm256_set_epi64x(pat(i + 3, salt), pat(i + 2, salt), pat(i + 1, salt), pat(i, salt));
        _mm256_stream_si256(reinterpret_cast<__m256i*>(b + i), v);
    }
    _mm_sfence();
}
// Copy a precomputed host-DRAM source with AVX2 non-temporal stores (the 4.3 NT path), then sfence.
inline void copy_nt(uint64_t* dst, const uint64_t* src, size_t words)   // 32 B aligned, words % 4 == 0
{
    for (size_t i = 0; i < words; i += 4)
        _mm256_stream_si256(reinterpret_cast<__m256i*>(dst + i), _mm256_load_si256(reinterpret_cast<const __m256i*>(src + i)));
    _mm_sfence();
}
inline uint64_t read_sum(const uint64_t* b, size_t lo, size_t hi)
{
    uint64_t s[4] = {0, 0, 0, 0};
    size_t i = lo;
    for (; i + 4 <= hi; i += 4) { s[0] += b[i]; s[1] += b[i + 1]; s[2] += b[i + 2]; s[3] += b[i + 3]; }
    for (; i < hi; ++i) s[0] += b[i];
    return s[0] + s[1] + s[2] + s[3];
}
uint64_t pat_sum(size_t lo, size_t hi, uint64_t salt)
{
    uint64_t s = 0; for (size_t i = lo; i < hi; ++i) s += pat(i, salt); return s;
}
struct Classify { uint64_t bad = 0, stale = 0, poison = 0, first = ~0ull; };
Classify classify(const uint64_t* b, size_t lo, size_t hi, uint64_t salt, uint64_t old)
{
    Classify c;
    for (size_t i = lo; i < hi; ++i)
    {
        const uint64_t v = b[i];
        if (v == pat(i, salt)) continue;
        if (c.first == ~0ull) c.first = i;
        ++c.bad;
        if (v == pat(i, old)) ++c.stale; else if (v == kPoison) ++c.poison;
    }
    return c;
}

// ── device ────────────────────────────────────────────────────────────────────
struct Dev
{
    pxl::runtime::Context* ctx = nullptr;
    pxl::kernel::Module* mod = nullptr;
    pxl::runtime::Job* job = nullptr;
    Cfg* cfg_ring = nullptr;      // one fresh line per launch: the device never read it before
    size_t cfg_next = 0, cfg_cap = 0;
    uint64_t* res = nullptr;      // kMaxTasks result lines
    std::vector<std::pair<std::string, pxl::kernel::Function*>> funcs;   // one Function per kernel name

    bool init()
    {
        pxl::registerSignalHandler({SIGINT, SIGTERM, SIGSEGV, SIGABRT, SIGBUS, SIGFPE, SIGILL});
        if (pxl::runtime::getNumDevice() == 0) { std::fprintf(stderr, "no PXL device\n"); return false; }
        ctx = pxl::runtime::createContext(0);
        if (!ctx) return false;
        char exe[4096] = {0};
        const ssize_t n = readlink("/proc/self/exe", exe, sizeof exe - 1);
        std::string dir = n > 0 ? std::string(exe, n) : std::string("./coh");
        dir = dir.substr(0, dir.find_last_of('/'));
        const std::string path = dir + "/mu_kernel/coh_kernel.mubin";   // absolute: cwd-independent
        mod = pxl::kernel::createModule(path.c_str());
        job = mod ? ctx->createJob() : nullptr;
        if (!job || job->load(mod) != pxl::PxlResult::Success) { std::fprintf(stderr, "module/job failed (%s)\n", path.c_str()); return false; }
        cfg_cap = (64 * MiB) / sizeof(Cfg);
        cfg_ring = static_cast<Cfg*>(ctx->memAlloc(64 * MiB));
        res = static_cast<uint64_t*>(ctx->memAlloc(kMaxTasks * kLine));
        return cfg_ring && res;
    }
    pxl::kernel::Function* fn(const char* name)
    {
        for (auto& [n, f] : funcs) if (n == name) return f;
        auto* f = mod->createFunction(name);
        if (!f) { std::fprintf(stderr, "createFunction(%s) failed\n", name); std::exit(4); }
        funcs.emplace_back(name, f);
        return f;
    }
    Cfg* cfg(const Cfg& c)
    {
        if (cfg_next >= cfg_cap) { std::fprintf(stderr, "cfg ring exhausted\n"); std::exit(4); }
        Cfg* p = &cfg_ring[cfg_next++];
        *p = c;
        flush_lib(p, sizeof(Cfg));
        return p;
    }
    void poison_res(size_t tasks)
    {
        for (size_t i = 0; i < tasks * kStride; ++i) res[i] = kPoison;
        flush_lib(res, tasks * kLine);
    }
    const uint64_t* read_res(size_t tasks) { flush_lib(res, tasks * kLine); return res; }
    // One launch on a fresh map (no handle aging, Sec. 4.1); returns host ms or -1.
    template <typename... A>
    double launch(pxl::kernel::Function* f, uint32_t tasks, A... args)
    {
        auto* m = job->buildMap(f, tasks);
        if (!m) return -1;
        const auto a = std::chrono::steady_clock::now();
        const bool ok = m->execute(args...) == pxl::PxlResult::Success && m->synchronize() == pxl::PxlResult::Success;
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - a).count();
        job->destroyMap(m);
        return ok ? ms : -1;
    }
    void fini()
    {
        if (res) ctx->memFree(res);
        if (cfg_ring) ctx->memFree(cfg_ring);
        for (auto& nf : funcs) mod->destroyFunction(nf.second);
        if (job) ctx->destroyJob(job);
        if (mod) pxl::kernel::destroyModule(mod);
        if (ctx) pxl::runtime::destroyContext(ctx);
    }
};
Dev D;

uint64_t* dev_alloc(size_t bytes)
{
    auto* p = static_cast<uint64_t*>(D.ctx->memAlloc(bytes));
    if (!p) { std::fprintf(stderr, "memAlloc(%zu) failed\n", bytes); std::exit(4); }
    return p;
}
void put_state(uint64_t* b, size_t words, const std::string& st, uint64_t salt)
{
    if (st == "dirty") write_pat(b, 0, words, salt);
    else { flush_lib(b, words * 8); if (st == "clean") { volatile uint64_t s = read_sum(b, 0, words); (void)s; } }
}
int reps_for(size_t bytes)
{
    int r = bytes <= 4 * KiB ? 1000 : bytes <= 64 * KiB ? 300 : bytes <= MiB ? 100 : bytes <= 16 * MiB ? 30 : bytes <= 256 * MiB ? 10 : 5;
    return g_quick ? std::max(3, r / 10) : r;
}
uint64_t g_salt = 0x5A17;
uint64_t next_salt() { return g_salt += 0x9E3779B97F4A7C15ull; }

// ── k1: host flush cost vs size, cache state, implementation ──────────────────
int k1()
{
    std::printf("#k1 header test,bytes,state,impl,reps,p50_ns,min_ns,max_ns,mean_ns,ns_per_line,state_held\n");
    const size_t cap = 1 * GiB;
    uint64_t* b = dev_alloc(cap);
    std::vector<size_t> sizes;
    for (size_t s = 64; s <= cap; s *= 4) sizes.push_back(s);
    for (size_t s : sizes)
    {
        const size_t words = s / 8;
        for (const char* st : {"dirty", "clean", "absent"})
            // lib_clflush_nofence = flushHostCache(opt=false): libpxl issues clflush and returns
            // WITHOUT the mfence of the opt=true path (objdump, 2026-10-10), so it times issue only.
            for (const char* impl : {"lib_clflushopt", "lib_clflush_nofence", "raw_clflushopt", "raw_clflush"})
            {
                std::vector<double> v;
                const int reps = reps_for(s);
                for (int r = 0; r < reps; ++r)
                {
                    put_state(b, words, st, next_salt());
                    const uint64_t a = t0();
                    if (!std::strcmp(impl, "lib_clflushopt")) flush_lib(b, s, true);
                    else if (!std::strcmp(impl, "lib_clflush_nofence")) flush_lib(b, s, false);
                    else if (!std::strcmp(impl, "raw_clflushopt")) flush_raw(b, s);
                    else flush_raw_clflush(b, s);
                    v.push_back(ns(t1() - a));
                }
                const Sum m = summarize(v);
                std::printf("k1,%zu,%s,%s,%d,%.1f,%.1f,%.1f,%.1f,%.3f,%d\n", s, st, impl, reps, m.p50, m.min, m.max, m.mean,
                            m.p50 / (s / kLine), s <= kL3 / 2 ? 1 : 0);
            }
        std::fflush(stdout);
    }
    // One 64 B descriptor published (store + flush), the resident-worker enqueue step (retest 0.9 us).
    std::vector<double> v;
    for (int r = 0; r < (g_quick ? 100 : 5000); ++r)
    {
        const uint64_t s = next_salt();
        const uint64_t a = t0();
        write_pat(b, 0, 8, s);
        flush_lib(b, 64);
        v.push_back(ns(t1() - a));
    }
    const Sum m = summarize(v);
    std::printf("k1,64,store+flush,lib_clflushopt,%zu,%.1f,%.1f,%.1f,%.1f,%.3f,1\n", m.n, m.p50, m.min, m.max, m.mean, m.p50);
    D.ctx->memFree(b);
    return 0;
}

// ── k2: flush cost vs dirty fraction of a 16 MiB range ────────────────────────
int k2()
{
    std::printf("#k2 header test,range_bytes,dirty_pct,layout,rest,dirty_lines,reps,p50_ns,min_ns,max_ns\n");
    const size_t R = 16 * MiB, lines = R / kLine;
    uint64_t* b = dev_alloc(R);
    std::vector<size_t> perm(lines);
    for (size_t i = 0; i < lines; ++i) perm[i] = i;
    std::mt19937_64 rng(42);
    std::shuffle(perm.begin(), perm.end(), rng);
    for (const char* rest : {"clean", "absent"})
        for (const char* layout : {"uniform", "clustered"})
            for (double pct : {0.0, 0.1, 1.0, 2.0, 5.0, 10.0, 25.0, 50.0, 75.0, 100.0})
            {
                const size_t k = size_t(lines * pct / 100.0 + 0.5);
                std::vector<double> v;
                const int reps = g_quick ? 5 : 30;
                for (int r = -2; r < reps; ++r)   // r < 0: warm-up, discarded
                {
                    const uint64_t s = next_salt();
                    flush_lib(b, R);
                    if (rest[0] == 'c') { volatile uint64_t x = read_sum(b, 0, R / 8); (void)x; }
                    for (size_t j = 0; j < k; ++j)
                    {
                        const size_t ln = layout[0] == 'u' ? perm[j] : j;
                        write_pat(b, ln * 8, ln * 8 + 8, s);
                    }
                    const uint64_t a = t0();
                    flush_lib(b, R);
                    if (r >= 0) v.push_back(ns(t1() - a));
                }
                const Sum m = summarize(v);
                std::printf("k2,%zu,%.1f,%s,%s,%zu,%d,%.1f,%.1f,%.1f\n", R, pct, layout, rest, k, reps, m.p50, m.min, m.max);
                std::fflush(stdout);
            }
    D.ctx->memFree(b);
    return 0;
}

// ── k3: bulk flush throughput vs threads ──────────────────────────────────────
int k3()
{
    std::printf("#k3 header test,total_bytes,state,threads,reps,p50_ms,gb_per_s\n");
    const size_t big = g_quick ? 2 * GiB : 16 * GiB;
    uint64_t* b = dev_alloc(big);
    struct Case { size_t bytes; const char* state; };
    for (Case c : {Case{64 * MiB, "dirty"}, Case{64 * MiB, "clean"}, Case{big, "absent"}})
        for (int th : {1, 2, 4, 8, 16, 32})
        {
            const size_t words = c.bytes / 8;
            std::vector<double> v;
            const int reps = c.bytes > kL3 ? 3 : 20;
            for (int r = 0; r < reps; ++r)
            {
                const uint64_t s = next_salt();
                parallel(th, [&](int t) {   // prepare the state with the same threads (same caches)
                    size_t lo, hi; split(words, th, t, lo, hi);
                    if (c.state[0] == 'd') write_pat(b, lo, hi, s);
                    else { flush_lib(b + lo, (hi - lo) * 8); if (c.state[0] == 'c') { volatile uint64_t x = read_sum(b, lo, hi); (void)x; } }
                });
                v.push_back(parallel(th, [&](int t) { size_t lo, hi; split(words, th, t, lo, hi); flush_lib(b + lo, (hi - lo) * 8); }));
            }
            const double p = median(v);
            std::printf("k3,%zu,%s,%d,%d,%.3f,%.2f\n", c.bytes, c.state, th, reps, p / 1e6, c.bytes / p);
            std::fflush(stdout);
        }
    D.ctx->memFree(b);
    return 0;
}

// Device-filled buffer shared by k4/k5: the host never wrote it, so it is the realistic
// "device produced it" case. Returns the salt.
uint64_t dev_fill(uint64_t* b, size_t words, uint64_t salt, uint64_t mode = M_EXIT_FLUSH)
{
    auto* f = D.fn("k_fill");
    Cfg c{words, kMaxTasks, salt, mode, 0, 0, 0, 0};
    if (D.launch(f, kMaxTasks, b, D.cfg(c), D.res) < 0) { std::fprintf(stderr, "k_fill failed\n"); std::exit(4); }
    return salt;
}

// ── k4: host read of device-written data: cold vs invalidate+read vs cached ───
int k4()
{
    // Arms: cold = host holds no copy, plain read. invalidate = host holds clean copies (as after
    // an earlier read), flushHostCache the whole slice, then read. invalidate_chunked = the same,
    // alternating 1 MiB flush + 1 MiB read (the Tier 2 return pattern). cached = host copies are
    // read without invalidating (fast, but stale if the device had written them).
    std::printf("#k4 header test,bytes,threads,arm,reps,p50_ns,gb_per_s,flush_part_ns,ok\n");
    const size_t cap = g_quick ? 1 * GiB : 4 * GiB, words_cap = cap / 8;
    uint64_t* b = dev_alloc(cap);
    const uint64_t salt = dev_fill(b, words_cap, next_salt());
    flush_lib(b, cap);
    std::vector<size_t> sizes = {4 * KiB, 64 * KiB, MiB, 16 * MiB, 64 * MiB, GiB};
    if (!g_quick) sizes.push_back(4 * GiB);
    const size_t chunk = MiB / 8;   // words
    for (size_t s : sizes)
    {
        const size_t words = s / 8;
        const uint64_t want = pat_sum(0, words, salt);
        for (int th : {1, 16})
        {
            if (th > 1 && s < 64 * MiB) continue;
            for (const char* arm : {"cold", "invalidate", "invalidate_chunked", "cached"})
            {
                const bool cached = !std::strcmp(arm, "cached"), chunked = !std::strcmp(arm, "invalidate_chunked");
                const bool inval = arm[0] == 'i';
                if (cached && s > kL3 / 2) continue;      // only where it can stay cached
                if (chunked && s < 4 * MiB) continue;     // needs several chunks
                std::vector<double> v, fp;
                bool ok = true;
                const int reps = std::max(3, reps_for(s) / 4);
                for (int r = 0; r < reps; ++r)
                {
                    std::vector<uint64_t> sums(th * 8, 0);
                    std::vector<double> fparts(th, 0);
                    flush_lib(b, s);
                    if (inval || cached)
                    {
                        if (th == 1) { volatile uint64_t x = read_sum(b, 0, words); (void)x; }
                        else parallel(th, [&](int t) { size_t lo, hi; split(words, th, t, lo, hi); volatile uint64_t x = read_sum(b, lo, hi); (void)x; });
                    }
                    auto body = [&](int t) {
                        size_t lo, hi; split(words, th, t, lo, hi);
                        uint64_t sum = 0;
                        if (chunked)
                            for (size_t c = lo; c < hi; c += chunk)
                            {
                                const size_t e = std::min(hi, c + chunk);
                                const uint64_t a = t0(); flush_lib(b + c, (e - c) * 8); fparts[t] += ns(t1() - a);
                                sum += read_sum(b, c, e);
                            }
                        else
                        {
                            if (inval) { const uint64_t a = t0(); flush_lib(b + lo, (hi - lo) * 8); fparts[t] = ns(t1() - a); }
                            sum = read_sum(b, lo, hi);
                        }
                        sums[t * 8] = sum;
                    };
                    if (th == 1) { const uint64_t a = t0(); body(0); v.push_back(ns(t1() - a)); }   // no thread start-up in the timing
                    else v.push_back(parallel(th, body));
                    uint64_t tot = 0; for (int t = 0; t < th; ++t) tot += sums[t * 8];
                    ok = ok && tot == want;
                    fp.push_back(*std::max_element(fparts.begin(), fparts.end()));
                }
                const double p = median(v);
                std::printf("k4,%zu,%d,%s,%d,%.1f,%.2f,%.1f,%d\n", s, th, arm, reps, p, s / p, median(fp), ok);
                std::fflush(stdout);
            }
        }
    }
    D.ctx->memFree(b);
    return 0;
}

// ── k5: per-object invalidate+read vs plain read (B-Link SDBI granularity) ────
int k5()
{
    std::printf("#k5 header test,obj_bytes,threads,arm,lookups,reps,p50_ns_per_lookup,ok\n");
    const size_t cap = g_quick ? 1 * GiB : 4 * GiB, words_cap = cap / 8;
    uint64_t* b = dev_alloc(cap);
    const uint64_t salt = dev_fill(b, words_cap, next_salt());
    flush_lib(b, cap);
    const size_t N = g_quick ? 20000 : 200000, hot = 4096;
    for (size_t o : {size_t(64), size_t(256), size_t(1024), size_t(4096)})
    {
        const size_t nobj = cap / o, ow = o / 8;
        for (int th : {1, 16})
            for (const char* arm : {"cold_plain", "cold_sdbi", "hot_plain", "hot_sdbi"})
            {
                std::vector<double> v;
                bool ok = true;
                const int reps = g_quick ? 2 : 5;
                for (int r = 0; r < reps; ++r)
                {
                    std::vector<std::vector<size_t>> idx(th);
                    for (int t = 0; t < th; ++t)
                    {
                        std::mt19937_64 rng(1000 * r + t);
                        idx[t].resize(N);
                        const size_t span = arm[0] == 'h' ? hot : nobj;
                        const size_t base = arm[0] == 'h' ? (size_t(t) * hot * 7919) % (nobj - hot) : 0;
                        for (auto& x : idx[t]) x = base + rng() % span;
                        if (arm[0] == 'h')   // warm the hot set into this thread's caches
                            for (size_t j = 0; j < hot; ++j) { volatile uint64_t s = read_sum(b, (base + j) * ow, (base + j + 1) * ow); (void)s; }
                    }
                    std::vector<uint64_t> badv(th * 8, 0);
                    const bool sdbi = std::strstr(arm, "sdbi") != nullptr;
                    const double el = parallel(th, [&](int t) {
                        uint64_t bad = 0;
                        for (size_t x : idx[t])
                        {
                            const uint64_t* p = b + x * ow;
                            if (sdbi) flush_lib(p, o);
                            for (size_t w = 0; w < ow; w += 8) bad += p[w] != pat(x * ow + w, salt);   // first word of each line
                        }
                        badv[t * 8] = bad;
                    });
                    for (int t = 0; t < th; ++t) ok = ok && badv[t * 8] == 0;
                    v.push_back(el / N);
                }
                std::printf("k5,%zu,%d,%s,%zu,%d,%.1f,%d\n", o, th, arm, N, reps, median(v), ok);
                std::fflush(stdout);
            }
    }
    D.ctx->memFree(b);
    return 0;
}

// Device check: k_check over [b, b+words) against salt (old = stale classifier). Returns totals.
Classify dev_check(uint64_t* b, size_t words, uint64_t salt, uint64_t old, uint64_t mode, uint32_t tasks, double* inval_cycles = nullptr)
{
    auto* f = D.fn("k_check");
    D.poison_res(tasks);
    Cfg c{words, tasks, salt, mode, old, 0, 0, 0};
    if (D.launch(f, tasks, b, D.cfg(c), D.res) < 0) { std::fprintf(stderr, "k_check failed\n"); std::exit(4); }
    const uint64_t* r = D.read_res(tasks);
    Classify tot;
    std::vector<double> inv;
    for (uint32_t t = 0; t < tasks; ++t)
    {
        const uint64_t* x = r + t * kStride;
        if (x[0] == kPoison) { std::printf("# k_check task %u wrote no result\n", t); tot.bad += 1; continue; }
        tot.bad += x[0]; tot.stale += x[2]; tot.poison += x[3];
        tot.first = std::min(tot.first, x[1]);
        inv.push_back(double(x[4]));
    }
    if (inval_cycles) *inval_cycles = median(inv);
    return tot;
}
uint32_t tasks_for(size_t words) { return uint32_t(std::min<size_t>(kMaxTasks, std::max<size_t>(1, words / 8))); }

// ── k6: host -> device handoff: which half is required? ───────────────────────
int k6()
{
    std::printf("#k6 header test,bytes,host_flush,dev_inval,rep,words,bad,stale,poison,other,dev_inval_cycles\n");
    const size_t cap = 64 * MiB;
    uint64_t* b = dev_alloc(cap);
    for (size_t s : {64 * KiB, 4 * MiB, 64 * MiB})
    {
        const size_t words = s / 8;
        const uint32_t tasks = tasks_for(words);
        for (int rep = 0; rep < (g_quick ? 1 : 3); ++rep)
            for (int hf : {1, 0})
                for (int di : {1, 0})
                {
                    const uint64_t old = next_salt(), neu = next_salt();
                    write_pat(b, 0, words, old);
                    flush_lib(b, s);
                    const Classify pre = dev_check(b, words, old, old, M_ENTRY_INVAL, tasks);   // device now caches old
                    if (pre.bad) std::printf("# k6 setup: device did not see the old pattern (%lu bad)\n", (unsigned long)pre.bad);
                    write_pat(b, 0, words, neu);
                    if (hf) flush_lib(b, s);
                    double cyc = 0;
                    const Classify c = dev_check(b, words, neu, old, di ? uint64_t(M_ENTRY_INVAL) : uint64_t(0), tasks, &cyc);
                    std::printf("k6,%zu,%d,%d,%d,%zu,%lu,%lu,%lu,%lu,%.0f\n", s, hf, di, rep, words, (unsigned long)c.bad,
                                (unsigned long)c.stale, (unsigned long)c.poison, (unsigned long)(c.bad - c.stale - c.poison), cyc);
                    flush_lib(b, s);   // leave no dirty host lines for the next arm
                    std::fflush(stdout);
                }
    }
    D.ctx->memFree(b);
    return 0;
}

// ── k7: do non-temporal stores need a flush? ──────────────────────────────────
int k7()
{
    // The timed write copies a precomputed host-DRAM source (so pattern generation is not timed):
    // nt_* = AVX2 streaming stores + sfence (4.3's NT loop), store_* = library memcpy.
    std::printf("#k7 header test,bytes,prior,method,rep,write_ns,gb_per_s,bad,stale,poison,other\n");
    const size_t cap = 64 * MiB;
    uint64_t* b = dev_alloc(cap);
    uint64_t* src = static_cast<uint64_t*>(std::aligned_alloc(2 * MiB, cap));
    for (size_t s : {64 * KiB, 4 * MiB, 64 * MiB})
    {
        const size_t words = s / 8;
        const uint32_t tasks = tasks_for(words);
        for (int rep = 0; rep < (g_quick ? 1 : 3); ++rep)
            for (const char* prior : {"absent", "clean", "dirty"})
                for (const char* method : {"nt_sfence", "nt_sfence_flush", "store_flush", "store_noflush"})
                {
                    const uint64_t old = next_salt(), mid = next_salt(), neu = next_salt();
                    write_pat(b, 0, words, old);
                    flush_lib(b, s);
                    dev_check(b, words, old, old, M_ENTRY_INVAL, tasks);          // device caches old
                    if (prior[0] == 'c') { volatile uint64_t x = read_sum(b, 0, words); (void)x; }
                    if (prior[0] == 'd') write_pat(b, 0, words, mid);            // dirty host lines, not flushed
                    for (size_t i = 0; i < words; ++i) src[i] = pat(i, neu);
                    { volatile uint64_t x = read_sum(src, 0, words); (void)x; }   // source warm, as in a real copy
                    const uint64_t a = t0();
                    if (method[0] == 'n') copy_nt(b, src, words); else std::memcpy(b, src, s);
                    if (!std::strcmp(method, "nt_sfence_flush") || !std::strcmp(method, "store_flush")) flush_lib(b, s);
                    const double w = ns(t1() - a);
                    const Classify c = dev_check(b, words, neu, old, M_ENTRY_INVAL, tasks);
                    std::printf("k7,%zu,%s,%s,%d,%.1f,%.2f,%lu,%lu,%lu,%lu\n", s, prior, method, rep, w, s / w, (unsigned long)c.bad,
                                (unsigned long)c.stale, (unsigned long)c.poison, (unsigned long)(c.bad - c.stale - c.poison));
                    flush_lib(b, s);
                    std::fflush(stdout);
                }
    }
    std::free(src);
    D.ctx->memFree(b);
    return 0;
}

// ── k8: device -> host handoff: which half is required? ───────────────────────
int k8()
{
    std::printf("#k8 header test,bytes,dev_flush,host_inval,rep,words,bad,stale,poison,other\n");
    const size_t cap = 64 * MiB;
    uint64_t* b = dev_alloc(cap);
    for (size_t s : {64 * KiB, 4 * MiB, 64 * MiB})
    {
        const size_t words = s / 8;
        for (int rep = 0; rep < (g_quick ? 1 : 3); ++rep)
            for (const char* df : {"range", "all", "none"})
                for (int hi : {1, 0})
                {
                    const uint64_t old = next_salt(), neu = next_salt();
                    write_pat(b, 0, words, old);
                    flush_lib(b, s);
                    { volatile uint64_t x = read_sum(b, 0, words); (void)x; }   // host caches old (clean)
                    const uint64_t mode = M_ENTRY_INVAL | (df[0] == 'r' ? uint64_t(M_EXIT_FLUSH) : df[0] == 'a' ? uint64_t(M_EXIT_ALL) : uint64_t(0));
                    auto* f = D.fn("k_fill");
                    Cfg c{words, tasks_for(words), neu, mode, 0, 0, 0, 0};
                    if (D.launch(f, tasks_for(words), b, D.cfg(c), D.res) < 0) { std::fprintf(stderr, "k_fill failed\n"); return 4; }
                    if (hi) flush_lib(b, s);
                    const Classify k = classify(b, 0, words, neu, old);
                    std::printf("k8,%zu,%s,%d,%d,%zu,%lu,%lu,%lu,%lu\n", s, df, hi, rep, words, (unsigned long)k.bad,
                                (unsigned long)k.stale, (unsigned long)k.poison, (unsigned long)(k.bad - k.stale - k.poison));
                    // final state: memory must hold the new pattern once both halves are done
                    flush_lib(b, s);
                    const Classify fin = classify(b, 0, words, neu, old);
                    if (fin.bad) std::printf("# k8 after host invalidate: %lu bad remain (dev_flush=%s)\n", (unsigned long)fin.bad, df);
                    std::fflush(stdout);
                }
    }
    D.ctx->memFree(b);
    return 0;
}

// ── k9: device-side flush cost ────────────────────────────────────────────────
int k9()
{
    std::printf("#k9a header test,slice_bytes,tasks,flush_dirty_cyc,flush_clean_cyc,flushall_dirty_cyc,write_cyc\n");
    auto* f = D.fn("k_devflush");
    const size_t maxslice = 256 * KiB;
    uint64_t* b = dev_alloc(size_t(kMaxTasks) * maxslice);
    for (size_t sl = 64; sl <= maxslice; sl *= 4)
        for (uint32_t P : {1u, kMaxTasks})
        {
            std::vector<double> fd, fc, fa, wr;
            for (int rep = 0; rep < (g_quick ? 1 : 3); ++rep)
            {
                D.poison_res(P);
                Cfg c{0, P, next_salt(), 0, sl, 0, 0, 0};
                if (D.launch(f, P, b, D.cfg(c), D.res) < 0) { std::fprintf(stderr, "k_devflush failed\n"); return 4; }
                const uint64_t* r = D.read_res(P);
                for (uint32_t t = 0; t < P; ++t)
                    if (r[t * kStride] != kPoison)
                    { fd.push_back(r[t * kStride]); fc.push_back(r[t * kStride + 1]); fa.push_back(r[t * kStride + 2]); wr.push_back(r[t * kStride + 3]); }
            }
            std::printf("k9a,%zu,%u,%.0f,%.0f,%.0f,%.0f\n", sl, P, median(fd), median(fc), median(fa), median(wr));
            std::fflush(stdout);
        }
    D.ctx->memFree(b);

    std::printf("#k9b header test,bytes,mode,rep,kernel_ms\n");
    const size_t s = g_quick ? 256 * MiB : GiB;
    uint64_t* c = dev_alloc(s);
    for (int rep = 0; rep < (g_quick ? 2 : 5); ++rep)
        for (uint64_t mode : {uint64_t(0), uint64_t(M_EXIT_FLUSH), uint64_t(M_EXIT_ALL)})
        {
            auto* ff = D.fn("k_fill");
            Cfg cf{s / 8, kMaxTasks, next_salt(), mode, 0, 0, 0, 0};
            const double ms = D.launch(ff, kMaxTasks, c, D.cfg(cf), D.res);
            std::printf("k9b,%zu,%s,%d,%.3f\n", s, mode == 0 ? "none" : mode == uint64_t(M_EXIT_FLUSH) ? "range" : "all", rep, ms);
            std::fflush(stdout);
        }
    D.ctx->memFree(c);
    return 0;
}

// ── k10: in-kernel publish race, powered ──────────────────────────────────────
int k10()
{
    std::printf("#k10 header test,mode,read,rep,lanes,chunks,lines_per_chunk,chunks_consumed,early_bad_lines,early_poison_lines,final_bad_lines,kernel_ms\n");
    const uint64_t P = kMaxTasks, C = 8, L = g_quick ? 64 : 512;
    const size_t words = P * kStride + P * C * L * 8;
    uint64_t* b = dev_alloc(words * 8);
    auto* f = D.fn("k_publish");
    const int R = 16;
    struct M { uint64_t mode; const char* name; };
    for (int rep = 0; rep < (g_quick ? 1 : 3); ++rep)
        for (const char* read : {"chase", "after_sync"})
            for (M m : {M{PUB_FLUSH_FENCE, "flush_fence"}, M{PUB_FLUSH, "flush"}, M{PUB_FLUSHALL, "flushall_fence"},
                        M{PUB_FLUSHALL_NOFENCE, "flushall"}, M{PUB_NOFLUSH, "noflush"}})
            {
                if (read[0] == 'a' && m.mode != PUB_FLUSH_FENCE) continue;   // the control needs one mode
                const uint64_t salt = next_salt();
                for (size_t i = 0; i < P * kStride; ++i) b[i] = 0;
                for (size_t i = P * kStride; i < words; ++i) b[i] = kPoison;
                flush_lib(b, words * 8);
                auto* map = D.job->buildMap(f, P);
                Cfg c{0, P, salt, m.mode, C, L, 0, 0};
                Cfg* cp = D.cfg(c);
                std::atomic<bool> done{false};
                std::atomic<uint64_t> consumed{0}, ebad{0}, epois{0};
                std::vector<std::thread> rd;
                auto verify_chunk = [&](uint64_t lane, uint64_t ch, uint64_t& bad, uint64_t& pois) {
                    const uint64_t base = pub_data(P, C, L, lane, ch);
                    flush_lib(&b[base], L * kLine);
                    for (uint64_t l = 0; l < L; ++l)
                    {
                        bool lb = false, lp = false;
                        for (uint64_t w = 0; w < 8; ++w)
                        {
                            const uint64_t v = b[base + l * 8 + w];
                            if (v != pat(base + l * 8 + w, salt)) { lb = true; lp = lp || v == kPoison; }
                        }
                        bad += lb; pois += lp;
                    }
                };
                if (read[0] == 'c')
                    for (int r = 0; r < R; ++r)
                        rd.emplace_back([&, r] {
                            std::vector<uint64_t> got(P, 0);
                            uint64_t bad = 0, pois = 0, n = 0;
                            for (;;)
                            {
                                const bool fin = done.load();
                                bool progress = false;
                                for (uint64_t lane = r; lane < P; lane += R)
                                {
                                    flush_lib(&b[pub_counter(lane)], kLine);
                                    const uint64_t cnt = std::min<uint64_t>(b[pub_counter(lane)], C);
                                    while (got[lane] < cnt) { verify_chunk(lane, got[lane], bad, pois); ++got[lane]; ++n; progress = true; }
                                }
                                if (fin && !progress) break;
                            }
                            consumed += n; ebad += bad; epois += pois;
                        });
                const auto a = std::chrono::steady_clock::now();
                const bool ok = map->execute(b, cp, D.res) == pxl::PxlResult::Success && map->synchronize() == pxl::PxlResult::Success;
                const double kms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - a).count();
                done = true;
                for (auto& t : rd) t.join();
                D.job->destroyMap(map);
                if (!ok) { std::fprintf(stderr, "k_publish failed\n"); return 4; }
                // final: everything must be correct once the kernel has completed
                uint64_t fbad = 0, fp = 0;
                for (uint64_t lane = 0; lane < P; ++lane)
                    for (uint64_t ch = 0; ch < C; ++ch) verify_chunk(lane, ch, fbad, fp);
                if (read[0] == 'a') { consumed = P * C; }
                std::printf("k10,%s,%s,%d,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%.1f\n", m.name, read, rep, (unsigned long)P, (unsigned long)C,
                            (unsigned long)L, (unsigned long)consumed.load(), (unsigned long)ebad.load(), (unsigned long)epois.load(),
                            (unsigned long)fbad, kms);
                std::fflush(stdout);
            }
    D.ctx->memFree(b);
    return 0;
}

// ── k11: one-line handoff round trip host -> device -> host ───────────────────
int k11()
{
    std::printf("#k11 header test,rep,rounds,answered,rt_p50_ns,rt_p99_ns,rt_min_ns,publish_p50_ns,dev_bad,host_bad,timeouts\n");
    auto* f = D.fn("k_pingpong");
    uint64_t* q = dev_alloc(MiB);
    const uint64_t N = g_quick ? 2000 : 20000;
    for (int rep = 0; rep < (g_quick ? 1 : 3); ++rep)
    {
        const uint64_t salt = next_salt();
        for (size_t i = 0; i < PP_WORDS; ++i) q[i] = 0;
        flush_lib(q, PP_WORDS * 8);
        auto* map = D.job->buildMap(f, 1);
        Cfg c{0, 1, salt, 0, N, uint64_t(1.4e9 * 120), 0, 0};   // device budget: 120 s at 1.4 GHz
        Cfg* cp = D.cfg(c);
        std::vector<double> rt, pub;
        uint64_t host_bad = 0, timeouts = 0;
        std::thread host([&] {
            for (uint64_t k = 1; k <= N; ++k)
            {
                const uint64_t a = t0();
                for (uint64_t j = 1; j < 8; ++j) q[PP_H2D + j] = pat(8 * k + j, salt);
                q[PP_H2D] = k;
                flush_lib(&q[PP_H2D], kLine);
                const uint64_t p = t1();
                bool got = false;
                for (;;)
                {
                    flush_lib(&q[PP_D2H], kLine);
                    if (q[PP_D2H] == k) { got = true; break; }
                    if (ns(timing::stop_precise() - a) > 1e9) break;
                }
                const uint64_t e = t1();
                if (!got) { ++timeouts; q[PP_STOP] = 1; flush_lib(&q[PP_STOP], kLine); break; }
                for (uint64_t j = 1; j < 8; ++j) host_bad += q[PP_D2H + j] != pat(8 * k + j, salt ^ 1);
                rt.push_back(ns(e - a));
                pub.push_back(ns(p - a));
            }
        });
        const bool ok = map->execute(q, cp) == pxl::PxlResult::Success && map->synchronize() == pxl::PxlResult::Success;
        host.join();
        D.job->destroyMap(map);
        flush_lib(&q[PP_DONE], kLine);
        std::vector<double> s = rt;
        std::sort(s.begin(), s.end());
        const double p99 = s.empty() ? 0 : s[std::min(s.size() - 1, size_t(s.size() * 0.99))];
        std::printf("k11,%d,%lu,%lu,%.1f,%.1f,%.1f,%.1f,%lu,%lu,%lu\n", rep, (unsigned long)N, (unsigned long)q[PP_DONE], median(rt), p99,
                    s.empty() ? 0 : s.front(), median(pub), (unsigned long)q[PP_DONE + 1], (unsigned long)host_bad, (unsigned long)timeouts);
        std::fflush(stdout);
        if (!ok) { std::fprintf(stderr, "k_pingpong failed\n"); return 4; }
    }
    D.ctx->memFree(q);
    return 0;
}


// ── k12: does a prior DEVICE read slow later host writes? (k7 vs Sec 4.3 discrepancy) ──
// k7's NT+flush ran 2.4x slower than Sec 4.3's identical path at 64 KiB-4 MiB; the setup
// difference is that k7's device read the range just before the host wrote it. Same writes,
// with and without that device read; the host cache state is "absent" in both arms.
int k12()
{
    std::printf("#k12 header test,bytes,device_preread,method,rep,write_ns,gb_per_s,bad\n");
    const size_t cap = 64 * MiB;
    uint64_t* b = dev_alloc(cap);
    uint64_t* src = static_cast<uint64_t*>(std::aligned_alloc(2 * MiB, cap));
    for (size_t s : {64 * KiB, MiB, 4 * MiB, 64 * MiB})
    {
        const size_t words = s / 8;
        const uint32_t tasks = tasks_for(words);
        for (int rep = 0; rep < (g_quick ? 2 : 5); ++rep)
            for (int pre : {1, 0})
                for (const char* method : {"nt_sfence", "nt_sfence_flush", "store_flush"})
                {
                    const uint64_t old = next_salt(), neu = next_salt();
                    write_pat(b, 0, words, old);
                    flush_lib(b, s);
                    if (pre) dev_check(b, words, old, old, M_ENTRY_INVAL, tasks);
                    else D.launch(D.fn("k_check"), 1, b, D.cfg(Cfg{8, 1, old, 0, old, 0, 0, 0}), D.res);   // same launch, touches one line
                    flush_lib(b, s);                                                                      // host state: absent
                    for (size_t i = 0; i < words; ++i) src[i] = pat(i, neu);
                    { volatile uint64_t x = read_sum(src, 0, words); (void)x; }
                    const uint64_t a = t0();
                    if (method[0] == 'n') copy_nt(b, src, words); else std::memcpy(b, src, s);
                    if (std::strcmp(method, "nt_sfence")) flush_lib(b, s);
                    const double w = ns(t1() - a);
                    const Classify c = dev_check(b, words, neu, old, M_ENTRY_INVAL, tasks);
                    std::printf("k12,%zu,%d,%s,%d,%.1f,%.2f,%lu\n", s, pre, method, rep, w, s / w, (unsigned long)c.bad);
                    std::fflush(stdout);
                }
    }
    std::free(src);
    D.ctx->memFree(b);
    return 0;
}


// ── k13: is the k7/k12 write slowdown core frequency after an idle wait? ─────
// k12 rejected "prior device read". k7/k12 time ONE write right after a kernel launch, i.e.
// after the host thread slept in synchronize(); Sec 4.3 times hundreds back to back. Arms:
//   after_launch = as k12 (launch + synchronize, then the timed write)
//   spun         = same, then a 50 ms busy spin before the timed write
//   steady       = 20 back-to-back writes, median of the last 10 (Sec 4.3's pattern)
// cur_khz = scaling_cur_freq of this CPU read just before the timed write (median).
long cur_khz()
{
    char path[96];
    std::snprintf(path, sizeof path, "/sys/devices/system/cpu/cpu%d/cpufreq/scaling_cur_freq", sched_getcpu());
    FILE* f = std::fopen(path, "r");
    long k = -1;
    if (f) { if (std::fscanf(f, "%ld", &k) != 1) k = -1; std::fclose(f); }
    return k;
}
void spin_ms(double ms)
{
    const uint64_t a = t0();
    volatile uint64_t x = 0;
    while (ns(t1() - a) < ms * 1e6) x = x + 1;
}
int k13()
{
    std::printf("#k13 header test,bytes,arm,method,reps,p50_write_ns,gb_per_s,cur_khz_p50,bad\n");
    const size_t cap = 64 * MiB;
    uint64_t* b = dev_alloc(cap);
    uint64_t* src = static_cast<uint64_t*>(std::aligned_alloc(2 * MiB, cap));
    for (size_t s : {64 * KiB, MiB, 4 * MiB, 64 * MiB})
    {
        const size_t words = s / 8;
        for (const char* arm : {"after_launch", "spun", "steady"})
            for (const char* method : {"nt_sfence", "nt_sfence_flush", "store_flush"})
            {
                std::vector<double> v, khz;
                uint64_t bad = 0;
                const int reps = g_quick ? 2 : 5;
                for (int rep = 0; rep < reps; ++rep)
                {
                    const uint64_t old = next_salt(), neu = next_salt();
                    write_pat(b, 0, words, old);
                    flush_lib(b, s);
                    D.launch(D.fn("k_check"), 1, b, D.cfg(Cfg{8, 1, old, 0, old, 0, 0, 0}), D.res);
                    for (size_t i = 0; i < words; ++i) src[i] = pat(i, neu);
                    { volatile uint64_t x = read_sum(src, 0, words); (void)x; }
                    flush_lib(b, s);
                    auto write = [&] {
                        const uint64_t a = t0();
                        if (method[0] == 'n') copy_nt(b, src, words); else std::memcpy(b, src, s);
                        if (std::strcmp(method, "nt_sfence")) flush_lib(b, s);
                        return ns(t1() - a);
                    };
                    if (arm[0] == 's' && arm[1] == 'p') spin_ms(50);
                    if (arm[1] == 't')   // steady
                    {
                        std::vector<double> w;
                        for (int i = 0; i < 20; ++i) { if (i >= 10) khz.push_back(cur_khz()); w.push_back(write()); flush_lib(b, s); }
                        v.push_back(median(std::vector<double>(w.begin() + 10, w.end())));
                    }
                    else { khz.push_back(cur_khz()); v.push_back(write()); }
                    bad += dev_check(b, words, neu, old, M_ENTRY_INVAL, tasks_for(words)).bad;
                }
                const double p = median(v);
                std::printf("k13,%zu,%s,%s,%d,%.1f,%.2f,%.0f,%lu\n", s, arm, method, reps, p, s / p, median(khz), (unsigned long)bad);
                std::fflush(stdout);
            }
    }
    std::free(src);
    D.ctx->memFree(b);
    return 0;
}

}  // namespace

int main(int argc, char** argv)
{
    std::vector<std::string> tests;
    for (int i = 1; i < argc; ++i)
    {
        const std::string a = argv[i];
        if (a == "--quick") g_quick = true;
        else if (a == "-h" || a == "--help") { std::printf("usage: sudo %s k1..k13|all [--quick]\n", argv[0]); return 0; }
        else tests.push_back(a);
    }
    if (tests.empty()) { std::fprintf(stderr, "usage: sudo %s k1..k13|all [--quick]\n", argv[0]); return 2; }
    if (geteuid() != 0) { std::fprintf(stderr, "needs root (host access to device memory SIGBUSes otherwise)\n"); return 1; }
    if (!timing::has_rdtscp()) { std::fprintf(stderr, "rdtscp missing\n"); return 1; }
    g_ghz = timing::tsc_ghz();
    std::printf("# coherence_tax  tsc_ghz=%.4f  quick=%d  cpu=%d\n", g_ghz, g_quick, sched_getcpu());
    if (!D.init()) return 1;
    int (*fns[])() = {k1, k2, k3, k4, k5, k6, k7, k8, k9, k10, k11, k12, k13};
    int rc = 0;
    for (const auto& t : tests)
    {
        if (t == "all") { for (auto* fn : fns) rc |= fn(); continue; }
        const int n = (t.size() > 1 && t[0] == 'k') ? std::atoi(t.c_str() + 1) : 0;
        if (n < 1 || n > 13) { std::fprintf(stderr, "unknown test %s\n", t.c_str()); rc |= 2; continue; }
        std::printf("# TEST k%d\n", n);
        rc |= fns[n - 1]();
    }
    D.fini();
    std::printf("# done rc=%d\n", rc);
    return rc;
}
