#!/bin/bash
set -e
cd "$(dirname "$0")"

export MU_LIB_PATH=${MU_LIB_PATH:-/usr/local/mu_library/mu}
# Toolchain version comes from the vendor manifest, not a hardcoded path that goes stale.
if [ -z "${MU_LLVM_PATH:-}" ] && [ -f "$MU_LIB_PATH/script/min_llvm_version_env.sh" ]; then
    # shellcheck disable=SC1091
    source "$MU_LIB_PATH/script/min_llvm_version_env.sh"
    MU_LLVM_PATH=/usr/local/mu_library/mu_llvm/${XCENA_LLVM_VERSION}/${MU_REVISION}
fi
export MU_LLVM_PATH=${MU_LLVM_PATH:-/usr/local/mu_library/mu_llvm/21.1.2/r4}
[ -x "$MU_LLVM_PATH/bin/clang" ] || { echo "MU toolchain not found at $MU_LLVM_PATH"; exit 1; }

rm -rf build
mkdir -p build
cd build
/usr/bin/cmake .. -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
ninja
ninja install
