#!/bin/bash
# node_sweep.sh - NDP lookup time vs node size, SF 1500, 3048 tasks, uniform keys, 100M probes.
# classic layout at 128 B..4 KB (header + keys + child/value arrays, binary search) and the compact
# layout (64 B, implicit children); each with probes unpartitioned and partitioned per task.
set -uo pipefail
cd "$(dirname "$0")/.." || exit 1
SF=${SF:-1500}
CSV=results/node_ndp_sf${SF}_$(date +%Y%m%d_%H%M%S).csv; ERR=${CSV%.csv}.err
echo "# node sweep NDP sf=$SF" | tee "$CSV"; ./main --header >> "$CSV"
run() { echo "  $(date +%T) $*" >&2; numactl --cpunodebind=0 --membind=0 ./main --mode device --label M4 --sf "$SF" \
        --par 3048 --dist uniform --lookups 100M --iters 3 "$@" >> "$CSV" 2>>"$ERR" || echo "  FAILED: $*" >&2; }
for part in 0 3048; do
  pp=(); [ "$part" != 0 ] && pp=(--partition "$part")
  run --layout compact "${pp[@]}"
  for nb in 128 256 512 1024 2048 4096; do run --layout classic --node-bytes "$nb" "${pp[@]}"; done
done
echo "results: $CSV"
