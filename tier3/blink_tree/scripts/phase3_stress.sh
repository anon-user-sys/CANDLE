#!/bin/bash
# ==============================================================================
# scripts/phase3_stress.sh
# Phase 3: Hotspot & Latch Contention Stress Test (Read/Update Only)
# ==============================================================================

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
cd "${PROJECT_ROOT}"

MODE_ARG=${1:-"all"}

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
echo " [Phase 3] Read/Update Contention Stress Test"
echo " Target Modes: ${TARGET_MODES[*]}"
echo "=================================================="

for mode in "${TARGET_MODES[@]}"; do
    EXEC_FILE="./blink_${mode}_tp"

    if [ ! -f "$EXEC_FILE" ]; then
        echo "❌ [ERROR] $EXEC_FILE binary not found!"
        exit 1
    fi

    echo ""
    echo "⚡ Stress Testing Mode: $mode"
    
    echo " [1/2] Testing Workload A (50% Read / 50% Update Contention)..."
    $EXEC_FILE -t 16 -o 1M --workload=workload_a > /dev/null 2>&1 || { echo "❌ Workload A Stress Failed"; exit 1; }

    echo " [2/2] Testing Extreme Zipfian Hotspot (Skew 0.99 Update/Read)..."
    $EXEC_FILE -t 16 -o 1M --workload=workload_a --skew=0.99 > /dev/null 2>&1 || { echo "❌ Zipfian Contention Failed"; exit 1; }

    echo "  -> Mode [$mode] Passed Stress Conditions."
done

echo ""
echo "✅ [SUCCESS] Phase 3 Passed: Read/Update contention verified under high skew."