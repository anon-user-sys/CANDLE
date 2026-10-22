// Translation-tax experiment driver (v2). See README.md.
//
//   ./main --device            run the pattern on MX1 MU cores (devdax, PXL)
//   ./main --host [--thp]      run the identical pattern on the host (4 KB pages, or THP)
//
// The pattern is a cross product of three independent knobs (see tt_common.hpp):
//   --region-bytes 268435456,1073741824      region size S (power of two)
//   --regions 8,16,32,64,128                 region count R (power of two), span = R * S
//   --lines-per-region 1,2,4                  lines per region L (power of two), K = R * L
//   --lines 4096                              instead of -per-region: L = K / R, so K is held
//                                             fixed across the sweep and cache demand with it
//   --layout spread|clump|both                where the L lines sit inside their region; only
//                                             clump makes the region count a real knob
//   --order random|rr|both                    access order inside the timed pass
//   --accesses N  --reps R  --buffer-gb G  --host-thp-cap-gb G
//
// Output: CSV on stdout, one row per (order, S, R, L, rep):
//   platform,pages,layout,order,region_bytes,regions,lines_per_region,lines,span_mb,accesses,rep,
//   ns_per_access,cycles_per_access,anon_huge_mb,sum_ok
// Device ns come from the device clock, converted with a calibration run. Host ns come from
// steady_clock around the timed pass only.

#include <sys/mman.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include <pxl/pxl.hpp>
#include <pxl/signal_handler.hpp>

#include "tt_common.hpp"

