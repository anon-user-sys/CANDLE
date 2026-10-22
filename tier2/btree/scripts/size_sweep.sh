#!/bin/bash
# size_sweep.sh host|device - B+tree size impact at full parallelism, compact layout, uniform keys,
# 100M probes per batch. Tree = 41 GB x SF/1500 (SF 10..3000: 0.27-82 GB), so it crosses the host
# LLC (96 MiB) and the device TLB reach (8 GB).
#   host   (system-ram): 128 pinned cores; all-DRAM, levels first 25% DRAM, interleaved 50%, all-CXL;
#                        each with and without probes partitioned per thread
#   device (devdax):     3048 tasks, probes unpartitioned and partitioned per task
set -uo pipefail
WHAT=${1:?host|device}; cd "$(dirname "$0")/.." || exit 1
SFS=${SFS:-"10 100 300 600 1500 3000"}; DISTS=${DISTS:-uniform}
CSV=results/size_${WHAT}_$(date +%Y%m%d_%H%M%S).csv; ERR=${CSV%.csv}.err
echo "# size sweep $WHAT sfs=$SFS dists=$DISTS dax=$(daxctl list 2>/dev/null | awk -F'"' '/"mode"/{print $4}')" | tee "$CSV"; ./main --header >> "$CSV"
run() { echo "  $(date +%T) $*" >&2; "$@" >> "$CSV" 2>>"$ERR" || echo "  FAILED: $*" >&2; }
for sf in $SFS; do for d in $DISTS; do
  c=(--sf "$sf" --layout compact --dist "$d" --lookups 100M --iters 3)
  if [ "$WHAT" = host ]; then
    for pl in "1 interleave" "0.25 block" "0.5 interleave" "0 interleave"; do
      set -- $pl; f=$1; p=$2; lab="M1_dram$(awk -v f="$f" 'BEGIN{printf "%d", f*100+0.5}')_$p"
      for part in 0 128; do
        pp=(); [ "$part" != 0 ] && pp=(--partition "$part")
        run numactl --physcpubind=0-127 --membind=1 choom -n 1000 -- ./main --mode host_split --label "$lab" --par 128 \
            --dram-frac "$f" --placement "$p" "${c[@]}" "${pp[@]}"
      done
    done
  else
    for part in 0 3048; do
      pp=(); [ "$part" != 0 ] && pp=(--partition "$part")
      run numactl --cpunodebind=0 --membind=0 ./main --mode device --label M4 --par 3048 "${c[@]}" "${pp[@]}"
    done
  fi
done; done
echo "# done $(date +%T)" | tee -a "$CSV"; echo "results: $CSV"
