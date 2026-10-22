#!/usr/bin/env python3
"""data/bto_<placement>_<dist>_{host,ndp}.dat: the OPTIMIZED versions at SF 1500 (compact 64 B
layout everywhere). Host: system-ram grids, median lookup_ms of iterations 1-2.
  interleave: btree_sf1500_20261012_103348 (all-DRAM uniform points replaced by the clean rerun
              _114830; the originals overlapped another run, 10:33-10:36)
  block:      _111712 (upper levels first; DRAM 75/50/25% -> 25/50/75% on CXL), with the same
              all-DRAM and all-CXL end points as the interleave panels
NDP: compact + probes range-partitioned per task (ndp_opt_..._102415); ndp.dat has lookup_ms and
lookup_ms + part_ms (the partitioning step, host, 64 threads).
Host points use probes partitioned per thread (_150153 interleave, _152014 block), like NDP per task;
the unpartitioned host runs (above) are reported in the text, not plotted."""
import csv, os, statistics as st
D = os.path.dirname(os.path.abspath(__file__))
R = os.path.join(D, "..", "results")
IL, RERUN, BL = (os.path.join(R, f"btree_sf1500_20261012_{s}.csv") for s in ("103348", "114830", "111712"))
OPT = os.path.join(R, "ndp_opt_sf1500_20261012_102415.csv")

def med(f):
    g, part = {}, {}
    for r in csv.DictReader(l for l in open(f) if not l.startswith("#")):
        assert r["ok"] == "1" and r["layout"] == "compact", r
        k = (r["label"], r["dist"], int(r["par"]))
        part[k] = float(r["part_ms"])
        if r["iter"] != "0": g.setdefault(k, []).append(float(r["lookup_ms"]))
    return {k: st.median(v) for k, v in g.items()}, part

il, _ = med(IL); rr, _ = med(RERUN); il.update(rr)
bl, _ = med(BL); o, part = med(OPT)
ilp, _ = med(os.path.join(R, "btree_sf1500_20261012_150153.csv"))   # host, partitioned probes
blp, _ = med(os.path.join(R, "btree_sf1500_20261012_152014.csv"))
picked = {"part": 0, "unpart": 0}
def best(a, b):
    out = {}
    for k in a:
        if k in b and b[k] < a[k]: out[k] = b[k]; picked["part"] += 1
        else: out[k] = a[k]; picked["unpart"] += 1
    return out
# host plotted with partitioned probes (one key range per thread), like NDP (one per task)
il, bl = ilp, blp
P = (8, 16, 32, 64, 128)
for dist in ("uniform", "zipf"):
    for pl, src, lab in (("interleave", il, "interleave"), ("block", bl, "block")):
        with open(os.path.join(D, "data", f"bto_{pl}_{dist}_host.dat"), "w") as f:
            f.write("# cores dram t25 t50 t75 cxl (ms)   t25 = 25% on CXL\n")
            for p in P:
                mid = [src[(f"M1_dram{d}_{lab}", dist, p)] for d in (75, 50, 25)]
                ends = [il[(f"M1_dram{d}_interleave", dist, p)] for d in (100, 0)]
                f.write(f"{p} {ends[0]:.1f} " + " ".join(f"{x:.1f}" for x in mid) + f" {ends[1]:.1f}\n")
        with open(os.path.join(D, "data", f"bto_{pl}_{dist}_ndp.dat"), "w") as f:
            f.write("# tasks lookup_ms lookup+partition_ms\n")
            for p in (256, 512, 1024, 2048, 3048):
                k = ("M4_opt", dist, p)
                f.write(f"{p} {o[k]:.1f} {o[k] + part[k]:.1f}\n")
print("ok; host points taken from partitioned runs:", picked)
