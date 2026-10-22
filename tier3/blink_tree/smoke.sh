#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"
STAMP=$(date +%Y%m%d_%H%M%S)
# Quick check: YCSB-A on a 1 M-key tree with Host@CXL, Hybrid, NDP-sync and NDP@CXL (resident worker).
# Each validation binary checks every result and the final tree state. Device session.
fail=0
run() { local name=$1; shift
  if timeout 600 "$@" > "/tmp/blink_smoke_${name}_$STAMP.txt" 2>&1; then echo "  ok   $name"
  else echo "  FAIL $name (/tmp/blink_smoke_${name}_$STAMP.txt)"; fail=1; fi; }
ARGS=(-o 1M --keys=1M --workload=a --batch=4096 --dtasks=64)
run host_cxl   ./blink_host_cxl_val   -t 16 "${ARGS[@]}"
run hybrid     ./blink_cxl_hybrid_val -t 16 "${ARGS[@]}"
run ndp_sync   ./blink_cxl_full_val   -t 4  "${ARGS[@]}"
run ndp_at_cxl ./blink_cxl_full_val   -t 4  "${ARGS[@]}" --resident
[ "$fail" = 0 ] && echo "PASS blink_tree (4 plans validated)" || { echo "FAIL blink_tree"; exit 1; }
