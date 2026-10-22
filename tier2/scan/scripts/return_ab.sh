#!/bin/bash
# return_ab.sh [sf] [iters]
#
# MX1 with the selected keys returned to host DRAM (devdax, root): serial (scan, then copy) vs
# pipelined (copy while the scan runs), at 50% and 100% selectivity, 3048 tasks, prefetch w4.
# Columns ret_ms / e2e_ms: serial = copy after the kernel / scan + copy; pipelined = the tail
# left after the kernel / kernel start to last key in host DRAM. Every returned key is checked.
#   MODES_RET="serial chunked"  THRS="25 50"  FLUSHES="range boundary none"  RET_THREADS=16
#   PAR=3048 (MU tasks; label _t<PAR> otherwise)  OUT_BYTES="8 4" (4 = uint32 offset from the task's base key, label _o4)  PF=4 (device prefetch window)
#   WRITE_LINES="0 1" (device whole-line stores)  RET_APIS="direct dma" (CPU loads | copyFromDevice)
#   CHUNK_ROWS=65536  STEPS="16" (chunked: launches per scan, label gets _s<steps>)  (env). Before each iteration the output is poisoned on the device and dropped
#   from the host cache (untimed), so stale reads fail validation instead of matching old keys.
set -uo pipefail
SF=${1:-1500}; ITERS=${2:-3}
cd "$(dirname "$0")/.." || exit 1
BIN=${DATA_ROOT:-$HOME/tpch}/sf_${SF}/lineitem_cols.bin; TBL=${DATA_ROOT:-$HOME/tpch}/sf_${SF}/lineitem.tbl
if [ -f "$BIN" ]; then SRC=(--bin "$BIN"); else SRC=(--tbl "$TBL"); fi
[ "$(daxctl list 2>/dev/null | awk -F'"' '/"mode"/{print $4}')" = devdax ] || { echo "needs devdax mode"; exit 1; }
mkdir -p results
CSV=results/return_sf${SF}_$(date +%Y%m%d_%H%M%S).csv; ERR=${CSV%.csv}.err
echo "# return A/B sf=$SF iters=$ITERS threads=${RET_THREADS:-16} chunk=${CHUNK_ROWS:-65536} load=$(cut -d' ' -f1 /proc/loadavg)" | tee "$CSV"
./main --header >> "$CSV"
for thr in ${THRS:-25 50}; do
  for fl in ${FLUSHES:-range}; do
  for ob in ${OUT_BYTES:-8}; do
  for wl in ${WRITE_LINES:-0}; do
  for api in ${RET_APIS:-direct}; do
  for m in ${MODES_RET:-serial chunked}; do
  for steps in $( [ "$m" = chunked ] && echo "${STEPS:-16}" || echo 0 ); do
    lab="M4_ret_${m}_flush_$fl"; [ "$m" = chunked ] && lab="M4_ret_chunked_s${steps}_flush_$fl"
    [ "$api" = dma ] && lab="${lab/flush_$fl/dma}"
    [ "$wl" = 1 ] && lab="${lab}_wl"
    [ "$ob" = 4 ] && lab="${lab}_o4"
    [ "${PF:-4}" != 4 ] && lab="${lab}_pf${PF}"
    [ "${PAR:-3048}" != 3048 ] && lab="${lab}_t${PAR}"
    pk=1; [ "$thr" = 0 ] && pk=0
    echo "  $(date +%H:%M:%S) $lab thr=$thr" >&2
    sudo numactl --cpunodebind=0 --membind=0 choom -n 1000 -- ./main --mode device --upload memcpy \
        --label "$lab" "${SRC[@]}" --par "${PAR:-3048}" --threshold "$thr" --materialize 1 --prefetch "${PF:-4}" --prefetch-keys $pk --out-bytes "$ob" \
        --return "$m" --ret-flush "$fl" --ret-api "$api" --write-lines "$wl" --return-threads "${RET_THREADS:-16}" --chunk-rows "${CHUNK_ROWS:-65536}" --ret-steps "$( [ "$steps" = 0 ] && echo 16 || echo "$steps")" \
        --iters "$ITERS" --verify 1 >> "$CSV" 2>>"$ERR" || echo "  FAILED: $lab thr=$thr (see $ERR)" >&2
  done
  done
  done
  done
  done
  done
done
echo "# done $(date +%H:%M:%S)" | tee -a "$CSV"
python3 - "$CSV" <<'PY'
import csv, sys, statistics as st
rows = [r for r in csv.DictReader(l for l in open(sys.argv[1]) if not l.startswith('#')) if r.get('ok') in ('0','1')]
g = {}
for r in rows:
    if int(r['iter']) >= 1:
        g.setdefault((r['label'], int(r['threshold'])), []).append(r)
print(f"\n{'mode':34} {'sel':>5} {'scan ms':>9} {'return ms':>10} {'end-to-end ms':>14}  ok")
for (lab, thr), v in sorted(g.items(), key=lambda kv: (kv[0][1], kv[0][0])):
    m = lambda k: st.median(float(x[k]) for x in v)
    print(f"{lab:34} {thr*2:>4}% {m('scan_ms'):9.0f} {m('ret_ms'):10.0f} {m('e2e_ms'):14.0f}  {all(x['ok']=='1' for x in v)}")
print(f"rows {len(rows)}, failed {sum(r['ok']!='1' for r in rows)}")
PY
echo "results: $CSV"
