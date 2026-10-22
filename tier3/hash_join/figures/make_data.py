#!/usr/bin/env python3
"""Data for fig_hj_row (sf_100 version, 2026-10-18): writes data/phase100.dat, perkey100.dat,
threads100_host.dat, threads100_dev.dat, sel100.dat from the run JSONs in the project root.
Per run: median of its iterations. Owner-computes build in every plan (C19 device, C29 host).
Edit the run lists below when better runs replace these."""
import json, os, statistics as st
D = os.path.dirname(os.path.abspath(__file__)); ROOT = os.path.abspath(os.path.join(D, "..", "results"))
K = ("pure_build_ms", "build_flush_ms", "pure_probe_ms", "probe_flush_ms", "total_ms")
def run(d, js):
    r = [json.loads(l) for l in open(os.path.join(ROOT, d, js))]
    return {k: st.median(x[k] for x in r) for k in K}
H, Y, N = "mode2_host_at_cxl.json", "mode3_hybrid.json", "mode4_ndp_sync.json"
P = "devdax_modes_sf100_sel50_p"
os.makedirs(os.path.join(D, "data"), exist_ok=True)

# (a) best partition count per plan (sf_100 owner-build sweep, 2026-10-18): host 8,192 (732/451/390/440 ms at
# 1,024/4,096/8,192/16,384), device 1,024 (677/877/1,004 at 1,024/4,096/8,192). provisional = 1 -> label "*".
PHASE = [("Host@CXL", P + "8192_w32_own_hd_20261018_155853", H, 0),
         ("Hybrid",   P + "8192_w32_own_hd_20261018_160337", Y, 0),
         ("NDP@CXL",  P + "1024_w32_own_20261017_014602",    N, 0)]
with open(os.path.join(D, "data/phase100.dat"), "w") as fo:
    fo.write("# plan build build_handoff probe probe_handoff total unused provisional (ms)\n")
    for name, d, js, prov in PHASE:
        r = run(d, js)
        fo.write(f'"{name}" {r["pure_build_ms"]:.1f} {r["build_flush_ms"]:.1f} {r["pure_probe_ms"]:.1f} {r["probe_flush_ms"]:.1f} {r["total_ms"]:.1f} 0 {prov}\n')

