#!/usr/bin/env bash
# Description: send first 13 images from a folder across buttons 1-13 using ulanzi_d200_daemon daemon
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

SHOW_HELP="${ROOT}/show_help.sh"
SOCK_PATH="/tmp/ulanzi_device.sock"

if [ "$#" -ne 1 ]; then
  exec "${SHOW_HELP}" "$(basename "$0")"
fi

dir_in="$1"
dir="${dir_in}"
if [ ! -d "${dir}" ] && [[ "${dir}" != /* ]] && [ -d "${ROOT}/${dir}" ]; then
  dir="${ROOT}/${dir}"
fi
if [ ! -d "${dir}" ] && [[ "${dir_in}" != /* ]] && [ -d "${ROOT}/build/${dir_in}" ]; then
  dir="${ROOT}/build/${dir_in}"
fi
if [ ! -d "${dir}" ]; then
  echo "Not a directory: ${dir_in} (tried as-is and under ${ROOT}/)" >&2
  exit 1
fi

icons=()
while IFS= read -r -d '' f; do
  if command -v realpath >/dev/null 2>&1; then
    icons+=("$(realpath -m "$f")")
  else
    if [[ "$f" == /* ]]; then icons+=("$f"); else icons+=("$(pwd -P)/$f"); fi
  fi
done < <(find "${dir}" -maxdepth 1 -type f \( -iname '*.png' -o -iname '*.webp' -o -iname '*.jpg' -o -iname '*.jpeg' \) | sort | head -n 13 | tr '\n' '\0')

if [ "${#icons[@]}" -eq 0 ]; then
  echo "No images found in ${dir}" >&2
  exit 1
fi

cmd="set-buttons-explicit"
for i in "${!icons[@]}"; do
  btn=$((i + 1))
  cmd+=" --button-${btn}=${icons[$i]}"
done

resp="$(printf '%s\n' "${cmd}" | nc -U "${SOCK_PATH}" || true)"
if [ "${resp}" = "ok" ]; then
  echo "Sent ${#icons[@]} icon(s) from folder ${dir}"
else
  echo "send_page_from_folder.sh failed (response: ${resp:-<no response>})" >&2
  exit 1
fi
