// Standalone TPC-H scan filter, extracted from the probe side of the 260920 Filter hash join.
//
//   SELECT l_orderkey FROM lineitem WHERE l_quantity <= threshold
//
//   --mode host       columns in host memory, host threads       (bind with numactl)
//   --mode host_cxl   columns on a CXL NUMA node, host threads    (numa_alloc_onnode; system-ram)
//   --mode device     columns in device memory, MU tasks          (PXL; devdax)
//   --mode host_split columns split between host DRAM (node 0) and CXL (--numa-node), host
//                     threads: --dram-frac F of every column in DRAM, the rest on CXL, placed
//                     with mbind in 2 MB chunks (--placement interleave = F/(1-F) chunk pattern so
//                     every thread sees the mix; block = first F of each column in DRAM). This is
//                     the restricted host using CXL as a memory expander (system-ram mode).
//
// Data is the real lineitem.tbl (l_orderkey = column 0, l_quantity = column 4). The first load
// parses the .tbl in parallel and writes a binary cache, later loads read the cache.
//
// Per iteration the columns are re-uploaded to the placement under test and timed as copy_ms
// (0 for --mode host), then scanned and timed as scan_ms. --upload memcpy reproduces the hash
// join (host memcpy + flushHostCache, needs root); --upload dma uses copyToDevice.
//
// Every iteration is validated against a reference computed once on the host by separate code:
// count + sum of passing row indices + sum of passing keys, and with --materialize 1 the output
// slices are read back and re-summed.
//
// CSV: label,mode,upload,rows,par,threshold,sel_pct,materialize,iter,copy_ms,scan_ms,
//      read_gb,gb_per_s,matches,ok

#include <fcntl.h>
#include <numa.h>
#include <numaif.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <immintrin.h>
#include <functional>
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

using namespace scanfilter;
using clk = std::chrono::steady_clock;
static double ms_since(clk::time_point t0) {
    return std::chrono::duration<double, std::milli>(clk::now() - t0).count();
}

namespace {

struct Opt {
    std::string mode = "host", label, tbl, bin, cache_dir = "cache", upload = "memcpy", placement = "interleave";
    double dram_frac = 1.0 / 3.0;
    std::string ret = "none";          // device: return the selected keys to host DRAM (serial|pipelined|chunked)
    std::uint64_t ret_steps = 16;      // chunked: launches per scan
    std::string ret_api = "direct";    // how readers copy: direct (CPU loads over CXL.mem) | dma (copyFromDevice)
    int write_lines = 0;               // device materialize: store whole 64 B lines (memcpyGlobal)
    int out_bytes = 8;                 // materialized element: 8 = l_orderkey, 4 = uint32 offset from the task's base key
    int ret_threads = 16;
    std::string ret_flush = "range";   // data invalidation per copied range: range | boundary | none
    std::uint64_t chunk_rows = 65536;
    std::uint64_t par = 128, threshold = 50;
    int debug = 0;
    int materialize = 0, iters = 3, verify = 1, numa_node = 2, flush_out = 0, scan = 1, prefetch = 0, prefetch_keys = 1;
};

struct Columns {
    std::vector<Key> keys;             // l_orderkey
    std::vector<std::uint8_t> qty;     // l_quantity
};

struct Expected { std::uint64_t n = 0, sum_idx = 0, sum_key = 0; };

unsigned load_threads() { return std::max(1u, std::min(64u, std::thread::hardware_concurrency())); }

// ---------------------------------------------------------------- loading

struct CacheHeader {
    char magic[8];
    std::uint64_t rows, src_size, src_mtime_ns;
};
constexpr char kMagic[8] = {'T', 'P', 'C', 'H', 'S', 'F', '0', '2'};   // 02: uint64 keys

std::string cache_path(const Opt& o, const std::string& tbl) {
    std::string name = tbl;
    for (char& c : name) if (c == '/' || c == '.') c = '_';
    return o.cache_dir + "/" + name + ".bin";
}

bool read_cache(const std::string& path, const struct stat& st, Columns& c) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    CacheHeader h{};
    bool ok = std::fread(&h, sizeof h, 1, f) == 1 && std::memcmp(h.magic, kMagic, 8) == 0 &&
              h.src_size == static_cast<std::uint64_t>(st.st_size) &&
              h.src_mtime_ns == static_cast<std::uint64_t>(st.st_mtim.tv_sec) * 1000000000ULL + st.st_mtim.tv_nsec;
    if (ok) {
        c.keys.resize(h.rows); c.qty.resize(h.rows);
        ok = std::fread(c.keys.data(), sizeof(Key), h.rows, f) == h.rows &&
             std::fread(c.qty.data(), 1, h.rows, f) == h.rows;
    }
    std::fclose(f);
    return ok;
}

void write_cache(const std::string& path, const struct stat& st, const Columns& c) {
    const std::string tmp = path + ".tmp";
    FILE* f = std::fopen(tmp.c_str(), "wb");
    if (!f) { std::fprintf(stderr, "warning: cannot write cache %s\n", tmp.c_str()); return; }
    CacheHeader h{};
    std::memcpy(h.magic, kMagic, 8);
    h.rows = c.keys.size();
    h.src_size = static_cast<std::uint64_t>(st.st_size);
    h.src_mtime_ns = static_cast<std::uint64_t>(st.st_mtim.tv_sec) * 1000000000ULL + st.st_mtim.tv_nsec;
    const bool ok = std::fwrite(&h, sizeof h, 1, f) == 1 &&
                    std::fwrite(c.keys.data(), sizeof(Key), h.rows, f) == h.rows &&
                    std::fwrite(c.qty.data(), 1, h.rows, f) == h.rows;
    if (std::fclose(f) != 0 || !ok || std::rename(tmp.c_str(), path.c_str()) != 0) {
        std::fprintf(stderr, "warning: cache write failed for %s\n", path.c_str());
        std::remove(tmp.c_str());
    }
}

// Parses one '|' line: column 0 = l_orderkey, column 4 = l_quantity ("17.00" -> 17), the same
// fields and truncation as TPCHTableReader::read in hash-join-xcena.
const char* parse_line(const char* p, const char* end, Key& key, std::uint8_t& qty) {
    std::uint64_t k = 0;
    while (p < end && *p >= '0' && *p <= '9') k = k * 10 + (*p++ - '0');
    for (int field = 0; field < 4 && p < end; ++field) {   // skip to column 4
        while (p < end && *p != '|' && *p != '\n') ++p;
        if (p < end && *p == '|') ++p;
    }
    std::uint64_t q = 0;
    while (p < end && *p >= '0' && *p <= '9') q = q * 10 + (*p++ - '0');
    key = static_cast<Key>(k);
    qty = static_cast<std::uint8_t>(q);
    const void* nl = std::memchr(p, '\n', end - p);
    return nl ? static_cast<const char*>(nl) + 1 : end;
}

