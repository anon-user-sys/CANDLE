#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"
STAMP=$(date +%Y%m%d_%H%M%S)
# The paper's runs (Fig. 13, §6.2). Device session. Each run writes results/<name>_<time>.csv
D="16 32 64 128 256 512 1024 2048 3048"
DTASK_LIST="$D" scripts/bench_fig_row.sh                                     # Host@CXL per operation + NDP-sync
OUT_PREFIX=fig_hostbatched HOST_ARGS="--host-dispatch=batched --batch=1M" DTASK_LIST="" scripts/bench_fig_row.sh   # Host@CXL, batched
OUT_PREFIX=fig_hybrid HOST_LIST="" DTASK_LIST="" HYB_LIST="$D" scripts/bench_fig_row.sh                           # Hybrid
OUT_PREFIX=fig_resident HOST_LIST="" FULL_ARGS="--resident" DTASK_LIST="$D" scripts/bench_fig_row.sh              # NDP@CXL (resident worker)
scripts/soak_hybrid.sh                                                       # §6.2.1 invalidation order
scripts/bench_combine.sh                                                     # §6.2.5 request combining
scripts/bench_nocombine_exec.sh                                              # §6.2.5 without combining
echo "Done. figures/make_data.py picks up the newest results/fig_*.csv; then ./figures/render.sh"
