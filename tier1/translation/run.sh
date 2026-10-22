#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"
STAMP=$(date +%Y%m%d_%H%M%S)

# Paper runs for Fig. 5: the device sweeps and the host 4 KB control. Device session.
scripts/run_tlb.sh device "results/$STAMP"
scripts/run_tlb.sh host4k "results/$STAMP"
echo "Done. Fits: python3 scripts/analyze_tlb.py results/$STAMP/device_*.csv"
