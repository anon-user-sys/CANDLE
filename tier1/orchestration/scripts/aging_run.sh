#!/usr/bin/env bash
# Fig. 3b: dispatch-handle aging. 6,000 dispatches of 64 tasks through one handle, then the same
# with the handle rebuilt every 220 dispatches (T1's 20 warm-up + 200 timed). Device session.
#   scripts/aging_run.sh [outdir]   (default: results/aging, read by figures/make_fig_data.py)
set -u
BIN="$(cd "$(dirname "$0")/.." && pwd)/main_profiler"
OUT=${1:-"$(cd "$(dirname "$0")/.." && pwd)/results/aging"}
mkdir -p "$OUT"
T2_N=6000 T2_TC=64 BENCH_CPU=none "$BIN" t2 > "$OUT/aging_single6000.txt" 2>&1
T2_N=6000 T2_TC=64 T2_REBUILD=220 BENCH_CPU=none "$BIN" t2 > "$OUT/aging_rebuild220.txt" 2>&1
echo "DONE -> $OUT"
