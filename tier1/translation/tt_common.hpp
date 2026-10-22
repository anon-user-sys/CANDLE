#pragma once
// Shared by the host driver and the MU kernel. The access pattern and the check values must be
// bit-identical on both sides, so everything that defines them lives here.
//
// Experiment (v2): place K = regions x lines_per_region distinct 64-byte target lines, then read
// them in a fixed number of accesses. The three knobs are independent:
//
//   region_bytes (S)      bytes between the bases of consecutive regions
//   regions (R)           how many distinct regions the pattern spans -> span = R * S
//   lines_per_region (L)  target lines inside each region             -> K = R * L
//   layout                how the L lines sit inside their region:
//                           spread - one per slot of S/L bytes, filling the region
//                           clump  - packed 64 B apart at a random clump-aligned base
//
// v1 tied K to the region count (one line per region), so cache demand and translation demand
// grew together: on the device the dense-stride baseline alone moved 43 -> 99 ns as K grew.
// Holding K fixed while R varies keeps cache demand constant and varies only how many
// translation granules the pattern touches, which is what isolates the translation tax.
//
// Why the layout knob exists: with `spread`, the K lines end up evenly covering the span at a
// spacing of span/K whatever R is, so R changes nothing about which granules are touched. The
// 2026-09-18 host run confirmed it (flat in R, 29 ns at a 1 GB span vs 45 ns at 16 GB for every
// R). `clump` packs each region's L lines into L*64 contiguous bytes, which fits inside any
// plausible granule, so the granules touched track R while span and K stay fixed. That is what
// isolates entry count from everything else.
//
// Model being tested (hypothesis from the 2026-09-15 data): the device has E translation
// entries of g bytes. With uniform random access over G = min(R, span/g) granules, the hit rate
// is E/G, so the per-access tax is miss_cost * max(0, 1 - E/G). The fitted candidate is
// E = 32, g = 256 MB (= the vendor's 8 GB of coverage), miss_cost ~ 590 ns.
#include <cstdint>

