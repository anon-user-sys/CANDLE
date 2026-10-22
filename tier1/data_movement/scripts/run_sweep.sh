#!/usr/bin/env bash
# Data-movement sweep: 3 runs x the figure-weight tests, fresh process each.
#
#   sudo ./scripts/run_sweep.sh              # full sweep incl. direct paths (needs root)
#        ./scripts/run_sweep.sh --no-direct  # DMA + host-baseline subset, no root needed
#
# Direct host access to memAlloc memory SIGBUSes without root, so the guards
# below refuse to start rather than risk it.
set -u
cd "$(dirname "$0")/.."
TESTS=${TESTS:-"d1 d2 d4 d10"}
REPS=${REPS:-3}
TIMEOUT=${TIMEOUT:-2400}
MODE=""; [ "${1:-}" = "--no-direct" ] && MODE="--no-direct"
EXTRA=${EXTRA:-}   # extra ./main flags, e.g. EXTRA=--fine

[ -x ./main ] || { echo "ABORT: ./main missing — run ./build.sh"; exit 1; }
daxctl list 2>/dev/null | grep -q '"mode":"devdax"' || {
  echo "ABORT: dax0.0 not in devdax mode. Fix with:"
  echo "  sudo daxctl reconfigure-device --mode=devdax --force dax0.0"
  echo "  sudo systemctl restart pxl_resourced.service   # only AFTER the flip"
  exit 1; }
if [ -z "$MODE" ] && [ "$(id -u)" -ne 0 ]; then
  echo "ABORT: direct paths need root. Use sudo, or pass --no-direct."; exit 1; fi
pgrep -x main >/dev/null && { echo "ABORT: another ./main is running"; exit 1; }

OUT=${OUT:-results/sweep_$(date +%Y%m%d_%H%M%S)}
mkdir -p "$OUT"
echo "mode: ${MODE:-full (direct paths on)}   tests: $TESTS   reps: $REPS   extra: ${EXTRA:-none}"
echo "out:  $OUT"
for r in $(seq 1 "$REPS"); do
  for t in $TESTS; do
    f="$OUT/r${r}_${t}.txt"; t0=$(date +%s)
    timeout "$TIMEOUT" ./main "$t" $MODE $EXTRA > "$f" 2>&1; rc=$?
    secs=$(( $(date +%s) - t0 )); rows=$(grep -vE '^#|^$' "$f" | grep -E '^[A-Za-z0-9_]+,' | grep -cE ',[0-9]+\.?[0-9]*(,|$)' || true)
    bad=$(grep -c "DATA INVALID\|VERIFY FAIL" "$f")
    st=ok
    [ "$rc" -eq 124 ] && st=TIMEOUT
    [ "$rows" -lt 3 ] && st=NO_DATA
    [ "$bad" -gt 0 ]  && st=INVALID
    [ "$rc" -ne 0 ] && [ "$rc" -ne 124 ] && st="CRASH(rc=$rc)"
    printf "r%s %-4s rc=%-3s %4ss rows=%-4s %s\n" "$r" "$t" "$rc" "$secs" "$rows" "$st"
    grep -h "COPY_CHECK" "$f" | sed 's/^/     /'
    # A bit-exact mismatch is only attributable if we capture contention NOW:
    # the state clears within minutes and then cannot be proven either way.
    if [ "$st" = INVALID ]; then
      a="$OUT/r${r}_${t}.attribution.txt"
      { echo "=== captured $(date -Is) after INVALID in $f ==="
        echo "--- xcena_cli num-device";  xcena_cli num-device 2>&1
        echo "--- xcena_cli device-info"; xcena_cli device-info 0 2>&1
        echo "--- fuser /dev/dax*";       fuser -v /dev/dax* 2>&1
        echo "--- daxctl list";           daxctl list 2>&1
        echo "--- who";                   who 2>&1
        echo "--- device processes";      ps -eo user,pid,etime,cmd | grep -iE 'pxl|xcena|mubin|[.]/main' | grep -v grep
        echo "--- dmesg tail";            dmesg -T 2>&1 | grep -iE 'mx_dma|cxl|dax|poison|uncorrect' | tail -20
      } > "$a" 2>&1
      echo "     ATTRIBUTION captured -> $a"
      grep -h "VERIFY FAIL" "$f" | sed 's/^/     /'
    fi
    [ "$st" = ok ] || echo "     ^^ see $f"
  done
done
chown -R "${SUDO_USER:-root}" "$OUT" 2>/dev/null
echo "done -> $OUT"
