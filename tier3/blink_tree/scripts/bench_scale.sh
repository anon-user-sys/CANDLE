#!/bin/bash
# Throughput by tree size x mode x batch size (throughput binaries, no validation)
#   KEYS_LIST  : prefill key counts (default "100k 10M": a tree that fits in the 96 MiB L3 / one that does not)
#   WORKLOADS  : default "a c"
#   SCRAMBLE_LIST : "1 0" = hot keys scattered / original order (default "1 0")
#   OPS_HOST / OPS_FULL / OPS_HYBRID : operations per mode (default 8M / 4M / 1M, so each run is long enough to measure)
#   REPS
# Result: results/bench_scale_<time>.csv
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
cd "${ROOT}"

KEYS_LIST="${KEYS_LIST:-100k 10M}"
WORKLOADS="${WORKLOADS:-a c}"
SCRAMBLE_LIST="${SCRAMBLE_LIST:-1 0}"
OPS_HOST="${OPS_HOST:-8M}"
OPS_FULL="${OPS_FULL:-4M}"
OPS_HYBRID="${OPS_HYBRID:-1M}"
REPS="${REPS:-1}"

mkdir -p results
OUT="results/bench_scale_$(date +%Y%m%d_%H%M%S).csv"
echo "mode,keys,scramble,workload,ops,threads,batch,dtasks,rep,throughput_mops,launches,avg_launch_us,device_lookups,host_locates,device_writes,host_split_inserts" > "${OUT}"

# mode | extra args
# Compares host_cxl / cxl_hybrid / cxl_full only (host_dram excluded, decided 2026-10-17)
CONFIGS=(
  "host_cxl|-t 16"
  "host_cxl|-t 64"
  "cxl_hybrid|-t 4 --batch=4096 --dtasks=64"
  "cxl_full|-t 4 --batch=4096 --dtasks=64"
  "cxl_full|-t 4 --batch=16384 --dtasks=256"
  "cxl_full|-t 4 --batch=65536 --dtasks=1024"
)

for keys in ${KEYS_LIST}; do
 for scr in ${SCRAMBLE_LIST}; do
  scr_flag=""; [ "${scr}" = "1" ] && scr_flag="--scramble"
  for wl in ${WORKLOADS}; do
    for cfg in "${CONFIGS[@]}"; do
      mode="${cfg%%|*}"; args="${cfg#*|}"
      case "${mode}" in host_*) ops="${OPS_HOST}" ;; cxl_full) ops="${OPS_FULL}" ;; *) ops="${OPS_HYBRID}" ;; esac
      for rep in $(seq 1 "${REPS}"); do
        log=$(timeout 1800 "./blink_${mode}_tp" -o "${ops}" --keys="${keys}" --workload="${wl}" ${scr_flag} ${args} 2>&1 | tr '\r' '\n') || true
        tp=$(echo "${log}" | grep "Throughput" | awk '{print $(NF-1)}')
        cfgline=$(echo "${log}" | grep "^\[Config\]")
        threads=$(echo "${cfgline}" | sed -n 's/.*threads=\([0-9]*\).*/\1/p')
        batch=$(echo "${cfgline}" | sed -n 's/.*batch=\([0-9]*\).*/\1/p')
        dtasks=$(echo "${cfgline}" | sed -n 's/.*dtasks=\([0-9]*\).*/\1/p')
        cxl=$(echo "${log}" | grep "^\[CXL\]" || true)
        get() { echo "${cxl}" | sed -n "s/.*$1=\([0-9.e+]*\).*/\1/p"; }
        echo "${mode},${keys},${scr},${wl},${ops},${threads},${batch},${dtasks},${rep},${tp:-ERR},$(get launches),$(get avg_launch_us),$(get device_lookups),$(get host_locates),$(get device_writes),$(get host_split_inserts)" | tee -a "${OUT}"
      done
    done
  done
 done
done
echo "results: ${OUT}"
