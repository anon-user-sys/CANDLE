#!/bin/bash
# run_devdax_modes.sh - Modes 2, 3, 4 at one configuration, with a canary before and after.
#
# Run as root (host access to memAlloc memory SIGBUSes otherwise), box in devdax mode:
#   sudo bash scripts/run_devdax_modes.sh [SF] [SEL]
# env: PARTITIONS (3048), HOST_THREADS (128), DEVICE_TASKS (3048), ITERS (5),
#      DATA_ROOT (TPC-H tables, $DATA_ROOT/sf_<SF>/), CANARY (tier1/data_movement d1 verify),
#      MODES ("2 3 4"), PREFETCH (0 = off; device probe batched prefetch window 1-32)
# sudo drops most env vars, so pass them through env:  sudo env MODES=4 PREFETCH=32 bash ...
#
# Stops at the first failure and captures device state (silent-degradation rule:
# capture immediately, the state clears on a shared box).

set -o pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
cd "${PROJECT_ROOT}" || exit 1   # Mode 3/4 load mu_kernel/*.mubin relative to here

SF="${1:-100}"
SEL="${2:-50}"
PARTITIONS="${PARTITIONS:-3048}"
HOST_THREADS="${HOST_THREADS:-128}"
DEVICE_TASKS="${DEVICE_TASKS:-3048}"
ITERS="${ITERS:-5}"
MODES="${MODES:-2 3 4}"
PREFETCH="${PREFETCH:-0}"
OWNER_BUILD="${OWNER_BUILD:-0}"   # 1 = owner-computes build: Mode 4 device (C19), Modes 2/3 host (C29)
OWN_ARGS=()
[ "${OWNER_BUILD}" = "1" ] && OWN_ARGS=(--owner-build)
PROBE_BATCHES="${PROBE_BATCHES:-0}"   # C20: probe in K batches (0 = one pass)
RESIDENT="${RESIDENT:-0}"             # C20: 1 = NDP@CXL resident probe kernel (Modes 3/4, needs PROBE_BATCHES)
[ "${PROBE_BATCHES}" != "0" ] && OWN_ARGS+=(--probe-batches "${PROBE_BATCHES}")
[ "${RESIDENT}" = "1" ] && OWN_ARGS+=(--resident)
MU_BATCH="${MU_BATCH:-0}"             # C21: tasks per MU thread (0 = runtime default 16)
[ "${MU_BATCH}" != "0" ] && OWN_ARGS+=(--mu-batch "${MU_BATCH}")
HOST_DIRECT="${HOST_DIRECT:-0}"       # C26: 1 = host probe without routing (Mode 2)
[ "${HOST_DIRECT}" = "1" ] && OWN_ARGS+=(--host-direct-probe)
BUILD_CXL="${BUILD_CXL:-0}"           # C30: 1 = Modes 2/3 host build reads orders from CXL
[ "${BUILD_CXL}" = "1" ] && OWN_ARGS+=(--build-input-cxl)
PIN="${PIN:-0}"                       # C28: 1 = pin host workers 1:1 to node0 cores (T42)
export HJ_PIN_THREADS="${PIN}"
DATA_ROOT="${DATA_ROOT:-$HOME/tpch}"
CANARY_DIR="${CANARY_DIR:-${PROJECT_ROOT}/../../tier1/data_movement}"
OWNER="${SUDO_USER:-$(id -un)}"

R_FILE="${DATA_ROOT}/sf_${SF}/orders.tbl"
S_FILE="${DATA_ROOT}/sf_${SF}/lineitem.tbl"
[ "$(id -u)" -eq 0 ] || { echo "run with sudo (host access to device memory needs root)"; exit 1; }
[ "$(basename "$(readlink -f /sys/bus/dax/devices/dax0.0/driver)")" = "device_dax" ] \
    || { echo "dax0.0 is not in devdax mode"; exit 1; }
[ -f "${R_FILE}" ] && [ -f "${S_FILE}" ] || { echo "data not found under ${DATA_ROOT}/sf_${SF}"; exit 1; }

OUT_DIR="./${OUT_ROOT:-results}/devdax_modes_sf${SF}_sel${SEL}_p${PARTITIONS}_w${PREFETCH}$([ "${OWNER_BUILD}" = "1" ] && echo _own)$([ "${PROBE_BATCHES}" != "0" ] && echo _k${PROBE_BATCHES})$([ "${RESIDENT}" = "1" ] && echo _res)$([ "${MU_BATCH}" != "0" ] && echo _mb${MU_BATCH})$([ "${HOST_DIRECT}" = "1" ] && echo _hd)$([ "${BUILD_CXL}" = "1" ] && echo _bc)$([ "${PIN}" = "1" ] && echo _pin)_$(date +%Y%m%d_%H%M%S)"
mkdir -p "${OUT_DIR}"
LOG="${OUT_DIR}/run.log"

log() { echo "$*" | tee -a "${LOG}"; }

