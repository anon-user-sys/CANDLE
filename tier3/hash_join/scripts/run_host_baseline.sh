#!/bin/bash
# run_host_baseline.sh - Host CPU DRAM Baseline Suite

set -eo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

source "${SCRIPT_DIR}/common_utils.sh"

cd "${PROJECT_ROOT}" || exit 1

SF="${1:-100}"
SEL="${2:-100}"
CXL_MEM_NODE="${CXL_MEM_NODE:-2}"

EXEC="./hashjoin_host_dram"
DATA_DIR="../dataset/sf_${SF}"
R_FILE="${DATA_DIR}/orders.tbl"
S_FILE="${DATA_DIR}/lineitem.tbl"
OUT_DIR="./host_dram_results_sf${SF}_sel${SEL}"

if [ -f "$R_FILE" ] && [ -f "$S_FILE" ]; then
    DATA_ARGS="-R $R_FILE -R-key 0 -S $S_FILE -S-key 0 -f ${SEL}"
else
    echo "[❌ ERROR] TPC-H tbl files ($R_FILE, $S_FILE) not found."
    exit 1
fi

rm -rf "${OUT_DIR}"
mkdir -p "${OUT_DIR}"

JEMALLOC_LIB=$(find /usr/lib* /usr/local/lib* /opt* -name "libjemalloc.so*" 2>/dev/null | head -n 1)
PRELOAD_CMD=""
if [ -n "${JEMALLOC_LIB}" ]; then
    PRELOAD_CMD="env LD_PRELOAD=${JEMALLOC_LIB}"
fi

HOST_DRAM_NODES=$(detect_host_dram_nodes "${CXL_MEM_NODE}")
CLOSEST_CPU_NODE=$(detect_closest_cpu_node "${CXL_MEM_NODE}")

get_host_numa_cmd() {
    local cores=$1
    get_numa_cmd_hpc "host" "$cores" "$CLOSEST_CPU_NODE" "$HOST_DRAM_NODES" "$CXL_MEM_NODE"
}

# 1. Scalability Test (-i 5)
CORES_LIST="1 2 4 8 16 32 64 128 256"
for c in ${CORES_LIST}; do
    NUMA_CMD=$(get_host_numa_cmd ${c})
    ${NUMA_CMD} ${PRELOAD_CMD} ${EXEC} ${c} \
        -i 5 --batch 2048 \
        ${DATA_ARGS} \
        -o "${OUT_DIR}/scalability_c${c}.json" \
        || { echo "[❌ ERROR] Execution failed at ${c} cores"; exit 1; }
done

# 2. Morsel Size Matrix Test (-i 5 throughout)
BATCH_LIST="64,128,512,2048,8192,32768,65536"
MATRIX_CORES="16 64 256"
for c in ${MATRIX_CORES}; do
    NUMA_CMD=$(get_host_numa_cmd ${c})
    ${NUMA_CMD} ${PRELOAD_CMD} ${EXEC} ${c} \
        -i 5 --batch ${BATCH_LIST} \
        ${DATA_ARGS} \
        -o "${OUT_DIR}/morsel_matrix_c${c}.json" \
        || { echo "[❌ ERROR] Morsel matrix failed at ${c} cores"; exit 1; }
done

# 3. Soak Test
NUMA_CMD=$(get_host_numa_cmd 256)
${NUMA_CMD} ${PRELOAD_CMD} ${EXEC} 256 \
    -i 50 --batch 2048 \
    ${DATA_ARGS} \
    -o "${OUT_DIR}/soak_stability.json" \
    || { echo "[❌ ERROR] Soak stability test failed"; exit 1; }

python3 -c "
import json, glob, os, sys, csv

out_dir = '${OUT_DIR}'
sf_val = '${SF}'
sel_val = '${SEL}'

files = glob.glob(os.path.join(out_dir, '*.json'))
results = {}
time_keys = ['total_ms', 'build_ms', 'probe_ms', 'time_ms', 'exec_time_ms', 'total_time_ms', 'elapsed_ms', 'latency_ms']

for fpath in sorted(files):
    fname = os.path.basename(fpath)
    file_tuples, file_checksums, file_times = set(), set(), []
    with open(fpath) as f:
        for line in f:
            line = line.strip()
            if not line: continue
            data = json.loads(line)
            file_tuples.add(data.get('output_tuples'))
            if 'checksum' in data and data['checksum'] is not None:
                chk_raw = data.get('checksum')
                file_checksums.add(f'0x{chk_raw & 0xFFFFFFFFFFFFFFFF:016x}' if isinstance(chk_raw, int) and not isinstance(chk_raw, bool) else str(chk_raw))
            for tk in time_keys:
                if tk in data and data[tk] is not None:
                    file_times.append(float(data[tk]))
                    break
    t_val = file_tuples.pop()
    c_val = file_checksums.pop() if file_checksums else 'N/A'
    valid_times = file_times[2:] if len(file_times) >= 5 else file_times
    avg_t = sum(valid_times) / len(valid_times) if valid_times else 'N/A'
    results[fname] = (t_val, c_val, avg_t)

summary_csv = os.path.join(out_dir, 'summary.csv')
with open(summary_csv, 'w', newline='') as f:
    writer = csv.writer(f)
    writer.writerow(['File', 'Scale_Factor', 'Selectivity', 'Tuples', 'Checksum', 'Avg_Time_ms'])
    for fname, (t, c, avg_t) in results.items():
        writer.writerow([fname, sf_val, sel_val, t, c, f'{avg_t:.2f}' if isinstance(avg_t, (int, float)) else avg_t])
"