#!/usr/bin/env bash
# Description: quantize a PNG to N colors (2-256) and write <name>_<N>.png alongside the input.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

COLORS=""
INPUT=""

usage() {
  echo "Usage: $(basename "$0") -c <2-256> <file.png>" >&2
  exit 1
}

while [ "$#" -gt 0 ]; do
  case "$1" in
    -c|--colors)
      COLORS="${2:-}"
      shift 2 || usage
      ;;
    --colors=*)
      COLORS="${1#*=}"
      shift
      ;;
    -h|--help)
      usage
      ;;
    *)
      INPUT="$1"
      shift
      ;;
  esac
done

[ -z "${COLORS}" ] && usage
[ -z "${INPUT}" ] && usage

if ! [[ "${COLORS}" =~ ^[0-9]+$ ]]; then
  echo "Invalid color count (must be integer 2-256)" >&2
  exit 1
fi
if [ "${COLORS}" -lt 2 ] || [ "${COLORS}" -gt 256 ]; then
  echo "Color count out of range (2-256)" >&2
  exit 1
fi
if [ ! -f "${INPUT}" ]; then
  echo "File not found: ${INPUT}" >&2
  exit 1
fi
case "${INPUT}" in
  *.png|*.PNG) ;;
  *) echo "Input must be a PNG file" >&2; exit 1;;
esac

MAGICK_BIN="$(command -v magick || command -v convert || true)"
PNGQUANT_BIN="$(command -v pngquant || true)"
OPTIPNG_BIN="$(command -v optipng || true)"
ADVPNG_BIN="$(command -v advpng || true)"
ZOPFLIPNG_BIN="$(command -v zopflipng || true)"

if [ -z "${MAGICK_BIN}" ]; then
  echo "ImageMagick (magick/convert) is required." >&2
  exit 1
fi

dir="$(dirname "${INPUT}")"
base="$(basename "${INPUT}")"
stem="${base%.*}"
ext="${base##*.}"
out_path="${dir}/${stem}_${COLORS}.${ext}"

# Decide target format (force PNG8 when palette size <=256)
TARGET_FORMAT="PNG8"
if [ "${COLORS}" -gt 256 ]; then
  TARGET_FORMAT="png32"
fi

tmp="$(mktemp "${dir}/.recolor.${stem}.XXXXXX")"
tmp_zopf="${tmp}.zopf"
final_tmp="${tmp}.final"

cleanup() { rm -f "${tmp}" "${tmp_zopf}" "${final_tmp}"; }
trap cleanup EXIT

# Step 1: quantize with ImageMagick palette + strip
"${MAGICK_BIN}" "${INPUT}" \
  -alpha on \
  -dither FloydSteinberg -colors "${COLORS}" -type PaletteAlpha \
  -strip \
  -define png:exclude-chunk=all \
  -define png:compression-level=9 \
  "${TARGET_FORMAT}:${tmp}"

# Step 2: pngquant (optional)
if [ -n "${PNGQUANT_BIN}" ]; then
  "${PNGQUANT_BIN}" --force --strip --speed 1 --quality 0-100 --output "${tmp}" "${COLORS}" "${tmp}" || true
fi

# Step 3: optipng (optional)
if [ -n "${OPTIPNG_BIN}" ]; then
  "${OPTIPNG_BIN}" -o7 "${tmp}" >/dev/null 2>&1 || true
fi

# Step 4: advpng (optional)
if [ -n "${ADVPNG_BIN}" ]; then
  "${ADVPNG_BIN}" -z4 "${tmp}" >/dev/null 2>&1 || true
fi

# Step 5: zopflipng (optional)
if [ -n "${ZOPFLIPNG_BIN}" ]; then
  "${ZOPFLIPNG_BIN}" -y --iterations=15 --filters=01234mepb "${tmp}" "${tmp_zopf}" >/dev/null 2>&1 || true
  if [ -f "${tmp_zopf}" ]; then mv -f "${tmp_zopf}" "${tmp}"; fi
fi

# Normalize final output
"${MAGICK_BIN}" "${tmp}" \
  -alpha on -type PaletteAlpha -depth 8 -colorspace sRGB \
  +set png:bit-depth +set png:color-type \
  "${TARGET_FORMAT}:${final_tmp}"
mv -f "${final_tmp}" "${out_path}"

echo "Recolored ${INPUT} -> ${out_path} (${COLORS} colors)"
