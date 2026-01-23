#!/usr/bin/env bash
# Description: run flash_icons against test_icons_<suffix>/ with optional interval passthrough
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
SHOW_HELP="${ROOT}/show_help.sh"

if [ "$#" -lt 1 ]; then
  exec "${SHOW_HELP}" "$(basename "$0")"
fi

if [[ "$1" == "-h" || "$1" == "--help" ]]; then
  exec "${SHOW_HELP}" "$(basename "$0")"
fi

SUFFIX="$1"; shift || true

if [[ "${SUFFIX}" == -* ]]; then
  echo "Unknown option: ${SUFFIX}" >&2
  exec "${SHOW_HELP}" "$(basename "$0")"
fi

ICON_DIR="${ROOT}/test_icons_${SUFFIX}"

if [ ! -d "${ICON_DIR}" ]; then
  echo "Directory not found: ${ICON_DIR}" >&2
  exit 1
fi

FLASH="${ROOT}/lib/flash_icons.sh"
if [ ! -x "${FLASH}" ]; then
  echo "flash_icons.sh not found at ${FLASH}" >&2
  exit 1
fi

exec "${FLASH}" "$@" "${ICON_DIR}"
