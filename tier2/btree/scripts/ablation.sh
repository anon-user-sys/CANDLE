#!/bin/bash
# ablation.sh [sf] - contribution of each lookup optimization at full parallelism:
#   layout classic (256 B nodes) | compact (64 B, implicit children)
#   x partition none | P key ranges (3, 6: around the 8 GB device TLB reach; 24; 3048 = per task)
#   x dist uniform | zipf, on the device (3048 tasks) and host DRAM (128 pinned cores).
# Group prefetch is measured separately (results/group_sf1500.csv).
set -uo pipefail
SF=${1:-1500}; ITERS=${ITERS:-3}; LOOKUPS=${LOOKUPS:-100M}
cd "$(dirname "$0")/.." || exit 1
CSV=results/ablation_sf${SF}_$(date +%Y%m%d_%H%M%S).csv; ERR=${CSV%.csv}.err
DAX=$(daxctl list 2>/dev/null | awk -F'"' '/"mode"/{print $4}')
echo "# ablation sf=$SF dax=$DAX iters=$ITERS lookups=$LOOKUPS" | tee "$CSV"; ./main --header >> "$CSV"
for dist in uniform zipf; do for layout in classic compact; do for P in 0 3 6 24 3048; do
  common=(--sf "$SF" --node-bytes 256 --layout "$layout" --dist "$dist" --lookups "$LOOKUPS" --iters "$ITERS")
  pd=(); [ "$P" != 0 ] && pd=(--partition "$P")
  if [ "$DAX" = devdax ]; then
    echo "  $(date +%T) M4 $layout P=$P $dist" >&2
    numactl --cpunodebind=0 --membind=0 ./main --mode device --label M4 --par 3048 "${common[@]}" "${pd[@]}" >> "$CSV" 2>>"$ERR" || echo "  FAILED M4 $layout P=$P $dist" >&2
  fi
  hp=(); [ "$P" != 0 ] && hp=(--partition $(( P > 128 ? 128 : P )))
  echo "  $(date +%T) M1 $layout P=$P $dist" >&2
  numactl --physcpubind=0-127 --membind=0 ./main --mode host --label M1 --par 128 "${common[@]}" "${hp[@]}" >> "$CSV" 2>>"$ERR" || echo "  FAILED M1 $layout P=$P $dist" >&2
done; done; done
echo "# done $(date +%T)" | tee -a "$CSV"; echo "results: $CSV"
