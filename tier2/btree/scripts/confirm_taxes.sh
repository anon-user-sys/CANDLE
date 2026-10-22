#!/bin/bash
# confirm_taxes.sh - B+tree checks of two Tier 1 taxes (SF 1500, compact, uniform, devdax):
#  orchestration: batch-size sweep, NDP (3048 tasks, partitioned) vs host DRAM (128 pinned cores)
#  translation:   key-range sweep for NDP (partition P), adding points between the ablation's 1/3/6/24/3048
set -uo pipefail
cd "$(dirname "$0")/.." || exit 1
STAMP=$(date +%Y%m%d_%H%M%S)
B=results/batch_sf1500_$STAMP.csv; T=results/ranges_sf1500_$STAMP.csv
./main --header | tee "$B" > "$T"
common=(--sf 1500 --layout compact --dist uniform)
for n in 10K 100K 1M 10M 100M; do
  it=5; [ "$n" = 100M ] && it=3
  echo "  $(date +%T) batch $n NDP" >&2
  numactl --cpunodebind=0 --membind=0 ./main --mode device --label M4_batch --par 3048 --partition 3048 --lookups $n --iters $it "${common[@]}" >> "$B" 2>>${B%.csv}.err || echo "  FAILED batch $n NDP" >&2
  echo "  $(date +%T) batch $n host" >&2
  numactl --physcpubind=0-127 --membind=0 ./main --mode host --label M1_batch --par 128 --lookups $n --iters $it "${common[@]}" >> "$B" 2>>${B%.csv}.err || echo "  FAILED batch $n host" >&2
done
for P in 2 4 5 8 12 96; do
  echo "  $(date +%T) ranges P=$P" >&2
  numactl --cpunodebind=0 --membind=0 ./main --mode device --label M4_ranges --par 3048 --partition $P --lookups 100M --iters 3 "${common[@]}" >> "$T" 2>>${T%.csv}.err || echo "  FAILED P=$P" >&2
done
echo "results: $B $T"
