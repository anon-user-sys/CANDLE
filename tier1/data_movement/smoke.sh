#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"
STAMP=$(date +%Y%m%d_%H%M%S)

# Quick check (also the device canary): bit-exact DMA round trips at every size. Device session, root.
out=$(numactl --cpunodebind=0 --membind=0 ./main d1 --verify-only 2>&1) || true
ok=$(echo "$out" | grep -c 'VERIFY OK' || true); bad=$(echo "$out" | grep -c 'VERIFY FAIL' || true)
[ "$ok" -gt 0 ] && [ "$bad" -eq 0 ] && echo "PASS data_movement ($ok sizes bit-exact)" || { echo "FAIL data_movement ($bad failed)"; exit 1; }
