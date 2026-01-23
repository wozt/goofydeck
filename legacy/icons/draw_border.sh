#!/usr/bin/env bash
# Description: draw a filled rounded square centered onto an image (max 196x196).
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

COLOR="${1:-}"
shift || true

SIZE=196
RADIUS=0
INFILE=""

while [ "$#" -gt 0 ]; do
  case "$1" in
    --size=*) SIZE="${1#*=}"; shift ;;
    --radius=*) RADIUS="${1#*=}"; shift ;;
    *) INFILE="$1"; shift ;;
  esac
done

if [ -z "${COLOR}" ] || [ -z "${INFILE}" ]; then
  echo "Usage: $0 <hexcolor|transparent> [--size=128] [--radius=20] <input.png>" >&2
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
if ! is_int "${RADIUS}"; then RADIUS=0; fi
if [ "${RADIUS}" -gt 50 ]; then RADIUS=50; fi
if [ "${RADIUS}" -lt 0 ]; then RADIUS=0; fi

if [ -z "${INFILE}" ]; then
  echo "Input file required." >&2
  exit 1
fi

if ! command -v convert >/dev/null 2>&1 && ! command -v magick >/dev/null 2>&1; then
  echo "ImageMagick (convert or magick) is required." >&2
  exit 1
fi
CMD="$(command -v magick || command -v convert)"

INPUT_PATH="${INFILE}"
if [[ "${INPUT_PATH}" != /* ]]; then
  INPUT_PATH="${ROOT}/${INPUT_PATH}"
fi
if [ ! -f "${INPUT_PATH}" ]; then
  echo "Input not found: ${INPUT_PATH}" >&2
  exit 1
fi

radius_px=$(( SIZE * RADIUS / 100 ))
TMP_OUT="${INPUT_PATH}"

if [ "${IS_TRANSPARENT}" -eq 1 ]; then
  # Cut out (make transparent) a centered rounded-rect area using DstOut.
  "${CMD}" "${INPUT_PATH}" \
    \( -size "${SIZE}x${SIZE}" xc:none \
       -fill white -stroke none \
       -draw "roundrectangle 0,0 $((SIZE-1)),$((SIZE-1)) ${radius_px},${radius_px}" \) \
    -gravity center -compose DstOut -composite "${TMP_OUT}"
else
  # Normal overlay
  "${CMD}" "${INPUT_PATH}" \
    \( -size "${SIZE}x${SIZE}" xc:none \
       -fill "#${COLOR}" -stroke none \
       -draw "roundrectangle 0,0 $((SIZE-1)),$((SIZE-1)) ${radius_px},${radius_px}" \) \
    -gravity center -compose over -composite "${TMP_OUT}"
fi

# Normalize colortype/alpha for safer composites
FIX_PATH="${TMP_OUT}.tmp"
"${CMD}" "${TMP_OUT}" \
  -alpha on -channel A -depth 8 +channel \
  -colorspace sRGB -type TrueColorAlpha \
  png32:"${FIX_PATH}"
mv -f "${FIX_PATH}" "${TMP_OUT}"

echo "Updated ${TMP_OUT}"
