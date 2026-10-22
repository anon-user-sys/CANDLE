#!/bin/bash
# run_btree.sh <sf> [iters] [outdir]
#
# B+tree point-lookup grid (the index counterpart of tier2/scan/run_scan.sh):
# placement mode x node size x probe distribution x miss rate x parallelism, on a B+tree over the
# 1.5M x SF TPC-H order keys (generated from dbgen's key formula, no input file needed).
#
#   M1         tree + probes in host DRAM, host threads             --membind=0     (any dax mode)
#   M1_split   restricted host + CXL expander: exactly DRAM_FRAC of the tree (and probes) in DRAM,
#              the rest on CXL node 2, mbind per 2 MB chunk; PLACEMENT=interleave|block (block =
#              upper levels + first leaves in DRAM, the top-down node layout)    (system-ram only)
#   M5         everything on the CXL node, host threads                          (system-ram only)
#   M4         tree + probes in device memory, MU tasks                          (devdax only)
#
# Device runs need no root: the devdax udev rule opens /dev/dax0.0 to users, and this workload
# uses host stores + flushHostCache for upload, never DMA.
set -uo pipefail

SF=${1:?sf}; ITERS=${2:-3}; OUT=${3:-results}
cd "$(dirname "$0")/.." || exit 1
MODES=${MODES:-"M1 M1_split M5 M4"}
want() { [[ " $MODES " == *" $1 "* ]]; }
LOOKUPS=${LOOKUPS:-100M}
NODES=${NODES:-"256 4096"}            # node bytes (fanout 15 / 255); ignored with LAYOUT=compact
LAYOUT=${LAYOUT:-classic}             # classic | compact (64 B nodes, implicit children)
DEV_PARTITION=${DEV_PARTITION:-0}     # 1: M4 range-partitions the probes per task (--partition = tasks)
HOST_PARTITION=${HOST_PARTITION:-0}   # 1: host modes range-partition the probes per thread (--partition = threads)
DISTS=${DISTS:-"uniform zipf"}        # zipf = scrambled Zipfian, THETA below
THETA=${THETA:-0.99}
MISSES=${MISSES:-0}                   # % of probes for absent keys
GROUP_SIZES=${GROUP_SIZES:-1}                # lookups in lockstep with prefetch (1 = plain loop)
MATS=${MATS:-0}                       # 1 = also write every lookup's value (see README: device caveat)
HOST_PAR=${HOST_PAR:-"8 16 32 64 128"}
DEV_PAR=${DEV_PAR:-"256 512 1024 2048 3048"}
CORE_BIND=${CORE_BIND:-0}             # 1: host modes get exactly <par> cores (0..par-1)
DRAM_FRACS=${DRAM_FRACS:-${DRAM_FRAC:-0.3333}}
PLACEMENTS=${PLACEMENTS:-interleave}
cpus() { if [ "$CORE_BIND" = 1 ]; then echo "--physcpubind=0-$(( $1 - 1 ))"; else echo "--cpunodebind=0"; fi; }

mkdir -p "$OUT"
STAMP=$(date +%Y%m%d_%H%M%S)
CSV="$OUT/btree_sf${SF}_$STAMP.csv"
ERR="$OUT/btree_sf${SF}_$STAMP.err"
DAXMODE=$(daxctl list 2>/dev/null | awk -F'"' '/"mode"/{print $4}')
echo "# sf=$SF dax=$DAXMODE iters=$ITERS lookups=$LOOKUPS layout=$LAYOUT dev_partition=$DEV_PARTITION host_partition=$HOST_PARTITION nodes=$NODES dists=$DISTS theta=$THETA misses=$MISSES groups=$GROUP_SIZES mats=$MATS core_bind=$CORE_BIND dram_fracs=$DRAM_FRACS placements=$PLACEMENTS load=$(cut -d' ' -f1 /proc/loadavg) $(numactl -H | awk '/free:/{printf "%s=%sMB ", $2, $4}')" | tee "$CSV"
./main --header >> "$CSV"

FAIL=0
run() {   # run <label> <mode> <par> [extra main args] -- <prefix cmd...>
    local label=$1 mode=$2 par=$3; shift 3
    local extra=()
    [ "$mode" != device ] && [ "$HOST_PARTITION" = 1 ] && extra+=(--partition "$par")
    while [ $# -gt 0 ] && [ "$1" != "--" ]; do extra+=("$1"); shift; done
    shift
    echo "  $(date +%H:%M:%S) $label par=$par ${extra[*]}" >&2
    "$@" choom -n 1000 -- ./main --mode "$mode" --label "$label" --sf "$SF" --par "$par" --lookups "$LOOKUPS" \
         --iters "$ITERS" --verify 1 "${extra[@]}" >> "$CSV" 2>>"$ERR" \
        || { echo "  FAILED: $label par=$par ${extra[*]} (see $ERR)" >&2; FAIL=1; }
}

for nb in $NODES; do for dist in $DISTS; do for miss in $MISSES; do for g in $GROUP_SIZES; do for mat in $MATS; do
    common=(--layout "$LAYOUT" --node-bytes "$nb" --dist "$dist" --theta "$THETA" --miss-pct "$miss" --group "$g" --materialize "$mat")
    want M1 && for p in $HOST_PAR; do run M1 host "$p" "${common[@]}" -- numactl $(cpus "$p") --membind=0; done
    if [ "$DAXMODE" = devdax ]; then
        want M4 && for p in $DEV_PAR; do run M4 device "$p" "${common[@]}" $([ "$DEV_PARTITION" = 1 ] && echo --partition "$p") -- numactl --cpunodebind=0 --membind=0; done
    else
        want M5 && for p in $HOST_PAR; do run M5 host_cxl "$p" "${common[@]}" -- numactl $(cpus "$p"); done
        if want M1_split; then
            for f in $DRAM_FRACS; do for pl in $PLACEMENTS; do for p in $HOST_PAR; do
                pct=$(awk -v f="$f" 'BEGIN{printf "%d", f*100+0.5}')
                run "M1_dram${pct}_$pl" host_split "$p" "${common[@]}" --dram-frac "$f" --placement "$pl" -- numactl $(cpus "$p") --membind=1
            done; done; done
        fi
    fi
done; done; done; done; done

if [ "$DAXMODE" = devdax ]; then echo "# skipped M1_split and M5 (need system-ram)" | tee -a "$CSV"
else                             echo "# skipped M4 (needs devdax)" | tee -a "$CSV"; fi
echo "# done $(date +%H:%M:%S) load=$(cut -d' ' -f1 /proc/loadavg)" | tee -a "$CSV"
echo "results: $CSV"
exit $FAIL