bool parse_tbl(const std::string& tbl, const struct stat& st, Columns& c) {
    const int fd = ::open(tbl.c_str(), O_RDONLY);
    if (fd < 0) return false;
    const std::size_t size = static_cast<std::size_t>(st.st_size);
    auto* base = static_cast<const char*>(::mmap(nullptr, size, PROT_READ, MAP_PRIVATE, fd, 0));
    ::close(fd);
    if (base == MAP_FAILED) return false;
    ::madvise(const_cast<char*>(base), size, MADV_SEQUENTIAL);
    const char* end = base + size;

    // Chunk boundaries at line starts.
    const unsigned T = load_threads();
    std::vector<const char*> b(T + 1);
    b[0] = base; b[T] = end;
    for (unsigned t = 1; t < T; ++t) {
        const char* p = base + size * t / T;
        const void* nl = std::memchr(p, '\n', end - p);
        b[t] = nl ? static_cast<const char*>(nl) + 1 : end;
        if (b[t] < b[t - 1]) b[t] = b[t - 1];
    }

    // Pass 1: lines per chunk (a non-empty unterminated tail counts as a line).
    std::vector<std::uint64_t> lines(T + 1, 0);
    {
        std::vector<std::thread> th;
        for (unsigned t = 0; t < T; ++t)
            th.emplace_back([&, t] {
                std::uint64_t n = 0;
                for (const char* p = b[t]; p < b[t + 1];) {
                    const void* nl = std::memchr(p, '\n', b[t + 1] - p);
                    if (!nl) { ++n; break; }
                    const char* q = static_cast<const char*>(nl);
                    if (q > p && !(q - p == 1 && *p == '\r')) ++n;   // skip blank lines
                    p = q + 1;
                }
                lines[t + 1] = n;
            });
        for (auto& x : th) x.join();
    }
    for (unsigned t = 0; t < T; ++t) lines[t + 1] += lines[t];
    c.keys.resize(lines[T]); c.qty.resize(lines[T]);

    // Pass 2: parse into place.
    {
        std::vector<std::thread> th;
        for (unsigned t = 0; t < T; ++t)
            th.emplace_back([&, t] {
                std::uint64_t r = lines[t];
                for (const char* p = b[t]; p < b[t + 1];) {
                    if (*p == '\n' || (*p == '\r' && p + 1 < b[t + 1] && p[1] == '\n')) {
                        p += (*p == '\n') ? 1 : 2; continue;
                    }
                    p = parse_line(p, b[t + 1], c.keys[r], c.qty[r]);
                    ++r;
                }
            });
        for (auto& x : th) x.join();
    }
    ::munmap(const_cast<char*>(base), size);
    return true;
}

// Column file written by gen_cols.sh (same layout as the cache: header, keys, qty), used for
// scale factors whose text is never stored. No source file to check against.
bool read_bin(const std::string& path, Columns& c) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) { std::fprintf(stderr, "cannot open %s\n", path.c_str()); return false; }
    CacheHeader h{};
    bool ok = std::fread(&h, sizeof h, 1, f) == 1 && std::memcmp(h.magic, kMagic, 8) == 0;
    if (!ok) std::fprintf(stderr, "%s is not a %.8s column file\n", path.c_str(), kMagic);
    if (ok) {
        c.keys.resize(h.rows); c.qty.resize(h.rows);
        ok = std::fread(c.keys.data(), sizeof(Key), h.rows, f) == h.rows &&
             std::fread(c.qty.data(), 1, h.rows, f) == h.rows;
        if (!ok) std::fprintf(stderr, "%s is truncated (header says %llu rows)\n", path.c_str(), (unsigned long long)h.rows);
    }
    std::fclose(f);
    return ok;
}

bool load(const Opt& o, Columns& c) {
    if (!o.bin.empty()) {
        const auto t0 = clk::now();
        if (!read_bin(o.bin, c)) return false;
        std::fprintf(stderr, "# loaded %zu rows from %s in %.0f ms\n", c.keys.size(), o.bin.c_str(), ms_since(t0));
        return true;
    }
    struct stat st{};
    if (::stat(o.tbl.c_str(), &st) != 0) { std::fprintf(stderr, "cannot stat %s\n", o.tbl.c_str()); return false; }
    ::mkdir(o.cache_dir.c_str(), 0775);
    const std::string cp = cache_path(o, o.tbl);
    const auto t0 = clk::now();
    if (read_cache(cp, st, c)) {
        std::fprintf(stderr, "# loaded %zu rows from cache %s in %.0f ms\n", c.keys.size(), cp.c_str(), ms_since(t0));
        return true;
    }
    if (!parse_tbl(o.tbl, st, c)) { std::fprintf(stderr, "cannot read %s\n", o.tbl.c_str()); return false; }
    std::fprintf(stderr, "# parsed %zu rows from %s in %.0f ms\n", c.keys.size(), o.tbl.c_str(), ms_since(t0));
    write_cache(cp, st, c);
    return true;
}

// ---------------------------------------------------------------- reference + reporting

// Written independently of scan_slice so a bug there cannot validate itself.
Expected expected(const Columns& c, std::uint64_t threshold) {
    const unsigned T = load_threads();
    const std::uint64_t rows = c.qty.size();
    std::vector<Expected> part(T);
    std::vector<std::thread> th;
    for (unsigned t = 0; t < T; ++t)
        th.emplace_back([&, t] {
            Expected e;
            for (std::uint64_t i = rows * t / T; i < rows * (t + 1) / T; ++i) {
                const bool pass = c.qty[i] <= threshold;
                e.n += pass;
                e.sum_idx += pass ? i : 0;
                e.sum_key += pass ? c.keys[i] : 0;
            }
            part[t] = e;
        });
    for (auto& x : th) x.join();
    Expected e;
    for (const auto& p : part) { e.n += p.n; e.sum_idx += p.sum_idx; e.sum_key += p.sum_key; }
    return e;
}

void check_quantity_range(const Columns& c) {
    std::uint64_t bad = 0;
    for (std::uint8_t q : c.qty) bad += (q < 1 || q > 50);
    if (bad) std::fprintf(stderr, "warning: %llu rows have l_quantity outside [1,50] - selectivity mapping is off\n",
                          (unsigned long long)bad);
}

void emit(const Opt& o, std::uint64_t rows, int iter, double copy_ms, double scan_ms,
          std::uint64_t matches, bool ok, double ret_ms = 0.0, double e2e_ms = 0.0) {
    // Bytes the scan reads: every l_quantity + the l_orderkey of passing rows.
    const double read_gb = (rows * 1.0 + matches * static_cast<double>(sizeof(Key))) / 1e9;
    std::printf("%s,%s,%s,%llu,%llu,%llu,%.1f,%d,%d,%.3f,%.3f,%.3f,%.2f,%llu,%d,%.3f,%.3f\n",
                o.label.c_str(), o.mode.c_str(), (o.mode == "host" || o.mode == "host_split") ? "none" : o.upload.c_str(),
                (unsigned long long)rows, (unsigned long long)o.par, (unsigned long long)o.threshold,
                100.0 * static_cast<double>(o.threshold) / 50.0, o.materialize, iter,
                copy_ms, scan_ms, read_gb, read_gb / (scan_ms / 1000.0),
                (unsigned long long)matches, ok, ret_ms, e2e_ms);
    std::fflush(stdout);
}

bool same(const Expected& got, const Expected& exp) {
    return got.n == exp.n && got.sum_idx == exp.sum_idx && got.sum_key == exp.sum_key;
}

// Count + key sum over the materialized output slices.
// T = Key: slots hold keys. T = uint32_t: slots hold key - base[t] (frame of reference).
template <typename T>
bool check_output(const T* out, std::uint64_t per, const std::vector<std::uint64_t>& n_per_task,
                  const Expected& exp, const Key* base = nullptr) {
    std::uint64_t n = 0, sum_key = 0;
    for (std::size_t t = 0; t < n_per_task.size(); ++t) {
        n += n_per_task[t];
        const Key b = base ? base[t] : 0;
        for (std::uint64_t j = 0; j < n_per_task[t]; ++j) sum_key += b + static_cast<Key>(out[t * per + j]);
    }
    return n == exp.n && sum_key == exp.sum_key;
}