capture_state() {
    log "!!! capturing device state at $(date +%T)"
    { xcena_cli num-device; xcena_cli device-info 0; fuser -v /dev/dax0.0; systemctl status pxl_resourced.service --no-pager | head -5; } \
        >> "${OUT_DIR}/failure_state.txt" 2>&1
}

finish() {
    chown -R "${OWNER}:" "${OUT_DIR}" 2>/dev/null
    log "=== end $(date +%T), results in ${OUT_DIR}"
}
trap finish EXIT

canary() {
    log "--- canary ($1) $(date +%T)"
    if ! (cd "${CANARY_DIR}" && timeout 120 ./main d1 --verify-only) > "${OUT_DIR}/canary_$1.txt" 2>&1 \
       || ! grep -q "failed=0" "${OUT_DIR}/canary_$1.txt"; then
        log "[❌ CANARY $1 FAILED] see ${OUT_DIR}/canary_$1.txt"
        capture_state
        exit 1
    fi
    log "    $(grep COPY_CHECK "${OUT_DIR}/canary_$1.txt")"
}

log "# sf=${SF} sel=${SEL} partitions=${PARTITIONS} host_threads=${HOST_THREADS} device_tasks=${DEVICE_TASKS} prefetch=${PREFETCH} owner_build=${OWNER_BUILD} batches=${PROBE_BATCHES} resident=${RESIDENT} mu_batch=${MU_BATCH} host_direct=${HOST_DIRECT} build_cxl=${BUILD_CXL} pin=${PIN} modes=[${MODES}] iters=${ITERS} load=$(cut -d' ' -f1 /proc/loadavg) start=$(date +%T)"

canary before

# Page sizes of the benchmark process, sampled once after its first validated iteration
# (read-only /proc access; needs root, which this script has). Answers T29 (devdax 2 MB pages).
sample_pages() {
    local name=$1 bin=$2
    timeout 7200 bash -c '
        log=$1; bin=$2; out=$3
        until grep -q "VALIDATION PASSED" "$log" 2>/dev/null; do sleep 2; done
        pid=$(pgrep -f "^\./$bin " | head -1); [ -n "$pid" ] || exit 0
        {
          echo "# $(date +%T) pid=$pid"
          awk "/^Size:/{sz=\$2} /^KernelPageSize:/{k[\$2]+=sz} /^AnonHugePages:/{a+=\$2} END{for(x in k) printf \"KernelPageSize %s kB: %.2f GB\\n\", x, k[x]/1048576; printf \"AnonHugePages: %.2f GB\\n\", a/1048576}" /proc/$pid/smaps
          grep -E "dax" /proc/$pid/maps | head -5
          echo "# dax0.0 resource: $(cat /sys/bus/dax/devices/dax0.0/resource 2>&1)"
        } > "$out" 2>&1
        # PAT memory types while the devdax range is mapped (T34)
        cat /sys/kernel/debug/x86/pat_memtype_list > "${out%.txt}_pat.txt" 2>&1
    ' _ "${OUT_DIR}/${name}.log" "${bin}" "${OUT_DIR}/${name}_pages.txt" &
}

run_mode() {
    local name=$1 bin=$2 count=$3
    log "--- ${name} (${bin} ${count}) start $(date +%T)"
    sample_pages "${name}" "${bin}"
    numactl --cpunodebind=0 --membind=0 "./${bin}" "${count}" -t "${HOST_THREADS}" -p "${PARTITIONS}" \
        --prefetch "${PREFETCH}" "${OWN_ARGS[@]}" -i "${ITERS}" --batch 2048 -R "${R_FILE}" -R-key 0 -S "${S_FILE}" -S-key 0 -f "${SEL}" \
        -o "${OUT_DIR}/${name}.json" 2>&1 | tee "${OUT_DIR}/${name}.log" \
        | grep -E --line-buffered "RESULT|CRITICAL|ERROR|FATAL|PXL|PIN" | tee -a "${LOG}"
    local rc=${PIPESTATUS[0]}
    local passed
    passed=$(grep -c "VALIDATION PASSED" "${OUT_DIR}/${name}.log")
    log "    ${name}: exit=${rc}, ${passed}/${ITERS} validated, end $(date +%T)"
    [ -s "${OUT_DIR}/${name}_pages.txt" ] && log "    pages: $(grep -E 'KernelPageSize' "${OUT_DIR}/${name}_pages.txt" | tr '\n' ' ')"
    if [ "${rc}" -ne 0 ] || [ "${passed}" -ne "${ITERS}" ]; then
        capture_state
        canary after_failure
        exit 1
    fi
}

for m in ${MODES}; do
    case "${m}" in
        2) run_mode mode2_host_at_cxl hashjoin_cxl_expander "${HOST_THREADS}" ;;
        3) run_mode mode3_hybrid      hashjoin_cxl_hybrid   "${DEVICE_TASKS}" ;;
        4) run_mode mode4_ndp_sync    hashjoin_cxl_full_nmc "${DEVICE_TASKS}" ;;
        *) log "unknown mode ${m}"; exit 1 ;;
    esac
done

canary after
