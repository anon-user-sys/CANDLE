#!/usr/bin/env python3
"""Data for the option-3 scan figures: x = share of the platform's threads used
(host cores / 128, NDP tasks / 3048). One host file (5 placements) and one NDP file per
(form, selectivity). Median of iterations >= 1, validated rows only; '?' = not measured.
Materialized = 4-byte encoding (host) / overlapped return with 32 launches (NDP)."""
import csv, os, statistics as st
R = os.path.join(os.path.dirname(os.path.abspath(__file__)), "../results")
OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "data")
os.makedirs(OUT, exist_ok=True)
def rows(*fs):
    for f in fs:
        for r in csv.DictReader(l for l in open(os.path.join(R, f)) if not l.startswith("#")):
            if r.get("ok") == "1" and r.get("iter", "").isdigit() and int(r["iter"]) >= 1:
                yield r
def med(rs, key="scan_ms"):
    v = [float(r[key]) for r in rs]
    return f"{st.median(v):.1f}" if v else "?"
PLACE = ["M1_dram100_interleave", "M1_dram75_interleave", "M1_dram50_interleave", "M1_dram25_interleave", "M1_dram0_interleave"]
COUNT = list(rows("tpch_scan_sf1500_20261003_192034.csv", "tpch_scan_sf1500_20261006_143437.csv"))
MAT4 = list(rows("tpch_scan_sf1500_20261006_151136.csv"))
NDPC = list(rows("tpch_scan_sf1500_20261003_214103.csv", "tpch_scan_sf1500_20261003_162249.csv"))
# every device-return run since the chunked-mode fix (2026-10-05 18:46); picks up new sweeps automatically
import glob
RET = list(rows(*sorted(os.path.basename(f) for f in glob.glob(os.path.join(R, "return_sf1500_*.csv"))
                        if os.path.basename(f)[14:29] >= "20261005_184600")))
# later reruns / sweeps: append file names here
for sel in (50, 100):          # 0% selectivity is reported in the text only
    s = f"{sel:.1f}"
    for form, src, sfx in (("count", COUNT, ""), ("mat", MAT4, "_o4")):
        with open(f"{OUT}/o3_{form}_sel{sel}_host.dat", "w") as f:
            f.write("# cores dram t25 t50 t75 cxl (ms)\n")
            for p in (8, 16, 32, 64, 128):
                vals = [med([r for r in src if r["label"] == lab + sfx and r["sel_pct"] == s and int(r["par"]) == p
                             and r["materialize"] == ("1" if form == "mat" else "0")]) for lab in PLACE]
                f.write(f"{p} " + " ".join(vals) + "\n")
    with open(f"{OUT}/o3_count_sel{sel}_ndp.dat", "w") as f:
        f.write("# tasks ms\n")
        for t in (256, 512, 1024, 2048, 3048):
            f.write(f"{t} {med([r for r in NDPC if r['label'].startswith('M4') and r['sel_pct'] == s and int(r['par']) == t])}\n")
    with open(f"{OUT}/o3_mat_sel{sel}_ndp.dat", "w") as f:
        f.write("# tasks ms (end-to-end, result in host DRAM)\n")
        for t in (256, 512, 1024, 2048, 3048):
            lab = "M4_ret_chunked_s32_flush_range_o4" + ("" if t == 3048 else f"_t{t}")
            f.write(f"{t} {med([r for r in RET if r['label'] == lab and r['sel_pct'] == s], 'e2e_ms')}\n")
for fn in sorted(os.listdir(OUT)):
    if fn.startswith("o3_mat"):
        print("==", fn); print(open(os.path.join(OUT, fn)).read(), end="")

# ---- direct labels at the right edge, nudged apart so they never overlap (log2 spacing >= GAP)
import math
GAP = 0.32
NAMES = ["Host-DRAM", "Tiered 25%", "Tiered 50%", "Tiered 75%", "Host@CXL"]
for sel in (50, 100):          # 0% selectivity is reported in the text only
    for form in ("count", "mat"):
        items = []
        host = [l.split() for l in open(f"{OUT}/o3_{form}_sel{sel}_host.dat") if not l.startswith("#")]
        ndp = [l.split() for l in open(f"{OUT}/o3_{form}_sel{sel}_ndp.dat") if not l.startswith("#")]
        for i, n in enumerate(NAMES):
            v = host[-1][i + 1]
            if v != "?": items.append([math.log2(float(v) / 1000), n, math.log2(float(v) / 1000)])
        if ndp[-1][1] != "?": items.append([math.log2(float(ndp[-1][1]) / 1000), "NDP", math.log2(float(ndp[-1][1]) / 1000)])
        items.sort()
        for _ in range(50):                      # relax: push neighbours apart symmetrically
            moved = False
            for a, b in zip(items, items[1:]):
                d = b[0] - a[0]
                if d < GAP:
                    a[0] -= (GAP - d) / 2; b[0] += (GAP - d) / 2; moved = True
            if not moved: break
        with open(f"{OUT}/o3_{form}_sel{sel}_labels.dat", "w") as f:
            f.write("# label_y_s data_y_s name\n")
            for y, n, y0 in items: f.write(f"{2**y:.4f} {2**y0:.4f} \"{n}\"\n")
