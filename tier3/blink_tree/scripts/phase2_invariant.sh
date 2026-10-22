#!/bin/bash
# ==============================================================================
# scripts/phase2_invariant.sh
# Phase 2: Invariant & Linearizability Validation (-DENABLE_VALIDATION)
# Usage: ./scripts/phase2_invariant.sh [1|2|3|4|host_dram|host_cxl|cxl_hybrid|cxl_full|all]
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
    all)          TARGET_MODES=("host_dram" "host_cxl" "cxl_hybrid" "cxl_full") ;;
    *)
        echo "❌ [ERROR] Invalid mode '$MODE_ARG'"
        exit 1
        ;;
esac

echo "=================================================="
echo " [Phase 2] Invariant & Linearizability Test Start"
echo " Target Modes: ${TARGET_MODES[*]}"
echo "=================================================="

for mode in "${TARGET_MODES[@]}"; do
    EXEC_FILE="./blink_${mode}_val"

    if [ ! -f "$EXEC_FILE" ]; then
        echo "❌ [ERROR] $EXEC_FILE binary not found!"
        exit 1
    fi

    echo ""
    echo "🔍 Mode [$mode] Verification (10 Iterations)..."
    for i in {1..10}; do
        echo -n "[Iteration $i/10] Verifying Invariants... "
        if $EXEC_FILE -t 2 -o 10000 -s 42 > /dev/null 2>&1; then
            echo "OK"
        else
            echo "FAILED"
            echo "❌ [FAIL] Invariant Violation detected in mode [$mode] at iteration $i!"
            exit 1
        fi
    done
done

echo "=================================================="
echo "✅ [SUCCESS] Phase 2 Passed: Invariants verified 100% across targeted modes."
echo "=================================================="