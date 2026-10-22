#!/bin/bash
# run_cxl_emulator.sh - QEMU/Emulator Functional Verification Suite

set -eo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

source "${SCRIPT_DIR}/common_utils.sh"

cd "${PROJECT_ROOT}" || exit 1

SF="${1:-0.1}"
SEL="${2:-100}"

EXEC_EXPANDER="./hashjoin_cxl_expander"
EXEC_HYBRID="./hashjoin_cxl_hybrid"
EXEC_FULL_NMC="./hashjoin_cxl_full_nmc"

DATA_DIR="../dataset/sf_${SF}"
R_FILE="${DATA_DIR}/orders.tbl"
S_FILE="${DATA_DIR}/lineitem.tbl"

if [ -f "$R_FILE" ] && [ -f "$S_FILE" ]; then
    DATA_ARGS="-R $R_FILE -R-key 0 -S $S_FILE -S-key 0 -f ${SEL}"
else
    echo "[❌ ERROR] TPC-H tbl files ($R_FILE, $S_FILE) not found."
    exit 1
fi

OUT_EXPANDER="./cxl_emu_expander_results_sf${SF}_sel${SEL}"
OUT_HYBRID="./cxl_emu_hybrid_results_sf${SF}_sel${SEL}"
OUT_FULL_NMC="./cxl_emu_full_nmc_results_sf${SF}_sel${SEL}"

rm -rf "${OUT_EXPANDER}" "${OUT_HYBRID}" "${OUT_FULL_NMC}"
mkdir -p "${OUT_EXPANDER}" "${OUT_HYBRID}" "${OUT_FULL_NMC}"

JEMALLOC_LIB=$(find /usr/lib* /usr/local/lib* /opt* -name "libjemalloc.so*" 2>/dev/null | head -n 1)
PRELOAD_CMD=""
if [ -n "${JEMALLOC_LIB}" ]; then
    PRELOAD_CMD="env LD_PRELOAD=${JEMALLOC_LIB}"
fi

run_emu_suite() {
    local exec_bin=$1
    local out_dir=$2
    local mode_label=$3

    echo "1. Core Scalability Test (-i 1 throughout)"
    CORES_LIST="1 2 4 8 16 32 64 128 256"
    for c in ${CORES_LIST}; do
        ${PRELOAD_CMD} ${exec_bin} ${c} \
            -i 1 --batch 2048 \
            ${DATA_ARGS} \
            -o "${out_dir}/scalability_c${c}.json" \
            || { echo "[❌ ERROR] Emulator execution failed at core ${c} for ${mode_label}"; exit 1; }
    done

    echo "2. Morsel Size Matrix Test (-i 1 throughout)"
    BATCH_LIST="64,128,512,2048,8192,32768,65536"
    MATRIX_CORES="2 8"
    for c in ${MATRIX_CORES}; do
        ${PRELOAD_CMD} ${exec_bin} ${c} \
            -i 1 --batch ${BATCH_LIST} \
            ${DATA_ARGS} \
            -o "${out_dir}/morsel_matrix_c${c}.json" \
            || { echo "[❌ ERROR] Emulator morsel sweep failed at core ${c} for ${mode_label}"; exit 1; }
    done

    echo "3. Long-Run Stability Test"
    ${PRELOAD_CMD} ${exec_bin} 8 \
        -i 1 --batch 2048 \
        ${DATA_ARGS} \
        -o "${out_dir}/soak_stability.json" \
        || { echo "[❌ ERROR] Emulator long-run stability test failed for ${mode_label}"; exit 1; }
}

run_emu_suite "${EXEC_EXPANDER}" "${OUT_EXPANDER}" "CXL-Expander"
run_emu_suite "${EXEC_HYBRID}"   "${OUT_HYBRID}"   "CXL-Hybrid-NMC"
run_emu_suite "${EXEC_FULL_NMC}" "${OUT_FULL_NMC}" "CXL-Full-NMC"

python3 -c "
import json, os, sys, glob, csv

dirs = {'Expander': '${OUT_EXPANDER}', 'Hybrid': '${OUT_HYBRID}', 'Full_NMC': '${OUT_FULL_NMC}'}
sf_val = '${SF}'
sel_val = '${SEL}'

all_file_results = []
time_keys = ['total_ms', 'build_ms', 'probe_ms', 'time_ms', 'exec_time_ms', 'total_time_ms', 'elapsed_ms', 'latency_ms']

for mode, d in dirs.items():
    files = glob.glob(os.path.join(d, '*.json'))
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
        all_file_results.append((mode, fname, t_val, c_val, avg_t))

summary_csv = 'cxl_emu_benchmark_summary.csv'
with open(summary_csv, 'w', newline='') as f:
    writer = csv.writer(f)
    writer.writerow(['Mode', 'File', 'Scale_Factor', 'Selectivity', 'Tuples', 'Checksum', 'Avg_Time_ms'])
    for mode, fname, t, c, avg_t in all_file_results:
        writer.writerow([mode, fname, sf_val, sel_val, t, c, f'{avg_t:.2f}' if isinstance(avg_t, (int, float)) else avg_t])
"