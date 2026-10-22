#!/usr/bin/env bash
# Result-return bench (ret_bench): can a RUNNING kernel hand results back safely,
# and what does it cost? Every accepted result is validated word by word against
# a poisoned buffer with a per-run salt.
#   sudo ./scripts/run_return_bench.sh [outdir]
# Order: ping-pong P=1 first (lowest risk), then streaming at P = 1, 64, 3048.
# 2026-10-08 first run: asyncMessage took ~200 us/message and stopped delivering
# after 63 in a back-to-back ping-pong, so msg is latency-only (M=50) and not
# streamed. Second run: msg delivered ZERO messages in a fresh process (stall
# persists across processes), so msg is off by default; WITH_MSG=1 re-enables it.
# Stops at the first run that times out or leaves results unwritten (rc != 0).
# early_bad > 0 is a FINDING (protocol unsafe), not a failure: keep going.
set -u
cd "$(dirname "$0")/.."
[ "$(id -u)" -eq 0 ] || { echo "ABORT: run with sudo (non-root host access SIGBUS)"; exit 1; }
daxctl list 2>/dev/null | grep -q '"mode":"devdax"' \
  || { echo "ABORT: dax0.0 not in devdax mode"; exit 1; }
pgrep -x main_profiler >/dev/null && { echo "ABORT: orchestration benchmark is using the device"; exit 1; }
[ -x ./main_spike ] || { echo "ABORT: ./main_spike missing; run ./build.sh"; exit 1; }

OUT=${1:-$PWD/results/spike_return_$(date +%Y%m%d_%H%M%S)}
mkdir -p "$OUT"
run() {   # name, args...
  local name=$1; shift
  local f="$OUT/$name.txt"
  timeout 120 ./main_spike "$@" > "$f" 2>&1; local rc=$?
  echo "[$name] rc=$rc  $(grep -E '^(RET|PP) ' "$f")"
  grep -E 'first bad' "$f" | sed 's/^/    /'
  if [ "$rc" -ne 0 ]; then
    echo "STOP: $name failed (rc=$rc). Check device: sudo xcena_cli num-device"; tail -5 "$f"
    chown -R "${SUDO_USER:-root}" "$OUT" 2>/dev/null; exit 1
  fi
}
for r in 1 2 3; do
  for m in tag counter ${WITH_MSG:+msg}; do
    M=10000; [ "$m" = msg ] && M=50   # msg: ~200 us each, stalls after 63 (2026-10-08)
    run "pp_${m}_r$r" --pingpong $M --mode $m
  done
done
for r in 1 2 3; do
  for P in 1 64 3048; do
    for m in counter tag msg; do
      [ "$m" = msg ] && continue    # doorbell stalls after ~63 messages: unusable for streaming
      run "st_${m}_P${P}_r$r" --ret 100000 --par $P --mode $m
    done
  done
done
chown -R "${SUDO_USER:-root}" "$OUT" 2>/dev/null
echo "results -> $OUT"
