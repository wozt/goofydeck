#!/usr/bin/env bash
# Description: compile the C driver (ulanzi_ctl) using libhidapi and zlib
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="${SCRIPT_DIR}"
SRC="${ROOT}/ulanzi_ctl.c"
OUT="${ROOT}/ulanzi_ctl"

if ! command -v gcc >/dev/null 2>&1; then
  echo "gcc is required." >&2
  exit 1
fi

gcc -std=c11 -O2 -o "${OUT}" "${SRC}" -lhidapi-libusb -lz
echo "Built ${OUT}"
