#!/bin/bash
# run_scan.sh <sf> [iters] [outdir]
#
# Standalone TPC-H scan-filter grid: placement mode x materialize {0,1} x selectivity
# {0,50,100%} (threshold 0/25/50) x parallelism, on $DATA_ROOT/sf_<sf>/lineitem.tbl.
#
#   M1         columns in host DRAM, host threads; overflow -> swap   --membind=0     (any dax mode)
#   M1_tiered  host DRAM first, overflow -> CXL node 2                --membind=0,2   (system-ram only)
#   M5         columns placed on the CXL NUMA node, host threads      numa_alloc_onnode (system-ram only)
#   M1_split   restricted host + CXL expander: exactly DRAM_FRAC of every column in DRAM, rest on
#              CXL node 2, placed with mbind (PLACEMENT=interleave|block); reference copy on node 1,
#              so no hugetlb budget and no sudo are needed                   (system-ram only)
#
# With a DRAM budget (dram_budget.sh apply 64), M1 vs M1_tiered differ only once the columns
# exceed the budget; below it they are the same run.
#   M4   columns in device memory, MU tasks              PXL                (devdax only)
#
# M4 uses upload=memcpy (what the hash join does; needs root). The DMA upload variant is
# opt-in (RUN_DMA=1) since the 2026-10-01 DMA incident - see README "Device DMA caveats".
# Device task counts stop at 3048 - the usable MU thread count.
set -uo pipefail

SF=${1:?sf}; ITERS=${2:-5}; OUT=${3:-results}
cd "$(dirname "$0")/.." || exit 1
# Input: lineitem.tbl, or a column file from gen_cols.sh for scale factors stored without text.
BIN=${BIN:-${DATA_ROOT:-$HOME/tpch}/sf_${SF}/lineitem_cols.bin}
TBL=${TBL:-${DATA_ROOT:-$HOME/tpch}/sf_${SF}/lineitem.tbl}
if [ -f "$BIN" ]; then SRC=(--bin "$BIN"); elif [ -f "$TBL" ]; then SRC=(--tbl "$TBL"); else echo "missing $TBL and $BIN"; exit 1; fi
MATS=${MATS:-"0 1"}                   # MATS=0 skips materialize (output too big for the budget)
# OUT_BYTES="8 4": materialized element (4 = uint32 offset from the task's base key, label _o4;
# only applies when mat=1, count runs are done once).
OUT_BYTES=${OUT_BYTES:-8}
# MODES: which modes to run (default all that the dax mode allows), e.g. MODES="M1_tiered M4_memcpy".
MODES=${MODES:-"M1 M1_tiered M5 M1_split M4_memcpy M4_dma"}
want() { [[ " $MODES " == *" $1 "* ]]; }
mkdir -p "$OUT"
STAMP=$(date +%Y%m%d_%H%M%S)
CSV="$OUT/tpch_scan_sf${SF}_$STAMP.csv"
ERR="$OUT/tpch_scan_sf${SF}_$STAMP.err"

THRESHOLDS="${THRESHOLDS:-0 25 50}"
HOST_PAR="${HOST_PAR:-8 16 32 64 128}"
DEV_PAR="${DEV_PAR:-256 512 1024 2048 3048}"
SUDO=${SUDO:-sudo}
RUN_DMA=${RUN_DMA:-0}
# CORE_BIND=1: restrict host modes to exactly <par> cores (0..par-1 on node 0) instead of all of
# node 0, so the thread count is a core budget. PREFETCH=N: device prefetch window for M4
# (key prefetch is turned off at threshold 0, where no key is read).
CORE_BIND=${CORE_BIND:-0}
PREFETCH=${PREFETCH:-0}
DRAM_FRACS=${DRAM_FRACS:-${DRAM_FRAC:-0.3333}}   # e.g. "1 0.75 0.5 0" (fraction of each column in DRAM)
PLACEMENTS=${PLACEMENTS:-interleave}
cpus() { if [ "$CORE_BIND" = 1 ]; then echo "--physcpubind=0-$(( $1 - 1 ))"; else echo "--cpunodebind=0"; fi; }
m4_pf() { [ "$PREFETCH" -gt 0 ] && { echo "--prefetch $PREFETCH"; [ "$1" = 0 ] && echo "--prefetch-keys 0"; }; }

