#!/usr/bin/env python3
"""Fit the translation model to a translation_tax v2 sweep.

    ./scripts/analyze_tlb.py results/host4k_A_span_*.csv [...]

Model: the platform holds E translation entries covering g bytes each. A pattern that touches G
distinct g-sized granules uniformly at random hits with probability E/G, so

    ns_per_access = base + miss_cost * max(0, 1 - E/G)

G is not a knob directly, so it is recomputed here from the pattern itself: the script replays
offset_of() from tt_common.hpp and counts the distinct granules the target lines fall in. The
fit is a grid search over (g, E) with base and miss_cost from least squares at each point, so
the reported g is the granule size the timings are most consistent with, not an assumption.
"""
import csv
import itertools
import sys
from collections import defaultdict
from statistics import median

M64 = (1 << 64) - 1


def mix64(x):
    x &= M64
    x ^= x >> 30
    x = (x * 0xBF58476D1CE4E5B9) & M64
    x ^= x >> 27
    x = (x * 0x94D049BB133111EB) & M64
    x ^= x >> 31
    return x


def offsets(region_bytes, regions, lines_per_region, layout):
    """Byte offset of every target line — must stay identical to tt::offset_of()."""
    l_shift = lines_per_region.bit_length() - 1
    region_shift = region_bytes.bit_length() - 1
    if layout == "clump":
        clump_shift = l_shift + 6
        line_shift, rand_shift = 6, clump_shift
        rand_mask = (1 << (region_shift - clump_shift)) - 1
        hash_shift = l_shift
    else:
        line_shift = region_shift - l_shift
        rand_shift = 6
        rand_mask = (1 << (line_shift - 6)) - 1
        hash_shift = 0
    for j in range(regions * lines_per_region):
        rnd = (mix64((j >> hash_shift) ^ 0x9E3779B97F4A7C15) & rand_mask) << rand_shift
        yield ((j >> l_shift) << region_shift) + ((j & (lines_per_region - 1)) << line_shift) + rnd


def granules(row, g_shift):
    key = (row["region_bytes"], row["regions"], row["lines_per_region"], row["layout"], g_shift)
    if key not in granules.cache:
        granules.cache[key] = len({o >> g_shift for o in offsets(*key[:4])})
    return granules.cache[key]


granules.cache = {}


def load(paths):
    rows = []
    for p in paths:
        for r in csv.DictReader(open(p)):
            if r["sum_ok"] != "1":
                sys.exit(f"{p}: checksum failure in {r} — the run is not trustworthy")
            for k in ("region_bytes", "regions", "lines_per_region", "lines", "span_mb", "rep"):
                r[k] = int(r[k])
            r["ns"] = float(r["ns_per_access"])
            rows.append(r)
    return rows


def fit(rows, g_shifts=range(12, 32)):
    """Grid search (g, E); base and miss_cost by least squares. Returns the best few fits."""
    pts = defaultdict(list)
    for r in rows:
        pts[(r["region_bytes"], r["regions"], r["lines_per_region"], r["layout"])].append(r["ns"])
    out = []
    for g_shift in g_shifts:
        gs = {k: len({o >> g_shift for o in offsets(*k[:4])}) for k in pts}
        for e in [1 << i for i in range(0, 14)] + [24, 48, 96, 192, 384, 768, 1536, 3072]:
            xs, ys = [], []
            for k, v in pts.items():
                g = gs[k]
                xs.append(max(0.0, 1.0 - e / g))
                ys.append(median(v))
            n = len(xs)
            sx, sy = sum(xs), sum(ys)
            sxx = sum(x * x for x in xs)
            sxy = sum(x * y for x, y in zip(xs, ys))
            den = n * sxx - sx * sx
            if den <= 1e-12:
                continue
            m = (n * sxy - sx * sy) / den
            b = (sy - m * sx) / n
            if m <= 0:
                continue
            sse = sum((b + m * x - y) ** 2 for x, y in zip(xs, ys))
            sst = sum((y - sy / n) ** 2 for y in ys)
            out.append(dict(g=1 << g_shift, E=e, base=b, miss=m, r2=1 - sse / sst if sst else 0,
                            rmse=(sse / n) ** 0.5, coverage=(1 << g_shift) * e))
    out.sort(key=lambda d: -d["r2"])
    return out


def human(n):
    for unit, s in ((1 << 30, "G"), (1 << 20, "M"), (1 << 10, "K")):
        if n >= unit and n % unit == 0:
            return f"{n // unit}{s}"
    return str(n)


def main(paths):
    rows = load(paths)
    by_platform = defaultdict(list)
    for r in rows:
        by_platform[(r["platform"], r["pages"])].append(r)

    for (platform, pages), rs in by_platform.items():
        print(f"\n=== {platform} / {pages} pages   ({len(rs)} rows)")
        groups = defaultdict(list)
        for r in rs:
            groups[(r["layout"], r["order"], r["span_mb"], r["region_bytes"], r["regions"],
                    r["lines_per_region"], r["lines"])].append(r["ns"])
        print(f"{'layout':7} {'order':6} {'span':>7} {'S':>7} {'R':>7} {'L':>6} {'K':>7} "
              f"{'G@2M':>7} {'ns':>8}")
        for k in sorted(groups, key=lambda k: (k[0], k[1], k[2], k[4])):
            layout, order, span_mb, s, r, l, lines_ = k
            g2m = len({o >> 21 for o in offsets(s, r, l, layout)})
            print(f"{layout:7} {order:6} {human(span_mb << 20):>7} {human(s):>7} {r:>7} {l:>6} "
                  f"{lines_:>7} {g2m:>7} {median(groups[k]):8.2f}")

        # The fit only means anything where the granule count is a real variable.
        fitrows = [r for r in rs if r["order"] == "random" and r["layout"] == "clump"]
        if len(fitrows) < 6:
            continue
        print(f"  best (g, E) fits over {len(fitrows)} clump/random points:")
        seen, shown = set(), 0
        for f in fit(fitrows):
            if f["g"] in seen:
                continue
            seen.add(f["g"])
            print(f"    g={human(f['g']):>5}  E={f['E']:>5}  coverage={human(f['coverage']):>5}"
                  f"  base={f['base']:7.2f} ns  miss={f['miss']:8.2f} ns  R2={f['r2']:.4f}"
                  f"  rmse={f['rmse']:.2f}")
            shown += 1
            if shown == 6:
                break


if __name__ == "__main__":
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    main(sys.argv[1:])
