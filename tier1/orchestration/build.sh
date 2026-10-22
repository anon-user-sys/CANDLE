export MU_LLVM_PATH=/usr/local/mu_library/mu_llvm/21.1.2/r4
export MU_LIB_PATH=/usr/local/mu_library/mu

rm -rf build
mkdir -p build
cd build
/usr/bin/cmake .. -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
ninja
ninja install
cd -
