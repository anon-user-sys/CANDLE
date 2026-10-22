#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"
STAMP=$(date +%Y%m%d_%H%M%S)

# Quick check: the three visibility tests (Table 4) in quick mode, with the canary before and after. Root.
QUICK=1 TESTS="k6 k7 k8" OUT="$PWD/results/smoke_$STAMP" scripts/run_suite.sh > /dev/null 2>&1 \
  && echo "PASS coherence (k6 k7 k8)" || { echo "FAIL coherence: see results/smoke_$STAMP"; exit 1; }
