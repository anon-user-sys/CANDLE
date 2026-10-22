#!/bin/bash

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="${SCRIPT_DIR}"

if [ ! -f "${SCRIPT_DIR}/CMakeLists.txt" ] && [ -f "${SCRIPT_DIR}/../CMakeLists.txt" ]; then
    PROJECT_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
fi

cd "${PROJECT_ROOT}" || exit 1

export MU_LLVM_PATH="${MU_LLVM_PATH:-/usr/local/mu_library/mu_llvm/21.1.2/r4}"
export MU_LIB_PATH="${MU_LIB_PATH:-/usr/local/mu_library/mu}"

rm -rf build
mkdir -p build
cd build
/usr/bin/cmake .. -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
ninja

if ninja -t targets | grep -q '^install:'; then
    ninja install
fi