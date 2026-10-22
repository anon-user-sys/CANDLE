#!/usr/bin/env python3
"""data/size_time.dat and data/size_gain.dat for the tree-size figure (compact layout, uniform keys,
100M probes, 128 pinned host cores / 3048 MU tasks; median lookup_ms of iterations 1-2).
Host: results/size_host_20261012_160912.csv (system-ram), probes partitioned per thread, like NDP per task
(unpartitioned host runs are within 1.6x, see size_gain.dat). NDP: results/size_device_20261012_161831.csv (devdax), partitioned
per task, and unpartitioned."""
import csv, os, statistics as st
D = os.path.dirname(os.path.abspath(__file__))
R = os.path.join(D, "..", "results")
def load(f):
    g, gb = {}, {}
    for r in csv.DictReader(l for l in open(os.path.join(R, f)) if not l.startswith("#")):
        assert r["ok"] == "1" and r["layout"] == "compact", r
        k = (float(r["sf"]), r["label"], int(r["partition"])); gb[float(r["sf"])] = float(r["tree_gb"])
        if r["iter"] != "0": g.setdefault(k, []).append(float(r["lookup_ms"]))
    return {k: st.median(v) for k, v in g.items()}, gb
h, gb = load("size_host_20261012_160912.csv")
d, _ = load("size_device_20261012_161831.csv")
best = lambda sf, l: h[(sf, l, 128)]   # host plotted with partitioned probes (one key range per thread)
with open(os.path.join(D, "data", "size_time.dat"), "w") as f:
    f.write("# tree_gb dram lvl75cxl il50 cxl ndp ndp_unpart (ms); lvl75cxl = levels first, 25% DRAM\n")
    for sf in sorted(gb):
        f.write(f"{gb[sf]:.3f} " + " ".join(f"{best(sf, l):.1f}" for l in
                ("M1_dram100_interleave", "M1_dram25_block", "M1_dram50_interleave", "M1_dram0_interleave"))
                + f" {d[(sf, 'M4', 3048)]:.1f} {d[(sf, 'M4', 0)]:.1f}\n")
with open(os.path.join(D, "data", "size_gain.dat"), "w") as f:
    f.write("# tree_gb gain_ndp gain_dram gain_cxl   (unpartitioned / partitioned lookup time)\n")
    for sf in sorted(gb):
        g = lambda l, p: h[(sf, l, 0)] / h[(sf, l, p)]
        f.write(f"{gb[sf]:.3f} {d[(sf, 'M4', 0)] / d[(sf, 'M4', 3048)]:.3f} {g('M1_dram100_interleave', 128):.3f} {g('M1_dram0_interleave', 128):.3f}\n")
print("ok")
