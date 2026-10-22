// d2h - device->host copy: throughput and correctness vs chunk size (MX1 in devdax).
//
// The device fills buf[i] = i (bw_fill kernel, 3048 tasks). After the kernel has finished, the
// host copies the whole buffer back in chunks of C bytes with T threads (chunk k goes to thread
// k mod T), via
//   --api dma     ctx->copyFromDevice, one shared context or one per thread (--ctx shared|thread)
//   --api direct  pxl::flushHostCache + memcpy through the device-memory mapping (needs root)
// Before every timed copy the host buffer is poisoned with ~0; afterwards every word is checked
// (wrong words, and how many of them are zero). Chunks are powers of two <= 1 GiB and the device
// buffer is 1 GiB-aligned, so no copy crosses a 1 GiB device address (see README DMA caveats).
// No kernel runs while copying: copyFromDevice overlapped with launches froze the host (2026-10-05).
//
// CSV: api,ctx,threads,chunk_bytes,gb,iter,ms,gb_per_s,bad_words,zero_words,failed_calls
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

#include "pxl/pxl.hpp"
#include "scan_common.hpp"

namespace {

using clk = std::chrono::steady_clock;
constexpr std::size_t kGiB = std::size_t(1) << 30;

double ms_since(clk::time_point t) { return std::chrono::duration<double, std::milli>(clk::now() - t).count(); }

template <typename Body>
void parallel(int threads, Body body) {
    std::vector<std::thread> th;
    for (int t = 0; t < threads; ++t) th.emplace_back(body, t);
    for (auto& x : th) x.join();
}

}  // namespace

