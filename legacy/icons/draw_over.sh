#!/usr/bin/env bash
# Description: overlay top image onto bottom image with proper alpha blending.
# Resizes top image to match bottom dimensions, then composites.
# Bottom file is replaced with the result.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

TOP_FILE="${1:-}"
BOTTOM_FILE="${2:-}"

if [ -z "${TOP_FILE}" ] || [ -z "${BOTTOM_FILE}" ]; then
  echo "Usage: $0 <top.png> <bottom.png>" >&2
  echo "  Resizes top.png to bottom.png dimensions and composites." >&2
  echo "  Bottom file is replaced with the result." >&2
  exit 1
fi

MAGICK_BIN="$(command -v magick || true)"
CONVERT_BIN="$(command -v convert || true)"
IDENTIFY_BIN="$(command -v identify || true)"

if [ -n "${MAGICK_BIN}" ]; then
  IM_IDENTIFY=("${MAGICK_BIN}" identify)
  IM_CONVERT=("${MAGICK_BIN}")
elif [ -n "${CONVERT_BIN}" ] && [ -n "${IDENTIFY_BIN}" ]; then
  IM_IDENTIFY=("${IDENTIFY_BIN}")
  IM_CONVERT=("${CONVERT_BIN}")
else
  echo "ImageMagick is required (magick, or convert+identify)." >&2
  exit 1
fi

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

read -r BOTTOM_W BOTTOM_H <<<"$("${IM_IDENTIFY[@]}" -format "%w %h" "${BOTTOM_FILE}")"
if [ -z "${BOTTOM_W}" ] || [ -z "${BOTTOM_H}" ]; then
  echo "Failed to get dimensions of bottom file: ${BOTTOM_FILE}" >&2
  exit 1
fi

echo "Bottom image: ${BOTTOM_W}x${BOTTOM_H}"

TMP_BASE="/dev/shm"
TMP_TEST="${TMP_BASE}/.draw_over_wtest_$$"
if ! ( : >"${TMP_TEST}" ) 2>/dev/null; then
  TMP_BASE="/tmp"
else
  rm -f "${TMP_TEST}" || true
fi

TMP_RESIZED="${TMP_BASE}/.draw_over_resized_${BOTTOM_FILE##*/}"
trap 'rm -f "${TMP_RESIZED}"' EXIT

read -r TOP_W TOP_H <<<"$("${IM_IDENTIFY[@]}" -format "%w %h" "${TOP_FILE}")"
echo "Top image: ${TOP_W}x${TOP_H}"

if [ "${TOP_W}" -eq "${BOTTOM_W}" ] && [ "${TOP_H}" -eq "${BOTTOM_H}" ]; then
  echo "Same dimensions, no resize needed"
  cp "${TOP_FILE}" "${TMP_RESIZED}"
else
  echo "Resizing top image from ${TOP_W}x${TOP_H} to ${BOTTOM_W}x${BOTTOM_H}"
  "${IM_CONVERT[@]}" "${TOP_FILE}" -resize "${BOTTOM_W}x${BOTTOM_H}!" "${TMP_RESIZED}"
fi

echo "Compositing overlay..."
"${IM_CONVERT[@]}" \
  "${BOTTOM_FILE}" \
  "${TMP_RESIZED}" \
  -gravity center -compose over -composite \
  "${BOTTOM_FILE}"

rm -f "${TMP_RESIZED}"
echo "Updated ${BOTTOM_FILE} with overlay from ${TOP_FILE}"
