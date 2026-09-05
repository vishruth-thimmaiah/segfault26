#!/usr/bin/env bash
set -euo pipefail

# build.sh — Build script for segfault26 OpenCL debugger
# Supports Debug and Release builds with CMake

BUILD_TYPE="${1:-Debug}"
BUILD_DIR="build"

echo "=== Configuring ocldbg (${BUILD_TYPE}) ==="
cmake -B "${BUILD_DIR}" \
      -DCMAKE_BUILD_TYPE="${BUILD_TYPE}" \
      -DCMAKE_EXPORT_COMPILE_COMMANDS=ON

echo "=== Building ocldbg ==="
cmake --build "${BUILD_DIR}" -- -j"$(nproc)"

echo "=== Build completed successfully ==="
echo "Binaries located in: ${BUILD_DIR}/"
