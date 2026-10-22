#!/usr/bin/env bash
# Follow-up checks after the pinned sweep (2026-10-09):
#   1. D12 (read/write mix) x4 under the paper configuration (kthreads 8,9, node 0)
#   2. Orchestration: does mx_dma kthread placement move the 4.1 null-kernel launch cost?
#      T1 on fresh handles (FRESH=1, as in 4.1), kthreads NEAR (8,9) vs FAR (136,137), interleaved x3
#   3. D1 x2 more under the paper configuration, to see whether 256 MiB D2H DMA (39-41 ms in
#      the pinned sweep vs 26 ms on 09-26 / under no-C-state) is stable or bimodal
#
#   sudo ./diag_followup.sh        (~5 min: T1 ~15 s each, D1 ~85 s each)
set -u
cd "$(dirname "$0")/.."
[ "$(id -u)" -eq 0 ] || { echo "ABORT: needs root"; exit 1; }
SUB=$(pgrep -x mx_submit_thd0); COMP=$(pgrep -x mx_complete_thd0)
ORCH="$(cd "$(dirname "$0")/../../orchestration" && pwd)/main_profiler"
[ -x "$ORCH" ] || { echo "ABORT: $ORCH missing"; exit 1; }
OUT=${OUT:-results/followup_$(date +%Y%m%d_%H%M%S)}; mkdir -p "$OUT"
trap 'taskset -pc 0-255 $SUB >/dev/null 2>&1; taskset -pc 0-255 $COMP >/dev/null 2>&1; chown -R ${SUDO_USER:-root} $OUT' EXIT
near() { taskset -pc 8 "$SUB" >/dev/null; taskset -pc 9 "$COMP" >/dev/null; }
far()  { taskset -pc 136 "$SUB" >/dev/null; taskset -pc 137 "$COMP" >/dev/null; }
kt()   { echo "submit=$(ps -o psr= -p "$SUB") complete=$(ps -o psr= -p "$COMP")"; }
echo "out: $OUT"

echo "== 1. D12 x4 (near)"; near
for r in 1 2 3 4; do
  f="$OUT/d12_r$r.txt"; numactl --cpunodebind=0 --membind=0 ./main d12 > "$f" 2>&1
  echo "d12 r$r rc=$? $(kt) rows=$(grep -cE '^[a-zA-Z_]+,[0-9]' "$f")"
done

echo "== 2. orchestration T1, near vs far (fresh handles, unpinned as in 4.1)"
for r in 1 2 3; do
  for pl in near far; do
    $pl; f="$OUT/t1_${pl}_r$r.txt"; t0=$(date +%s)
    FRESH=1 BENCH_CPU=none timeout 900 "$ORCH" t1 > "$f" 2>&1; rc=$?
    echo "t1 $pl r$r rc=$rc $(( $(date +%s)-t0 ))s $(kt)"
  done
done

echo "== 3. D1 x2 (near), 256 MiB stability"; near
for r in 4 5; do
  f="$OUT/d1_r$r.txt"; numactl --cpunodebind=0 --membind=0 ./main d1 --fine > "$f" 2>&1
  echo "d1 r$r rc=$? $(grep COPY_CHECK "$f") verify_fails=$(grep -c 'VERIFY FAIL' "$f")" \
       "256M_D2H=$(awk -F, '$1=="copyFromDevice" && $3==268435456 {print $4}' "$f")"
done
echo "done -> $OUT"
