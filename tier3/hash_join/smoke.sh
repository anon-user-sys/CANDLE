#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"
STAMP=$(date +%Y%m%d_%H%M%S)
export DATA_ROOT=${DATA_ROOT:-$HOME/tpch}
# Quick check: Host@CXL, Hybrid and NDP@CXL on TPC-H SF 1, 50% selected, one iteration each.
# Passes if every run matches the reference join; the device canary runs before and after. Root.
[ -f "$DATA_ROOT/sf_1/orders.tbl" ] || { echo "FAIL hash_join: no TPC-H SF 1 tables under $DATA_ROOT/sf_1 (set DATA_ROOT)"; exit 1; }
OUT_ROOT="results/smoke_$STAMP" ITERS=1 MODES="2 3 4" PARTITIONS=1024 PREFETCH=32 HOST_DIRECT=1 OWNER_BUILD=1 \
  scripts/run_devdax_modes.sh 1 50 > /dev/null 2>&1 \
  && echo "PASS hash_join (Host@CXL, Hybrid, NDP@CXL validated)" \
  || { echo "FAIL hash_join: see results/smoke_$STAMP"; exit 1; }
