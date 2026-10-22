#!/bin/bash
# run_quick4.sh - fig_hj_row (c)/(d) at each plan's best partition count, and the sf_10 bars of (a)
# (2026-10-18 16:1x). Host@CXL/Hybrid owner build at P = 8,192 (sf_100 sweep: 732/451/390/440 ms at
# P = 1,024/4,096/8,192/16,384); NDP@CXL stays at P = 1,024 (877/1,004 at 4,096/8,192), whose (c)/(d) runs exist.
# Uses the C34
# cached ground truth (validation took ~2 min per sf_100 iteration). Uses the C33 column cache
# (col_cache/; each sf_100 process then loads in seconds instead of ~22 min of text parsing).
#
#   sudo bash scripts/run_quick.sh            # box in devdax mode, nothing else running
#   tail -f quick_*.log                       # progress
#
# Every plan uses the owner-computes build (Host@CXL/Hybrid: C29 on the host; NDP@CXL: C19 on the
# device), 1,024 partitions, prefetch 32, host-direct probe. Same failure rule as run_night.sh: a
# failed configuration is skipped if its canary passes; a failed canary stops the queue.

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
cd "${PROJECT_ROOT}" || exit 1
[ "$(id -u)" -eq 0 ] || { echo "run with sudo"; exit 1; }

LOG="${PROJECT_ROOT}/quick4_$(date +%Y%m%d_%H%M%S).log"
OWNER="${SUDO_USER:-$(id -un)}"
note() { echo "[$(date +%T)] $*" | tee -a "${LOG}"; chown "${OWNER}:" "${LOG}" 2>/dev/null; }

n_cache=$(ls col_cache/*.u32 2>/dev/null | wc -l)
note "quick queue start, ${n_cache} cached columns in col_cache/ (6 expected for sf_10 + sf_100)"

step() {
    local label=$1 sf=$2 sel=$3; shift 3
    note "START ${label}: sf=${sf} sel=${sel} $*"
    env "$@" bash "${SCRIPT_DIR}/run_devdax_modes.sh" "${sf}" "${sel}" > /dev/null 2>&1
    local rc=$?
    local dir; dir=$(ls -td ./results/devdax_modes_sf${sf}_sel${sel}_* 2>/dev/null | head -1)
    if [ "${rc}" -eq 0 ]; then note "  ok   ${dir}"; return 0; fi
    if grep -q "CANARY .* FAILED" "${dir}/run.log" 2>/dev/null; then
        note "  CANARY FAILED in ${dir} -- stopping the queue"; exit 1
    fi
    note "  FAIL (rc=${rc}, canary ok) ${dir} -- skipping to next"
}

OWN=(PARTITIONS=1024 PREFETCH=32 HOST_DIRECT=1 OWNER_BUILD=1)

HOST8=(PARTITIONS=8192 PREFETCH=32 HOST_DIRECT=1 OWNER_BUILD=1)
OWN1=(PARTITIONS=1024 PREFETCH=32 HOST_DIRECT=1 OWNER_BUILD=1)

# --- (a) sf_10 bars: Host@CXL varies between processes (T42), so 3 processes; NDP@CXL sf_10 owner runs exist
for i in 1 2 3; do step "sf10-p1024-$i" 10 50 MODES="2 3" "${OWN1[@]}"; done
step "sf10-p8192" 10 50 MODES="2 3" "${HOST8[@]}"          # is 8,192 also best at sf_10?

# --- (d) selectivity, Host@CXL + Hybrid at P = 8,192 (sel 50 exists) ---------------------------
for sel in 0 100; do step "sf100-s${sel}-p8192" 100 "$sel" MODES="2 3" "${HOST8[@]}"; done

# --- (c) share of threads at P = 8,192 (128 threads / 3,048 tasks exist) ------------------------
for t in 16 32 64; do step "sf100-host-t${t}-p8192" 100 50 MODES=2 "${HOST8[@]}" HOST_THREADS="$t"; done
for d in 512 1024 2048; do step "sf100-hyb-d${d}-p8192" 100 50 MODES=3 "${HOST8[@]}" DEVICE_TASKS="$d"; done

note "quick4 queue done"
