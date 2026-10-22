#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"
STAMP=$(date +%Y%m%d_%H%M%S)

# Paper runs for Fig. 6 and Table 3. Device session, root.
OUT="results/pinned_$STAMP" TESTS="d1 d10 d12" scripts/run_pinned_sweep.sh   # kthreads pinned, node 0, default C-states
scripts/diag_dma_floor.sh                                                      # DMA floor vs. kthread placement and C-states (§4.3.2)
echo "Done. Point figures/make_data.py (PINNED, FOLLOW) at results/pinned_$STAMP, then ./figures/render.sh"
