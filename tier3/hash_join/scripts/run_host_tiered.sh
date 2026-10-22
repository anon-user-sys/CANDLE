#!/bin/bash
# run_host_tiered.sh - Mode 5: Host CPU + DRAM/CXL tiered placement sweep
#
# The MX1 must be in system-ram mode (CXL memory visible as NUMA node ${CXL_MEM_NODE}).
# This is exclusive with devdax, so Modes 2-4 cannot run in the same session.
#
# Usage: scripts/run_host_tiered.sh [SF] [SEL]
#   env: CXL_MEM_NODE (2), DRAM_NODE (0), CXL_PCTS ("0 25 50 75 100"), PLACEMENT (interleave|block),
#        CORES ("16 64 128"), ITERS (5), BATCH (2048), DATA_ROOT (../dataset),
#        PARTITIONS (unset = same as cores)

set -eo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
cd "${PROJECT_ROOT}" || exit 1

SF="${1:-100}"
SEL="${2:-100}"
CXL_MEM_NODE="${CXL_MEM_NODE:-2}"
DRAM_NODE="${DRAM_NODE:-0}"
CXL_PCTS="${CXL_PCTS:-0 25 50 75 100}"
PLACEMENT="${PLACEMENT:-interleave}"
CORES="${CORES:-16 64 128}"
ITERS="${ITERS:-5}"
BATCH="${BATCH:-2048}"
DATA_ROOT="${DATA_ROOT:-../dataset}"
PARTITIONS="${PARTITIONS:-}"
P_ARGS=()
P_TAG=""
if [ -n "${PARTITIONS}" ]; then P_ARGS=(-p "${PARTITIONS}"); P_TAG="_p${PARTITIONS}"; fi

EXEC="./hashjoin_host_tiered"
R_FILE="${DATA_ROOT}/sf_${SF}/orders.tbl"
S_FILE="${DATA_ROOT}/sf_${SF}/lineitem.tbl"

[ -x "${EXEC}" ] || { echo "[❌ ERROR] ${EXEC} not built (needs libnuma)"; exit 1; }
[ -f "${R_FILE}" ] && [ -f "${S_FILE}" ] || { echo "[❌ ERROR] ${R_FILE} / ${S_FILE} not found"; exit 1; }

size_mb=$(numactl --hardware 2>/dev/null | awk -v n="${CXL_MEM_NODE}" '$1=="node" && $2==n && $3=="size:" {print $4}')
if [ -z "${size_mb}" ] || [ "${size_mb}" -le 0 ]; then
    echo "[❌ ERROR] NUMA node ${CXL_MEM_NODE} has no memory - switch the MX1 to system-ram mode first."
    exit 1
fi

OUT_DIR="./host_tiered_results_sf${SF}_sel${SEL}_${PLACEMENT}${P_TAG}"
rm -rf "${OUT_DIR}"
mkdir -p "${OUT_DIR}"

echo "# sf=${SF} sel=${SEL} cxl_node=${CXL_MEM_NODE} (${size_mb} MB) dram_node=${DRAM_NODE} placement=${PLACEMENT} partitions=${PARTITIONS:-cores} cores=[${CORES}] pcts=[${CXL_PCTS}] load=$(cut -d' ' -f1 /proc/loadavg)"

for c in ${CORES}; do
    for pct in ${CXL_PCTS}; do
        echo "--- cores=${c} cxl_pct=${pct}"
        # Other allocations (ground truth, result sets, morsel buffers) stay on DRAM_NODE;
        # the placed buffers are bound per 2 MB chunk with mbind, which overrides this policy.
        numactl --cpunodebind="${DRAM_NODE}" --membind="${DRAM_NODE}" \
            "${EXEC}" "${c}" -t "${c}" "${P_ARGS[@]}" -i "${ITERS}" --batch "${BATCH}" \
            -R "${R_FILE}" -R-key 0 -S "${S_FILE}" -S-key 0 -f "${SEL}" \
            --cxl-pct "${pct}" --numa-node "${CXL_MEM_NODE}" --dram-node "${DRAM_NODE}" --placement "${PLACEMENT}" \
            -o "${OUT_DIR}/tiered_c${c}_cxl${pct}.json" \
            2>&1 | tee "${OUT_DIR}/tiered_c${c}_cxl${pct}.log" \
            || { echo "[❌ ERROR] failed at cores=${c} cxl_pct=${pct}"; exit 1; }
    done
done

echo "Results in ${OUT_DIR}"
