#pragma once

// Mode 5 (host tiered): restricted host + CXL expander in system-ram mode.
// A fixed fraction of every buffer is placed on the CXL NUMA node, the rest on host DRAM,
// with mbind in 2 MB chunks before first touch (same scheme as tpch_scan_filter host_split).

namespace tiered {
// Parsed in every build; only used by MODE_HOST_TIERED.
struct Options {
    double cxl_pct = 0.0;           // 0-100: share of every buffer placed on the CXL node
    int cxl_node = 2;               // MX1 in system-ram mode
    int dram_node = 0;
    bool interleave = true;         // true: CXL chunks spread evenly; false: last chunks of each buffer on CXL
};
} // namespace tiered

#if defined(MODE_HOST_TIERED)

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <vector>

#include <numa.h>
#include <numaif.h>
#include <sys/mman.h>

namespace tiered {

constexpr std::size_t kChunk = std::size_t(2) << 20;

// A buffer whose 2 MB chunks are bound to DRAM or CXL. Pages are not touched here.
class PlacedBuffer {
public:
    PlacedBuffer() = default;
    PlacedBuffer(const PlacedBuffer&) = delete;
    PlacedBuffer& operator=(const PlacedBuffer&) = delete;
    PlacedBuffer(PlacedBuffer&& o) noexcept { *this = std::move(o); }
    PlacedBuffer& operator=(PlacedBuffer&& o) noexcept {
        if (this != &o) {
            reset();
            _map_base = o._map_base; _map_len = o._map_len; _base = o._base; _bytes = o._bytes;
            o._map_base = nullptr; o._map_len = 0; o._base = nullptr; o._bytes = 0;
        }
        return *this;
    }
    ~PlacedBuffer() { reset(); }

    PlacedBuffer(std::size_t bytes, const Options& opt) : _bytes(bytes) {
        const std::size_t len = (bytes + kChunk - 1) / kChunk * kChunk;
        _map_len = len + kChunk;
        _map_base = ::mmap(nullptr, _map_len, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
        if (_map_base == MAP_FAILED) { _map_base = nullptr; throw std::runtime_error("tiered: mmap failed"); }
        _base = reinterpret_cast<char*>((reinterpret_cast<std::uintptr_t>(_map_base) + kChunk - 1) & ~(kChunk - 1));
        ::madvise(_base, len, MADV_NOHUGEPAGE);

        const double frac = opt.cxl_pct / 100.0;
        const std::size_t n = len / kChunk;
        const std::size_t ncxl = static_cast<std::size_t>(n * frac + 0.5);
        auto on_cxl = [&](std::size_t c) {
            return opt.interleave ? std::uint64_t((c + 1) * frac) > std::uint64_t(c * frac) : c >= n - ncxl;
        };
        for (std::size_t c = 0; c < n;) {   // bind runs of consecutive chunks with the same node
            const bool cxl = on_cxl(c);
            std::size_t e = c + 1;
            while (e < n && on_cxl(e) == cxl) ++e;
            unsigned long mask = 1UL << (cxl ? opt.cxl_node : opt.dram_node);
            if (::mbind(_base + c * kChunk, (e - c) * kChunk, MPOL_BIND, &mask, sizeof(mask) * 8, 0) != 0) {
                std::perror("mbind");
                reset();
                throw std::runtime_error("tiered: mbind failed");
            }
            c = e;
        }
    }

    void reset() {
        if (_map_base) ::munmap(_map_base, _map_len);
        _map_base = nullptr; _map_len = 0; _base = nullptr; _bytes = 0;
    }

    char* data() const noexcept { return _base; }
    std::size_t size() const noexcept { return _bytes; }
    explicit operator bool() const noexcept { return _base != nullptr; }

private:
    void* _map_base{nullptr};
    std::size_t _map_len{0};
    char* _base{nullptr};
    std::size_t _bytes{0};
};

// Refuses to run unless the CXL node has memory (dax device in system-ram mode).
inline void check_nodes(const Options& opt) {
    if (numa_available() < 0) throw std::runtime_error("tiered: libnuma unavailable");
    if (opt.cxl_pct > 0.0 && numa_node_size64(opt.cxl_node, nullptr) <= 0) {
        throw std::runtime_error("tiered: node " + std::to_string(opt.cxl_node) +
                                 " reports no memory - put the dax device in system-ram mode first");
    }
}

// Where the pages actually landed: one page per 2 MB chunk, queried with move_pages.
inline void report_placement(const char* name, const void* p, std::size_t bytes, const Options& opt) {
    std::vector<void*> pages;
    for (std::size_t off = 0; off < bytes; off += kChunk) pages.push_back(const_cast<char*>(static_cast<const char*>(p)) + off);
    if (pages.empty()) return;
    std::vector<int> status(pages.size(), -1);
    if (::move_pages(0, pages.size(), pages.data(), nullptr, status.data(), 0) != 0) { std::perror("move_pages"); return; }
    std::size_t ndram = 0, ncxl = 0, other = 0;
    for (int st : status) {
        if (st == opt.dram_node) ++ndram; else if (st == opt.cxl_node) ++ncxl; else ++other;
    }
    const double gb = bytes / 1e9, tot = static_cast<double>(pages.size());
    std::fprintf(stderr, "# placement %-14s %7.2f GB: DRAM %5.1f%%  CXL %5.1f%%%s\n", name, gb,
                 100.0 * ndram / tot, 100.0 * ncxl / tot,
                 other ? "  WARNING: pages on other nodes or not present" : "");
}

} // namespace tiered

#endif // MODE_HOST_TIERED
