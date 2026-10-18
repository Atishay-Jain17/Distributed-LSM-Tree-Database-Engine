#!/usr/bin/env bash
# ==============================================================================
# build_linux_all.sh
# Reproducible build and verification script for the complete Distributed LSM-Tree
# Database Engine, including Linux POSIX SSTables, gRPC Raft cluster, and tests.
# ==============================================================================
set -euo pipefail

echo "==> Step 1: Checking required build tools and libraries..."
MISSING_PKGS=()
for cmd in cmake g++ pkg-config protoc grpc_cpp_plugin; do
    if ! command -v "$cmd" >/dev/null 2>&1; then
        MISSING_PKGS+=("$cmd")
    fi
done

if [ ${#MISSING_PKGS[@]} -ne 0 ]; then
    echo "ERROR: The following required dependencies are missing: ${MISSING_PKGS[*]}"
    echo "To install all prerequisites on Ubuntu/Debian, run:"
    echo "  sudo apt-get update && sudo apt-get install -y \\"
    echo "      build-essential cmake pkg-config libgrpc++-dev libprotobuf-dev \\"
    echo "      protobuf-compiler protobuf-compiler-grpc libgtest-dev"
    exit 1
fi

BUILD_DIR="build/linux-verify"
echo "==> Step 2: Configuring CMake in ${BUILD_DIR} with gRPC, GoogleTest, and SkipList..."
cmake -S . -B "${BUILD_DIR}" \
    -DCMAKE_BUILD_TYPE=Release \
    -DLSMDB_BUILD_SUHANI_MODULES=ON \
    -DLSMDB_BUILD_SUHANI_RPC=ON \
    -DLSMDB_BUILD_SUHANI_TESTS=ON \
    -DLSMDB_USE_ATISHAY_SKIPLIST=ON

echo "==> Step 3: Compiling all targets (Storage, Compaction, Raft, gRPC node, CLI)..."
cmake --build "${BUILD_DIR}" -j"$(nproc)"

echo "==> Step 4: Running full CTest verification suite..."
ctest --test-dir "${BUILD_DIR}" --output-on-failure

echo "==> Step 5: Executing storage performance benchmark..."
"${BUILD_DIR}/lsm_storage_benchmark"

echo "=============================================================================="
echo "SUCCESS: Full project build, POSIX SSTable tests, and gRPC Raft tests passed!"
echo "=============================================================================="
