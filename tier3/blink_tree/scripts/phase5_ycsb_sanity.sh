#!/bin/bash
# ==============================================================================
# scripts/phase5_ycsb_sanity.sh
# Phase 5: YCSB Golden Reference Data Integrity Validation (-DENABLE_VALIDATION)
# ==============================================================================

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
cd "${PROJECT_ROOT}"

MODE_ARG=${1:-"all"}
# workload_d removed (read/update workloads only)
WORKLOADS=("workload_a" "workload_b" "workload_c" "workload_f")
TEST_OPS="100K"
THREADS=8

case "$MODE_ARG" in
    1|host_dram)  TARGET_MODES=("host_dram") ;;
    2|host_cxl)   TARGET_MODES=("host_cxl") ;;
    3|cxl_hybrid) TARGET_MODES=("cxl_hybrid") ;;
    4|cxl_full)   TARGET_MODES=("cxl_full") ;;
    cxl|34)       TARGET_MODES=("cxl_hybrid" "cxl_full") ;;
    host|12)      TARGET_MODES=("host_dram" "host_cxl") ;;
    all)          TARGET_MODES=("host_dram" "host_cxl" "cxl_hybrid" "cxl_full") ;;
    *)
        echo "❌ [ERROR] Invalid mode '$MODE_ARG'"
        exit 1
        ;;
esac

echo "=================================================="
echo " [Phase 5] YCSB Golden Reference Correctness Check"
echo " Target Modes  : ${TARGET_MODES[*]}"
echo " Workloads     : ${WORKLOADS[*]}"
echo " Test Scale    : $TEST_OPS ops per workload"
echo "=================================================="

for mode in "${TARGET_MODES[@]}"; do
    VAL_BIN="./blink_${mode}_val"

    if [ ! -f "$VAL_BIN" ]; then
        echo "❌ [ERROR] Validation binary '$VAL_BIN' not found for mode '$mode'!"
        exit 1
    fi

    echo ""
    echo " 🚀 Testing Architecture Mode: $mode"

    for wl in "${WORKLOADS[@]}"; do
        echo -n "  -> [$mode] Validating Workload: $wl ... "
        
        if $VAL_BIN -t $THREADS -o $TEST_OPS --workload=$wl > /dev/null 2>&1; then
            echo "✅ PASSED"
        else
            echo "❌ FAILED"
            echo "❌ Data Mismatch detected in mode [$mode] workload [$wl]"
            exit 1
        fi
    done
done

echo ""
echo " 🎉 Phase 5 Passed! All Target Modes & Workloads are 100% Data-Correct."