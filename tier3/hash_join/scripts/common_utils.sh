#!/bin/bash
# common_utils.sh - XCENA CXL Benchmark Common Functions (Hardened & Optimized)

# 1. Detect QEMU / virtualized environments
is_qemu_environment() {
    if command -v systemd-detect-virt &> /dev/null; then
        local virt_type
        virt_type=$(systemd-detect-virt 2>/dev/null || true)
        if [[ "${virt_type}" =~ ^(qemu|kvm|bochs)$ ]]; then
            return 0
        fi
    fi

    if [ -f /sys/class/dmi/id/sys_vendor ]; then
        local vendor
        vendor=$(cat /sys/class/dmi/id/sys_vendor 2>/dev/null | tr '[:upper:]' '[:lower:]')
        if [[ "${vendor}" =~ (qemu|bochs|kvm) ]]; then
            return 0
        fi
    fi

    if [ -f /sys/class/dmi/id/product_name ]; then
        local prod
        prod=$(cat /sys/class/dmi/id/product_name 2>/dev/null | tr '[:upper:]' '[:lower:]')
        if [[ "${prod}" =~ (qemu|bochs|kvm) ]]; then
            return 0
        fi
    fi

    if grep -q -i "qemu" /proc/cpuinfo 2>/dev/null; then
        return 0
    fi

    return 1
}

# 2. Detect the CXL NUMA node
is_cxl_available() {
    local cxl_node=${1:-2}
    if command -v numactl &> /dev/null; then
        local size_mb
        size_mb=$(numactl --hardware 2>/dev/null | grep -E "^node ${cxl_node} size:" | awk '{print $4}' | tr -d '\r')
        if [ -n "${size_mb}" ] && [ "${size_mb:-0}" -gt 0 ]; then
            return 0
        fi
    fi
    return 1
}

# 3. CXL guard for the physical server
check_cxl_required_or_exit() {
    local cxl_node=$1
    local mode_name=$2

    if is_qemu_environment; then
        echo "[INFO] QEMU Environment Detected: Bypassing physical CXL check for ${mode_name}."
        return 0
    fi

    # Modes 2-4 use PXL memAlloc (devdax), so a missing NUMA node is expected in devdax mode
    if compgen -G "/dev/dax*" > /dev/null; then
        return 0
    fi

    if ! is_cxl_available "${cxl_node}"; then
        echo "----------------------------------------------------------"
        echo "[❌ ERROR] Physical Server Environment Detected!"
        echo "CXL Hardware (devdax or NUMA Node ${cxl_node}) is missing or has 0 MB."
        echo "Aborting ${mode_name} execution to prevent invalid benchmark data."
        echo "----------------------------------------------------------"
        exit 1
    fi
}

detect_checksum_status() {
    local exec_bin=$1
    local project_root=${2:-"$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"}
    
    local cmake_cache=""
    for cache_path in "${project_root}/build/CMakeCache.txt" "${project_root}/CMakeCache.txt"; do
        if [ -f "${cache_path}" ]; then
            cmake_cache="${cache_path}"
            break
        fi
    done

    if [ -n "${cmake_cache}" ]; then
        local chk_val
        chk_val=$(grep -i "ENABLE_CHECKSUM" "${cmake_cache}" 2>/dev/null | head -n 1 | cut -d'=' -f2 | tr -d '\r' | tr '[:upper:]' '[:lower:]')
        if [[ "${chk_val}" =~ ^(on|1|true|yes)$ ]]; then
            echo "ENABLED (Validation Mode - Bit-Exact Checksum Active)"
            return
        elif [[ "${chk_val}" =~ ^(off|0|false|no)$ ]]; then
            echo "DISABLED (Pure Benchmark Mode)"
            return
        fi
    fi

    if [ -f "${exec_bin}" ]; then
        if strings "${exec_bin}" 2>/dev/null | grep -q "ENABLE_CHECKSUM_ON_BUILD_FLAG"; then
            echo "ENABLED (Validation Mode - Bit-Exact Checksum Active)"
        else
            echo "DISABLED/UNKNOWN (CMakeCache missing, defaulting to Pure Mode)"
        fi
    else
        echo "UNKNOWN (Executable not found)"
    fi
}

detect_host_dram_nodes() {
    local cxl_node=${1:-2}
    if command -v numactl &> /dev/null; then
        local node_list
        node_list=$(numactl --hardware 2>/dev/null | grep -E "^node [0-9]+ size:" | awk '{print $2}' | tr -d '\r')
        if [ -z "$node_list" ]; then
            echo "0"
            return
        fi
        local dram_nodes=()
        for node in $node_list; do
            local size_mb
            size_mb=$(numactl --hardware 2>/dev/null | grep -E "^node $node size:" | awk '{print $4}' | tr -d '\r')
            if [ "$node" -ne "$cxl_node" ] && [ "${size_mb:-0}" -gt 0 ]; then
                dram_nodes+=("$node")
            fi
        done
        if [ ${#dram_nodes[@]} -eq 0 ]; then
            echo "0"
        else
            echo "$(IFS=,; echo "${dram_nodes[*]}")"
        fi
    else
        echo "0"
    fi
}

detect_closest_cpu_node() {
    local cxl_node=${1:-2}
    if command -v numactl &> /dev/null; then
        local closest
        closest=$(numactl --hardware 2>/dev/null | awk -v cxl="${cxl_node}" '
            $1 == cxl ":" {
                min=999; target=0;
                for(i=2; i<=NF; i++) {
                    node_id = i - 2;
                    if (node_id != cxl && $i > 0 && $i < min) {
                        min = $i; target = node_id;
                    }
                }
                print target;
            }')
        if [ -n "${closest}" ]; then
            echo "${closest}"
        else
            echo "0"
        fi
    else
        echo "0"
    fi
}

# With more than 128 cores, bind cpunodebind to the detected dram_nodes instead of a hard-coded 0,1
get_numa_cmd_hpc() {
    local mode=$1
    local cores=$2
    local closest_cpu_node=${3:-0}
    local dram_nodes=${4:-"0"}
    local cxl_mem_node=${5:-2}

    if ! command -v numactl &> /dev/null; then
        echo ""
        return
    fi

    case "$mode" in
        "host")
            if [ "$cores" -le 128 ]; then
                echo "numactl --cpunodebind=${closest_cpu_node} --membind=${closest_cpu_node}"
            else
                echo "numactl --cpunodebind=${dram_nodes} --interleave=${dram_nodes}"
            fi
            ;;
        "nmc")
            echo "numactl --cpunodebind=${closest_cpu_node} --membind=${closest_cpu_node}"
            ;;
        "expander")
            if is_cxl_available "${cxl_mem_node}"; then
                if [ "$cores" -le 128 ]; then
                    echo "numactl --cpunodebind=${closest_cpu_node} --membind=${cxl_mem_node}"
                else
                    echo "numactl --cpunodebind=${dram_nodes} --membind=${cxl_mem_node}"
                fi
            else
                if [ "$cores" -le 128 ]; then
                    echo "numactl --cpunodebind=${closest_cpu_node}"
                else
                    echo "numactl --cpunodebind=${dram_nodes}"
                fi
            fi
            ;;
    esac
}