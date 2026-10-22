#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"
export DATA_ROOT=${DATA_ROOT:-$HOME/tpch}    # TPC-H data: $DATA_ROOT/sf_<SF>/
SESSION=$(daxctl list 2>/dev/null | grep -o '"mode":"[a-z-]*"' | head -1 | cut -d'"' -f4)
# Quick check: 1 M device lookups in an SF 1 tree (keys from dbgen's formula, no input files).
out=$(./main --mode device --sf 1 --par 256 --lookups 1M --iters 1 2>&1) || { echo "FAIL btree"; echo "$out" | tail -5; exit 1; }
rows=$(echo "$out" | awk -F, 'NF>=22 && $22=="1"' | wc -l); bad=$(echo "$out" | awk -F, 'NF>=22 && $22=="0"' | wc -l)
[ "$rows" -gt 0 ] && [ "$bad" -eq 0 ] && ! echo "$out" | grep -q MISMATCH && echo "PASS btree ($rows rows validated)" \
  || { echo "FAIL btree ($bad rows wrong)"; echo "$out" | grep -E 'MISMATCH|error' | head -3; exit 1; }
