#!/usr/bin/env python3
"""data/node_ndp.dat for the node-size figure: NDP at SF 1500, 3048 tasks, uniform keys, 100M
probes (results/node_ndp_sf1500_20261012_164318.csv; median lookup_ms of iterations 1-2).
64 B = compact layout (implicit children, one line per level); 128 B-4 KB = classic layout
(header + keys + child/value arrays, binary search). Lines per lookup are computed from the node
layout and the search code (classic: header line + probed key lines + value line per node).
Host-DRAM: results/node_host_M1_20261012_170245.csv (devdax session, 128 pinned cores). All host points
use probes partitioned per thread (128 key ranges), like NDP per task."""
import csv, os, statistics as st
D = os.path.dirname(os.path.abspath(__file__))
F = os.path.join(D, "..", "results", "node_ndp_sf1500_20261012_164318.csv")
g, h = {}, {}
for r in csv.DictReader(l for l in open(F) if not l.startswith("#")):
    assert r["ok"] == "1", r
    k = (int(r["node_bytes"]), int(r["partition"])); h[k[0]] = int(r["height"])
    if r["iter"] != "0": g.setdefault(k, []).append(float(r["lookup_ms"]))
m = {k: st.median(v) for k, v in g.items()}
hg = {}
for r in csv.DictReader(l for l in open(os.path.join(D, "..", "results", "node_host_M1_20261012_170245.csv")) if not l.startswith("#")):
    assert r["ok"] == "1", r
    if r["iter"] != "0": hg.setdefault((int(r["node_bytes"]), int(r["partition"])), []).append(float(r["lookup_ms"]))
hm = {k: st.median(v) for k, v in hg.items()}
# Restricted host (system-ram session 2026-10-13, 128 pinned cores): all on CXL and 50% interleaved.
def host_part(name):
    g = {}
    for r in csv.DictReader(l for l in open(os.path.join(D, "..", "results", name)) if not l.startswith("#")):
        assert r["ok"] == "1", r
        if r["iter"] != "0": g.setdefault((int(r["node_bytes"]), int(r["partition"])), []).append(float(r["lookup_ms"]))
    m_ = {k: st.median(v) for k, v in g.items()}
    # a point exists only once both runs (unpartitioned and partitioned) are in
    return {nb: m_[(nb, 128)] for nb in {k[0] for k in m_} if (nb, 128) in m_}   # host plotted partitioned
import glob
def latest(prefix):
    fs = sorted(glob.glob(os.path.join(D, "..", "results", prefix + "_2*.csv")))
    return os.path.basename(fs[-1])
cxl = host_part(latest("node_host_M1_dram0_interleave"))
t50 = host_part(latest("node_host_M1_dram50_interleave"))
lines = {64: 11, 128: 24, 256: 24, 512: 28, 1024: 30, 2048: 30, 4096: 28}
with open(os.path.join(D, "data", "node_ndp.dat"), "w") as f:
    f.write("# node_bytes unpart_ms part_ms height lines_per_lookup host_dram_ms host_cxl_ms tiered50_ms   (64 = compact layout)\n")
    for nb in sorted(h):
        f.write(f"{nb} {m[(nb, 0)]:.1f} {m[(nb, 3048)]:.1f} {h[nb]} {lines[nb]} {hm[(nb, 128)]:.1f} {cxl[nb]:.1f} {(f'{t50[nb]:.1f}' if nb in t50 else '?')}\n")
print("ok")
