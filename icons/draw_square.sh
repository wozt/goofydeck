#!/usr/bin/env bash
# Description: create a solid color square PNG (writes to the provided path, resolved relative to project root)
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

COLOR="${1:-}"
shift || true
SIZE=196
FILENAME=""

while [ "$#" -gt 0 ]; do
  case "$1" in
    --size=*) SIZE="${1#*=}"; shift ;;
    *) FILENAME="$1"; shift ;;
  esac
done

if [ -z "${COLOR}" ] || [ -z "${FILENAME}" ]; then
  echo "Usage: $0 <hexcolor|transparent> [--size=196] <filename.png>" >&2
  exit 1
fi

IS_TRANSPARENT=0
if [[ "${COLOR,,}" == "transparent" ]]; then
  IS_TRANSPARENT=1
elif ! [[ "${COLOR}" =~ ^[0-9A-Fa-f]{6}$ ]]; then
  echo "Invalid color: ${COLOR} (expected 6-digit hex or 'transparent')" >&2
  exit 1
fi

is_int() { [[ "$1" =~ ^[0-9]+$ ]]; }
if ! is_int "${SIZE}"; then SIZE=196; fi
if [ "${SIZE}" -gt 196 ]; then SIZE=196; fi
if [ "${SIZE}" -lt 1 ]; then SIZE=1; fi

if [[ "${FILENAME}" != *.png ]]; then
  echo "Filename must end with .png" >&2
  exit 1
fi

if ! command -v convert >/dev/null 2>&1 && ! command -v magick >/dev/null 2>&1; then
  echo "ImageMagick (convert or magick) is required." >&2
  exit 1
fi

CMD="$(command -v magick || command -v convert)"

resolve_root_path() {
  local p="$1"
  if [[ "$p" == /* ]]; then
    printf "%s\n" "$p"
  else
    printf "%s/%s\n" "${ROOT}" "$p"
  fi
}

OUT_PATH="$(resolve_root_path "${FILENAME}")"
mkdir -p "$(dirname "${OUT_PATH}")"

if [ "${IS_TRANSPARENT}" -eq 1 ]; then
  "${CMD}" -size "${SIZE}x${SIZE}" xc:none "${OUT_PATH}"
else
  "${CMD}" -size "${SIZE}x${SIZE}" "xc:#${COLOR}" "${OUT_PATH}"
fi

# Normalize colortype/alpha for safer composites
FIX_PATH="${OUT_PATH}.tmp"
"${CMD}" "${OUT_PATH}" \
  -alpha on -channel A -depth 8 +channel \
  -colorspace sRGB -type TrueColorAlpha \
  png32:"${FIX_PATH}"
mv -f "${FIX_PATH}" "${OUT_PATH}"

echo "Created ${OUT_PATH}"
