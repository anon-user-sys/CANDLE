// bw: aggregate read bandwidth of the three memory paths the scan uses, without the scan.
//
//   --path host      host threads read host DRAM               (bind with numactl)
//   --path hostdev   host threads read MX1 device memory directly over CXL.mem (devdax, root)
//   --path device    MU tasks read device memory                (devdax), --prefetch W optional
//   --path flush     host threads pxl::flushHostCache their slice of device memory (root);
//                    --state absent|clean|dirty sets what the CPU cache holds before the flush:
//                    absent = nothing (flushed already), clean = host just read it,
//                    dirty = host just wrote it (the device then re-reads and checks the sum)
//
// Each worker sums its slice of uint64 words (buffer[i] = i), so every run is checked against
// n(n-1)/2. Device buffers come from the same 1 GiB-aligned allocation as the scan and are filled
// by a device kernel (no host stores, no DMA). Host threads are started before the clock and
// released together, so thread creation is not timed.
//
// CSV: path,gb,par,prefetch,iter,ms,gb_per_s,ok

#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include <pxl/pxl.hpp>
#include <pxl/signal_handler.hpp>

#include "scan_common.hpp"

using clk = std::chrono::steady_clock;

namespace {

struct Opt {
    std::string path = "host", state = "absent";
    double gb = 16.0;
    std::uint64_t par = 64;
    int iters = 5, prefetch = 0;
};

constexpr std::size_t kGiB = std::size_t(1) << 30;

void emit(const Opt& o, int it, double ms, std::uint64_t bytes, bool ok) {
    const std::string label = o.path == "flush" ? "flush_" + o.state : o.path;
    std::printf("%s,%.2f,%llu,%d,%d,%.3f,%.2f,%d\n", label.c_str(), bytes / 1e9, (unsigned long long)o.par,
                o.prefetch, it, ms, bytes / 1e6 / ms, ok);
    std::fflush(stdout);
}

std::uint64_t expected_sum(std::uint64_t n) {   // sum of 0..n-1, mod 2^64
    return (n % 2 == 0) ? (n / 2) * (n - 1) : n * ((n - 1) / 2);
}

std::uint64_t sum_slice(const std::uint64_t* buf, std::uint64_t lo, std::uint64_t hi) {
    std::uint64_t s[8] = {0, 0, 0, 0, 0, 0, 0, 0};
    std::uint64_t i = lo;
    for (; i + 8 <= hi; i += 8)
        for (int k = 0; k < 8; ++k) s[k] += buf[i + k];
    for (; i < hi; ++i) s[0] += buf[i];
    return s[0] + s[1] + s[2] + s[3] + s[4] + s[5] + s[6] + s[7];
}

// Host threads read `buf`; returns elapsed ms from a common release to the last finish.
double host_read(const std::uint64_t* buf, std::uint64_t words, std::uint64_t par, std::uint64_t& total) {
    std::vector<std::uint64_t> sums(par * 8, 0);
    std::atomic<std::uint64_t> ready{0};
    std::atomic<bool> go{false};
    std::vector<std::thread> th;
    for (std::uint64_t t = 0; t < par; ++t)
        th.emplace_back([&, t] {
            std::uint64_t lo, hi; scanfilter::slice(words, par, t, lo, hi);
            ready.fetch_add(1);
            while (!go.load(std::memory_order_acquire)) {}
            sums[t * 8] = sum_slice(buf, lo, hi);
        });
    while (ready.load() < par) {}
    const auto t0 = clk::now();
    go.store(true, std::memory_order_release);
    for (auto& x : th) x.join();
    const double ms = std::chrono::duration<double, std::milli>(clk::now() - t0).count();
    total = 0;
    for (std::uint64_t t = 0; t < par; ++t) total += sums[t * 8];
    return ms;
}

// Runs body(t, lo, hi) on par threads released together; returns ms to the last finish.
template <typename Body>
double timed_parallel(std::uint64_t words, std::uint64_t par, Body body) {
    std::atomic<std::uint64_t> ready{0};
    std::atomic<bool> go{false};
    std::vector<std::thread> th;
    for (std::uint64_t t = 0; t < par; ++t)
        th.emplace_back([&, t] {
            std::uint64_t lo, hi; scanfilter::slice(words, par, t, lo, hi);
            ready.fetch_add(1);
            while (!go.load(std::memory_order_acquire)) {}
            body(t, lo, hi);
        });
    while (ready.load() < par) {}
    const auto t0 = clk::now();
    go.store(true, std::memory_order_release);
    for (auto& x : th) x.join();
    return std::chrono::duration<double, std::milli>(clk::now() - t0).count();
}

int run_host(const Opt& o) {
    const std::uint64_t words = static_cast<std::uint64_t>(o.gb * kGiB) / 8;
    const std::size_t bytes = (words * 8 + (2 << 20) - 1) & ~std::size_t((2 << 20) - 1);
    auto* buf = static_cast<std::uint64_t*>(std::aligned_alloc(2 << 20, bytes));
    if (!buf) { std::fprintf(stderr, "aligned_alloc failed\n"); return 1; }
    {   // parallel first touch so pages land where the numactl policy says
        std::vector<std::thread> th;
        for (std::uint64_t t = 0; t < o.par; ++t)
            th.emplace_back([&, t] { std::uint64_t lo, hi; scanfilter::slice(words, o.par, t, lo, hi);
                                     for (std::uint64_t i = lo; i < hi; ++i) buf[i] = i; });
        for (auto& x : th) x.join();
    }
    const std::uint64_t exp = expected_sum(words);
    for (int it = 0; it < o.iters; ++it) {
        std::uint64_t total = 0;
        const double ms = host_read(buf, words, o.par, total);
        emit(o, it, ms, words * 8, total == exp);
    }
    std::free(buf);
    return 0;
}

int run_dev(const Opt& o) {
    const bool host_reads = o.path == "hostdev";
    const bool flush = o.path == "flush";
    if ((host_reads || flush) && ::geteuid() != 0) { std::fprintf(stderr, "--path %s needs root (host access to device memory)\n", o.path.c_str()); return 1; }
    pxl::registerSignalHandler({SIGINT, SIGTERM, SIGSEGV, SIGABRT, SIGBUS, SIGFPE, SIGILL});
    if (pxl::runtime::getNumDevice() == 0) { std::fprintf(stderr, "no PXL device\n"); return 1; }
    auto* ctx = pxl::runtime::createContext(0);
    if (!ctx) { std::fprintf(stderr, "createContext failed\n"); return 1; }
    auto module = pxl::kernel::createModule("mu_kernel/scan_filter_kernel.mubin");
    auto* job = module ? ctx->createJob() : nullptr;
    if (!job) { std::fprintf(stderr, "module/createJob failed\n"); return 1; }
    job->load(module);
    auto* f_fill = module->createFunction("bw_fill");
    auto* f_read = module->createFunction("bw_read");
    // The device always fills; reads use par tasks (device) - fill with 3048 tasks either way.
    constexpr std::uint32_t kFillTasks = 3048;
    auto* m_fill = f_fill ? job->buildMap(f_fill, kFillTasks) : nullptr;
    // flush: the device read (checking dirty-flush visibility) always uses kFillTasks tasks.
    const std::uint32_t read_tasks = flush ? kFillTasks : static_cast<std::uint32_t>(o.par);
    auto* m_read = (!host_reads && f_read) ? job->buildMap(f_read, read_tasks) : nullptr;
    if (!m_fill || (!host_reads && !m_read)) { std::fprintf(stderr, "buildMap failed\n"); return 1; }

    const std::uint64_t words = static_cast<std::uint64_t>(o.gb * kGiB) / 8;
    const std::size_t bytes = (words * 8 + kGiB - 1) / kGiB * kGiB;
    const std::uint64_t stride = 8;
    auto* buf = static_cast<std::uint64_t*>(ctx->memAlloc(bytes));
    auto* sums = static_cast<std::uint64_t*>(ctx->memAlloc(kGiB));
    auto* cfg = static_cast<scanfilter::Config*>(ctx->memAlloc(kGiB));
    if (!buf || !sums || !cfg || reinterpret_cast<std::uintptr_t>(buf) % kGiB) {
        std::fprintf(stderr, "device allocation failed or not 1 GiB-aligned\n"); return 1;
    }
    scanfilter::Config c{words, kFillTasks, 0, 0, 0, 0, static_cast<std::uint64_t>(o.prefetch), 0, 0};
    if (ctx->copyToDevice(cfg, &c, sizeof c) != pxl::PxlResult::Success ||
        m_fill->execute(buf, cfg) != pxl::PxlResult::Success || m_fill->synchronize() != pxl::PxlResult::Success) {
        std::fprintf(stderr, "device fill failed\n"); return 1;
    }
    c.par = read_tasks;
    if (ctx->copyToDevice(cfg, &c, sizeof c) != pxl::PxlResult::Success) { std::fprintf(stderr, "copyToDevice failed\n"); return 1; }
    if (host_reads || flush) pxl::flushHostCache(buf, words * 8);   // drop any stale host lines (not timed)

    const std::uint64_t exp = expected_sum(words);
    std::vector<std::uint64_t> hs(std::max<std::uint64_t>(o.par, kFillTasks) * stride);
    int rc = 0;
    for (int it = 0; it < o.iters; ++it) {
        std::uint64_t total = 0;
        double ms;
        if (flush) {
            // Put the CPU cache into the requested state (not timed), then flush in parallel.
            const std::uint64_t salt = 0x9e3779b97f4a7c15ULL * static_cast<std::uint64_t>(it + 1);
            if (o.state == "clean") {
                std::uint64_t dummy = 0; host_read(buf, words, 64, dummy);
            } else if (o.state == "dirty") {
                timed_parallel(words, 64, [&](std::uint64_t, std::uint64_t lo, std::uint64_t hi) {
                    for (std::uint64_t i = lo; i < hi; ++i) buf[i] = i ^ salt;
                });
            }
            ms = timed_parallel(words, o.par, [&](std::uint64_t, std::uint64_t lo, std::uint64_t hi) {
                if (hi > lo) pxl::flushHostCache(buf + lo, (hi - lo) * 8);
            });
            if (o.state == "dirty") {   // the device must now see exactly what the host wrote
                std::uint64_t want = 0;
                for (std::uint64_t i = 0; i < words; ++i) want += i ^ salt;
                if (m_read->execute(buf, sums, cfg) != pxl::PxlResult::Success || m_read->synchronize() != pxl::PxlResult::Success ||
                    ctx->copyFromDevice(hs.data(), sums, std::min<std::size_t>(hs.size(), read_tasks * stride) * 8) != pxl::PxlResult::Success) {
                    std::fprintf(stderr, "device check failed\n"); rc = 1; break;
                }
                for (std::uint32_t t = 0; t < read_tasks; ++t) total += hs[t * stride];
                emit(o, it, ms, words * 8, total == want);
                if (total != want) { std::fprintf(stderr, "MISMATCH: device does not see the flushed host writes\n"); rc = 1; break; }
            } else {
                emit(o, it, ms, words * 8, true);
            }
            continue;
        }
        if (host_reads) {
            ms = host_read(buf, words, o.par, total);
            pxl::flushHostCache(buf, words * 8);            // next iteration reads CXL again, not L3
        } else {
            const auto t0 = clk::now();
            if (m_read->execute(buf, sums, cfg) != pxl::PxlResult::Success || m_read->synchronize() != pxl::PxlResult::Success) {
                std::fprintf(stderr, "read kernel failed\n"); rc = 1; break;
            }
            ms = std::chrono::duration<double, std::milli>(clk::now() - t0).count();
            if (ctx->copyFromDevice(hs.data(), sums, hs.size() * 8) != pxl::PxlResult::Success) { rc = 1; break; }
            for (std::uint64_t t = 0; t < o.par; ++t) total += hs[t * stride];
        }
        emit(o, it, ms, words * 8, total == exp);
        if (total != exp) { std::fprintf(stderr, "MISMATCH: sum %llu expected %llu\n", (unsigned long long)total, (unsigned long long)exp); rc = 1; break; }
    }
    ctx->memFree(cfg); ctx->memFree(sums); ctx->memFree(buf);
    if (m_read) job->destroyMap(m_read);
    job->destroyMap(m_fill);
    ctx->destroyJob(job);
    module->destroyFunction(f_read); module->destroyFunction(f_fill);
    pxl::kernel::destroyModule(module);
    pxl::runtime::destroyContext(ctx);
    return rc;
}

}  // namespace

