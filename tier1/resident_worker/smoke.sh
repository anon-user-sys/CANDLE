#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"
STAMP=$(date +%Y%m%d_%H%M%S)

# Quick check: a resident kernel sees host-enqueued work and hands back coherent results. Root.
./main_spike > "/tmp/resident_smoke_$STAMP.txt" 2>&1 && echo "PASS resident_worker" \
  || { echo "FAIL resident_worker"; tail -5 "/tmp/resident_smoke_$STAMP.txt"; exit 1; }
