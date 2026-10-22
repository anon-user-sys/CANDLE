#!/bin/bash
set -euo pipefail # stop on a failed command, an unset variable or a pipeline error

# Script and project root
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="${SCRIPT_DIR}"

# If this script sits in a subfolder such as scripts/, the project root is one level up
if [ ! -f "${SCRIPT_DIR}/CMakeLists.txt" ] && [ -f "${SCRIPT_DIR}/../CMakeLists.txt" ]; then
    PROJECT_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
fi

cd "${PROJECT_ROOT}"

# Environment for the CXL kernel build (defaults apply only when not set from outside)
# The installed MU LLVM version is read from min_llvm_version_env.sh (e.g. 21.1.2/r4)
if [ -z "${MU_LLVM_PATH:-}" ] && [ -f /usr/local/mu_library/mu/script/min_llvm_version_env.sh ]; then
    source /usr/local/mu_library/mu/script/min_llvm_version_env.sh
    MU_LLVM_PATH="/usr/local/mu_library/mu_llvm/${XCENA_LLVM_VERSION}/${MU_REVISION}"
fi
export MU_LLVM_PATH="${MU_LLVM_PATH:-/usr/local/mu_library/mu_llvm/21.1.2/r4}"
export MU_LIB_PATH="${MU_LIB_PATH:-/usr/local/mu_library/mu}"

# Build options (can be changed through the environment)
BUILD_TYPE="${BUILD_TYPE:-Release}"

# Start from a fresh build directory
rm -rf build
mkdir -p build
cd build

# Configure with CMake and build with Ninja
cmake .. \
    -G Ninja \
    -DCMAKE_BUILD_TYPE="${BUILD_TYPE}"

ninja

# Run the install target if Ninja has one
if ninja -t targets | grep -q '^install:'; then
    ninja install
fi

echo -e "\n=========================================="
echo " B-Link Tree Build Completed Successfully! "
echo "=========================================="
echo "Generated Binaries in Project Root (${PROJECT_ROOT}):"
echo " [Mode 1: host_dram]  blink_host_dram_val | blink_host_dram_tp | blink_host_dram_lat"
echo " [Mode 2: host_cxl]   blink_host_cxl_val  | blink_host_cxl_tp  | blink_host_cxl_lat"
echo " [Mode 3: cxl_hybrid] blink_cxl_hybrid_val | blink_cxl_hybrid_tp | blink_cxl_hybrid_lat"
echo " [Mode 4: cxl_full]   blink_cxl_full_val  | blink_cxl_full_tp  | blink_cxl_full_lat"
echo "=========================================="