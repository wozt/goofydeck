#!/usr/bin/env bash
# Description: run a script by name from root/bin/icons/standalone/install with provided args
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SHOW_HELP="${SCRIPT_DIR}/show_help.sh"
ROOT="${SCRIPT_DIR}"

if [[ "${1:-}" == "--complete" ]]; then
  # Output available script basenames from root + bin/ + icons/ + standalone/
  find "${ROOT}" -maxdepth 1 -type f -name "*.sh" -printf "%f\n"
  find "${ROOT}/lib" -maxdepth 1 -type f -name "*.sh" -printf "%f\n"
  find "${ROOT}/icons" -maxdepth 1 -type f -name "*.sh" -printf "%f\n"
  find "${ROOT}/standalone" -maxdepth 1 -type f -name "*.sh" -printf "%f\n"
  exit 0
fi

if [ "$#" -lt 1 ]; then
  exec "${SHOW_HELP}" "$(basename "$0")"
fi

if [[ "$1" == "-h" || "$1" == "--help" ]]; then
  exec "${SHOW_HELP}" "$(basename "$0")"
fi

TARGET_NAME="$1"
shift || true

if [[ "${TARGET_NAME}" == -* ]]; then
  echo "Unknown option: ${TARGET_NAME}" >&2
  exec "${SHOW_HELP}" "$(basename "$0")"
fi

candidates=(
  "${ROOT}/${TARGET_NAME}"
  "${ROOT}/bin/${TARGET_NAME}"
  "${ROOT}/icons/${TARGET_NAME}"
  "${ROOT}/standalone/${TARGET_NAME}"
)

target_path=""
for cand in "${candidates[@]}"; do
  if [ -f "$cand" ]; then
    target_path="$cand"
    break
  fi
done

if [ -z "${target_path}" ]; then
  echo "Script not found: ${TARGET_NAME}" >&2
  exec "${SHOW_HELP}" "$(basename "$0")"
fi

echo "Running ${target_path} $*"
exec bash "${target_path}" "$@"
