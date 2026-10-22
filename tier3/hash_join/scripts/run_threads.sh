#!/bin/bash
# run_threads.sh - device MU threads, not tasks (2026-10-18). The runtime runs 16 tasks per MU thread by
# default, so 3,048 tasks occupy ~191 of the 3,048 MU threads. This runs the device phases at 6,096 / 12,192 /
# 24,384 / 48,768 tasks = 381 / 762 / 1,524 / 3,048 MU threads (12.5-100% of the device), plus 3,048 tasks at one
# task per thread (--mu-batch 1) as a cross-check, for the hash join (sf_100, 50%) and the scan (sf_100, 50%).
# Was: run_quick5.sh - fig_hj_row (c): device task counts at the same shares of the platform as the host
# threads (12.5/25/50% of 3,048 = 381/762/1,524 tasks; host 16/32/64 of 128), each plan at its best P.
# Previous device points were 512/1,024/2,048 tasks (17/34/67%). Was: run_quick4.sh - fig_hj_row (c)/(d)
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

LOG="${PROJECT_ROOT}/threads_$(date +%Y%m%d_%H%M%S).log"
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

# --- hash join: Hybrid (P = 8,192) and NDP@CXL (P = 1,024) ------------------------------------------
for d in 6096 12192 24384 48768; do
    step "hj-hyb-d${d}" 100 50 MODES=3 "${HOST8[@]}" DEVICE_TASKS="$d"
    step "hj-ndp-d${d}" 100 50 MODES=4 "${OWN1[@]}" DEVICE_TASKS="$d"
done
step "hj-hyb-d3048-mb1" 100 50 MODES=3 "${HOST8[@]}" DEVICE_TASKS=3048 MU_BATCH=1
step "hj-ndp-d3048-mb1" 100 50 MODES=4 "${OWN1[@]}" DEVICE_TASKS=3048 MU_BATCH=1

# --- scan (tier2/scan, M4 device, memcpy upload as in the paper): 50%, prefetch 0 and 2 --
# 3,048 tasks repeated here as the same-session baseline. One process per point; CSV in its results/.
SCAN="${PROJECT_ROOT}/../../tier2/scan"
SCSV="${SCAN}/results/threads_sf100_$(date +%Y%m%d_%H%M%S).csv"
(cd "${SCAN}" && ./main --header > "${SCSV}")
for w in 0 2; do
    for p in 3048 6096 12192 24384 48768; do
        note "START scan-w${w}-p${p}"
        if (cd "${SCAN}" && timeout 1800 numactl --cpunodebind=0 --membind=0 ./main --mode device \
                --tbl "${DATA_ROOT:-$HOME/tpch}/sf_100/lineitem.tbl" --par "$p" --threshold 25 --iters 5 \
                --upload memcpy --materialize 0 --prefetch "$w" --label "threads_w${w}" >> "${SCSV}" 2>> "${SCSV%.csv}.err"); then
            note "  ok   scan p=${p} w=${w}"
        else
            note "  FAIL scan p=${p} w=${w} (see ${SCSV%.csv}.err)"
        fi
    done
done
chown "${OWNER}:" "${SCSV}" "${SCSV%.csv}.err" 2>/dev/null
note "  scan results: ${SCSV}"

(cd "${CANARY_DIR:-${PROJECT_ROOT}/../../tier1/data_movement}" && timeout 120 ./main d1 --verify-only 2>&1 | grep COPY_CHECK) | while read -r l; do note "closing canary: $l"; done
note "threads queue done"
