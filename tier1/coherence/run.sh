#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"
STAMP=$(date +%Y%m%d_%H%M%S)

# Paper runs for Fig. 7 and Table 4 (k1 k2 k4 k5 k6 k7 k8 k10, plus k13 for §4.3.3). Device session, root.
TESTS="k1 k2 k4 k5 k6 k7 k8 k10 k13" OUT="$PWD/results/suite_$STAMP" scripts/run_suite.sh
echo "Done. Point figures/make_data.py (RUN) at results/suite_$STAMP, then ./figures/render.sh"
