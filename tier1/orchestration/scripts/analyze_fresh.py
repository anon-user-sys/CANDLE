#!/usr/bin/env python3
"""Paired aged (FRESH=0) vs fresh (FRESH=1) analysis. Medians of reps."""
import sys, glob, os, re, statistics as st
import numpy as np
OUT = sys.argv[1]
def med(v): return st.median(v) if v else float('nan')

def rows(test, fresh, nkey, p50_idx, keyfilter=None):
    """{key: [p50 per rep]} for stat rows: nkey leading key fields, then stats."""
    d = {}
    for f in sorted(glob.glob(f'{OUT}/r*_t{test}_f{fresh}.txt')):
        for ln in open(f):
            if ln.startswith(('#', 'GS,')) or 'nan' in ln: continue
            p = ln.strip().split(',')
            if len(p) != nkey + 8: continue
            key = ','.join(p[:nkey])
            if keyfilter and not keyfilter(p): continue
            try: d.setdefault(key, []).append(float(p[p50_idx]))
            except ValueError: pass
    return {k: med(v) for k, v in d.items()}

def gs(test, fresh, phase):
    d = {}
    for f in sorted(glob.glob(f'{OUT}/r*_t{test}_f{fresh}.txt')):
        for ln in open(f):
            p = ln.strip().split(',')
            if len(p) == 6 and p[0] == 'GS' and p[2] == phase:
                d.setdefault(p[1], []).append(float(p[3]))
    return {k: med(v) for k, v in d.items()}

def fails():
    bad = []
    for f in glob.glob(f'{OUT}/r*_t*_f*.txt'):
        for ln in open(f):
            m = re.search(r'DISPATCH_CHECK (\S+): calls=(\d+) ok=\d+ failed=(\d+)', ln)
            if m and int(m.group(3)): bad.append((os.path.basename(f), m.group(3)))
    return bad

def pair(title, test, nkey, p50_idx, order, label, keyfilter=None, gsphase=None):
    a = rows(test, 0, nkey, p50_idx, keyfilter); f = rows(test, 1, nkey, p50_idx, keyfilter)
    ga = gs(test, 0, gsphase) if gsphase else {}; gf = gs(test, 1, gsphase) if gsphase else {}
    print(f"\n### {title}")
    hdr = f"| {label} | aged | fresh | aging (aged−fresh) | aged/fresh |"
    if gsphase: hdr += " DeviceInit aged | DeviceInit fresh |"
    print(hdr); print("|" + "---|" * (hdr.count('|') - 1))
    for k in order:
        if k not in a or k not in f: continue
        line = f"| {k} | {a[k]:.0f} | **{f[k]:.0f}** | {a[k]-f[k]:+.0f} | {a[k]/f[k]:.2f}× |"
        if gsphase:
            kk = k.split(',')[0]
            line += f" {ga.get(kk, float('nan')):.0f} | {gf.get(kk, float('nan')):.0f} |"
        print(line)
    ks = [k for k in order if k in a and k in f]
    if len(ks) >= 2:
        fa = [a[k] for k in ks]; ff = [f[k] for k in ks]
        print(f"\nspan across {label}: aged {max(fa)/min(fa):.2f}×   fresh {max(ff)/min(ff):.2f}×")
    return a, f

print("# Aged vs fresh handle — paired sweep")
b = fails(); print(f"\ndispatch failures: {'NONE' if not b else b}")

TCS = [str(x) for x in [1,16,64,128,256,512,1024,2048,4096,8192,16384,32768,65536,131072]]
a1, f1 = pair("T1 — dispatch sweep (p50 µs)", 1, 1, 4, TCS, "tasks")
if f1:
    flat = [f1[k] for k in ['1','16','64'] if k in f1]
    hi = [int(k) for k in TCS if k in f1 and int(k) >= 1024]
    x = np.array(hi, float); y = np.array([f1[str(k)] for k in hi])
    sl, ic = np.polyfit(x, y, 1)
    r2 = 1 - ((y - (sl*x+ic))**2).sum() / ((y - y.mean())**2).sum()
    print(f"\n**fresh α_launch** (median of tc 1/16/64) = {med(flat):.0f} µs")
    print(f"**fresh α_task** (fit tc ≥ 1024) = {sl:.4f} µs/task  (R²={r2:.4f})")

# T3
d = {}
for fr in (0, 1):
    for fn in sorted(glob.glob(f'{OUT}/r*_t3_f{fr}.txt')):
        for ln in open(fn):
            p = ln.strip().split(',')
            if len(p) == 7 and p[0].isdigit() and 'nan' not in ln:
                d.setdefault((fr, int(p[0])), []).append((float(p[1]), float(p[3]), float(p[5])))
print("\n### T3 — execute/sync split, fresh handle (mean µs)")
print("| tasks | execute | sync | total | execute share | aged total |"); print("|---|---|---|---|---|---|")
for tc in sorted({k[1] for k in d}):
    fr = d.get((1, tc)); ag = d.get((0, tc))
    if fr:
        e = med([x[0] for x in fr]); s = med([x[1] for x in fr]); t = med([x[2] for x in fr])
        at = med([x[2] for x in ag]) if ag else float('nan')
        print(f"| {tc} | {e:.1f} | {s:.0f} | {t:.0f} | {100*e/t:.2f}% | {at:.0f} |")

pair("T6 — Sub count (p50 µs)", 6, 1, 4, ['1','2','4','8','16','24'], "numSub", gsphase='DeviceInit')
pair("T7 — batch size (p50 µs)", 7, 2, 5, ['1,64','2,32','4,16','8,8','16,4','32,2','64,1'], "bs,threads")
pair("T8 — cluster bitmap (p50 µs)", 8, 2, 5, ['0x00000001,1','0x00000003,2','0x0000000f,4','0x00000000,all'], "mask,pop")

a14 = rows(14, 0, 2, 5); f14 = rows(14, 1, 2, 5)
print("\n### T14 — partial subFree (p50 µs)")
print("| released | aged base | aged after | aged Δ | fresh base | fresh after | fresh Δ |"); print("|---|---|---|---|---|---|---|")
for rc in sorted({k.split(',')[0] for k in list(a14)+list(f14) if k.split(',')[0].isdigit()}, key=int):
    ab, aa = a14.get(f'{rc},baseline_all_subs'), a14.get(f'{rc},after_release')
    fb, fa = f14.get(f'{rc},baseline_all_subs'), f14.get(f'{rc},after_release')
    if None in (ab, aa, fb, fa): continue
    print(f"| {rc} | {ab:.0f} | {aa:.0f} | {aa-ab:+.0f} | {fb:.0f} | {fa:.0f} | **{fa-fb:+.0f}** |")