DAXMODE=$(daxctl list 2>/dev/null | awk -F'"' '/"mode"/{print $4}')
echo "# sf=$SF src=${SRC[*]} dax=$DAXMODE iters=$ITERS core_bind=$CORE_BIND prefetch=$PREFETCH dram_fracs=$DRAM_FRACS placements=$PLACEMENTS load=$(cut -d' ' -f1 /proc/loadavg) $(numactl -H | awk '/free:/{printf "%s=%sMB ", $2, $4}')swap_used=$(free -m | awk '/Swap:/{print $3}')MB" | tee "$CSV"
./main --header >> "$CSV"

# Parse once up front so the first grid point doesn't pay for it.
./main --mode host "${SRC[@]}" --par 1 --iters 0 --verify 0 2>>"$ERR" || { echo "load failed, see $ERR"; exit 1; }

FAIL=0
run() {   # run <label> <mode> <par> <thr> <mat> [extra main args] -- <prefix cmd...>
    local label=$1 mode=$2 par=$3 thr=$4 mat=$5; shift 5
    local extra=()
    while [ $# -gt 0 ] && [ "$1" != "--" ]; do extra+=("$1"); shift; done
    shift
    echo "  $(date +%H:%M:%S) $label par=$par thr=$thr mat=$mat ${extra[*]}" >&2
    # choom: if memory runs out (DRAM budget + swap), the OOM killer takes this run, never
    # another user's process on this shared box.
    "$@" choom -n 1000 -- ./main --mode "$mode" --label "$label" "${SRC[@]}" --par "$par" --threshold "$thr" \
         --materialize "$mat" --iters "$ITERS" --verify 1 "${extra[@]}" >> "$CSV" 2>>"$ERR" \
        || { echo "  FAILED: $label par=$par thr=$thr mat=$mat (see $ERR)" >&2; FAIL=1; }
}

for mat in $MATS; do
 for ob in $( [ "$mat" = 1 ] && echo $OUT_BYTES || echo 8 ); do
  sfx=""; [ "$ob" = 4 ] && sfx="_o4"
  for thr in $THRESHOLDS; do
    want M1 && for p in $HOST_PAR; do run M1 host $p "$thr" "$mat" -- numactl $(cpus $p) --membind=0; done
    if [ "$DAXMODE" = devdax ]; then
        want M4_memcpy && for p in $DEV_PAR; do run M4_memcpy device $p "$thr" "$mat" --upload memcpy $(m4_pf $thr) -- $SUDO numactl --cpunodebind=0 --membind=0; done
        if [ "$RUN_DMA" = 1 ] && want M4_dma; then
            for p in $DEV_PAR; do run M4_dma device $p "$thr" "$mat" --upload dma -- numactl --cpunodebind=0 --membind=0; done
        fi
    else
        want M1_tiered && for p in $HOST_PAR; do run M1_tiered host     $p "$thr" "$mat" -- numactl $(cpus $p) --membind=0,2; done
        want M5        && for p in $HOST_PAR; do run M5        host_cxl $p "$thr" "$mat" -- numactl $(cpus $p); done
        if want M1_split; then
            for f in $DRAM_FRACS; do for pl in $PLACEMENTS; do for p in $HOST_PAR; do
                pct=$(awk -v f="$f" 'BEGIN{printf "%d", f*100+0.5}')
                run "M1_dram${pct}_$pl$sfx" host_split $p "$thr" "$mat" --dram-frac "$f" --placement "$pl" --out-bytes "$ob" -- numactl $(cpus $p) --membind=1
            done; done; done
        fi
    fi
  done
 done
done

if [ "$DAXMODE" = devdax ]; then echo "# skipped M1_tiered and M5 (need system-ram)" | tee -a "$CSV"
else                             echo "# skipped M4 (needs devdax)" | tee -a "$CSV"; fi
echo "# done $(date +%H:%M:%S) load=$(cut -d' ' -f1 /proc/loadavg)" | tee -a "$CSV"
echo "results: $CSV"
exit $FAIL
