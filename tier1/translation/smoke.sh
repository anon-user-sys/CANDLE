#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"
STAMP=$(date +%Y%m%d_%H%M%S)

# Quick check: one pointer-chase configuration on the device. Passes if the checksum matches the host replay.
out=$(numactl --cpunodebind=0 --membind=0 ./main --device --region-bytes 1073741824 --regions 16 --lines 4096 \
      --order random --layout clump --accesses 200000 --reps 1 --buffer-gb 16 2>&1) || { echo "FAIL translation"; echo "$out" | tail -5; exit 1; }
rows=$(echo "$out" | grep -c ',1$' || true); bad=$(echo "$out" | grep -c ',0$' || true)
[ "$rows" -gt 0 ] && [ "$bad" -eq 0 ] && echo "PASS translation ($rows rows, sum_ok)" || { echo "FAIL translation (sum_ok=0 or no rows)"; exit 1; }
