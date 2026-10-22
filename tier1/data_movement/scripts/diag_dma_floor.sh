#!/usr/bin/env bash
# Why does the DMA per-call floor vary by session (27 us 09-26, 37-43 us 10-09)?
# Tests one factor at a time with D2 (64 B per call, ~1 s per run), 3 runs each.
# The measuring process is always bound to node 0 (the device's socket).
#
#   sudo ./diag_dma_floor.sh            # D2 conditions only (~1 min)
#   sudo FULL=1 ./diag_dma_floor.sh     # + D1 x3 under the best condition (~5 min)
#
# Conditions:
#   E0 baseline     kthreads where the scheduler left them, C-states as configured
#   E1 kt-near      mx_submit/mx_complete pinned to node-0 CPUs 8,9
#   E2 kt-far       mx_submit/mx_complete pinned to node-1 CPUs 136,137
#   E3 no-cstate    E1 + /dev/cpu_dma_latency held at 0 (no deep idle states)
#   E4 baseline     E0 repeated at the end, to catch drift during the script
# Everything is restored on exit (kthread affinity 0-255, latency fd closed).
set -u
cd "$(dirname "$0")/.."
[ "$(id -u)" -eq 0 ] || { echo "ABORT: needs root"; exit 1; }
daxctl list 2>/dev/null | grep -q '"mode":"devdax"' || { echo "ABORT: not devdax"; exit 1; }
[ -x ./main ] || { echo "ABORT: ./main missing"; exit 1; }

OUT=${OUT:-results/diag_$(date +%Y%m%d_%H%M%S)}; mkdir -p "$OUT"
SUB=$(pgrep -x mx_submit_thd0); COMP=$(pgrep -x mx_complete_thd0)
[ -n "$SUB" ] && [ -n "$COMP" ] || { echo "ABORT: mx kthreads not found"; exit 1; }
LATPID=""

restore() {
  taskset -pc 0-255 "$SUB"  >/dev/null 2>&1
  taskset -pc 0-255 "$COMP" >/dev/null 2>&1
  [ -n "$LATPID" ] && kill "$LATPID" 2>/dev/null
  chown -R "${SUDO_USER:-root}" "$OUT" 2>/dev/null
}
trap restore EXIT

state() {  # where the kthreads are and what the host looks like
  echo "# $(date -Is) submit=$(ps -o psr= -p "$SUB") complete=$(ps -o psr= -p "$COMP")" \
       "submit_aff=$(taskset -pc "$SUB" | awk '{print $NF}') complete_aff=$(taskset -pc "$COMP" | awk '{print $NF}')" \
       "gov=$(cat /sys/devices/system/cpu/cpu8/cpufreq/scaling_governor)" \
       "load=$(cut -d' ' -f1 /proc/loadavg) resourced_since=$(systemctl show pxl_resourced -p ActiveEnterTimestamp --value)"
}

run_d2() {  # $1 = condition label
  for r in 1 2 3; do
    f="$OUT/$1_r$r.txt"
    { state; numactl --cpunodebind=0 --membind=0 ./main d2; state; } > "$f" 2>&1
    printf "%-10s r%s  " "$1" "$r"
    awk -F, '/^copyToDevice|^copyFromDevice|^roundtrip_cxl/{printf "%s p50=%.1f  ", $1, $7}' "$f"
    grep -q "failed=0" "$f" && echo || echo " <- CHECK $f"
  done
}

echo "out: $OUT"; state | tee "$OUT/start_state.txt"
run_d2 E0_base

taskset -pc 8 "$SUB" >/dev/null; taskset -pc 9 "$COMP" >/dev/null
run_d2 E1_near

taskset -pc 136 "$SUB" >/dev/null; taskset -pc 137 "$COMP" >/dev/null
run_d2 E2_far

taskset -pc 8 "$SUB" >/dev/null; taskset -pc 9 "$COMP" >/dev/null
python3 -c 'import os,struct,signal; fd=os.open("/dev/cpu_dma_latency",os.O_WRONLY); os.write(fd,struct.pack("i",0)); signal.pause()' &
LATPID=$!; sleep 1
run_d2 E3_nocst

if [ "${FULL:-0}" = 1 ]; then   # full D1 under E3: floor, large-size bimodality, crossovers
  for r in 1 2 3; do
    f="$OUT/E3_d1_r$r.txt"
    { state; numactl --cpunodebind=0 --membind=0 ./main d1 --fine; state; } > "$f" 2>&1
    echo "E3 d1 r$r  $(grep COPY_CHECK "$f")  $(grep -c 'VERIFY FAIL' "$f") verify-fails"
  done
fi

kill "$LATPID" 2>/dev/null; LATPID=""
taskset -pc 0-255 "$SUB" >/dev/null; taskset -pc 0-255 "$COMP" >/dev/null
run_d2 E4_base
echo "done -> $OUT"
