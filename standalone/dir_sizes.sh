#!/usr/bin/env bash
# Description: list file sizes (recursive on dirs) and print total bytes
set -euo pipefail

gather() {
  local path="$1"
  if [ -d "$path" ]; then
    find "$path" -type f -print
  elif [ -f "$path" ]; then
    printf '%s\n' "$path"
  elif [[ "$path" != /* ]] && [ -d "${ROOT}/$path" ]; then
    find "${ROOT}/$path" -type f -print
  elif [[ "$path" != /* ]] && [ -f "${ROOT}/$path" ]; then
    printf '%s\n' "${ROOT}/$path"
  else
    echo "Path not found: $path" >&2
  fi
}

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SHOW_HELP="${ROOT}/show_help.sh"
ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

if [ "$#" -lt 1 ]; then
  exec "${SHOW_HELP}" "$(basename "$0")"
fi

for arg in "$@"; do
  if [[ "$arg" == "-h" || "$arg" == "--help" ]]; then
    exec "${SHOW_HELP}" "$(basename "$0")"
  fi
  if [[ "$arg" == -* ]]; then
    echo "Unknown option: $arg" >&2
    exec "${SHOW_HELP}" "$(basename "$0")"
  fi
done

total=0

while IFS= read -r file; do
  [ -f "$file" ] || continue
  size=$(stat -c %s "$file")
  printf "%10d %s\n" "$size" "$file"
  total=$((total + size))
done < <(for p in "$@"; do gather "$p"; done)

echo "Total: ${total} bytes"