// Frame-of-reference bases (keys[slice lo] per task), after checking that every key of each slice
// lies in [base, base + 2^32) - true when lineitem is in l_orderkey order. False if not.
bool for4_bases(const Columns& c, std::uint64_t par, std::vector<Key>& base) {
    const std::uint64_t rows = c.qty.size();
    base.assign(par, 0);
    std::atomic<bool> ok{true};
    std::vector<std::thread> th;
    for (int w = 0; w < 64; ++w)
        th.emplace_back([&, w] {
            for (std::uint64_t t = static_cast<std::uint64_t>(w); t < par; t += 64) {
                std::uint64_t lo, hi; slice(rows, par, t, lo, hi);
                if (lo >= hi) continue;
                base[t] = c.keys[lo];
                for (std::uint64_t i = lo; i < hi; ++i)
                    if (c.keys[i] < base[t] || c.keys[i] - base[t] > 0xffffffffULL) { ok = false; return; }
            }
        });
    for (auto& x : th) x.join();
    return ok;
}

// On an output mismatch: compare every task's slice with what it should contain and describe
// the damage (which tasks, where in the slice, what came back instead).
template <typename T>
void diagnose_output(const Columns& c, std::uint64_t par, std::uint64_t threshold,
                     const T* out, std::uint64_t per, const std::vector<std::uint64_t>& n_per_task,
                     const Key* base = nullptr) {
    const std::uint64_t rows = c.qty.size();
    std::uint64_t bad_tasks = 0, bad_elems = 0, zero_elems = 0, shown = 0;
    std::uint64_t first_bad_pos_min = ~0ULL, first_bad_pos_max = 0;
    for (std::uint64_t t = 0; t < par; ++t) {
        std::uint64_t lo, hi; slice(rows, par, t, lo, hi);
        std::uint64_t j = 0, bad = 0, first = ~0ULL;
        const Key b = base ? base[t] : 0;
        for (std::uint64_t i = lo; i < hi; ++i) {
            if (c.qty[i] > threshold) continue;
            if (j < n_per_task[t] && b + static_cast<Key>(out[t * per + j]) != c.keys[i]) {
                ++bad; zero_elems += out[t * per + j] == 0;
                if (first == ~0ULL) first = j;
                if (shown < 8) {
                    std::fprintf(stderr, "  task %llu slot %llu (row %llu): expected %llu got %llu (0x%016llx)\n",
                                 (unsigned long long)t, (unsigned long long)j, (unsigned long long)i,
                                 (unsigned long long)c.keys[i], (unsigned long long)(b + static_cast<Key>(out[t * per + j])),
                                 (unsigned long long)out[t * per + j]);
                    ++shown;
                }
            }
            ++j;
        }
        if (j != n_per_task[t])
            std::fprintf(stderr, "  task %llu: device count %llu, expected %llu\n", (unsigned long long)t,
                         (unsigned long long)n_per_task[t], (unsigned long long)j);
        if (bad) {
            ++bad_tasks; bad_elems += bad;
            first_bad_pos_min = std::min(first_bad_pos_min, first);
            first_bad_pos_max = std::max(first_bad_pos_max, first);
        }
    }
    std::fprintf(stderr, "  summary: %llu/%llu tasks bad, %llu wrong slots (%llu are zero), first bad slot per task in [%llu, %llu], slot = %zu B\n",
                 (unsigned long long)bad_tasks, (unsigned long long)par, (unsigned long long)bad_elems,
                 (unsigned long long)zero_elems,
                 (unsigned long long)(bad_tasks ? first_bad_pos_min : 0), (unsigned long long)first_bad_pos_max,
                 sizeof(T));
}

// ---------------------------------------------------------------- explicit DRAM/CXL placement

