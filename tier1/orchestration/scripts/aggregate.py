#!/usr/bin/env python3
"""Aggregate the 3-run paper sweep: median + min/max per measurement row.

Wall-clock rows are whatever does NOT start with the GS sentinel (C1).
Reports relative spread so you can see which effects survive run-to-run noise.
"""
import sys, os, re, glob, statistics as st

OUT = sys.argv[1] if len(sys.argv) > 1 else '.'

def rows(path, want_p50_idx=True):
    """Yield (key, p50) for wall-clock stat rows: key fields then 8 stat cols."""
    out = {}
    if not os.path.exists(path): return out
    for ln in open(path):
        if ln.startswith('#') or ln.startswith('GS,'): continue
        p = ln.rstrip('\n').split(',')
        if len(p) < 9: continue
        # locate the stats block: 8 trailing numeric cols (mean,std,min,p50,p95,p99,max,n)
        try:
            nums = [i for i, v in enumerate(p) if re.fullmatch(r'-?\d+(\.\d+)?', v)]
            if len(nums) < 8: continue
            base = len(p) - 8
            if base <= 0: continue
            key = ','.join(p[:base])
            out[key] = float(p[base + 3])      # p50
        except Exception:
            pass
    return out

tests = [str(i) for i in range(1, 16)]
print(f"{'test':>5} {'key':>26} {'median':>10} {'min':>10} {'max':>10} {'spread':>7} {'n':>3}")
print("-" * 76)
flagged = []
for t in tests:
    per_key = {}
    for f in sorted(glob.glob(f'{OUT}/r*_t{t}.txt')):
        for k, v in rows(f).items():
            per_key.setdefault(k, []).append(v)
    for k, vs in sorted(per_key.items()):
        if len(vs) < 2: continue
        med, lo, hi = st.median(vs), min(vs), max(vs)
        spread = hi / lo if lo else float('inf')
        mark = '  <-- noisy' if spread > 1.30 else ''
        print(f"{('t'+t):>5} {k[:26]:>26} {med:>10.1f} {lo:>10.1f} {hi:>10.1f} {spread:>6.2f}x {len(vs):>3}{mark}")
        if spread > 1.30: flagged.append((t, k, spread))
print()
print(f"rows with >1.30x run-to-run spread: {len(flagged)}")
for t, k, s in flagged[:15]:
    print(f"   t{t} {k}: {s:.2f}x")