namespace tt {

constexpr std::uint64_t kLine = 64;

// Access order within the timed pass.
enum : std::uint64_t {
    kLayoutSpread = 0,   // one line per slot of S/L bytes: the lines cover the whole region
    kLayoutClump = 1,    // L lines packed 64 B apart at a clump-aligned pseudo-random base
};

enum : std::uint64_t {
    kOrderRandom = 0,      // uniform random over all K lines; tax ~ miss_cost * (1 - E/G)
    kOrderRoundRobin = 1,  // regions in a fixed cycle; with R > E every access should miss under
                           // LRU/FIFO, so the added time IS the miss cost, with no fitting
};

// Two cache lines. The host writes the inputs (copyToDevice), the kernel writes the outputs.
struct Config {
    std::uint64_t region_bytes;      // S, power of two, >= lines_per_region * kLine
    std::uint64_t regions;           // R, power of two
    std::uint64_t lines_per_region;  // L, power of two
    std::uint64_t order;             // kOrderRandom | kOrderRoundRobin
    std::uint64_t layout;            // kLayoutSpread | kLayoutClump
    std::uint64_t accesses;          // random accesses in the timed pass
    std::uint64_t seed;              // PRNG seed for the access order
    std::uint64_t cycles;            // out: device clock cycles spent in the timed pass
    std::uint64_t sum;               // out: sum of the values loaded in the timed pass
    std::uint64_t done;              // out: 1 after init, 2 after access, 3 after calibration
    std::uint64_t spare[6];
};

inline std::uint64_t mix64(std::uint64_t x) {
    x ^= x >> 30; x *= 0xbf58476d1ce4e5b9ULL;
    x ^= x >> 27; x *= 0x94d049bb133111ebULL;
    x ^= x >> 31;
    return x;
}

// xorshift64: cheap, identical on both sides, no memory access of its own.
inline std::uint64_t next(std::uint64_t& s) {
    s ^= s << 13; s ^= s >> 7; s ^= s << 17;
    return s;
}

inline std::uint64_t value_at(std::uint64_t offset) { return mix64(offset + 1); }

// Shifts and masks derived once per configuration, outside the timed loop.
//
// Every access must execute exactly the same instructions whatever the configuration, or compute
// differences masquerade as translation cost. In v1 a 64-bit modulo used only for sparse strides
// added ~50 ns per access on an MU core and mimicked a tax. Everything here is shifts and masks:
// S, R and L are powers of two, so no division ever reaches the timed loop.
// Both layouts share one expression, so the timed loop runs the same instructions either way:
//
//   offset(j) = (region << region_shift) + (index << line_shift) + ((hash & rand_mask) << rand_shift)
//
//   spread: index is the slot, line_shift = log2(S/L), the hash picks a line inside the slot
//   clump:  index is the line, line_shift = log2(64), the hash picks the clump base (hashed per
//           region, via hash_shift = log2(L), so all L lines of a region share one base)
struct Pattern {
    std::uint64_t l_shift;       // log2(L)
    std::uint64_t l_mask;        // L - 1
    std::uint64_t region_shift;  // log2(S)
    std::uint64_t line_shift;    // log2(S / L) spread, log2(kLine) clump
    std::uint64_t rand_mask;     // positions the hash may pick
    std::uint64_t rand_shift;    // scale of those positions
    std::uint64_t hash_shift;    // 0 spread (per line), log2(L) clump (per region)
    std::uint64_t k_mask;        // K - 1
    std::uint64_t r_mask;        // R - 1
};

inline std::uint64_t ilog2(std::uint64_t x) { return 63 - static_cast<std::uint64_t>(__builtin_clzll(x)); }

inline Pattern make_pattern(const Config& c) {
    Pattern p{};
    p.l_shift = ilog2(c.lines_per_region);
    p.l_mask = c.lines_per_region - 1;
    p.region_shift = ilog2(c.region_bytes);
    p.k_mask = (c.regions << p.l_shift) - 1;
    p.r_mask = c.regions - 1;
    if (c.layout == kLayoutClump) {
        const std::uint64_t clump_shift = p.l_shift + ilog2(kLine);   // log2(L * 64)
        p.line_shift = ilog2(kLine);
        p.rand_mask = (1ULL << (p.region_shift - clump_shift)) - 1;   // clump-aligned bases in S
        p.rand_shift = clump_shift;
        p.hash_shift = p.l_shift;
    } else {
        p.line_shift = p.region_shift - p.l_shift;                    // = log2(S / L)
        p.rand_mask = (1ULL << (p.line_shift - ilog2(kLine))) - 1;    // lines inside one slot
        p.rand_shift = ilog2(kLine);
        p.hash_shift = 0;
    }
    return p;
}

// Byte offset of target line j. Its region is j>>l_shift and its index inside that region is
// j&l_mask; the hash keeps the lines off region and clump boundaries. Lines are distinct in both
// layouts: spread gives each one its own slot, clump gives each its own 64 B inside the clump.
inline std::uint64_t offset_of(const Pattern& p, std::uint64_t j) {
    const std::uint64_t region = j >> p.l_shift;
    const std::uint64_t index = j & p.l_mask;
    const std::uint64_t rnd = (mix64((j >> p.hash_shift) ^ 0x9e3779b97f4a7c15ULL) & p.rand_mask) << p.rand_shift;
    return (region << p.region_shift) + (index << p.line_shift) + rnd;
}

inline std::uint64_t pick_random(const Pattern& p, std::uint64_t& s) { return (next(s) >> 17) & p.k_mask; }

// Access a visits region (a mod R); the line inside it is still random, so the number of
// distinct lines and the cache demand match the random order exactly.
inline std::uint64_t pick_rr(const Pattern& p, std::uint64_t a, std::uint64_t& s) {
    const std::uint64_t region = a & p.r_mask;
    return (region << p.l_shift) | ((next(s) >> 17) & p.l_mask);
}

inline std::uint64_t lines(const Config& c) { return c.regions * c.lines_per_region; }
inline std::uint64_t span_bytes(const Config& c) { return c.regions * c.region_bytes; }

}  // namespace tt
