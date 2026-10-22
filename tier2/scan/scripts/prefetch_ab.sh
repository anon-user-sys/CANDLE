#!/bin/bash
# prefetch_ab.sh [sf] [iters]
#
# Before/after for the device scan's software prefetch (M4 only, devdax, count mode).
# For each selectivity and task count, runs every prefetch window back to back
# (window 0 = the original kernel), so drift on the shared box hits all windows alike.
# Device upload is memcpy (root) - run from a shell where `sudo -v` has been done.
#
#   PAR="1024 3048"  WINDOWS="0 1 2 4 8 16 32"  (override via env)
set -uo pipefail

SF=${1:-100}; ITERS=${2:-5}
cd "$(dirname "$0")/.." || exit 1
TBL=${DATA_ROOT:-$HOME/tpch}/sf_${SF}/lineitem.tbl
BIN=${DATA_ROOT:-$HOME/tpch}/sf_${SF}/lineitem_cols.bin
if [ -f "$BIN" ]; then SRC=(--bin "$BIN"); else SRC=(--tbl "$TBL"); fi
PAR=${PAR:-"1024 3048"}
WINDOWS=${WINDOWS:-"0 1 2 4 8 16 32"}
mkdir -p results
STAMP=$(date +%Y%m%d_%H%M%S)
CSV=results/prefetch_ab_sf${SF}_$STAMP.csv
ERR=results/prefetch_ab_sf${SF}_$STAMP.err

[ "$(daxctl list 2>/dev/null | awk -F'"' '/"mode"/{print $4}')" = devdax ] || { echo "needs devdax mode"; exit 1; }
echo "# prefetch A/B sf=$SF iters=$ITERS par=$PAR windows=$WINDOWS load=$(cut -d' ' -f1 /proc/loadavg)" | tee "$CSV"
echo "window,$(./main --header)" >> "$CSV"

FAIL=0
for thr in 0 25 50; do
  for p in $PAR; do
    for w in $WINDOWS; do
      echo "  $(date +%H:%M:%S) thr=$thr par=$p window=$w" >&2
      sudo numactl --cpunodebind=0 --membind=0 choom -n 1000 -- ./main --mode device --upload memcpy \
          --label M4_pf$w "${SRC[@]}" --par "$p" --threshold "$thr" --prefetch "$w" \
          --iters "$ITERS" --verify 1 2>>"$ERR" | grep -E '^M4_' | sed "s/^/$w,/" >> "$CSV" \
          || { echo "  FAILED: thr=$thr par=$p window=$w (see $ERR)" >&2; FAIL=1; }
    done
  done
done

# Summary: median scan_ms of iterations >= 2, and speedup vs window 0.
python3 - "$CSV" <<'EOF'
import csv, sys, statistics as st
rows = [r for r in csv.DictReader(l for l in open(sys.argv[1]) if not l.startswith('#')) if r.get('ok') in ('0', '1')]
bad = [r for r in rows if r['ok'] != '1']
g = {}
for r in rows:
    if int(r['iter']) >= 2:
        g.setdefault((int(r['threshold']), int(r['par']), int(r['window'])), []).append(float(r['scan_ms']))
print(f"\n{'sel':>5} {'tasks':>5} | " + " ".join(f"w{w:<6}" for w in sorted({k[2] for k in g})) + " | best")
for thr, par in sorted({(k[0], k[1]) for k in g}):
    ws = sorted(w for (t, p, w) in g if t == thr and p == par)
    med = {w: st.median(g[(thr, par, w)]) for w in ws}
    base = med.get(0)
    best = min(med, key=med.get)
    print(f"{thr*2:>4}% {par:>5} | " + " ".join(f"{med[w]:7.1f}" for w in ws)
          + (f" | w{best}: {base/med[best]:.2f}x vs w0" if base else ""))
print(f"\nrows {len(rows)}, validation failures {len(bad)}")
EOF
echo "results: $CSV"
exit $FAIL
