#!/bin/bash
# node_sweep_host.sh [label dram_frac placement] - host lookup time vs node size, SF 1500, 128 pinned
# cores, uniform keys, 100M probes; compact (64 B) and classic 128 B-4 KB, each with probes
# unpartitioned and partitioned per thread. Default: all in DRAM (mode host, any dax mode).
# With a DRAM fraction (system-ram only): e.g.  ./node_sweep_host.sh M1_dram25_block 0.25 block
set -uo pipefail
cd "$(dirname "$0")/.." || exit 1
LABEL=${1:-M1}; FRAC=${2:-}; PL=${3:-interleave}
CSV=results/node_host_${LABEL}_$(date +%Y%m%d_%H%M%S).csv; ERR=${CSV%.csv}.err
echo "# node sweep host label=$LABEL frac=$FRAC placement=$PL" | tee "$CSV"; ./main --header >> "$CSV"
if [ -z "$FRAC" ]; then MODE=(--mode host); MEM=0; else MODE=(--mode host_split --dram-frac "$FRAC" --placement "$PL"); MEM=1; fi
run() { echo "  $(date +%T) $*" >&2; numactl --physcpubind=0-127 --membind=$MEM choom -n 1000 -- ./main "${MODE[@]}" --label "$LABEL" \
        --sf 1500 --par 128 --dist uniform --lookups 100M --iters 3 "$@" >> "$CSV" 2>>"$ERR" || echo "  FAILED: $*" >&2; }
for part in 0 128; do
  pp=(); [ "$part" != 0 ] && pp=(--partition "$part")
  run --layout compact "${pp[@]}"
  for nb in 128 256 512 1024 2048 4096; do run --layout classic --node-bytes "$nb" "${pp[@]}"; done
done
echo "results: $CSV"
