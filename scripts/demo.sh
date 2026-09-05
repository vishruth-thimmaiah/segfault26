#!/usr/bin/env bash
set -euo pipefail

# demo.sh — Run demo session testing the reduction bug or hello kernel

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"

echo "=== segfault26 OpenCL Debugger Demo ==="
if [ ! -f "${ROOT_DIR}/build/ocldbg" ]; then
    echo "ocldbg not built yet. Running build.sh first..."
    "${SCRIPT_DIR}/build.sh"
fi

echo "Running ocldbg with --help / usage demonstration:"
"${ROOT_DIR}/build/ocldbg" || true
