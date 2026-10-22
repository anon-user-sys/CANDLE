#!/usr/bin/env bash
# Paper-grade sweep under a CONTROLLED host configuration (see diag_dma_floor.sh):
#   - mx_dma kthreads (mx_submit_thd0, mx_complete_thd0) pinned to device-socket CPUs 8,9
#   - measuring process bound to node 0 (CPU + memory)
#   - NOCSTATE=1 additionally holds /dev/cpu_dma_latency at 0 (no deep idle states)
# Without this, the DMA per-call floor is 27-44 us depending on where the scheduler
# happened to leave the kthreads (2026-10-09 diagnosis).
#
#   sudo ./scripts/run_pinned_sweep.sh                # kthreads pinned, default C-states
#   sudo NOCSTATE=1 ./scripts/run_pinned_sweep.sh     # + deep idle states blocked
# Knobs passed through to run_sweep.sh: TESTS (default "d1 d2 d4 d10"), REPS, EXTRA (default --fine), OUT.
set -u
cd "$(dirname "$0")/.."
[ "$(id -u)" -eq 0 ] || { echo "ABORT: needs root"; exit 1; }
SUB=$(pgrep -x mx_submit_thd0); COMP=$(pgrep -x mx_complete_thd0)
[ -n "$SUB" ] && [ -n "$COMP" ] || { echo "ABORT: mx kthreads not found"; exit 1; }
LATPID=""
restore() {
  taskset -pc 0-255 "$SUB" >/dev/null 2>&1; taskset -pc 0-255 "$COMP" >/dev/null 2>&1
  [ -n "$LATPID" ] && kill "$LATPID" 2>/dev/null
}
trap restore EXIT

taskset -pc 8 "$SUB" >/dev/null; taskset -pc 9 "$COMP" >/dev/null
if [ "${NOCSTATE:-0}" = 1 ]; then
  python3 -c 'import os,struct,signal; fd=os.open("/dev/cpu_dma_latency",os.O_WRONLY); os.write(fd,struct.pack("i",0)); signal.pause()' &
  LATPID=$!; sleep 1
fi
CFG="kthreads=8,9 node0 nocstate=${NOCSTATE:-0}"
export TESTS=${TESTS:-"d1 d2 d4 d10"} EXTRA=${EXTRA:---fine}
export OUT=${OUT:-results/pinned_nocstate${NOCSTATE:-0}_$(date +%Y%m%d_%H%M%S)}
mkdir -p "$OUT"; echo "$CFG" > "$OUT/CONFIG.txt"
echo "config: $CFG"
numactl --cpunodebind=0 --membind=0 ./scripts/run_sweep.sh
echo "# kthreads at end: submit=$(ps -o psr= -p "$SUB") complete=$(ps -o psr= -p "$COMP")" | tee -a "$OUT/CONFIG.txt"
