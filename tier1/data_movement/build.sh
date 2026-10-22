#!/bin/bash
set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

rm -rf build
mkdir -p build
cd build
/usr/bin/cmake .. -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
ninja
ninja install
cd -