# (b) owner build ns per key. Host: P = 1,024 at sf_1/10 (tables already fit in cache), best P at sf_100.
KEYS = {1: 1.5e6, 10: 15e6, 100: 150e6}
def median_dir(dirs, js):
    """The run (dir) whose total is the median (lower middle) -- Host@CXL varies between processes (T42)."""
    return sorted(dirs, key=lambda d: run(d, js)["total_ms"])[(len(dirs) - 1) // 2]
SF10 = ["devdax_modes_sf10_sel50_p1024_w32_own_hd_20261018_" + t for t in ("150701", "161057", "161108", "161116")]
HOST_B = {1: ("devdax_modes_sf1_sel50_p1024_w32_own_hd_20261018_150846", H),
          10: (median_dir(SF10, H), H),        # P = 1,024 is the host's best at sf_10 (8,192: build 34 vs 27 ms)
          100: (PHASE[0][1], H)}
DEV_B = {1: ("devdax_modes_sf1_sel50_p1024_w32_own_20261016_235305", N),
         10: ("devdax_modes_sf10_sel50_p1024_w32_own_20261016_223815", N),
         100: (PHASE[2][1], N)}
# probe ns per lineitem row (pure probe, no hand-off; 50% of rows look up), same runs as the build
ROWS = {1: 6001215, 10: 59986052, 100: 600037902}
# hand-offs from the Hybrid runs (the plan with both): build hand-off (write back the host-built table) per
# orders key, probe hand-off (invalidate the device's result) per lineitem row
HYB_B = {1: ("devdax_modes_sf1_sel50_p1024_w32_own_hd_20261018_150846", Y),
         10: (median_dir(SF10, Y), Y),
         100: (PHASE[1][1], Y)}
with open(os.path.join(D, "data/perkey100.dat"), "w") as fo:
    fo.write("# sf host_atomic host_owner dev_atomic dev_owner host_probe dev_probe build_handoff probe_handoff\n"
             "# build ns per orders key; probe ns per lineitem row; hand-offs (Hybrid) per key / per row; atomic unused\n")
    for sf in (1, 10, 100):
        hr, vr, yr = run(*HOST_B[sf]), run(*DEV_B[sf]), run(*HYB_B[sf])
        fo.write(f"{sf} NaN {hr['pure_build_ms'] * 1e6 / KEYS[sf]:.3f} NaN {vr['pure_build_ms'] * 1e6 / KEYS[sf]:.3f} "
                 f"{hr['pure_probe_ms'] * 1e6 / ROWS[sf]:.3f} {vr['pure_probe_ms'] * 1e6 / ROWS[sf]:.3f} "
                 f"{yr['build_flush_ms'] * 1e6 / KEYS[sf]:.3f} {yr['probe_flush_ms'] * 1e6 / ROWS[sf]:.3f}\n")

# (c) share of threads at each plan's best P (host 8,192; device 1,024): Host@CXL 16/32/64/128 threads,
# device tasks 381/762/1,524/3,048 (= 12.5/25/50/100% of 3,048, the host's shares) -- run_quick4/5.sh
Q = "devdax_modes_sf100_sel50_p"
with open(os.path.join(D, "data/threads100_host.dat"), "w") as fo:
    for t, d in ((16, "8192_w32_own_hd_20261018_161332"), (32, "8192_w32_own_hd_20261018_161419"),
                 (64, "8192_w32_own_hd_20261018_161456")):
        fo.write(f"{t} {run(Q + d, H)['total_ms']:.1f}\n")
    fo.write(f"128 {run(PHASE[0][1], H)['total_ms']:.1f}\n")
with open(os.path.join(D, "data/threads100_dev.dat"), "w") as fo:
    for t, hy, nd in ((381, "8192_w32_own_hd_20261018_162500", "1024_w32_own_hd_20261018_162539"),
                      (762, "8192_w32_own_hd_20261018_162625", "1024_w32_own_hd_20261018_162659"),
                      (1524, "8192_w32_own_hd_20261018_162734", "1024_w32_own_hd_20261018_162804")):
        fo.write(f"{t} {run(Q + hy, Y)['total_ms']:.1f} {run(Q + nd, N)['total_ms']:.1f}\n")
    fo.write(f"3048 {run(PHASE[1][1], Y)['total_ms']:.1f} {run(PHASE[2][1], N)['total_ms']:.1f}\n")

# (d) selectivity at each plan's best P (host + Hybrid 8,192; NDP@CXL 1,024)
S = "devdax_modes_sf100_sel"
SEL = {0:   (S + "0_p8192_w32_own_hd_20261018_161132",   S + "0_p8192_w32_own_hd_20261018_161132",   S + "0_p1024_w32_own_hd_20261018_152224"),
       50:  (PHASE[0][1],                                PHASE[1][1],                                PHASE[2][1]),
       100: (S + "100_p8192_w32_own_hd_20261018_161225", S + "100_p8192_w32_own_hd_20261018_161225", S + "100_p1024_w32_own_hd_20261018_153747")}
with open(os.path.join(D, "data/sel100.dat"), "w") as fo:
    fo.write("# sel host hybrid ndp (total ms)\n")
    for sel, (hh, yy, nn) in SEL.items():
        fo.write(f"{sel} {run(hh, H)['total_ms']:.1f} {run(yy, Y)['total_ms']:.1f} {run(nn, N)['total_ms']:.1f}\n")
print("data/phase100.dat perkey100.dat threads100_host.dat threads100_dev.dat sel100.dat")
