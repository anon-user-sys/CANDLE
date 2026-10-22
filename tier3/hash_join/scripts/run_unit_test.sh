#!/bin/bash
# run_unit_test.sh - Basic Functional & Debug Test Script (4 Modes)

set -eo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

source "${SCRIPT_DIR}/common_utils.sh"

cd "${PROJECT_ROOT}" || exit 1

# Arguments: $1 = scale factor (default 10), $2 = selectivity (default 100)
SF="${1:-10}"
SEL="${2:-100}"
CXL_MEM_NODE="${CXL_MEM_NODE:-2}"

NUM_TASKS="${NUM_TASKS:-32}"
BATCH_SIZE="${BATCH_SIZE:-2048}"
R_FILE="../dataset/sf_${SF}/orders.tbl"
S_FILE="../dataset/sf_${SF}/lineitem.tbl"

# Include the selectivity in OUT_DIR
OUT_DIR="./unit_test_results_sf${SF}_sel${SEL}"

if [ -f "$R_FILE" ] && [ -f "$S_FILE" ]; then
    # pass the selectivity (-f) in DATA_ARGS
    DATA_ARGS="-R $R_FILE -R-key 0 -S $S_FILE -S-key 0 -f ${SEL}"
else
    echo "[❌ ERROR] TPC-H tbl files ($R_FILE, $S_FILE) not found."
    exit 1
fi

rm -rf "${OUT_DIR}"
mkdir -p "${OUT_DIR}"

CHECKSUM_STATUS=$(detect_checksum_status "./hashjoin_host_dram" "${PROJECT_ROOT}")

echo "=========================================================="
echo " Starting Basic Functional Debug Test (Scale Factor: ${SF}, Selectivity: ${SEL}%)"
echo " Tasks: ${NUM_TASKS} | Batch Size: ${BATCH_SIZE}"
echo " CXL Memory NUMA Node ID: ${CXL_MEM_NODE}"
echo " Checksum Status: ${CHECKSUM_STATUS}"
echo "=========================================================="

JEMALLOC_LIB=$(find /usr/lib* /usr/local/lib* /opt* -name "libjemalloc.so*" 2>/dev/null | head -n 1)
PRELOAD_CMD=""
if [ -n "${JEMALLOC_LIB}" ]; then
    PRELOAD_CMD="env LD_PRELOAD=${JEMALLOC_LIB}"
fi

run_unit_mode() {
    local exec_bin=$1
    local mode_name=$2
    local mode_type=$3

    local prefix="${PRELOAD_CMD}"

    if [ ! -f "${exec_bin}" ]; then
        echo "[❌ ERROR] Executable ${exec_bin} not found!"
        exit 1
    fi

    if [ "$mode_type" != "host" ]; then
        check_cxl_required_or_exit "${CXL_MEM_NODE}" "${mode_name}"
    fi

    echo "Running ${exec_bin} (${mode_name}) with -i 2..."
    ${prefix} ${exec_bin} $NUM_TASKS -i 2 --batch $BATCH_SIZE $DATA_ARGS -o "${OUT_DIR}/${mode_name}.json" \
        || { echo "[❌ ERROR] Execution failed for ${mode_name}"; exit 1; }
}

echo "----------------------------------------------------------"
echo " [1/4] Testing Mode 1: Host DRAM"
echo "----------------------------------------------------------"
run_unit_mode "./hashjoin_host_dram" "host_dram" "host"

echo "----------------------------------------------------------"
echo " [2/4] Testing Mode 2: CXL Expander"
echo "----------------------------------------------------------"
run_unit_mode "./hashjoin_cxl_expander" "cxl_expander" "expander"

echo "----------------------------------------------------------"
echo " [3/4] Testing Mode 3: CXL Hybrid NMC"
echo "----------------------------------------------------------"
run_unit_mode "./hashjoin_cxl_hybrid" "cxl_hybrid" "nmc"

echo "----------------------------------------------------------"
echo " [4/4] Testing Mode 4: CXL Full NMC"
echo "----------------------------------------------------------"
run_unit_mode "./hashjoin_cxl_full_nmc" "cxl_full_nmc" "nmc"

echo ""
echo "=========================================================="
echo " 🔍 Comprehensive Multi-Line Unit Test Data Correctness Check..."
echo "=========================================================="
python3 -c "
import json, glob, os, sys, csv

