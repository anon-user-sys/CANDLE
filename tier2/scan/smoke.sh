#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"
export DATA_ROOT=${DATA_ROOT:-$HOME/tpch}    # TPC-H data: $DATA_ROOT/sf_<SF>/
SESSION=$(daxctl list 2>/dev/null | grep -o '"mode":"[a-z-]*"' | head -1 | cut -d'"' -f4)
# Quick check: the device scan on TPC-H SF 1, 50% selected. Passes if every row matches the reference.
TBL=${TBL:-$DATA_ROOT/sf_1/lineitem.tbl}
[ -f "$TBL" ] || { echo "FAIL scan: no TPC-H SF 1 data at $TBL (set DATA_ROOT, see README)"; exit 1; }
out=$(./main --mode device --tbl "$TBL" --par 256 --threshold 25 --iters 1 2>&1) || { echo "FAIL scan"; echo "$out" | tail -5; exit 1; }
rows=$(echo "$out" | awk -F, 'NF>=15 && $15=="1"' | wc -l); bad=$(echo "$out" | awk -F, 'NF>=15 && $15=="0"' | wc -l)
[ "$rows" -gt 0 ] && [ "$bad" -eq 0 ] && ! echo "$out" | grep -q MISMATCH && echo "PASS scan ($rows rows validated)" \
  || { echo "FAIL scan ($bad rows wrong)"; echo "$out" | grep -E 'MISMATCH|error' | head -3; exit 1; }
