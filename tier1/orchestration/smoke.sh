#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"
STAMP=$(date +%Y%m%d_%H%M%S)

# Quick check: one fresh-handle T1 sweep. Passes if every dispatch succeeds. Device session.
out=$(FRESH=1 BENCH_CPU=none ./main_profiler t1 2>&1) || { echo "FAIL orchestration"; echo "$out" | tail -5; exit 1; }
echo "$out" | grep -q "T1_dispatch_sweep" && echo "PASS orchestration" || { echo "FAIL orchestration (no T1 output)"; exit 1; }
