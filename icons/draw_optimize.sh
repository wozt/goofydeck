#!/usr/bin/env bash
# Description: quantize a PNG to 64 colors and apply max PNG compression (in place)
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

FILENAME=""
COLOR_COUNT=64

while [ "$#" -gt 0 ]; do
  case "$1" in
    -c|--color)
      val="${2:-}"
      if [ -n "${val}" ]; then
        COLOR_COUNT="${val}"
        shift 2 || shift 1
      else
        COLOR_COUNT=64
        shift 1
      fi
      ;;
    --color=*)
      COLOR_COUNT="${1#*=}"
      shift
      ;;
    *)
      FILENAME="$1"
      shift
      ;;
  esac
done

if [ -z "${FILENAME}" ]; then
  echo "Usage: $0 [-c|--color <n<=256>] <filename.png>" >&2
  exit 1
fi

if [[ "${FILENAME}" != *.png ]]; then
  echo "Filename must end with .png" >&2
  exit 1
fi

is_int() { [[ "$1" =~ ^[0-9]+$ ]]; }
if ! is_int "${COLOR_COUNT}"; then COLOR_COUNT=64; fi
if [ "${COLOR_COUNT}" -lt 1 ]; then COLOR_COUNT=1; fi
if [ "${COLOR_COUNT}" -gt 256 ]; then COLOR_COUNT=256; fi

MAGICK_BIN="$(command -v magick || command -v convert)"
PNGQUANT_BIN="$(command -v pngquant || true)"
OPTIPNG_BIN="$(command -v optipng || true)"
ADVPNG_BIN="$(command -v advpng || true)"
ZOPFLIPNG_BIN="$(command -v zopflipng || true)"
if [ -z "${MAGICK_BIN}" ]; then
  echo "ImageMagick (convert or magick) is required." >&2
  exit 1
fi

IN_PATH="${FILENAME}"
if [[ "${IN_PATH}" != /* ]]; then
  IN_PATH="${ROOT}/${IN_PATH}"
fi

if [ ! -f "${IN_PATH}" ]; then
  echo "Input not found: ${IN_PATH}" >&2
  exit 1
fi

TMP_OUT="${IN_PATH}.opt"
TMP_ZOPF="${TMP_OUT}.zopf"

# Step 1: quantize to N colors with IM (no resize)
"${MAGICK_BIN}" "${IN_PATH}" \
  -alpha on \
  -dither FloydSteinberg -colors "${COLOR_COUNT}" -type PaletteAlpha \
  -strip \
  -define png:exclude-chunk=all \
  -define png:compression-level=9 \
  png32:"${TMP_OUT}"

# Step 2: if pngquant available, re-quantize & compress
if [ -n "${PNGQUANT_BIN}" ]; then
  "${PNGQUANT_BIN}" --force --strip --speed 1 --quality 0-100 --output "${TMP_OUT}" "${COLOR_COUNT}" "${TMP_OUT}"
fi

# Step 3: if optipng available, recompress
if [ -n "${OPTIPNG_BIN}" ]; then
  "${OPTIPNG_BIN}" -o7 "${TMP_OUT}" >/dev/null 2>&1 || true
fi

# Step 4: if advpng available, recompress further
if [ -n "${ADVPNG_BIN}" ]; then
  "${ADVPNG_BIN}" -z4 "${TMP_OUT}" >/dev/null 2>&1 || true
fi

# Step 5: if zopflipng available, final pass
if [ -n "${ZOPFLIPNG_BIN}" ]; then
  "${ZOPFLIPNG_BIN}" -y --iterations=15 --filters=01234mepb "${TMP_OUT}" "${TMP_ZOPF}" >/dev/null 2>&1 || true
  if [ -f "${TMP_ZOPF}" ]; then
    mv -f "${TMP_ZOPF}" "${TMP_OUT}"
  fi
fi

# Final normalize to clear png:* defines and enforce 8-bit RGBA
FINAL_OUT="${TMP_OUT}.norm"
"${MAGICK_BIN}" "${TMP_OUT}" \
  -alpha on -type TrueColorAlpha -depth 8 -colorspace sRGB \
  +set png:bit-depth +set png:color-type \
  png32:"${FINAL_OUT}"
mv -f "${FINAL_OUT}" "${TMP_OUT}"

mv -f "${TMP_OUT}" "${IN_PATH}"
echo "Optimized ${IN_PATH} (${COLOR_COUNT} colors, max compression)"
