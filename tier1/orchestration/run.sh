#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"
STAMP=$(date +%Y%m%d_%H%M%S)

# Paper runs for Fig. 3 and Table 2. Device session. Output: results/*_<stamp>/
scripts/fresh_sweep.sh "results/fresh_$STAMP"        # T1 T3 T6 T7 T8 T14, aged vs. fresh handles (Fig. 3a, 3c, Table 2)
scripts/final_sweep.sh "results/final_$STAMP"        # T1-T15, three runs (§4.1 text: batch size, streams, host threads)
mkdir -p "results/t15_$STAMP"
for r in 1 2 3; do for f in 0 1; do                  # T15 iterations per dispatch (Fig. 3d)
  FRESH=$f BENCH_CPU=none ./main_profiler t15 > "results/t15_$STAMP/r${r}_f$f.txt" 2>&1
done; done
scripts/aging_run.sh "results/aging"                 # handle aging, 6,000 dispatches (Fig. 3b)
echo "Done. Point figures/make_data.py (D, T) at results/fresh_$STAMP and results/t15_$STAMP, then ./figures/render.sh"