out_dir = '${OUT_DIR}'
sf_val = '${SF}'
sel_val = '${SEL}'

files = glob.glob(os.path.join(out_dir, '*.json'))
if len(files) != 4:
    print(f'[❌ ERROR] Expected 4 output files, found {len(files)}')
    sys.exit(1)

results = {}
has_checksum = False
time_keys = ['total_ms', 'build_ms', 'probe_ms', 'time_ms', 'exec_time_ms', 'total_time_ms', 'elapsed_ms', 'latency_ms']

for fpath in sorted(files):
    fname = os.path.basename(fpath)
    file_tuples = set()
    file_checksums = set()
    file_times = []
    line_count = 0

    with open(fpath) as f:
        for line in f:
            line = line.strip('\r\n ').strip()
            if not line: continue
            try:
                data = json.loads(line)
                file_tuples.add(data.get('output_tuples'))
                if 'checksum' in data and data['checksum'] is not None:
                    chk_raw = data.get('checksum')
                    chk_str = f'0x{chk_raw & 0xFFFFFFFFFFFFFFFF:016x}' if isinstance(chk_raw, int) and not isinstance(chk_raw, bool) else str(chk_raw)
                    file_checksums.add(chk_str)
                    has_checksum = True
                for tk in time_keys:
                    if tk in data and data[tk] is not None:
                        file_times.append(float(data[tk]))
                        break
                line_count += 1
            except Exception as e:
                print(f'[❌ ERROR] Invalid JSON line in {fname}: {e}')
                sys.exit(1)

    if line_count == 0:
        print(f'[❌ ERROR] File {fname} is empty!')
        sys.exit(1)

    if len(file_tuples) > 1:
        print(f'[❌ ERROR] Intra-file tuple mismatch in {fname} across iterations!')
        sys.exit(1)

    if len(file_checksums) > 1:
        print(f'[❌ ERROR] Intra-file checksum mismatch in {fname} across iterations!')
        sys.exit(1)

    t_val = file_tuples.pop()
    c_val = file_checksums.pop() if file_checksums else 'N/A'
    valid_times = file_times[2:] if len(file_times) >= 5 else file_times
    avg_t = sum(valid_times) / len(valid_times) if valid_times else 'N/A'
    results[fname] = (t_val, c_val, avg_t)

chk_msg = 'ENABLED (Validation Mode)' if has_checksum else 'DISABLED (Pure Benchmark Mode)'
print(f'[INFO] Detected Checksum Mode: {chk_msg}\n')
print('[UNIT TEST MATCH RESULTS]')
for fname, (t, c, avg_t) in results.items():
    t_str = f'{avg_t:.2f} ms' if isinstance(avg_t, (int, float)) else str(avg_t)
    if has_checksum:
        print(f'  - {fname:25s}: Tuples={t:,} | Checksum={c} | Avg Time={t_str}')
    else:
        print(f'  - {fname:25s}: Tuples={t:,} | Avg Time={t_str}')

tuple_vals = [t for t, c, avg_t in results.values()]
checksum_vals = [c for t, c, avg_t in results.values()]

tuples_ok = (len(tuple_vals) == 4 and len(set(tuple_vals)) == 1)
checksums_ok = (not has_checksum) or (len(checksum_vals) == 4 and len(set(checksum_vals)) == 1)

summary_csv = os.path.join(out_dir, 'summary.csv')
with open(summary_csv, 'w', newline='') as f:
    writer = csv.writer(f)
    # add Scale_Factor and Selectivity columns to the CSV header
    writer.writerow(['File', 'Scale_Factor', 'Selectivity', 'Tuples', 'Checksum', 'Avg_Time_ms'])
    for fname, (t, c, avg_t) in results.items():
        writer.writerow([fname, sf_val, sel_val, t, c, f'{avg_t:.2f}' if isinstance(avg_t, (int, float)) else avg_t])
print(f'\n[INFO] Performance summary saved to {summary_csv}')

if tuples_ok and checksums_ok:
    print(f'[✅ PASSED] Unit Test Success! All 4 modes match in tuple count & checksum!')
else:
    print(f'\n[❌ ERROR] Unit Test Result Mismatch Detected across modes!')
    sys.exit(1)
"

echo "=========================================================="
echo " Basic Debug Test Completed Successfully!"
echo "=========================================================="