namespace {

struct Options {
    bool device = false, host = false, thp = false;
    std::vector<std::uint64_t> region_bytes{1ULL << 28, 1ULL << 30};
    std::vector<std::uint64_t> regions{8, 16, 32, 64, 128};
    std::vector<std::uint64_t> lines_per_region{1};
    std::uint64_t lines = 0;              // non-zero: hold K fixed, L = K / R
    std::vector<std::uint64_t> orders{tt::kOrderRandom};
    std::vector<std::uint64_t> layouts{tt::kLayoutClump};
    std::uint64_t accesses = 4'000'000;
    int reps = 3;
    std::uint64_t buffer_gb = 64;
    std::uint64_t host_thp_cap_gb = 16;   // THP commits 2 MB per touched region; stay in budget
};

std::vector<std::uint64_t> parse_list(const char* s) {
    std::vector<std::uint64_t> v;
    std::stringstream ss(s);
    std::string tok;
    while (std::getline(ss, tok, ',')) v.push_back(std::stoull(tok));
    return v;
}

bool pow2(std::uint64_t x) { return x && !(x & (x - 1)); }

const char* order_name(std::uint64_t o) { return o == tt::kOrderRoundRobin ? "rr" : "random"; }
const char* layout_name(std::uint64_t l) { return l == tt::kLayoutClump ? "clump" : "spread"; }

// Every (S, R, L) the sweep will actually run, in order.
std::vector<tt::Config> plan(const Options& o, std::uint64_t buffer_bytes) {
    std::vector<tt::Config> v;
    for (std::uint64_t layout : o.layouts)
      for (std::uint64_t order : o.orders)
        for (std::uint64_t s : o.region_bytes)
            for (std::uint64_t r : o.regions) {
                std::vector<std::uint64_t> ls;
                if (o.lines) {
                    if (o.lines % r) continue;                       // K must divide evenly
                    const std::uint64_t l = o.lines / r;
                    if (!l || !pow2(l)) continue;
                    ls.push_back(l);
                } else {
                    ls = o.lines_per_region;
                }
                for (std::uint64_t l : ls) {
                    if (!pow2(l) || l * tt::kLine > s) continue;     // need >= one line per slot
                    tt::Config c{s, r, l, order, layout, o.accesses, 0, 0, 0, 0, {}};
                    if (tt::span_bytes(c) > buffer_bytes) continue;
                    v.push_back(c);
                }
            }
    return v;
}

std::uint64_t expected_sum(const tt::Config& c) {
    const tt::Pattern p = tt::make_pattern(c);
    std::uint64_t s = c.seed, sum = 0;
    if (c.order == tt::kOrderRoundRobin)
        for (std::uint64_t a = 0; a < c.accesses; ++a) sum += tt::value_at(tt::offset_of(p, tt::pick_rr(p, a, s)));
    else
        for (std::uint64_t a = 0; a < c.accesses; ++a) sum += tt::value_at(tt::offset_of(p, tt::pick_random(p, s)));
    return sum;
}

// Distinct 2 MB pages the target lines fall in — what a THP run actually commits.
std::uint64_t huge_pages_touched(const tt::Config& c) {
    const tt::Pattern p = tt::make_pattern(c);
    std::vector<std::uint64_t> v;
    v.reserve(tt::lines(c));
    for (std::uint64_t j = 0; j < tt::lines(c); ++j) v.push_back(tt::offset_of(p, j) >> 21);
    std::sort(v.begin(), v.end());
    return static_cast<std::uint64_t>(std::unique(v.begin(), v.end()) - v.begin());
}

long anon_huge_mb() {
    std::ifstream f("/proc/self/smaps_rollup");
    std::string key; long kb;
    while (f >> key) {
        if (key == "AnonHugePages:") { f >> kb; return kb / 1024; }
        std::string rest; std::getline(f, rest);
    }
    return -1;
}

void emit(const char* platform, const char* pages, const tt::Config& c, int rep, double ns,
          const char* cpa, long huge_mb, bool ok) {
    std::printf("%s,%s,%s,%s,%llu,%llu,%llu,%llu,%llu,%llu,%d,%.3f,%s,", platform, pages,
                layout_name(c.layout), order_name(c.order),
                (unsigned long long)c.region_bytes, (unsigned long long)c.regions,
                (unsigned long long)c.lines_per_region, (unsigned long long)tt::lines(c),
                (unsigned long long)(tt::span_bytes(c) >> 20), (unsigned long long)c.accesses, rep, ns, cpa);
    if (huge_mb >= 0) std::printf("%ld", huge_mb);
    std::printf(",%d\n", ok);
    std::fflush(stdout);
}

// ------------------------------------------------------------------------------------------
int run_host(const Options& o) {
    const std::uint64_t bytes = o.buffer_gb << 30;
    // NORESERVE: only lines actually touched commit memory, so a 64 GB virtual span costs a
    // few pages with 4 KB pages. With THP every touched 2 MB region commits in full.
    void* p = ::mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (p == MAP_FAILED) { std::perror("mmap"); return 1; }
    ::madvise(p, bytes, o.thp ? MADV_HUGEPAGE : MADV_NOHUGEPAGE);
    auto* buf = static_cast<std::uint8_t*>(p);
    const char* pages = o.thp ? "thp" : "4k";

    // Spin ~2 s first so CPU frequency scaling has settled; otherwise the first patterns run on
    // a cold core and look slower for reasons unrelated to memory.
    {
        std::uint64_t s = 1, sink = 0;
        const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (std::chrono::steady_clock::now() < end) for (int i = 0; i < 100000; ++i) sink += tt::next(s);
        (void)sink;
    }

    for (tt::Config c : plan(o, bytes)) {
        if (o.thp && (huge_pages_touched(c) << 21) > (o.host_thp_cap_gb << 30)) continue;
        const tt::Pattern pat = tt::make_pattern(c);
        ::madvise(p, bytes, MADV_DONTNEED);   // start each pattern from unpopulated memory
        for (std::uint64_t j = 0; j < tt::lines(c); ++j) {
            const std::uint64_t off = tt::offset_of(pat, j);
            const std::uint64_t v = tt::value_at(off);
            std::memcpy(buf + off, &v, sizeof v);
        }
        for (int rep = 0; rep < o.reps; ++rep) {
            c.seed = 0x2545F4914F6CDD1DULL + rep;
            const bool rr = c.order == tt::kOrderRoundRobin;
            std::uint64_t s = c.seed, warm = 0;
            for (std::uint64_t a = 0; a < c.accesses; ++a) {
                const std::uint64_t j = rr ? tt::pick_rr(pat, a, s) : tt::pick_random(pat, s);
                std::uint64_t v; std::memcpy(&v, buf + tt::offset_of(pat, j), 8); warm += v;
            }
            (void)warm;
            s = c.seed;
            std::uint64_t sum = 0;
            const auto t0 = std::chrono::steady_clock::now();
            if (rr) {
                for (std::uint64_t a = 0; a < c.accesses; ++a) {
                    std::uint64_t v; std::memcpy(&v, buf + tt::offset_of(pat, tt::pick_rr(pat, a, s)), 8); sum += v;
                }
            } else {
                for (std::uint64_t a = 0; a < c.accesses; ++a) {
                    std::uint64_t v; std::memcpy(&v, buf + tt::offset_of(pat, tt::pick_random(pat, s)), 8); sum += v;
                }
            }
            const auto t1 = std::chrono::steady_clock::now();
            const double ns = std::chrono::duration<double, std::nano>(t1 - t0).count() / c.accesses;
            emit("host", pages, c, rep, ns, "", anon_huge_mb(), sum == expected_sum(c));
        }
    }
    ::munmap(p, bytes);
    return 0;
}

// ------------------------------------------------------------------------------------------
int run_device(const Options& o) {
    pxl::registerSignalHandler({SIGINT, SIGTERM, SIGSEGV, SIGABRT, SIGBUS, SIGFPE, SIGILL});
    if (pxl::runtime::getNumDevice() == 0) { std::fprintf(stderr, "no PXL device\n"); return 1; }
    auto* ctx = pxl::runtime::createContext(0);
    if (!ctx) { std::fprintf(stderr, "createContext failed\n"); return 1; }

    const std::string mubin = "mu_kernel/tt_kernel.mubin";
    auto module = pxl::kernel::createModule(mubin.c_str());
    if (!module) { std::fprintf(stderr, "cannot load %s\n", mubin.c_str()); return 1; }
    auto* job = ctx->createJob();
    if (!job) { std::fprintf(stderr, "createJob failed\n"); return 1; }
    job->load(module);
    auto* f_init = module->createFunction("tt_init");
    auto* f_acc = module->createFunction("tt_access");
    auto* f_cal = module->createFunction("tt_calibrate");
    if (!f_init || !f_acc || !f_cal) { std::fprintf(stderr, "createFunction failed\n"); return 1; }
    // One task: the per-access cost is measured on a single MU thread, with no contention.
    auto* m_init = job->buildMap(f_init, 1);
    auto* m_acc = job->buildMap(f_acc, 1);
    auto* m_cal = job->buildMap(f_cal, 1);
    if (!m_init || !m_acc || !m_cal) { std::fprintf(stderr, "buildMap failed\n"); return 1; }

    const std::uint64_t bytes = o.buffer_gb << 30;
    auto* buf = static_cast<std::uint8_t*>(ctx->memAlloc(bytes));
    auto* cfg = static_cast<tt::Config*>(ctx->memAlloc(sizeof(tt::Config)));
    if (!buf || !cfg) { std::fprintf(stderr, "memAlloc of %llu GB failed\n", (unsigned long long)o.buffer_gb); return 1; }

    auto dispatch = [&](pxl::runtime::Map* m, tt::Config& c, bool with_buf) -> bool {
        if (ctx->copyToDevice(cfg, &c, sizeof c) != pxl::PxlResult::Success) return false;
        const auto r = with_buf ? m->execute(buf, cfg) : m->execute(cfg);
        if (r != pxl::PxlResult::Success || m->synchronize() != pxl::PxlResult::Success) return false;
        return ctx->copyFromDevice(&c, cfg, sizeof c) == pxl::PxlResult::Success;
    };

    // Calibration: a long memory-free spin, timed by the host, converts cycles to ns.
    tt::Config cal{64, 1, 1, tt::kOrderRandom, tt::kLayoutSpread, 400'000'000ULL, 0x9E3779B97F4A7C15ULL, 0, 0, 0, {}};
    const auto c0 = std::chrono::steady_clock::now();
    if (!dispatch(m_cal, cal, false) || cal.done != 3 || cal.cycles == 0) { std::fprintf(stderr, "calibration failed\n"); return 1; }
    const double ns_per_cycle = std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - c0).count() / cal.cycles;
    std::fprintf(stderr, "calibration: %llu cycles for %llu spins, ~%.4f ns/cycle (host wall incl. dispatch)\n",
                 (unsigned long long)cal.cycles, (unsigned long long)cal.accesses, ns_per_cycle);

