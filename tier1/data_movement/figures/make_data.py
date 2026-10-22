#!/usr/bin/env python3
"""Build figure data for Sec. 4.3 from the verified 2026-10-09 runs.

Paper configuration: mx_dma kthreads pinned to device-socket CPUs 8,9, process bound to
node 0, default C-states (results/pinned_nocstate0_20261009_171421, 3 runs; D12 from
results/followup_20261009_183624, 4 runs). Each value is the median across runs of the
per-run p50; min/max are the extremes of the per-run p50s.

Usage: python3 make_data.py   (run from figures/)
"""
import collections
import statistics as st
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent / "results"
PINNED = ROOT / "pinned_nocstate0_20261009_171421"
FOLLOW = ROOT / "followup_20261009_183624"
# NT stores without the flush (coherence_tax k7/k13 showed it is unnecessary): same config,
# 2026-10-10; the other arms of this run agree with PINNED within ~1% (median new/old 0.99-1.01).
NTNF = ROOT / "pinned_nocstate0_20261010_142434"
OUT = Path(__file__).resolve().parent / "data"


def agg(vals):
    return st.median(vals), min(vals), max(vals)


def d1():
    d = collections.defaultdict(list)
    for r in (1, 2, 3):
        for line in open(PINNED / f"r{r}_d1.txt"):
            f = line.strip().split(",")
            if line.startswith("#") or len(f) < 10 or f[0] == "api":
                continue
            d[(f[0], f[1], int(f[2]))].append(float(f[3]))
    for r in (1, 2, 3):
        for line in open(NTNF / f"r{r}_d1.txt"):
            f = line.strip().split(",")
            if line.startswith("direct_ntstore_noflush,"):
                d[(f[0], f[1], int(f[2]))].append(float(f[3]))
    sizes = sorted({k[2] for k in d})
    cols = {
        "h2d": ["copyToDevice", "direct_memcpy", "direct_word", "direct_ntstore", "direct_ntstore_noflush"],
        "d2h": ["copyFromDevice", "direct_memcpy", "direct_word"],
    }
    for direction, apis in cols.items():
        dirn = "H2D" if direction == "h2d" else "D2H"
        with open(OUT / f"transfer_{direction}.dat", "w") as fh:
            fh.write("# size_bytes " + "  ".join(f"{a}_p50 {a}_min {a}_max" for a in apis) + "\n")
            for s in sizes:
                row = [str(s)]
                for a in apis:
                    row += [f"{v:.4f}" for v in agg(d[(a, dirn, s)])]
                fh.write("  ".join(row) + "\n")


def d10():
    d = collections.defaultdict(list)
    for r in (1, 2, 3):
        for line in open(PINNED / f"r{r}_d10.txt"):
            f = line.strip().split(",")
            if line.startswith("#") or len(f) < 9 or f[0] == "api":
                continue
            if f[1] != "268435456":
                continue
            d[(f[0], f[3], f[4])].append(float(f[7]))
    order = [("read", "64", "seq"), ("read", "4096", "4K"), ("read", "random", "rand"),
             ("write", "64", "seq"), ("write", "4096", "4K"), ("write", "random", "rand")]
    with open(OUT / "access_pattern.dat", "w") as fh:
        fh.write("# idx label cxl dram ratio   (256 MiB buffer, median of 3 runs)\n")
        RW_ABBR = {"read": "RD", "write": "WR"}
        for i, (dr, sl, lab) in enumerate(order):
            c = st.median(d[("memAlloc", dr, sl)])
            h = st.median(d[("host_hugepage", dr, sl)])
            fh.write(f'{i} "{lab}\\n{RW_ABBR[dr]}" {c:.3f} {h:.3f} {c / h:.2f}\n')


def d12():
    d = collections.defaultdict(list)
    for r in (1, 2, 3, 4):
        for line in open(FOLLOW / f"d12_r{r}.txt"):
            f = line.strip().split(",")
            if len(f) == 6 and f[0].isdigit():
                d[int(f[1])].append(float(f[4]))
    with open(OUT / "rw_mix.dat", "w") as fh:
        fh.write("# write_pct  p50  min  max  n   (median/extremes across runs)\n")
        for w in sorted(d):
            m, lo, hi = agg(d[w])
            fh.write(f"{w} {m:.3f} {lo:.3f} {hi:.3f} {len(d[w])}\n")


if __name__ == "__main__":
    d1()
    d10()
    d12()
    print("wrote", *sorted(p.name for p in OUT.glob("*.dat")))