int main(int argc, char** argv) {
    std::string api = "dma", ctx_mode = "shared";
    double gb = 8;
    int threads = 4, iters = 2;
    std::vector<std::size_t> chunks = {std::size_t(64) << 10, std::size_t(256) << 10, std::size_t(1) << 20,
                                       std::size_t(4) << 20, std::size_t(16) << 20, std::size_t(64) << 20,
                                       std::size_t(256) << 20};
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto v = [&]() -> const char* { if (i + 1 >= argc) { std::fprintf(stderr, "%s needs a value\n", a.c_str()); std::exit(2); } return argv[++i]; };
        if (a == "--api") api = v();
        else if (a == "--ctx") ctx_mode = v();
        else if (a == "--gb") gb = std::atof(v());
        else if (a == "--threads") threads = std::atoi(v());
        else if (a == "--iters") iters = std::atoi(v());
        else if (a == "--chunks") {   // comma-separated KiB, e.g. 64,1024,16384
            chunks.clear();
            std::string s = v();
            for (std::size_t p = 0; p < s.size();) {
                const std::size_t q = s.find(',', p);
                chunks.push_back(std::stoull(s.substr(p, q - p)) << 10);
                p = q == std::string::npos ? s.size() : q + 1;
            }
        } else if (a == "--header") { std::printf("api,ctx,threads,chunk_bytes,gb,iter,ms,gb_per_s,bad_words,zero_words,failed_calls\n"); return 0; }
        else { std::fprintf(stderr, "unknown option %s\n", a.c_str()); return 2; }
    }
    if (api != "dma" && api != "direct") { std::fprintf(stderr, "--api dma|direct\n"); return 2; }
    if (ctx_mode != "shared" && ctx_mode != "thread") { std::fprintf(stderr, "--ctx shared|thread\n"); return 2; }
    if (api == "direct" && ::geteuid() != 0) { std::fprintf(stderr, "--api direct needs root\n"); return 1; }
    for (std::size_t c : chunks)
        if (c == 0 || (c & (c - 1)) || c > kGiB) { std::fprintf(stderr, "chunk %zu: must be a power of two <= 1 GiB\n", c); return 2; }

    pxl::registerSignalHandler({SIGINT, SIGTERM, SIGSEGV, SIGABRT, SIGBUS, SIGFPE, SIGILL});
    if (pxl::runtime::getNumDevice() == 0) { std::fprintf(stderr, "no PXL device\n"); return 1; }
    auto* ctx = pxl::runtime::createContext(0);
    if (!ctx) { std::fprintf(stderr, "createContext failed\n"); return 1; }
    auto module = pxl::kernel::createModule("mu_kernel/scan_filter_kernel.mubin");
    auto* job = module ? ctx->createJob() : nullptr;
    if (!job) { std::fprintf(stderr, "module/createJob failed\n"); return 1; }
    job->load(module);
    auto* f_fill = module->createFunction("bw_fill");
    constexpr std::uint32_t kFillTasks = 3048;
    auto* m_fill = f_fill ? job->buildMap(f_fill, kFillTasks) : nullptr;
    if (!m_fill) { std::fprintf(stderr, "buildMap failed\n"); return 1; }

    const std::size_t bytes = static_cast<std::size_t>(gb * kGiB + kGiB - 1) / kGiB * kGiB;
    const std::uint64_t words = bytes / 8;
    auto* dbuf = static_cast<std::uint64_t*>(ctx->memAlloc(bytes));
    auto* cfg = static_cast<scanfilter::Config*>(ctx->memAlloc(kGiB));
    if (!dbuf || !cfg || reinterpret_cast<std::uintptr_t>(dbuf) % kGiB) {
        std::fprintf(stderr, "device allocation failed or not 1 GiB-aligned\n"); return 1;
    }
    scanfilter::Config c{};
    c.rows = words; c.par = kFillTasks;
    if (ctx->copyToDevice(cfg, &c, sizeof c) != pxl::PxlResult::Success) { std::fprintf(stderr, "copyToDevice failed\n"); return 1; }
    // Fill twice and time the second: device write bandwidth with plain stores (first touch excluded).
    for (int f = 0; f < 2; ++f) {
        const auto tf = clk::now();
        if (m_fill->execute(dbuf, cfg) != pxl::PxlResult::Success || m_fill->synchronize() != pxl::PxlResult::Success) {
            std::fprintf(stderr, "device fill failed\n"); return 1;
        }
        if (f == 1) std::fprintf(stderr, "# device fill (plain stores, 3048 tasks): %.1f ms = %.1f GB/s written\n",
                                 ms_since(tf), bytes / 1e6 / ms_since(tf));
    }

    // Copy contexts: the shared one, or one more per thread.
    std::vector<pxl::runtime::Context*> cctx(threads, ctx);
    if (api == "dma" && ctx_mode == "thread")
        for (int t = 0; t < threads; ++t)
            if (!(cctx[t] = pxl::runtime::createContext(0))) { std::fprintf(stderr, "createContext %d failed\n", t); return 1; }

    auto* hbuf = static_cast<std::uint64_t*>(std::aligned_alloc(2 << 20, bytes));
    if (!hbuf) { std::fprintf(stderr, "host allocation failed\n"); return 1; }
    std::fprintf(stderr, "# d2h api=%s ctx=%s threads=%d buffer=%.1f GiB (device %p)\n", api.c_str(),
                 api == "dma" ? ctx_mode.c_str() : "-", threads, bytes / double(kGiB), static_cast<void*>(dbuf));

    int rc = 0;
    for (std::size_t chunk : chunks) {
        const std::size_t nchunks = bytes / chunk;
        for (int it = 0; it < iters; ++it) {
            parallel(32, [&](int t) {   // poison (untimed)
                const std::size_t lo = bytes / 32 * t, hi = t == 31 ? bytes : bytes / 32 * (t + 1);
                std::memset(reinterpret_cast<char*>(hbuf) + lo, 0xff, hi - lo);
            });
            std::atomic<long> failed{0};
            const auto t0 = clk::now();
            parallel(threads, [&](int t) {
                for (std::size_t k = static_cast<std::size_t>(t); k < nchunks; k += static_cast<std::size_t>(threads)) {
                    char* dst = reinterpret_cast<char*>(hbuf) + k * chunk;
                    char* src = reinterpret_cast<char*>(dbuf) + k * chunk;
                    if (api == "dma") {
                        if (cctx[t]->copyFromDevice(dst, src, chunk) != pxl::PxlResult::Success) ++failed;
                    } else {
                        pxl::flushHostCache(src, chunk);
                        std::memcpy(dst, src, chunk);
                    }
                }
            });
            const double ms = ms_since(t0);
            std::atomic<std::uint64_t> bad{0}, zero{0};
            parallel(32, [&](int t) {
                const std::uint64_t lo = words / 32 * t, hi = t == 31 ? words : words / 32 * (t + 1);
                std::uint64_t b = 0, z = 0;
                for (std::uint64_t i = lo; i < hi; ++i)
                    if (hbuf[i] != i) { ++b; z += hbuf[i] == 0; }
                bad += b; zero += z;
            });
            if (bad && it == 0) {   // which GiB of the buffer is wrong, and what is in it
                std::fprintf(stderr, "#   chunk %zu: per GiB bad words (first wrong value):", chunk);
                for (std::size_t g = 0; g < bytes / kGiB; ++g) {
                    std::uint64_t b = 0, first = 0; bool seen = false;
                    for (std::uint64_t i = g * (kGiB / 8); i < (g + 1) * (kGiB / 8); ++i)
                        if (hbuf[i] != i) { if (!seen) { first = hbuf[i]; seen = true; } ++b; }
                    std::fprintf(stderr, " [%zu] %llu", g, (unsigned long long)b);
                    if (seen) std::fprintf(stderr, " (0x%llx)", (unsigned long long)first);
                }
                std::fprintf(stderr, "\n");
            }
            std::printf("%s,%s,%d,%zu,%.3f,%d,%.3f,%.2f,%llu,%llu,%ld\n", api.c_str(), api == "dma" ? ctx_mode.c_str() : "-",
                        threads, chunk, bytes / 1e9, it, ms, bytes / 1e6 / ms, (unsigned long long)bad.load(),
                        (unsigned long long)zero.load(), failed.load());
            std::fflush(stdout);
            if (bad || failed) rc = 1;
        }
    }
    std::free(hbuf);
    ctx->memFree(cfg); ctx->memFree(dbuf);
    return rc;
}
