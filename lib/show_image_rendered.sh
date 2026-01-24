#!/usr/bin/env bash
# Description: show a pre-rendered 14-tile image folder on the D200 via ulanzi_d200_demon (set-buttons-explicit-14)
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

SHOW_HELP="${ROOT}/show_help.sh"
SOCK_PATH="/tmp/ulanzi_device.sock"

abs_path() {
  local p="$1"
  if command -v realpath >/dev/null 2>&1; then
    realpath -m "$p"
  else
    if [[ "$p" == /* ]]; then
      printf "%s\n" "$p"
    else
      printf "%s/%s\n" "$(pwd -P)" "$p"
    fi
  fi
}

usage() {
  cat >&2 <<EOF
Usage: ./lib/show_image_rendered.sh <folder>

Folder must contain 14 PNG files matching:
  *-1.png ... *-14.png

Example:
  ./lib/show_image_rendered.sh mymedia/wallpapers/valley
EOF
}

if [ "$#" -ne 1 ] || [ "${1}" = "-h" ] || [ "${1}" = "--help" ]; then
  if [ "${1:-}" = "-h" ] || [ "${1:-}" = "--help" ]; then
    usage
    exit 0
  fi
  usage
  exit 2
fi

DIR="$1"
if [ ! -d "${DIR}" ] && [[ "${DIR}" != /* ]] && [ -d "${ROOT}/${DIR}" ]; then
  DIR="${ROOT}/${DIR}"
fi
if [ ! -d "${DIR}" ]; then
  echo "Folder not found: ${1}" >&2
  exit 1
fi
DIR="$(abs_path "${DIR}")"

cmd="set-buttons-explicit-14"
for n in $(seq 1 14); do
  shopt -s nullglob
  matches=("${DIR}"/*-"${n}".png)
  shopt -u nullglob

  if [ "${#matches[@]}" -eq 0 ]; then
    echo "Missing tile: ${DIR}/*-${n}.png" >&2
    exit 1
  fi
  if [ "${#matches[@]}" -gt 1 ]; then
    echo "Ambiguous tile for -${n}.png (multiple matches):" >&2
    printf '  %s\n' "${matches[@]}" >&2
    exit 1
  fi

  f="$(abs_path "${matches[0]}")"
  cmd+=" --button-${n}=${f}"
done

resp="$(printf '%s\n' "${cmd}" | nc -U "${SOCK_PATH}" || true)"
if [ "${resp}" = "ok" ]; then
  echo "Sent rendered folder: ${DIR}"
else
  echo "show_image_rendered.sh failed (response: ${resp:-<no response>})" >&2
  exit 1
fi
