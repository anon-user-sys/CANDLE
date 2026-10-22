#!/usr/bin/env python3
"""Regenerate the Fig. 3 data from the raw runs. Run from tier1/orchestration.
  fig1 (Fig. 3a)        <- results/data_fresh/r*_t1_f{0,1}.txt  (paired sweep 2026-09-14)
  fig_subcount (3c)     <- results/data_fresh/r*_t6_f{0,1}.txt
  fig_amortization (3d) <- results/data_t15fix/r*_f{0,1}.txt    (T15 fix 2026-09-14)
  fig3_aging (3b)       <- results/aging/aging_{single6000,rebuild220}.txt from scripts/aging_run.sh,
                           only when present (the plotted data was kept; its raw runs were not)
"""
import glob, os, statistics as st
D = 'results/data_fresh'; T = 'results/data_t15fix'; O = 'figures/data'

def t6(fresh):
    lat, di = {}, {}
    for f in sorted(glob.glob(f'{D}/r*_t6_f{fresh}.txt')):
        for ln in open(f):
            p = ln.strip().split(',')
            if p[0] == 'GS' and p[2] == 'DeviceInit':
                di.setdefault(int(p[1]), []).append(float(p[3]))
            elif len(p) == 9 and p[0].isdigit():
                lat.setdefault(int(p[0]), []).append(float(p[4]))  # p50
    return lat, di

la, da = t6(0); lf, df = t6(1)
with open(f'{O}/fig_subcount_fresh_aged.dat', 'w') as o:
    o.write('# T6 paired sweep 2026-09-14, 64 tasks, median/min/max of 3 runs (p50 us)\n')
    o.write('# numSub fresh_med fresh_min fresh_max aged_med aged_min aged_max DevInit_fresh DevInit_aged\n')
    for s in sorted(lf):
        F, A = lf[s], la[s]
        o.write(f'{s} {st.median(F):.1f} {min(F):.1f} {max(F):.1f} '
                f'{st.median(A):.1f} {min(A):.1f} {max(A):.1f} '
                f'{st.median(df[s]):.1f} {st.median(da[s]):.1f}\n')

def t15(fresh):
    d = {}
    for f in sorted(glob.glob(f'{T}/r*_f{fresh}.txt')):
        for ln in open(f):
            p = ln.strip().split(',')
            if len(p) == 7 and p[1] == 'persistent':
                d.setdefault(int(p[0]), []).append(float(p[5]))  # us per iteration
    return d

f1, f0 = t15(1), t15(0)
with open(f'{O}/fig_amortization_fresh_aged.dat', 'w') as o:
    o.write('# T15 multi-iteration dispatch, 64 tasks, tax per iteration (us)\n')
    o.write('# fresh: median/min/max of 3 runs (fresh handle per timed dispatch); aged: 1 control run, one shared handle\n')
    o.write('# k fresh_med fresh_min fresh_max aged\n')
    for k in sorted(f1):
        v = f1[k]
        o.write(f'{k} {st.median(v):.3f} {min(v):.3f} {max(v):.3f} {st.median(f0[k]):.3f}\n')

def t1(fresh):
    lat = {}
    for f in sorted(glob.glob(f'{D}/r*_t1_f{fresh}.txt')):
        for ln in open(f):
            p = ln.strip().split(',')
            if len(p) == 9 and p[0].isdigit():
                lat.setdefault(int(p[0]), []).append(float(p[4]))  # p50
    return lat

for fresh, name in ((0, 'aged'), (1, 'fresh')):
    lat = t1(fresh)
    with open(f'{O}/fig1_{name}.dat', 'w') as o:
        o.write(f'# paired sweep 2026-09-14, FRESH={fresh}: taskCount p50_median p50_min p50_max\n')
        for tc in sorted(lat):
            v = lat[tc]
            o.write(f'{tc} {st.median(v):.1f} {min(v):.1f} {max(v):.1f}\n')

def aging(src, dst, label, bin_=50):
    """median latency per bin of `bin_` dispatches, from T2's taskCount,invocation,latency_us rows"""
    xs = []
    for ln in open(src):
        p = ln.strip().split(',')
        if len(p) == 3 and p[0].isdigit() and p[1].isdigit():
            xs.append(float(p[2]))
    with open(dst, 'w') as o:
        o.write(f'# dispatch_index  median_us  (tc=64, {label}, bin={bin_})\n')
        for b in range(0, len(xs) - bin_ + 1, bin_):
            o.write(f'{b + bin_ // 2} {st.median(xs[b:b + bin_]):.1f}\n')

A = 'results/aging'
if os.path.exists(f'{A}/aging_single6000.txt'):
    aging(f'{A}/aging_single6000.txt', f'{O}/fig3_aging_single6000.dat', 'single map')
if os.path.exists(f'{A}/aging_rebuild220.txt'):
    aging(f'{A}/aging_rebuild220.txt', f'{O}/fig3_aging_rebuild.dat', 'rebuild every 220')
