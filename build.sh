#!/bin/bash

# Specify LLVM installation directory
LLVM_DIR=/opt/llvm
# Specify Z3 installation directory
Z3_DIR=/opt/z3

# Build Alive2
cmake -GNinja -Bbuild \
    -DLLVM_ROOT=$LLVM_DIR \
    -DZ3_ROOT=$Z3_DIR \
    -DBUILD_TV=ON \
    -DCMAKE_BUILD_TYPE=Release
cmake --build build
