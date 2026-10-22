#!/usr/bin/env python3
"""Data for fig_blink_row (B-link tree, 10M keys, scrambled Zipf 0.99, 8M ops per run), YCSB A and B,
from the newest ../results/fig_row_*.csv per workload (scripts/bench_fig_row.sh). Times in ms per 1M ops.
Timing rules (user decisions 2026-10-17):
  Host@CXL: batched (--host-dispatch=batched --batch=1M; option 1, 2026-10-17): the same batch pipeline
  on host threads, same rule as NDP@CXL: parallel locate + parallel writes + reporting timed, dedup and
  replay not counted. From results/fig_hostbatched_*.csv (newest file per thread count).
  NDP@CXL = resident device worker (--resident, results/fig_resident_*.csv); NDP-sync = one kernel launch per
  job (fig_row_*.csv). Both (cxl_full, batch 1M, combining on): request staging + execute->synchronize (dispatch + device
  work) + result readback incl. invalidation (staging + readback = the SDBI hand-off) + host split
  fallback + reporting. Dedup, replay and write grouping (host prep) not counted.
Only runs from 2026-10-17 20:20 on count (clflushopt readback + execute-based NDP timing).
  Hybrid (128 host threads, batch 1M): host locate (incl. post-locate leaf invalidation) + staging + write
  kernel + readback + splits + reporting; dedup, replay, write grouping not counted. From
  results/fig_hybrid_*.csv. Bar: index ops = locate + write kernel, SDBI = leaf invalidation + staging + readback.
Writes data/phase.dat, data/scale_{host,dev,hyb}_<wl>.dat."""
import csv, glob, os, re
D = os.path.dirname(os.path.abspath(__file__)); ROOT = os.path.abspath(os.path.join(D, ".."))
WLS = ["a"]                                         # YCSB A only (B removed 2026-10-17)
SINCE = "20261017_202000"
STAGING_SINCE = "20261017_205107"                        # runs from here on count staging themselves
os.makedirs(os.path.join(D, "data"), exist_ok=True)

def newest_rows(wl):
    """rows of the newest fig_row csv (from SINCE on) that has this workload"""
    for f in sorted(glob.glob(os.path.join(ROOT, "results", "fig_row_*.csv")), reverse=True):
        stamp = re.search(r"fig_row_(\d{8}_\d{6})", f).group(1)
        if stamp < SINCE: continue
        rows = [r for r in csv.DictReader(open(f)) if r["workload"] == wl and r["mops"]]
        if stamp >= STAGING_SINCE:
            for r in rows: r["_staging_counted"] = "1"
        if rows: return os.path.basename(f), rows
    return None, []

def ms_per_mop(r):
    """timed ms per 1M ops. NDP@CXL runs before 2026-10-17 21:00 were timed without request staging;
    it is logged per run (stage_ms), so it is added here: the timed region is staging + execute ->
    batch end (whole SDBI hand-off + device work)."""
    t = 1000.0 / float(r["mops"])
    if r["mode"] == "cxl_full" and r.get("stage_ms") and not r.get("_staging_counted"):
        t += float(r["stage_ms"]) / 8.0                     # 8M ops per run -> per 1M
    return t

def host_batched_rows(wl):
    """batched Host@CXL rows: for each thread count, the runs of the newest fig_hostbatched file that has it"""
    by_t = {}
    for f in sorted(glob.glob(os.path.join(ROOT, "results", "fig_hostbatched_*.csv"))):   # oldest first
        rs = [r for r in csv.DictReader(open(f)) if r["workload"] == wl and r["mops"]]
        for t in {r["threads"] for r in rs}: by_t[t] = [r for r in rs if r["threads"] == t]
    return [r for t in by_t for r in by_t[t]]

def resident_rows(wl):
    """NDP@CXL = resident worker (--resident) from the newest fig_resident csv; staging already timed"""
    for f in sorted(glob.glob(os.path.join(ROOT, "results", "fig_resident_*.csv")), reverse=True):
        rs = [dict(r, mode="cxl_full_res", _staging_counted="1")
              for r in csv.DictReader(open(f)) if r["workload"] == wl and r["mops"]]
        if rs: return rs
    return []

def hybrid_rows(wl):
    """hybrid rows from the newest fig_hybrid csv that has this workload"""
    for f in sorted(glob.glob(os.path.join(ROOT, "results", "fig_hybrid_*.csv")), reverse=True):
        rs = [r for r in csv.DictReader(open(f)) if r["workload"] == wl and r["mops"]]
        if rs: return rs
    return []

