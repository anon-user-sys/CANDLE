#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"
export DATA_ROOT=${DATA_ROOT:-$HOME/tpch}    # TPC-H data: $DATA_ROOT/sf_<SF>/
SESSION=$(daxctl list 2>/dev/null | grep -o '"mode":"[a-z-]*"' | head -1 | cut -d'"' -f4)
SF=${SF:-1500}
# The paper's runs (Fig. 8, Table 5). Each session runs its half:
if [ "$SESSION" = devdax ]; then                       # NDP and the host-DRAM reference; root for the return path
  CORE_BIND=1 PREFETCH=4 MODES="M1 M4_memcpy" scripts/run_scan.sh "$SF"
  MODES_RET="serial chunked" STEPS=32 OUT_BYTES=4 FLUSHES=range THRS="25 50" scripts/return_ab.sh "$SF"   # Table 5
  for p in 256 512 1024 2048; do                       # materialize-and-return curve of Fig. 8
    PAR=$p MODES_RET=chunked STEPS=32 OUT_BYTES=4 FLUSHES=range THRS="25 50" scripts/return_ab.sh "$SF"
  done
  scripts/bw_sweep.sh                                  # device read rate quoted in §5.1
else                                                   # system-ram: Host@CXL and the Tiered placements
  CORE_BIND=1 MODES=M1_split DRAM_FRACS="1 0.75 0.5 0.25 0" PLACEMENTS=interleave scripts/run_scan.sh "$SF"
fi
echo "Done ($SESSION). Point figures/make_data.py at the new results/*.csv, then ./figures/render.sh"
