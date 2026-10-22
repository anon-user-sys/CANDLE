#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"
STAMP=$(date +%Y%m%d_%H%M%S)

# Paper runs for §4.1.4: device dequeue cost and host publication cost (drain_bench, 1M and 10M items). Root.
scripts/run_enqueue_bench.sh enqueue "$PWD/results/enqueue_$STAMP"
