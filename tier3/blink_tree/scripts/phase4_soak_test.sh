#!/bin/bash
# ==============================================================================
# scripts/phase4_soak_test.sh
# Phase 4: Automated Op-Bound & Memory Leak Monitoring (Verification Pipeline)
# Usage: ./scripts/phase4_soak_test.sh [mode] [ops_count] [workload]
# Example: ./scripts/phase4_soak_test.sh cxl_hybrid 50M workload_a
# ==============================================================================

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
cd "${PROJECT_ROOT}"

MODE_ARG=${1:-"host_dram"}
OPS_COUNT=${2:-"50M"}
WORKLOAD=${3:-"workload_a"} # Default: read/update 50:50 (no node splits)
THREADS=16

case "$MODE_ARG" in
    1|host_dram)  TARGET_MODE="host_dram" ;;
    2|host_cxl)   TARGET_MODE="host_cxl" ;;
    3|cxl_hybrid) TARGET_MODE="cxl_hybrid" ;;
    4|cxl_full)   TARGET_MODE="cxl_full" ;;
    *)
        echo "❌ [ERROR] Invalid mode '$MODE_ARG'."
        exit 1
        ;;
esac

BENCH_BIN="./blink_${TARGET_MODE}_tp"
LOG_FILE="soak_test_${TARGET_MODE}.log"
ZERO_CPU_LIMIT=3
MEM_LEAK_THRESHOLD_PCT=30  # allowed memory growth after warm-up (%)

if [ ! -f "$BENCH_BIN" ]; then
    echo "❌ [ERROR] Binary '$BENCH_BIN' not found!"
    exit 1
fi

echo "=================================================="
echo " [Phase 4 Verification] Automated Soak Test Start"
echo " Target Mode   : ${TARGET_MODE}"
echo " Operations    : ${OPS_COUNT}"
echo " Workload      : ${WORKLOAD}"
echo " Log File      : ${LOG_FILE}"
echo "=================================================="

# Pass workload A (no node splits) or the given workload explicitly
$BENCH_BIN -t $THREADS -o $OPS_COUNT --workload=$WORKLOAD > /dev/null 2>&1 &
BENCH_PID=$!

START_TIME=$(date +%s)
echo "Timestamp, CPU_Usage(%), RSS_Memory(KB)" > $LOG_FILE

ZERO_CPU_COUNT=0
RSS_HISTORY=()

while kill -0 $BENCH_PID 2>/dev/null; do
    STATS=$(ps -p $BENCH_PID -o %cpu,rss --no-headers 2>/dev/null)
    if [ -z "$STATS" ]; then break; fi

    CPU=$(echo $STATS | awk '{print $1}' | cut -d. -f1)
    RSS=$(echo $STATS | awk '{print $2}')
    NOW=$(date +%s)
    ELAPSED=$((NOW - START_TIME))

    echo "[$(date '+%Y-%m-%d %H:%M:%S')] (Elapsed: ${ELAPSED}s) CPU: ${CPU}%, RSS: ${RSS} KB" | tee -a $LOG_FILE

    # deadlock watch: stop if CPU use stays near 0%
    if [ -n "$CPU" ] && [ "$CPU" -le 1 ]; then
        ZERO_CPU_COUNT=$((ZERO_CPU_COUNT + 1))
        if [ $ZERO_CPU_COUNT -ge $ZERO_CPU_LIMIT ]; then
            echo "❌ [FAIL] Deadlock Detected! CPU usage dropped to ~0% for ${ZERO_CPU_LIMIT} consecutive checks."
            kill -9 $BENCH_PID 2>/dev/null
            exit 1
        fi
    else
        ZERO_CPU_COUNT=0
    fi

    if [ -n "$RSS" ]; then
        RSS_HISTORY+=("$RSS")
    fi

    sleep 30
done

wait $BENCH_PID
EXIT_CODE=$?

if [ $EXIT_CODE -ne 0 ]; then
    echo "❌ [FAIL] Process crashed unexpectedly! (Exit Code: $EXIT_CODE)"
    exit $EXIT_CODE
fi

# leak watch: does RSS rise above the threshold after the midpoint?
TOTAL_SAMPLES=${#RSS_HISTORY[@]}

if [ $TOTAL_SAMPLES -gt 2 ]; then
    MID_INDEX=$((TOTAL_SAMPLES / 2))
    INITIAL_RSS=${RSS_HISTORY[$MID_INDEX]}
    FINAL_RSS=${RSS_HISTORY[$((TOTAL_SAMPLES - 1))]}

    if [ $INITIAL_RSS -gt 0 ] && [ $FINAL_RSS -gt 0 ]; then
        MEM_DIFF=$((FINAL_RSS - INITIAL_RSS))
        INCREASE_PCT=$(( (MEM_DIFF > 0 ? MEM_DIFF : 0) * 100 / INITIAL_RSS ))

        echo "📊 Baseline RSS: ${INITIAL_RSS} KB, Final RSS: ${FINAL_RSS} KB (Growth: ${INCREASE_PCT}%)"

        if [ $INCREASE_PCT -gt $MEM_LEAK_THRESHOLD_PCT ]; then
            echo "❌ [FAIL] Memory Leak Detected! Post-warmup RSS increased by ${INCREASE_PCT}%"
            exit 1
        fi
    fi
fi

ELAPSED_TOTAL=$(($(date +%s) - START_TIME))
echo "✅ [SUCCESS] Phase 4 Passed for mode [${TARGET_MODE}]: Completed in ${ELAPSED_TOTAL}s."