int main(int argc, char** argv) {
    Opt o;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto v = [&]() -> const char* { if (i + 1 >= argc) { std::fprintf(stderr, "%s needs a value\n", a.c_str()); std::exit(2); } return argv[++i]; };
        if (a == "--path") o.path = v();
        else if (a == "--gb") o.gb = std::atof(v());
        else if (a == "--par") o.par = std::stoull(v());
        else if (a == "--iters") o.iters = std::atoi(v());
        else if (a == "--prefetch") o.prefetch = std::atoi(v());
        else if (a == "--state") o.state = v();
        else if (a == "--header") { std::printf("path,gb,par,prefetch,iter,ms,gb_per_s,ok\n"); return 0; }
        else { std::fprintf(stderr, "unknown option %s\n", a.c_str()); return 2; }
    }
    if (o.path != "host" && o.path != "hostdev" && o.path != "device" && o.path != "flush") { std::fprintf(stderr, "--path host|hostdev|device|flush\n"); return 2; }
    if (o.state != "absent" && o.state != "clean" && o.state != "dirty") { std::fprintf(stderr, "--state absent|clean|dirty\n"); return 2; }
    if (o.par == 0 || o.gb <= 0) { std::fprintf(stderr, "--par and --gb must be > 0\n"); return 2; }
    return o.path == "host" ? run_host(o) : run_dev(o);
}
