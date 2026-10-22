#!/bin/bash
# run_tlb.sh <device|host4k|hostthp> [outdir]
#
# Three sweeps that measure the translation structure itself, rather than just showing that a
# tax exists (that was run.sh / v1). All of them hold the number of target lines K fixed, so
# cache demand is constant and only the number of translation granules touched varies.
#
#   A  granules      span and line count K held fixed, region count R swept (S = span / R), with
#                    the lines clumped so the granules touched track R. Under "E entries of g
#                    bytes" the cost is flat while R <= E and then rises as miss_cost*(1 - E/R),
#                    which gives E. Each R is also run with the spread layout as a control: there
#                    the lines cover the span whatever R is, so spread must stay flat.
#   B  entry-size    S set explicitly, R swept. For S >= g the onset is at a fixed R (= E);
#                    for S < g it is at a fixed span (= E*g). Where the onset switches from
#                    one to the other is g.
#   C  miss-cost     round-robin vs random over the same regions. With R > E, round robin
#                    should miss on every access under LRU/FIFO, so the added time is the miss
#                    cost directly; random costs miss_cost * (1 - E/R). The pair also says
#                    something about the eviction policy.
#
# Device runs need dax0.0 in devdax mode + pxl_resourced, and must never be killed mid-run.
set -uo pipefail
cd "$(dirname "$0")/.."
MODE=${1:?usage: run_tlb.sh <device|host4k|hostthp> [outdir]}
OUT=${2:-results}
mkdir -p "$OUT"
STAMP=$(date +%Y%m%d_%H%M%S)
PIN="numactl --cpunodebind=0 --membind=0"

case "$MODE" in
  device)
    FLAGS="--device"; ACC=4000000; BUF=128
    # E is expected around 32 entries of 256 MB; sweep well past both.
    A_SPANS_GB="64 16"; A_LINES=4096; A_REGIONS="1 2 4 8 16 32 64 128 256 512 1024 2048 4096"
    B_REGION_BYTES="16777216,67108864,268435456,536870912,1073741824,2147483648"
    B_REGIONS="1,2,4,8,16,32,64,128,256"; B_LINES=4096   # K=256 is cache-resident on an MU and shows no tax
    C_REGION_BYTES="1073741824"; C_REGIONS="1,2,4,8,16,32,64,128"; C_LINES=4096
    D_REGION_BYTES="1073741824"; D_REGIONS="8,16,32,64"
    ;;
  host4k|hostthp)
    FLAGS="--host"; [ "$MODE" = hostthp ] && FLAGS="--host --thp"
    ACC=20000000; BUF=64
    # The host validation: 4 KB pages hold ~1.5-2 k STLB entries, THP moves the granule to 2 MB.
    A_SPANS_GB="16 1"; A_LINES=65536; A_REGIONS="1 4 16 64 256 1024 4096 16384 65536"
    B_REGION_BYTES="4096,65536,1048576,2097152,16777216,268435456"
    B_REGIONS="64,256,1024,4096,16384,65536"; B_LINES=65536
    C_REGION_BYTES="2097152"; C_REGIONS="64,256,1024,4096,16384"; C_LINES=65536
    D_REGION_BYTES="2097152"; D_REGIONS="256,16384"
    ;;
  *) echo "mode must be device, host4k or hostthp"; exit 2 ;;
esac
THP_CAP="--host-thp-cap-gb 8"
[ "$MODE" = hostthp ] || THP_CAP=""

echo "[$(date +%H:%M:%S)] $MODE  load $(cut -d' ' -f1 /proc/loadavg)"

# --- A: span fixed, region count swept (S = span / R, so one invocation per R) ---------------
A="$OUT/${MODE}_A_span_$STAMP.csv"
: > "$A"
for span_gb in $A_SPANS_GB; do
  for r in $A_REGIONS; do
    s=$(( (span_gb << 30) / r ))
    [ "$s" -ge $(( A_LINES / r * 64 )) ] || continue     # need at least one line slot per region
    [ $(( A_LINES % r )) -eq 0 ] || continue
    out=$($PIN ./main $FLAGS --region-bytes "$s" --regions "$r" --lines "$A_LINES" \
              --layout both --order random --accesses "$ACC" --reps 3 --buffer-gb "$BUF" $THP_CAP 2>>"$A.err")
    if [ -s "$A" ]; then echo "$out" | tail -n +2 >> "$A"; else echo "$out" >> "$A"; fi
    echo "[$(date +%H:%M:%S)]   A span=${span_gb}G R=$r S=$s"
  done
done

# --- B: region size explicit, region count swept ---------------------------------------------
echo "[$(date +%H:%M:%S)] B entry-size"
$PIN ./main $FLAGS --region-bytes "$B_REGION_BYTES" --regions "$B_REGIONS" --lines "$B_LINES" \
    --layout clump --order random --accesses "$ACC" --reps 3 --buffer-gb "$BUF" $THP_CAP \
    > "$OUT/${MODE}_B_entry_$STAMP.csv" 2> "$OUT/${MODE}_B_entry_$STAMP.err"

# --- C: round-robin vs random over the same regions ------------------------------------------
echo "[$(date +%H:%M:%S)] C miss-cost"
$PIN ./main $FLAGS --region-bytes "$C_REGION_BYTES" --regions "$C_REGIONS" --lines "$C_LINES" \
    --layout clump --order both --accesses "$ACC" --reps 3 --buffer-gb "$BUF" $THP_CAP \
    > "$OUT/${MODE}_C_miss_$STAMP.csv" 2> "$OUT/${MODE}_C_miss_$STAMP.err"

# --- D: working set at a fixed granule count (when is the tax paid at all?) -------------------
# Below ~32 lines the data stays in MU cache and no access reaches memory, so no translation
# happens; the miss cost itself also grows with the working set (software MMU metadata).
echo "[$(date +%H:%M:%S)] D working-set"
$PIN ./main $FLAGS --region-bytes "$D_REGION_BYTES" --regions "$D_REGIONS" \
    --lines-per-region 1,2,4,8,16,32,64,128,256,512,1024 \
    --layout clump --order random --accesses "$ACC" --reps 3 --buffer-gb "$BUF" $THP_CAP \
    > "$OUT/${MODE}_D_workingset_$STAMP.csv" 2> "$OUT/${MODE}_D_workingset_$STAMP.err"

echo "[$(date +%H:%M:%S)] done: $OUT/${MODE}_{A_span,B_entry,C_miss,D_workingset}_$STAMP.csv"