def phases(r):
    """(index ops, invalidation, staging/copy, other) in ms per 1M ops (2026-10-18: the former "SDBI" segment split
    into coherency work and data movement). invalidation = Hybrid's post-locate leaf invalidation + result-readback
    invalidation; staging/copy = request staging + result readback without its invalidation; other = the rest
    (host split fallback + reporting)."""
    total = ms_per_mop(r)
    k = 1.0 / 8.0                                           # 8M ops per run -> per 1M
    rinv = float(r.get("readback_inval_ms") or 0)
    if r["mode"] == "cxl_hybrid":                           # host locate + write kernel | leaf + result inval | staging + copy | rest
        post = float(r["post_inval_ms"] or 0)
        idx = (float(r["locate_host_ms"]) - post + float(r["launch_ms"])) * k
        inval = (post + rinv) * k
        copy = (float(r["stage_ms"]) + float(r["readback_ms"]) - rinv) * k
        return idx, inval, copy, total - idx - inval - copy
    if r["mode"] == "host_cxl":
        if r.get("host_locate_ms"):                        # batched host: locate + write | report
            idx = (float(r["host_locate_ms"]) + float(r["host_write_ms"])) * k
            return idx, 0.0, 0.0, total - idx
        return total, 0.0, 0.0, 0.0
    dev = float(r["launch_ms"]) * k
    inval = rinv * k                                        # result readback invalidation
    copy = (float(r["stage_ms"]) + float(r["readback_ms"]) - rinv) * k
    return dev, inval, copy, total - dev - inval - copy     # rest = split fallback + reporting

def pooled(rs):
    s = sorted(rs, key=ms_per_mop)
    return s[(len(s) - 1) // 2], ms_per_mop(s[0]), ms_per_mop(s[-1])

# panel a rows: (plan, workload) -> y; Hybrid (y = 1) stays empty for now
XMAX = [0.0]                                            # largest whisker end in panel a (ms)
YS = {("Host@CXL", "a"): 3, ("Hybrid", "a"): 2, ("NDP-sync", "a"): 1, ("NDP@CXL", "a"): 0}
with open(os.path.join(D, "data/phase.dat"), "w") as fo:
    fo.write("# ms per 1M ops\n")
    fo.write("# plan wl y index_ops invalidation staging_copy other total total_min total_max n_runs best_config (lowest median time)\n")
    for wl in WLS:
        src, rows = newest_rows(wl)
        if not rows: print(f"wl={wl}: no data yet")
        rows = [r for r in rows if r["mode"] not in ("host_cxl", "cxl_hybrid")] + host_batched_rows(wl) + hybrid_rows(wl) + resident_rows(wl)
        # panel a: each plan's BEST configuration for this workload (lowest median time): host threads
        # for Host@CXL, device tasks for NDP@CXL
        for plan, mode, key in (("Host@CXL", "host_cxl", "threads"), ("Hybrid", "cxl_hybrid", "dtasks"),
                                ("NDP-sync", "cxl_full", "dtasks"), ("NDP@CXL", "cxl_full_res", "dtasks")):
            groups = {}
            for r in rows:
                if r["mode"] == mode: groups.setdefault(r[key], []).append(r)
            if not groups: print(f"{plan} {wl.upper()}: no runs yet"); continue
            best = min(groups, key=lambda v: ms_per_mop(pooled(groups[v])[0]))
            rs = groups[best]
            r, lo, hi = pooled(rs)
            a, b, c, d = phases(r)
            XMAX[0] = max(XMAX[0], hi)
            fo.write(f'"{plan}" {wl.upper()} {YS[(plan, wl)]} {a:.2f} {b:.2f} {c:.2f} {d:.2f} {ms_per_mop(r):.2f} {lo:.2f} {hi:.2f} {len(rs)} {key}={best}\n')
            print(f"{src} {plan:9s} {wl.upper()} best {key}={best} n={len(rs)} total {ms_per_mop(r):.1f} [{lo:.1f}-{hi:.1f}] = {a:.1f} + {b:.1f} + {c:.1f} + {d:.1f}")
        for mode, key, name in (("host_cxl", "threads", "host"), ("cxl_full", "dtasks", "dev"), ("cxl_hybrid", "dtasks", "hyb"),
                                ("cxl_full_res", "dtasks", "res")):
            with open(os.path.join(D, "data", f"scale_{name}_{wl}.dat"), "w") as fo2:
                fo2.write(f"# {key} median min max (ms per 1M ops), wl={wl.upper()}\n")
                for v in sorted({int(r[key]) for r in rows if r["mode"] == mode}):
                    r, lo, hi = pooled([x for x in rows if x["mode"] == mode and int(x[key]) == v])
                    fo2.write(f"{v} {ms_per_mop(r):.2f} {lo:.2f} {hi:.2f}\n")

# panel a x range: room for the whisker plus its "A 123" label, rounded up to a nice step
import math
xmax = XMAX[0] * 1.35 if XMAX[0] > 0 else 160
step = 50 if xmax <= 300 else 100 if xmax <= 600 else 250 if xmax <= 1500 else 500
xmax = math.ceil(xmax / step) * step
with open(os.path.join(D, "data/phase_range.gp"), "w") as fo:
    fo.write(f"# written by make_data.py\nXMAX = {xmax}; XSTEP = {step}\n")
print(f"panel a x range: 0..{xmax} (tics every {step})")
