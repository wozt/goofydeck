#!/usr/bin/env bash
# Description: overlay top image onto bottom image with proper alpha blending
# Resizes top image to match bottom dimensions, then composites.
# Bottom file is replaced with the result.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

ARGS=()

while [ "$#" -gt 0 ]; do
  case "$1" in
    *) ARGS+=("$1"); shift ;;
  esac
done

TOP_FILE="${ARGS[0]:-}"
BOTTOM_FILE="${ARGS[1]:-}"

if [ -z "${TOP_FILE}" ] || [ -z "${BOTTOM_FILE}" ]; then
  echo "Usage: $0 <top.png> <bottom.png>" >&2
  echo "  Resizes top.png to bottom.png dimensions and composites." >&2
  echo "  Bottom file is replaced with the result." >&2
  exit 1
fi

if ! command -v convert >/dev/null 2>&1 && ! command -v magick >/dev/null 2>&1; then
  echo "ImageMagick (convert or magick) is required." >&2
  exit 1
fi
CMD="$(command -v magick || command -v convert)"

if [[ "${TOP_FILE}" != /* ]]; then
  TOP_FILE="${ROOT}/${TOP_FILE}"
fi
if [ ! -f "${TOP_FILE}" ]; then
  echo "Top file not found: ${TOP_FILE}" >&2
  exit 1
fi

if [[ "${BOTTOM_FILE}" != /* ]]; then
  BOTTOM_FILE="${ROOT}/${BOTTOM_FILE}"
fi
if [ ! -f "${BOTTOM_FILE}" ]; then
  echo "Bottom file not found: ${BOTTOM_FILE}" >&2
  exit 1
fi

# Get bottom file dimensions
read -r BOTTOM_W BOTTOM_H <<<"$("${CMD}" identify -format "%w %h" "${BOTTOM_FILE}")"

if [ -z "${BOTTOM_W}" ] || [ -z "${BOTTOM_H}" ]; then
  echo "Failed to get dimensions of bottom file: ${BOTTOM_FILE}" >&2
  exit 1
fi

echo "Bottom image: ${BOTTOM_W}x${BOTTOM_H}"

# Create temporary files in /dev/shm
TMP_TOP="/dev/shm/.draw_over_top_${BOTTOM_FILE##*/}"
TMP_RESIZED="/dev/shm/.draw_over_resized_${BOTTOM_FILE##*/}"

trap 'rm -f "${TMP_TOP}" "${TMP_RESIZED}"' EXIT

# STEP 1: Get top file dimensions
read -r TOP_W TOP_H <<<"$("${CMD}" identify -format "%w %h" "${TOP_FILE}")"

echo "Top image: ${TOP_W}x${TOP_H}"

# STEP 2: Resize top.png to bottom.png dimensions
if [ "${TOP_W}" -eq "${BOTTOM_W}" ] && [ "${TOP_H}" -eq "${BOTTOM_H}" ]; then
  echo "Same dimensions, no resize needed"
  cp "${TOP_FILE}" "${TMP_RESIZED}"
else
  echo "Resizing top image from ${TOP_W}x${TOP_H} to ${BOTTOM_W}x${BOTTOM_H}"
  "${CMD}" "${TOP_FILE}" -resize "${BOTTOM_W}x${BOTTOM_H}" "${TMP_RESIZED}"
fi

# STEP 3: Compose images (top OVER bottom)
echo "Compositing overlay..."
"${CMD}" \
  "${BOTTOM_FILE}" \
  "${TMP_RESIZED}" \
  -gravity center -compose over -composite \
  "${BOTTOM_FILE}"

# Clean up temporary files
rm -f "${TMP_TOP}" "${TMP_RESIZED}"

echo "Updated ${BOTTOM_FILE} with overlay from ${TOP_FILE}"
