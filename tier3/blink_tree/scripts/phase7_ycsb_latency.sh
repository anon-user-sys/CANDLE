#!/bin/bash
# ==============================================================================
# scripts/phase7_ycsb_latency.sh
# Phase 7: YCSB Tail Latency Measurement (-DMEASURE_LATENCY)
# ==============================================================================

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
cd "${PROJECT_ROOT}"

MODE_ARG=${1:-"all"}
# workload_d removed
WORKLOADS=("workload_a" "workload_b" "workload_c" "workload_f")
THREADS=(1 2 4 8 16 32)
TOTAL_OPS="10M"
OUTPUT_CSV="ycsb_latency_results.csv"

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
echo " [Phase 7] YCSB Tail Latency Benchmarking"
echo " Target Modes  : ${TARGET_MODES[*]}"
echo "=================================================="

if [ ! -f "$OUTPUT_CSV" ]; then
    echo "Mode,Workload,Threads,Ops,Throughput_MOPS,Avg_Lat_us,P50_Lat_us,P99_Lat_us,P999_Lat_us" > "$OUTPUT_CSV"
fi

for mode in "${TARGET_MODES[@]}"; do
    BENCH_BIN="./blink_${mode}_lat"

    if [ ! -f "$BENCH_BIN" ]; then
        echo "❌ [ERROR] Latency binary '$BENCH_BIN' not found for mode '$mode'!"
        exit 1
    fi

    echo ""
    echo " ⏱️ Measuring Latency Metrics for Mode: $mode"

    for wl in "${WORKLOADS[@]}"; do
        echo "--------------------------------------------------"
        echo " Workload: $wl ($mode)"
        echo "--------------------------------------------------"
        
        for t in "${THREADS[@]}"; do
            echo -n "  -> Executing with $t threads... "
            RAW_RESULT=$($BENCH_BIN -t $t -o $TOTAL_OPS --workload=$wl --csv-out | tail -n 1)
            FINAL_LINE="${mode},${RAW_RESULT}"
            echo "$FINAL_LINE" >> "$OUTPUT_CSV"
            echo "Done [$FINAL_LINE]"
        done
    done
done

echo " 🎉 Phase 7 Latency Benchmarking Completed!"