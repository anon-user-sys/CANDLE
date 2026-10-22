#!/bin/bash
# ==============================================================================
# scripts/run_all_phases.sh
# Top-level Orchestration Script (Supports Modes 1-4 & All Workloads A, B, C, F)
# Usage: ./scripts/run_all_phases.sh [1|2|3|4|host_dram|host_cxl|cxl_hybrid|cxl_full|cxl|host|all]
# ==============================================================================

set -euo pipefail

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
        echo "Usage: $0 [1|2|3|4|host_dram|host_cxl|cxl_hybrid|cxl_full|cxl|host|all]"
        exit 1
        ;;
esac

REQUIRED_SCRIPTS=(
    "phase2_invariant.sh"
    "phase3_stress.sh"
    "phase4_soak_test.sh"
    "phase5_ycsb_sanity.sh"
    "phase6_ycsb_performance.sh"
    "phase7_ycsb_latency.sh"
)

echo "=================================================="
echo " 🚀 B-Link Tree Automated Full Pipeline Start"
echo " Target Modes : ${TARGET_MODES[*]}"
echo " Workloads    : Workload A, B, C, F (Workload D Excluded)"
echo " Project Root : ${PROJECT_ROOT}"
echo "=================================================="

# 1. check that the required scripts exist and are executable
for script in "${REQUIRED_SCRIPTS[@]}"; do
    SCRIPT_PATH="${SCRIPT_DIR}/${script}"
    if [ ! -f "$SCRIPT_PATH" ]; then
        echo "❌ [ERROR] Script '$script' not found in ${SCRIPT_DIR}"
        exit 1
    fi
    chmod +x "$SCRIPT_PATH"
done

# 2. check the binary variants (_val, _tp, _lat)
for mode in "${TARGET_MODES[@]}"; do
    for variant in val tp lat; do
        if [ ! -f "./blink_${mode}_${variant}" ]; then
            echo "❌ [ERROR] Binary './blink_${mode}_${variant}' not found in ${PROJECT_ROOT}!"
            echo "     Please run build first."
            exit 1
        fi
    done
done

START_TIME=$(date +%s)

# Step 1: Invariants Correctness
echo ""
echo "=================================================="
echo " [Step 1/6] Executing Phase 2: Invariants & Correctness"
echo "=================================================="
for mode in "${TARGET_MODES[@]}"; do
    "${SCRIPT_DIR}/phase2_invariant.sh" "$mode"
done

# Step 2: Read/Update Stress
echo ""
echo "=================================================="
echo " [Step 2/6] Executing Phase 3: Hotspot & Contention Stress Test"
echo "=================================================="
for mode in "${TARGET_MODES[@]}"; do
    "${SCRIPT_DIR}/phase3_stress.sh" "$mode"
done

# Step 3: YCSB Sanity Check (Golden Reference)
echo ""
echo "=================================================="
echo " [Step 3/6] Executing Phase 5: YCSB Golden Reference Sanity (-DENABLE_VALIDATION)"
echo "=================================================="
for mode in "${TARGET_MODES[@]}"; do
    "${SCRIPT_DIR}/phase5_ycsb_sanity.sh" "$mode"
done

# Step 4: Pure Throughput Performance
echo ""
echo "=================================================="
echo " [Step 4/6] Executing Phase 6: YCSB Pure Performance (Throughput)"
echo "=================================================="
for mode in "${TARGET_MODES[@]}"; do
    "${SCRIPT_DIR}/phase6_ycsb_performance.sh" "$mode"
done

# Step 5: Tail Latency
echo ""
echo "=================================================="
echo " [Step 5/6] Executing Phase 7: YCSB Tail Latency (-DMEASURE_LATENCY)"
echo "=================================================="
for mode in "${TARGET_MODES[@]}"; do
    "${SCRIPT_DIR}/phase7_ycsb_latency.sh" "$mode"
done

# Step 6: soak test (the heaviest, longest test runs last)
echo ""
echo "=================================================="
echo " [Step 6/6] Executing Phase 4: Soak Test & Resource Audit"
echo "=================================================="
for mode in "${TARGET_MODES[@]}"; do
    echo " -> Running Soak Test for mode: $mode (50M ops)"
    "${SCRIPT_DIR}/phase4_soak_test.sh" "$mode" "50M"
done

END_TIME=$(date +%s)
TOTAL_ELAPSED=$((END_TIME - START_TIME))

echo ""
echo "=================================================="
echo " 🎉 ALL TEST PHASES COMPLETED SUCCESSFULLY!"
echo " Total Elapsed Time: ${TOTAL_ELAPSED} seconds"
echo " Metric Results:"
echo "   - Throughput Metrics : ycsb_performance_results.csv"
echo "   - Tail Latency Metrics: ycsb_latency_results.csv"
echo "=================================================="