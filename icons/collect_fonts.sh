#!/usr/bin/env bash
# Collect all TTF fonts from common system locations into ./fonts/.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
FONT_DIR="${ROOT}/fonts"
mkdir -p "${FONT_DIR}"

declare -a SEARCH_DIRS=(
  "/usr/share/fonts"
  "/usr/local/share/fonts"
  "/Library/Fonts"
  "/System/Library/Fonts"
  "${HOME}/.local/share/fonts"
  "${HOME}/Library/Fonts"
)

shopt -s nullglob
copied=0
for dir in "${SEARCH_DIRS[@]}"; do
  [ -d "${dir}" ] || continue
  while IFS= read -r f; do
    dest="${FONT_DIR}/$(basename "$f")"
    if [ ! -f "${dest}" ]; then
      cp -n "$f" "${dest}" && copied=$((copied+1))
    fi
  done < <(find "${dir}" -type f -iname "*.ttf" 2>/dev/null)
done
shopt -u nullglob

echo "Collected ${copied} font(s) into ${FONT_DIR}"
