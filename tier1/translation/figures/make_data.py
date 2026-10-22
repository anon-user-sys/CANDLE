#!/usr/bin/env python3
"""results -> figures/data for Fig. 5 (translation characterization).

  Fig. 5a  dev_random, dev_rr <- results/device_C_miss_*.csv      median/min/max per granule count G
           dev_model          <- the fitted model 94.2 + 587.2 * max(0, 1 - 8/G), G = 1.05^i
  Fig. 5b  dev_S<size>        <- results/device_B2_entry_*.csv    median per span, one curve per region spacing
  Fig. 5c  dev_ws_G8, G64     <- results/device_D_workingset_*.csv median per target-line count
           dev_ws_G16         <- results/device_E_smallK_*.csv
All rows: layout clump, 1 GB granules, checksum-validated runs only (sum_ok = 1).
Every file matches the paper's plotted data exactly except dev_random, which the paper drew from a
one-off pool of sweeps; from sweep C alone it is within 0.25% (medians within 1.6 ns).
"""
import csv, glob, os, statistics as st

D = os.path.dirname(os.path.abspath(__file__))
R = os.path.join(D, "..", "results")
OUT = os.path.join(D, "data")
os.makedirs(OUT, exist_ok=True)
GB = 1 << 30


def rows(pattern):
    out = []
    for f in sorted(glob.glob(os.path.join(R, pattern))):
        with open(f) as fh:
            for r in csv.DictReader(l for l in fh if not l.startswith("#")):
                if r.get("platform") and r.get("sum_ok") == "1":
                    out.append(r)
    return out


def write(name, header, lines):
    with open(os.path.join(OUT, name + ".dat"), "w") as f:
        f.write(header + "\n")
        f.writelines(l + "\n" for l in lines)


# Fig. 5a: granule count, random vs. round-robin order
C = rows("device_C_miss_*.csv")
for order, name in (("random", "dev_random"), ("rr", "dev_rr")):
    g = {}
    for r in C:
        if r["order"] == order and r["layout"] == "clump":
            g.setdefault(int(r["regions"]), []).append(float(r["ns_per_access"]))
    write(name, "# granules_1GB  median_ns  min  max",
          [f"{k} {st.median(v):.2f} {min(v):.2f} {max(v):.2f}" for k, v in sorted(g.items())])
write("dev_model", "# G  model_ns   (base 94.2 + 587.2 * max(0,1-8/G))",
      [f"{1.05 ** i:.3f} {94.2 + 587.2 * max(0.0, 1 - 8 / 1.05 ** i):.2f}" for i in range(114)])

# Fig. 5b: address span, one curve per region spacing S
B = rows("device_B2_entry_*.csv")
for S, name in ((16 << 20, "dev_S16M"), (256 << 20, "dev_S256M"), (1 << 30, "dev_S1024M"), (2 << 30, "dev_S2048M")):
    g = {}
    for r in B:
        if int(r["region_bytes"]) == S and r["order"] == "random" and r["layout"] == "clump":
            g.setdefault(int(r["regions"]) * S / GB, []).append(float(r["ns_per_access"]))
    write(name, "# span_GB  ns", [f"{k:.4f} {st.median(v):.2f}" for k, v in sorted(g.items())])

# Fig. 5c: target-line count at a fixed number of granules
for G, pattern in ((8, "device_D_workingset_*.csv"), (16, "device_E_smallK_*.csv"), (64, "device_D_workingset_*.csv")):
    g = {}
    for r in rows(pattern):
        if int(r["regions"]) == G and int(r["region_bytes"]) == GB and r["order"] == "random" and r["layout"] == "clump":
            g.setdefault(int(r["lines"]), []).append(float(r["ns_per_access"]))
    write(f"dev_ws_G{G}", "# lines  ns", [f"{k} {st.median(v):.2f}" for k, v in sorted(g.items())])
