#!/bin/bash
# Data for fig_blink_row: YCSB A, 10M keys, scramble, 8M ops, 3 reps.
#   host_cxl at 1..128 threads (setup excluded), cxl_full batch 1M at 64..3048 device tasks (combining on).
# Result: results/fig_row_<time>.csv
set -uo pipefail
cd "$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
WL="${WL:-a}"
HOST_LIST="${HOST_LIST-1 2 4 8 16 32 64 128}"     # host_cxl thread counts ("" = none)
HOST_ARGS="${HOST_ARGS-}"                         # extra host_cxl args, e.g. "--host-dispatch=batched --batch=1M"
OUT_PREFIX="${OUT_PREFIX:-fig_row}"
FULL_ARGS="${FULL_ARGS-}"                         # extra cxl_full args, e.g. "--resident"
HYB_LIST="${HYB_LIST-}"                           # cxl_hybrid device task counts ("" = none)
HYB_THREADS="${HYB_THREADS:-128}"                 # cxl_hybrid host threads (parallel locate)
DTASK_LIST="${DTASK_LIST-64 128 256 512 1024 2048 3048}" # cxl_full device task counts ("" = none)
mkdir -p results
if [ -n "${APPEND_TO:-}" ]; then OUT="$APPEND_TO"; else
OUT="results/${OUT_PREFIX}_$(date +%Y%m%d_%H%M%S).csv"
echo "mode,workload,threads,dtasks,rep,mops,setup_ms,batch_total_ms,launch_ms,stage_ms,readback_ms,readback_inval_ms,dedup_ms,replay_ms,write_prep_ms,split_ms,report_ms,host_locate_ms,host_write_ms,locate_host_ms,post_inval_ms" > "$OUT"
fi
one() { local mode=$1 rep=$2; shift 2
  local log; log=$(timeout 1800 "./blink_${mode}_tp" -o 8M --keys=10M --scramble --workload="$WL" "$@" 2>&1 | tr '\r' '\n')
  local c; c=$(echo "$log" | grep "^\[Config\]"); local ph; ph=$(echo "$log" | grep -E "CXL-phases|HOST-batched" || true)
  f() { echo "$c" | sed -n "s/.* $1=\([0-9]*\).*/\1/p"; }
  g() { echo "$ph" | sed -n "s/.* $1=\([0-9.]*\).*/\1/p"; }
  echo "$mode,$WL,$(f threads),$(f dtasks),$rep,$(echo "$log" | grep Throughput | awk '{print $(NF-1)}'),$(echo "$log" | sed -n 's/.*partitioning \([0-9.]*\) ms.*/\1/p'),$(g batch_total),$(g launch),$(g stage),$(g readback),$(g readback_inval),$(g dedup),$(g replay),$(g write_prep),$(g split),$(g report),$(echo "$ph" | grep HOST-batched | sed -n "s/.* locate=\([0-9.]*\).*/\1/p"),$(echo "$ph" | grep HOST-batched | sed -n "s/.* write=\([0-9.]*\).*/\1/p"),$(g locate_host),$(g post_inval)" | tee -a "$OUT"
}
for t in $HOST_LIST; do for rep in 1 2 3; do one host_cxl $rep -t $t $HOST_ARGS; done; done
for d in $DTASK_LIST; do for rep in 1 2 3; do one cxl_full $rep -t 4 --batch=1M --dtasks=$d $FULL_ARGS; done; done
for d in $HYB_LIST; do for rep in 1 2 3; do one cxl_hybrid $rep -t $HYB_THREADS --batch=1M --dtasks=$d; done; done
echo "results: $OUT"