    int rc = 0;
    for (tt::Config c : plan(o, bytes)) {
        if (!dispatch(m_init, c, true) || c.done != 1) { std::fprintf(stderr, "init failed\n"); rc = 1; break; }
        for (int rep = 0; rep < o.reps; ++rep) {
            c.seed = 0x2545F4914F6CDD1DULL + rep;
            c.cycles = c.sum = c.done = 0;
            if (!dispatch(m_acc, c, true) || c.done != 2) { std::fprintf(stderr, "access failed\n"); rc = 1; break; }
            const bool ok = c.sum == expected_sum(c);
            const double cpa = double(c.cycles) / c.accesses;
            char cpa_s[32];
            std::snprintf(cpa_s, sizeof cpa_s, "%.4f", cpa);
            emit("device", "none", c, rep, cpa * ns_per_cycle, cpa_s, -1, ok);
            if (!ok) { std::fprintf(stderr, "checksum mismatch: device read wrong data, stopping\n"); rc = 1; break; }
        }
        if (rc) break;
    }
    ctx->memFree(cfg);
    ctx->memFree(buf);
    ctx->destroyJob(job);
    pxl::runtime::destroyContext(ctx);
    return rc;
}

}  // namespace

int main(int argc, char** argv) {
    Options o;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto val = [&]() -> const char* { if (i + 1 >= argc) { std::fprintf(stderr, "%s needs a value\n", a.c_str()); std::exit(2); } return argv[++i]; };
        if (a == "--device") o.device = true;
        else if (a == "--host") o.host = true;
        else if (a == "--thp") o.thp = true;
        else if (a == "--region-bytes") o.region_bytes = parse_list(val());
        else if (a == "--regions") o.regions = parse_list(val());
        else if (a == "--lines-per-region") o.lines_per_region = parse_list(val());
        else if (a == "--lines") o.lines = std::stoull(val());
        else if (a == "--order") {
            const std::string v = val();
            if (v == "random") o.orders = {tt::kOrderRandom};
            else if (v == "rr") o.orders = {tt::kOrderRoundRobin};
            else if (v == "both") o.orders = {tt::kOrderRandom, tt::kOrderRoundRobin};
            else { std::fprintf(stderr, "--order must be random, rr or both\n"); return 2; }
        }
        else if (a == "--layout") {
            const std::string v = val();
            if (v == "spread") o.layouts = {tt::kLayoutSpread};
            else if (v == "clump") o.layouts = {tt::kLayoutClump};
            else if (v == "both") o.layouts = {tt::kLayoutSpread, tt::kLayoutClump};
            else { std::fprintf(stderr, "--layout must be spread, clump or both\n"); return 2; }
        }
        else if (a == "--accesses") o.accesses = std::stoull(val());
        else if (a == "--reps") o.reps = std::atoi(val());
        else if (a == "--buffer-gb") o.buffer_gb = std::stoull(val());
        else if (a == "--host-thp-cap-gb") o.host_thp_cap_gb = std::stoull(val());
        else { std::fprintf(stderr, "unknown option %s\n", a.c_str()); return 2; }
    }
    // Powers of two everywhere keep offset_of() division- and branch-free for every pattern.
    for (auto s : o.region_bytes) if (s < tt::kLine || !pow2(s)) { std::fprintf(stderr, "--region-bytes must be powers of two >= 64\n"); return 2; }
    for (auto r : o.regions) if (!pow2(r)) { std::fprintf(stderr, "--regions must be powers of two\n"); return 2; }
    for (auto l : o.lines_per_region) if (!pow2(l)) { std::fprintf(stderr, "--lines-per-region must be powers of two\n"); return 2; }
    if (o.lines && !pow2(o.lines)) { std::fprintf(stderr, "--lines must be a power of two\n"); return 2; }
    if (o.device == o.host) { std::fprintf(stderr, "choose exactly one of --device / --host\n"); return 2; }
    if (plan(o, o.buffer_gb << 30).empty()) { std::fprintf(stderr, "no valid configuration in this sweep (check --lines / --buffer-gb)\n"); return 2; }
    std::printf("platform,pages,layout,order,region_bytes,regions,lines_per_region,lines,span_mb,accesses,rep,"
                "ns_per_access,cycles_per_access,anon_huge_mb,sum_ok\n");
    return o.device ? run_device(o) : run_host(o);
}