// Allocates `bytes` and binds each 2 MB chunk to DRAM (node 0) or `cxl_node` before first touch:
// interleave puts chunk c in DRAM when floor((c+1)F) > floor(cF), i.e. a 1:2 pattern for F = 1/3;
// block puts the first round(nchunks*F) chunks in DRAM. 4 KB pages (MADV_NOHUGEPAGE), like the
// other host modes. Returns nullptr on failure.
void* place_alloc(std::size_t bytes, double frac, bool interleave, int cxl_node, std::size_t& map_len, void*& map_base) {
    constexpr std::size_t kChunk = std::size_t(2) << 20;
    const std::size_t len = (bytes + kChunk - 1) / kChunk * kChunk;
    map_len = len + kChunk;
    map_base = ::mmap(nullptr, map_len, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (map_base == MAP_FAILED) return nullptr;
    auto* base = reinterpret_cast<char*>((reinterpret_cast<std::uintptr_t>(map_base) + kChunk - 1) & ~(kChunk - 1));
    ::madvise(base, len, MADV_NOHUGEPAGE);
    const std::size_t n = len / kChunk, ndram = static_cast<std::size_t>(n * frac + 0.5);
    auto in_dram = [&](std::size_t c) {
        return interleave ? std::uint64_t((c + 1) * frac) > std::uint64_t(c * frac) : c < ndram;
    };
    for (std::size_t c = 0; c < n;) {   // bind runs of consecutive chunks with the same node
        const bool d = in_dram(c);
        std::size_t e = c + 1;
        while (e < n && in_dram(e) == d) ++e;
        unsigned long mask = 1UL << (d ? 0 : cxl_node);
        if (::mbind(base + c * kChunk, (e - c) * kChunk, MPOL_BIND, &mask, sizeof(mask) * 8, 0) != 0) {
            std::perror("mbind"); return nullptr;
        }
        c = e;
    }
    return base;
}

// Where did the pages actually land? One page per 2 MB chunk, via move_pages(query).
void report_placement(const char* name, const void* p, std::size_t bytes, int cxl_node) {
    constexpr std::size_t kChunk = std::size_t(2) << 20;
    std::vector<void*> pages;
    for (std::size_t off = 0; off < bytes; off += kChunk) pages.push_back(const_cast<char*>(static_cast<const char*>(p)) + off);
    std::vector<int> status(pages.size(), -1);
    if (::move_pages(0, pages.size(), pages.data(), nullptr, status.data(), 0) != 0) { std::perror("move_pages"); return; }
    std::size_t n0 = 0, ncxl = 0, other = 0;
    for (int st : status) { if (st == 0) ++n0; else if (st == cxl_node) ++ncxl; else ++other; }
    const double gb = bytes / 1e9, tot = static_cast<double>(pages.size());
    std::fprintf(stderr, "# placement %-12s %6.1f GB: DRAM %5.1f%% (%.1f GB)  CXL %5.1f%% (%.1f GB)%s\n", name, gb,
                 100.0 * n0 / tot, gb * n0 / tot, 100.0 * ncxl / tot, gb * ncxl / tot,
                 other ? "  WARNING: pages on other nodes" : "");
}

std::uint64_t vmstat(const char* key) {   // e.g. numa_pages_migrated, from /proc/vmstat
    FILE* f = std::fopen("/proc/vmstat", "r");
    if (!f) return 0;
    char name[128]; unsigned long long v = 0, out = 0;
    while (std::fscanf(f, "%127s %llu", name, &v) == 2) if (std::strcmp(name, key) == 0) { out = v; break; }
    std::fclose(f);
    return out;
}

// ---------------------------------------------------------------- host / host_cxl / host_split

int run_host(const Opt& o, const Columns& c, const Expected& exp) {
    const std::uint64_t rows = c.qty.size();
    const std::uint64_t per = (rows + o.par - 1) / o.par;
    const std::size_t key_bytes = rows * sizeof(Key), qty_bytes = rows;
    const bool out4 = o.materialize && o.out_bytes == 4;
    const std::size_t out_bytes = o.materialize ? o.par * per * static_cast<std::size_t>(o.out_bytes) : 0;
    std::vector<Key> base4;
    if (out4 && !for4_bases(c, o.par, base4)) {
        std::fprintf(stderr, "--out-bytes 4: some task slice spans >= 2^32 keys (lineitem not in l_orderkey order?)\n"); return 1;
    }
    const bool cxl = o.mode == "host_cxl";
    const bool split = o.mode == "host_split";

    const Key* keys = c.keys.data();
    const std::uint8_t* qty = c.qty.data();
    Key *cxl_keys = nullptr, *out = nullptr;
    std::uint8_t* cxl_qty = nullptr;
    std::size_t split_len[2] = {0, 0};
    std::uint64_t split_migrated0 = 0;
    void* split_map[2] = {nullptr, nullptr};
    if (split) {
        if (numa_available() < 0 || numa_node_size64(o.numa_node, nullptr) <= 0) {
            std::fprintf(stderr, "node %d reports no memory - put the dax device in system-ram mode first\n", o.numa_node);
            return 1;
        }
        const bool il = o.placement == "interleave";
        auto* pk = static_cast<Key*>(place_alloc(key_bytes, o.dram_frac, il, o.numa_node, split_len[0], split_map[0]));
        auto* pq = static_cast<std::uint8_t*>(place_alloc(qty_bytes, o.dram_frac, il, o.numa_node, split_len[1], split_map[1]));
        if (!pk || !pq) { std::fprintf(stderr, "placement failed\n"); return 1; }
        // Fill once, in parallel, outside the timer: the data is resident in this placement.
        const auto tf = clk::now();
        std::vector<std::thread> th;
        for (std::uint64_t t = 0; t < 64; ++t)
            th.emplace_back([&, t] {
                std::uint64_t lo, hi; slice(rows, 64, t, lo, hi);
                std::memcpy(pk + lo, c.keys.data() + lo, (hi - lo) * sizeof(Key));
                std::memcpy(pq + lo, c.qty.data() + lo, hi - lo);
            });
        for (auto& x : th) x.join();
        std::fprintf(stderr, "# split: dram_frac %.4f, placement %s, filled in %.0f ms\n", o.dram_frac, o.placement.c_str(), ms_since(tf));
        report_placement("l_orderkey", pk, key_bytes, o.numa_node);
        report_placement("l_quantity", pq, qty_bytes, o.numa_node);
        keys = pk; qty = pq;
        split_migrated0 = vmstat("numa_pages_migrated");
        if (out_bytes) {
            out = static_cast<Key*>(numa_alloc_onnode(out_bytes, 0));
            if (!out) { std::fprintf(stderr, "numa_alloc_onnode failed\n"); return 1; }
        }
    } else if (cxl) {
        if (numa_available() < 0) { std::fprintf(stderr, "libnuma unavailable\n"); return 1; }
        // numa_alloc_onnode silently falls back to local memory when the node has none, which
        // would turn a CXL run into a DRAM run - refuse instead.
        if (numa_node_size64(o.numa_node, nullptr) <= 0) {
            std::fprintf(stderr, "node %d reports no memory - put the dax device in system-ram mode first\n", o.numa_node);
            return 1;
        }
        cxl_keys = static_cast<Key*>(numa_alloc_onnode(key_bytes, o.numa_node));
        cxl_qty = static_cast<std::uint8_t*>(numa_alloc_onnode(qty_bytes, o.numa_node));
        if (out_bytes) out = static_cast<Key*>(numa_alloc_onnode(out_bytes, o.numa_node));
        if (!cxl_keys || !cxl_qty || (out_bytes && !out)) { std::fprintf(stderr, "numa_alloc_onnode failed\n"); return 1; }
        keys = cxl_keys; qty = cxl_qty;
    } else if (out_bytes) {
        out = static_cast<Key*>(std::aligned_alloc(2 << 20, (out_bytes + (2 << 20) - 1) & ~std::size_t((2 << 20) - 1)));
        if (!out) { std::fprintf(stderr, "aligned_alloc failed\n"); return 1; }
    }

    std::vector<SliceResult> res(o.par);
    std::vector<std::uint64_t> n_per_task(o.par);
    int rc = 0;
    for (int it = 0; it < o.iters; ++it) {
        double copy_ms = 0.0;
        if (cxl) {   // single-threaded memcpy, like the hash join's upload
            const auto tc = clk::now();
            std::memcpy(cxl_keys, c.keys.data(), key_bytes);
            std::memcpy(cxl_qty, c.qty.data(), qty_bytes);
            copy_ms = ms_since(tc);
        }
        const auto t0 = clk::now();
        {
            std::vector<std::thread> th;
            for (std::uint64_t t = 0; t < o.par; ++t)
                th.emplace_back([&, t] {
                    std::uint64_t lo, hi; slice(rows, o.par, t, lo, hi);
                    const auto thr = static_cast<std::uint32_t>(o.threshold);
                    res[t] = out4 ? scan_slice_out<std::uint32_t>(qty, keys, reinterpret_cast<std::uint32_t*>(out) + t * per,
                                                                  lo, hi, thr, base4[t])
                                  : scan_slice(qty, keys, out ? out + t * per : nullptr, lo, hi, thr);
                });
            for (auto& x : th) x.join();
        }
        const double scan_ms = ms_since(t0);

        Expected got;
        for (std::uint64_t t = 0; t < o.par; ++t) {
            got.n += res[t].n; got.sum_idx += res[t].sum_idx; got.sum_key += res[t].sum_key;
            n_per_task[t] = res[t].n;
        }
        bool ok = !o.verify || same(got, exp);
        if (o.verify && out)
            ok = ok && (out4 ? check_output(reinterpret_cast<const std::uint32_t*>(out), per, n_per_task, exp, base4.data())
                             : check_output(out, per, n_per_task, exp));
        emit(o, rows, it, copy_ms, scan_ms, got.n, ok);
        if (!ok) { std::fprintf(stderr, "MISMATCH: host result does not match the reference\n"); rc = 1; break; }
    }

    if (split) {
        // Placement must be unchanged after the timed iterations (no migration mid-run).
        report_placement("l_orderkey(after)", keys, key_bytes, o.numa_node);
        report_placement("l_quantity(after)", qty, qty_bytes, o.numa_node);
        std::fprintf(stderr, "# numa_pages_migrated during the run (system-wide): %llu\n",
                     (unsigned long long)(vmstat("numa_pages_migrated") - split_migrated0));
        for (int i = 0; i < 2; ++i) ::munmap(split_map[i], split_len[i]);
        if (out) numa_free(out, out_bytes);
    } else if (cxl) { numa_free(cxl_keys, key_bytes); numa_free(cxl_qty, qty_bytes); if (out) numa_free(out, out_bytes); }
    else std::free(out);
    return rc;
}

// ---------------------------------------------------------------- device

// ---- MX1 DMA workarounds (found 2026-10-01, see README "Device DMA caveats") ----
// 1. A single copyToDevice that crosses a 1 GiB-aligned device address silently stops there.
// 2. On buffers whose start is not 1 GiB-aligned, DMA and the MU cores disagree about what lives
//    above the first 1 GiB boundary (device-written output read back as zeros), and DMA writes
//    there were followed by kernel launches failing until a cold power cycle.
// So every device buffer is sized to whole GiB (the allocator then returns a 1 GiB-aligned
// start), DMA copies never cross a 1 GiB boundary, and materialized output is validated on the
// device (and, as root, by host loads) - never through copyFromDevice.
constexpr std::size_t kGiB = std::size_t(1) << 30;

void* dev_alloc(pxl::runtime::Context* ctx, std::size_t bytes) {
    const std::size_t rounded = (bytes + kGiB - 1) / kGiB * kGiB;
    void* p = ctx->memAlloc(rounded);
    if (p && reinterpret_cast<std::uintptr_t>(p) % kGiB != 0) {
        std::fprintf(stderr, "device buffer %p is not 1 GiB-aligned - refusing (DMA is unsafe above its first 1 GiB boundary)\n", p);
        ctx->memFree(p);
        return nullptr;
    }
    return p;
}

// Splits at 1 GiB device-address boundaries (and into <= 64 MiB pieces).
template <typename Copy>
bool dma_split(void* dev, std::size_t bytes, Copy&& copy) {
    constexpr std::size_t kChunk = std::size_t(64) << 20;
    std::size_t off = 0;
    while (off < bytes) {
        const auto addr = reinterpret_cast<std::uintptr_t>(dev) + off;
        const std::size_t to_boundary = kGiB - addr % kGiB;
        const std::size_t n = std::min({kChunk, to_boundary, bytes - off});
        if (!copy(off, n)) return false;
        off += n;
    }
    return true;
}

bool dma_to(pxl::runtime::Context* ctx, void* dst, const void* src, std::size_t bytes) {
    return dma_split(dst, bytes, [&](std::size_t off, std::size_t n) {
        return ctx->copyToDevice(static_cast<char*>(dst) + off, static_cast<const char*>(src) + off, n) == pxl::PxlResult::Success;
    });
}

bool dma_from(pxl::runtime::Context* ctx, void* dst, const void* src, std::size_t bytes) {
    return dma_split(const_cast<void*>(src), bytes, [&](std::size_t off, std::size_t n) {
        return ctx->copyFromDevice(static_cast<char*>(dst) + off, static_cast<const char*>(src) + off, n) == pxl::PxlResult::Success;
    });
}

// Copies newly published keys of the tasks reader `rid` owns from device memory to host DRAM.
// `prog` = per-task published key counts (strided by 8): the progress counters while the kernel
// runs, the final counts after it. Each pass: invalidate the counter line, read it, invalidate the
// key range below it, copy. When `fin` was set before a pass starts, that pass is the last.
// flush: 2 = invalidate the whole new range before reading it (always correct);
//        1 = only the line holding the first new key (the one that may have been read while the
//            device was still filling it); 0 = no data invalidation. The counter line is always
//            invalidated, or the poll would spin on a cached copy forever.
void return_reader(int rid, int nreaders, std::uint64_t par, const std::uint64_t* prog, const char* d_out,
                   char* h_out, std::uint64_t per_out, std::size_t esize, std::vector<std::uint64_t>& done,
                   const std::atomic<bool>& fin, int flush, bool dev_prog, pxl::runtime::Context* dma_ctx,
                   std::atomic<bool>& dma_err) {
    constexpr std::uint64_t stride = 64 / sizeof(std::uint64_t);
    for (;;) {
        const bool last = fin.load(std::memory_order_acquire);
        for (std::uint64_t t = static_cast<std::uint64_t>(rid); t < par; t += static_cast<std::uint64_t>(nreaders)) {
            const std::uint64_t* pp = prog + t * stride;
            if (dev_prog) pxl::flushHostCache(const_cast<std::uint64_t*>(pp), 64);   // else a host snapshot
            const std::uint64_t p = *const_cast<const volatile std::uint64_t*>(pp);
            // clflushopt is not ordered against older loads: keep the data invalidate (and the
            // data loads) behind the counter load, and the loads behind the invalidate.
            _mm_mfence();
            if (p > done.at(t)) {
                const std::size_t off = (t * per_out + done.at(t)) * esize;   // slots are esize bytes
                const char* src = d_out + off;
                const std::size_t n = (p - done.at(t)) * esize;
                if (dma_ctx) {   // SDK copy: the DMA engine reads device memory, no host cache involved
                    if (!dma_from(dma_ctx, h_out + off, src, n)) dma_err = true;
                    done.at(t) = p;
                    continue;
                }
                if (flush == 2) pxl::flushHostCache(const_cast<char*>(src), n);
                else if (flush == 3) {
                    // Only the rest of the 4 KiB page holding the first new key: the host's hardware
                    // prefetchers do not cross a 4 KiB page, so that page is the only place a line of the
                    // new range can have been cached before it was complete (read while copying the
                    // previous range). Valid only when the range was complete before the previous read
                    // (chunked); the poisoned output catches it if not.
                    const auto a = reinterpret_cast<std::uintptr_t>(src);
                    const auto pend = (a & ~std::uintptr_t(4095)) + 4096;
                    const auto e = std::min<std::uintptr_t>(a + n, pend);
                    pxl::flushHostCache(reinterpret_cast<void*>(a & ~std::uintptr_t(63)), e - (a & ~std::uintptr_t(63)));
                }
                else if (flush == 1)
                    pxl::flushHostCache(reinterpret_cast<void*>(reinterpret_cast<std::uintptr_t>(src) & ~std::uintptr_t(63)), 64);
                _mm_mfence();
                std::memcpy(h_out + off, src, n);
                done.at(t) = p;
            }
        }
        if (last) return;
    }
}

int run_device(const Opt& o, const Columns& c, const Expected& exp) {
    const bool root = ::geteuid() == 0;
    const bool ret = o.ret != "none", pipelined = o.ret == "pipelined", chunked = o.ret == "chunked";
    if (ret && (!root || !o.materialize)) {
        std::fprintf(stderr, "--return needs root (host loads on device memory) and --materialize 1\n");
        return 1;
    }
    // Host loads/stores on memAlloc memory SIGBUS without root (the mmap fails silently).
    if (o.upload == "memcpy" && !root) {
        std::fprintf(stderr, "--upload memcpy needs root (host stores to device memory); run with sudo or use --upload dma\n");
        return 1;
    }
    pxl::registerSignalHandler({SIGINT, SIGTERM, SIGSEGV, SIGABRT, SIGBUS, SIGFPE, SIGILL});
    if (pxl::runtime::getNumDevice() == 0) { std::fprintf(stderr, "no PXL device\n"); return 1; }
    auto* ctx = pxl::runtime::createContext(0);
    if (!ctx) { std::fprintf(stderr, "createContext failed\n"); return 1; }
    auto module = pxl::kernel::createModule("mu_kernel/scan_filter_kernel.mubin");
    if (!module) { std::fprintf(stderr, "cannot load mu_kernel/scan_filter_kernel.mubin\n"); return 1; }
    auto* job = ctx->createJob();
    if (!job) { std::fprintf(stderr, "createJob failed\n"); return 1; }
    job->load(module);
    auto* fn = module->createFunction("scan_filter");
    auto* fn_sum = module->createFunction("sum_output");
    if (!fn || !fn_sum) { std::fprintf(stderr, "createFunction failed\n"); return 1; }
    auto* map = job->buildMap(fn, static_cast<std::uint32_t>(o.par));
    auto* map_sum = o.materialize ? job->buildMap(fn_sum, static_cast<std::uint32_t>(o.par)) : nullptr;
    auto* fn_pub = pipelined ? module->createFunction("scan_filter_pub") : nullptr;
    // Before each returned iteration the output buffer is overwritten with a pattern that is never
    // a valid key sequence (bw_fill: word i = i), so a stale host-cached line shows up as a
    // validation failure instead of silently matching last iteration's identical keys.
    auto* fn_fill = ret ? module->createFunction("bw_fill") : nullptr;
    auto* map_fill = fn_fill ? job->buildMap(fn_fill, 3048) : nullptr;
    const int rflush = o.ret_flush == "range" ? 2 : o.ret_flush == "page" ? 3 : o.ret_flush == "boundary" ? 1 : 0;
    auto* map_pub = fn_pub ? job->buildMap(fn_pub, static_cast<std::uint32_t>(o.par)) : nullptr;
    auto* fn_step = chunked ? module->createFunction("scan_filter_step") : nullptr;
    auto* map_step = fn_step ? job->buildMap(fn_step, static_cast<std::uint32_t>(o.par)) : nullptr;
    if (!map || (o.materialize && !map_sum) || (pipelined && !map_pub) || (chunked && !map_step)) { std::fprintf(stderr, "buildMap failed\n"); return 1; }

    const std::uint64_t rows = c.qty.size();
    const std::uint64_t per = (rows + o.par - 1) / o.par;
    const std::uint64_t stride = 64 / sizeof(std::uint64_t);
    const std::size_t key_bytes = rows * sizeof(Key), qty_bytes = rows;
    // Output slices start on a 64 B line, so no cache line is shared by two MU tasks.
    const bool out4 = o.materialize && o.out_bytes == 4;
    const std::size_t esize = out4 ? 4 : sizeof(Key);
    const std::uint64_t per_line = 64 / esize;
    const std::uint64_t per_out = (per + per_line - 1) / per_line * per_line;   // slots per task
    const std::size_t out_bytes = o.par * per_out * esize;
    std::vector<Key> base4;
    if (out4 && !for4_bases(c, o.par, base4)) {
        std::fprintf(stderr, "--out-bytes 4: some task slice spans >= 2^32 keys (lineitem not in l_orderkey order?)\n"); return 1;
    }
    const std::size_t stat_bytes = o.par * stride * sizeof(std::uint64_t);

    auto* d_keys = static_cast<Key*>(dev_alloc(ctx, key_bytes));
    auto* d_qty = static_cast<std::uint8_t*>(dev_alloc(ctx, qty_bytes));
    auto* d_out = o.materialize ? static_cast<Key*>(dev_alloc(ctx, out_bytes)) : nullptr;
    auto* d_counts = static_cast<std::uint64_t*>(dev_alloc(ctx, stat_bytes));
    auto* d_sidx = static_cast<std::uint64_t*>(dev_alloc(ctx, stat_bytes));
    auto* d_skey = static_cast<std::uint64_t*>(dev_alloc(ctx, stat_bytes));
    auto* d_osum = o.materialize ? static_cast<std::uint64_t*>(dev_alloc(ctx, stat_bytes)) : nullptr;
    auto* d_cfg = static_cast<Config*>(dev_alloc(ctx, sizeof(Config)));
    auto* d_prog = (pipelined || chunked) ? static_cast<std::uint64_t*>(dev_alloc(ctx, stat_bytes)) : nullptr;
    auto* d_pcfg = ret ? static_cast<Config*>(dev_alloc(ctx, sizeof(Config))) : nullptr;
    if (!d_keys || !d_qty || !d_counts || !d_sidx || !d_skey || !d_cfg || (o.materialize && (!d_out || !d_osum)) || ((pipelined || chunked) && !d_prog)) {
        std::fprintf(stderr, "device allocation failed\n"); return 1;
    }

    Config cfg{rows, o.par, o.threshold, per_out, static_cast<std::uint64_t>(o.materialize),
               static_cast<std::uint64_t>(o.flush_out), static_cast<std::uint64_t>(o.prefetch),
               static_cast<std::uint64_t>(o.prefetch_keys ? 0 : 1), o.chunk_rows, 0, 0,
               static_cast<std::uint64_t>(o.write_lines), static_cast<std::uint64_t>(out4)};
    // Host DRAM destination of the returned keys, pre-faulted outside the timer.
    Key* h_ret = nullptr;
    if (ret) {
        h_ret = static_cast<Key*>(std::aligned_alloc(2 << 20, (out_bytes + (2 << 20) - 1) & ~std::size_t((2 << 20) - 1)));
        if (!h_ret) { std::fprintf(stderr, "host return buffer allocation failed\n"); return 1; }
        std::vector<std::thread> th;
        for (int t = 0; t < 64; ++t)
            th.emplace_back([&, t] { std::uint64_t lo, hi; slice(out_bytes, 64, t, lo, hi);
                                     std::memset(reinterpret_cast<char*>(h_ret) + lo, 0, hi - lo); });
        for (auto& x : th) x.join();
        std::fprintf(stderr, "# return %s via %s: %d reader threads, chunk %llu rows, %llu steps, data flush %s, host buffer %.1f GB\n",
                     o.ret.c_str(), o.ret_api.c_str(), o.ret_threads, (unsigned long long)o.chunk_rows, (unsigned long long)o.ret_steps,
                     o.ret_api == "dma" ? "n/a" : o.ret_flush.c_str(), out_bytes / 1e9);
        Config pc{out_bytes / 8, 3048, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
        if (!map_fill || !d_pcfg || !dma_to(ctx, d_pcfg, &pc, sizeof pc)) { std::fprintf(stderr, "poison setup failed\n"); return 1; }
    }
    if (o.materialize) std::fprintf(stderr, "# device writes: %s\n", o.write_lines ? "whole 64 B lines (memcpyGlobal)" : "plain stores");
    if (o.prefetch) std::fprintf(stderr, "# device prefetch: window %d blocks (%d rows) ahead, keys %s\n",
                                 o.prefetch, o.prefetch * 64, o.prefetch_keys ? "on" : "off");
    if (!dma_to(ctx, d_cfg, &cfg, sizeof cfg)) { std::fprintf(stderr, "copyToDevice failed\n"); return 1; }

    std::vector<std::uint64_t> hc(o.par * stride), hi(o.par * stride), hk(o.par * stride), hs(o.par * stride), n_per_task(o.par);
    int rc = 0;
    for (int it = 0; it < o.iters; ++it) {
        const auto tc = clk::now();
        if (o.upload == "memcpy") {   // what hash-join-xcena does in run_probe()
            std::memcpy(d_keys, c.keys.data(), key_bytes);
            std::memcpy(d_qty, c.qty.data(), qty_bytes);
            const double memcpy_ms = ms_since(tc);
            const auto tf = clk::now();
            pxl::flushHostCache(d_keys, key_bytes);
            pxl::flushHostCache(d_qty, qty_bytes);
            std::fprintf(stderr, "# iter %d upload: memcpy %.1f ms (%.2f GB/s) + flush %.1f ms\n", it, memcpy_ms,
                         (key_bytes + qty_bytes) / 1e6 / memcpy_ms, ms_since(tf));
        } else if (!dma_to(ctx, d_keys, c.keys.data(), key_bytes) || !dma_to(ctx, d_qty, c.qty.data(), qty_bytes)) {
            std::fprintf(stderr, "copyToDevice failed\n"); rc = 1; break;
        }
        const double copy_ms = ms_since(tc);
        if (!o.scan) {   // --scan 0: upload-only, to separate upload behaviour from the kernel
            emit(o, rows, it, copy_ms, 0.0, 0, true);
            continue;
        }

        if (ret) {   // poison the output on the device, then drop every host-cached line of it (untimed)
            const auto tp = clk::now();
            if (map_fill->execute(d_out, d_pcfg) != pxl::PxlResult::Success || map_fill->synchronize() != pxl::PxlResult::Success) {
                std::fprintf(stderr, "poison kernel failed\n"); rc = 1; break;
            }
            pxl::flushHostCache(d_out, out_bytes);
            std::fprintf(stderr, "# iter %d poison + host invalidate: %.0f ms (untimed)\n", it, ms_since(tp));
        }
        double scan_ms = 0.0, ret_ms = 0.0, e2e_ms = 0.0;
        std::vector<std::uint64_t> done(o.par, 0);
        std::atomic<bool> fin{false}, dma_err{false};
        pxl::runtime::Context* rctx = o.ret_api == "dma" ? ctx : nullptr;
        const auto t0 = clk::now();
        if (!ret) {
            if (map->execute(d_qty, d_keys, d_out, d_counts, d_sidx, d_skey, d_cfg) != pxl::PxlResult::Success ||
                map->synchronize() != pxl::PxlResult::Success) { std::fprintf(stderr, "scan kernel failed\n"); rc = 1; break; }
            scan_ms = ms_since(t0);
        } else if (!pipelined && !chunked) {
            // serial: scan everything, then return everything (final counts = what to copy)
            if (map->execute(d_qty, d_keys, d_out, d_counts, d_sidx, d_skey, d_cfg) != pxl::PxlResult::Success ||
                map->synchronize() != pxl::PxlResult::Success) { std::fprintf(stderr, "scan kernel failed\n"); rc = 1; break; }
            scan_ms = ms_since(t0);
            const auto tr = clk::now();
            fin.store(true, std::memory_order_release);
            std::vector<std::thread> th;
            for (int r = 0; r < o.ret_threads; ++r)
                th.emplace_back(return_reader, r, o.ret_threads, o.par, d_counts, reinterpret_cast<const char*>(d_out), reinterpret_cast<char*>(h_ret), per_out, esize, std::ref(done), std::cref(fin), rflush, true, rctx, std::ref(dma_err));
            for (auto& x : th) x.join();
            ret_ms = ms_since(tr);
            e2e_ms = ms_since(t0);
        } else if (chunked) {
            // chunked: ret_steps launches, each scanning the next part of every task's slice.
            // After each synchronize() the cursors are snapshotted into host DRAM (`avail`);
            // readers copy only below the snapshot, so they never read data a running launch
            // may still be writing back. execute/synchronize stay on this thread.
            std::vector<std::uint64_t> avail(o.par * stride, 0);
            std::vector<std::thread> th;
            for (int r = 0; r < o.ret_threads; ++r)
                th.emplace_back(return_reader, r, o.ret_threads, o.par, avail.data(), reinterpret_cast<const char*>(d_out), reinterpret_cast<char*>(h_ret), per_out, esize, std::ref(done), std::cref(fin), rflush, false, rctx, std::ref(dma_err));
            bool kok = true;
            const auto tk = clk::now();
            for (std::uint64_t st = 0; st < o.ret_steps && kok; ++st) {
                kok = map_step->execute(d_qty, d_keys, d_out, d_counts, d_sidx, d_skey, d_prog, d_cfg, (st << 32) | o.ret_steps) == pxl::PxlResult::Success &&
                      map_step->synchronize() == pxl::PxlResult::Success;
                if (!kok) break;
                pxl::flushHostCache(d_prog, stat_bytes);
                std::uint64_t tot = 0, copied = 0;
                for (std::uint64_t t = 0; t < o.par; ++t) {
                    const std::uint64_t v = *const_cast<const volatile std::uint64_t*>(d_prog + t * stride);
                    __atomic_store_n(&avail[t * stride], v, __ATOMIC_RELEASE);
                    tot += v; copied += __atomic_load_n(&done[t], __ATOMIC_RELAXED);
                }
                if (o.debug) std::fprintf(stderr, "#   step %llu done at %.1f ms: %llu keys available, %llu already copied\n",
                                          (unsigned long long)st, ms_since(tk), (unsigned long long)tot, (unsigned long long)copied);
            }
            scan_ms = ms_since(tk);
            fin.store(true, std::memory_order_release);
            for (auto& x : th) x.join();
            e2e_ms = ms_since(tk);
            ret_ms = e2e_ms - scan_ms;   // the tail left after the last launch finished
            if (!kok) { std::fprintf(stderr, "scan kernel failed\n"); rc = 1; break; }
        } else {
            // pipelined: readers poll the progress counters while the kernel runs. execute and
            // synchronize stay on this thread (splitting them across threads wedges the device).
            std::memset(d_prog, 0, stat_bytes);
            pxl::flushHostCache(d_prog, stat_bytes);
            std::vector<std::thread> th;
            for (int r = 0; r < o.ret_threads; ++r)
                th.emplace_back(return_reader, r, o.ret_threads, o.par, d_prog, reinterpret_cast<const char*>(d_out), reinterpret_cast<char*>(h_ret), per_out, esize, std::ref(done), std::cref(fin), rflush, true, rctx, std::ref(dma_err));
            const auto tk = clk::now();
            const bool kok = map_pub->execute(d_qty, d_keys, d_out, d_counts, d_sidx, d_skey, d_prog, d_cfg) == pxl::PxlResult::Success &&
                             map_pub->synchronize() == pxl::PxlResult::Success;
            scan_ms = ms_since(tk);
            fin.store(true, std::memory_order_release);
            for (auto& x : th) x.join();
            e2e_ms = ms_since(tk);
            ret_ms = e2e_ms - scan_ms;   // the tail left after the kernel finished
            if (!kok) { std::fprintf(stderr, "scan kernel failed\n"); rc = 1; break; }
        }

        if (!dma_from(ctx, hc.data(), d_counts, stat_bytes) || !dma_from(ctx, hi.data(), d_sidx, stat_bytes) ||
            !dma_from(ctx, hk.data(), d_skey, stat_bytes)) {
            std::fprintf(stderr, "copyFromDevice failed\n"); rc = 1; break;
        }
        Expected got;
        for (std::uint64_t t = 0; t < o.par; ++t) {
            got.n += hc[t * stride]; got.sum_idx += hi[t * stride]; got.sum_key += hk[t * stride];
            n_per_task[t] = hc[t * stride];
        }
        bool ok = !o.verify || same(got, exp);
        if (dma_err) { std::fprintf(stderr, "copyFromDevice failed during the return\n"); ok = false; }

        if (o.verify && ret && ok) {
            // The keys the host received, in host DRAM: count + key sum against the reference.
            for (std::uint64_t t = 0; t < o.par && ok; ++t)
                if (done.at(t) != n_per_task[t]) {
                    std::fprintf(stderr, "task %llu: returned %llu keys, device counted %llu\n", (unsigned long long)t,
                                 (unsigned long long)done.at(t), (unsigned long long)n_per_task[t]);
                    ok = false;
                }
            const auto* h4 = reinterpret_cast<const std::uint32_t*>(h_ret);
            if (ok && !(out4 ? check_output(h4, per_out, n_per_task, exp, base4.data())
                             : check_output(h_ret, per_out, n_per_task, exp))) {
                if (out4) diagnose_output(c, o.par, o.threshold, h4, per_out, n_per_task, base4.data());
                else diagnose_output(c, o.par, o.threshold, h_ret, per_out, n_per_task);
                ok = false;
            }
        } else if (o.verify && d_out && ok) {
            // (a) Device re-reads every output slice: what the next operator on the device would see.
            if (map_sum->execute(d_out, d_counts, d_osum, d_cfg) != pxl::PxlResult::Success ||
                map_sum->synchronize() != pxl::PxlResult::Success || !dma_from(ctx, hs.data(), d_osum, stat_bytes)) {
                std::fprintf(stderr, "output re-sum kernel failed\n"); rc = 1; break;
            }
            std::uint64_t bad_tasks = 0;
            for (std::uint64_t t = 0; t < o.par; ++t) bad_tasks += hs[t * stride] != hk[t * stride];
            if (bad_tasks) { std::fprintf(stderr, "device re-read of the output disagrees in %llu tasks\n", (unsigned long long)bad_tasks); ok = false; }
            // (b) As root, the host reads the output through CXL.mem: what the host would consume.
            if (ok && root) {
                pxl::flushHostCache(d_out, out_bytes);
                if (!check_output(d_out, per_out, n_per_task, exp)) {
                    diagnose_output(c, o.par, o.threshold, d_out, per_out, n_per_task);
                    ok = false;
                }
            }
        }
        emit(o, rows, it, copy_ms, scan_ms, got.n, ok, ret_ms, e2e_ms);
        if (!ok) { std::fprintf(stderr, "MISMATCH: device result does not match the reference\n"); rc = 1; break; }
    }

    ctx->memFree(d_cfg); ctx->memFree(d_skey); ctx->memFree(d_sidx); ctx->memFree(d_counts);
    if (d_prog) ctx->memFree(d_prog);
    if (d_pcfg) ctx->memFree(d_pcfg);
    std::free(h_ret);
    if (d_osum) ctx->memFree(d_osum);
    if (d_out) ctx->memFree(d_out);
    ctx->memFree(d_qty); ctx->memFree(d_keys);
    if (map_sum) job->destroyMap(map_sum);
    if (map_pub) job->destroyMap(map_pub);
    if (map_fill) job->destroyMap(map_fill);
    job->destroyMap(map);
    ctx->destroyJob(job);
    module->destroyFunction(fn_sum);
    if (fn_pub) module->destroyFunction(fn_pub);
    if (fn_fill) module->destroyFunction(fn_fill);
    module->destroyFunction(fn);
    pxl::kernel::destroyModule(module);
    pxl::runtime::destroyContext(ctx);
    return rc;
}

constexpr const char* kHeader =
    "label,mode,upload,rows,par,threshold,sel_pct,materialize,iter,copy_ms,scan_ms,read_gb,gb_per_s,matches,ok,ret_ms,e2e_ms";

}  // namespace

int main(int argc, char** argv) {
    Opt o;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto v = [&]() -> const char* { if (i + 1 >= argc) { std::fprintf(stderr, "%s needs a value\n", a.c_str()); std::exit(2); } return argv[++i]; };
        if (a == "--mode") o.mode = v();
        else if (a == "--label") o.label = v();
        else if (a == "--tbl") o.tbl = v();
        else if (a == "--bin") o.bin = v();
        else if (a == "--cache-dir") o.cache_dir = v();
        else if (a == "--par") o.par = std::stoull(v());
        else if (a == "--threshold" || a == "-f") o.threshold = std::stoull(v());
        else if (a == "--materialize") o.materialize = std::atoi(v());
        else if (a == "--iters") o.iters = std::atoi(v());
        else if (a == "--verify") o.verify = std::atoi(v());
        else if (a == "--numa-node") o.numa_node = std::atoi(v());
        else if (a == "--dram-frac") o.dram_frac = std::atof(v());
        else if (a == "--placement") o.placement = v();
        else if (a == "--return") o.ret = v();
        else if (a == "--return-threads") o.ret_threads = std::atoi(v());
        else if (a == "--ret-flush") o.ret_flush = v();
        else if (a == "--chunk-rows") o.chunk_rows = std::stoull(v());
        else if (a == "--ret-steps") o.ret_steps = std::stoull(v());
        else if (a == "--debug") o.debug = std::atoi(v());
        else if (a == "--ret-api") o.ret_api = v();
        else if (a == "--write-lines") o.write_lines = std::atoi(v());
        else if (a == "--out-bytes") o.out_bytes = std::atoi(v());
        else if (a == "--upload") o.upload = v();
        else if (a == "--flush-out") o.flush_out = std::atoi(v());
        else if (a == "--scan") o.scan = std::atoi(v());
        else if (a == "--prefetch") o.prefetch = std::atoi(v());
        else if (a == "--prefetch-keys") o.prefetch_keys = std::atoi(v());
        else if (a == "--header") { std::printf("%s\n", kHeader); return 0; }
        else { std::fprintf(stderr, "unknown option %s\n", a.c_str()); return 2; }
    }
    if (o.label.empty()) o.label = o.mode;
    if (o.mode != "host" && o.mode != "host_cxl" && o.mode != "host_split" && o.mode != "device") { std::fprintf(stderr, "--mode must be host | host_cxl | host_split | device\n"); return 2; }
    if (o.ret_flush != "range" && o.ret_flush != "page" && o.ret_flush != "boundary" && o.ret_flush != "none") { std::fprintf(stderr, "--ret-flush range|page|boundary|none\n"); return 2; }
    if (o.ret != "none" && o.ret != "serial" && o.ret != "pipelined" && o.ret != "chunked") { std::fprintf(stderr, "--return none|serial|pipelined|chunked\n"); return 2; }
    if (o.ret == "pipelined") { std::fprintf(stderr, "--return pipelined was removed (its in-kernel publish raced); use --return chunked\n"); return 2; }
    if (o.out_bytes != 8 && o.out_bytes != 4) { std::fprintf(stderr, "--out-bytes 8|4\n"); return 2; }
    if (o.out_bytes == 4 && o.mode == "device" && (o.ret == "none" || o.ret == "pipelined")) {
        std::fprintf(stderr, "--out-bytes 4 on the device needs --return serial|chunked (validation decodes on the host)\n"); return 2;
    }
    if (o.ret_api != "direct" && o.ret_api != "dma") { std::fprintf(stderr, "--ret-api direct|dma\n"); return 2; }
    // 2026-10-05: copyFromDevice while kernels were launching froze the whole host (box down overnight),
    // and serial copyFromDevice returned wrong bytes. Only allow DMA after the kernel has finished.
    if (o.ret_api == "dma" && o.ret != "serial") { std::fprintf(stderr, "--ret-api dma is only allowed with --return serial (it froze the host when overlapped with launches)\n"); return 2; }
    if (o.ret_steps == 0) { std::fprintf(stderr, "--ret-steps must be > 0\n"); return 2; }
    if (o.placement != "interleave" && o.placement != "block") { std::fprintf(stderr, "--placement interleave|block\n"); return 2; }
    if (o.dram_frac < 0 || o.dram_frac > 1) { std::fprintf(stderr, "--dram-frac must be 0..1\n"); return 2; }
    if (o.upload != "memcpy" && o.upload != "dma") { std::fprintf(stderr, "--upload must be memcpy | dma\n"); return 2; }
    if (o.threshold > 50) { std::fprintf(stderr, "--threshold must be 0..50 (l_quantity is 1..50)\n"); return 2; }
    if (o.par == 0) { std::fprintf(stderr, "--par must be > 0\n"); return 2; }
    if (o.tbl.empty() == o.bin.empty()) { std::fprintf(stderr, "give exactly one of --tbl <lineitem.tbl> or --bin <columns.bin>\n"); return 2; }

    Columns c;
    if (!load(o, c)) return 1;
    if (c.qty.empty()) { std::fprintf(stderr, "no rows in %s\n", o.tbl.c_str()); return 1; }
    check_quantity_range(c);
    const Expected exp = o.verify ? expected(c, o.threshold) : Expected{};
    return o.mode == "device" ? run_device(o, c, exp) : run_host(o, c, exp);
}
