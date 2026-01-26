#!/usr/bin/env bash
# Description: optimize a PNG (or all PNGs in a directory) in place with palette quantization and max compression.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

COLOR_COUNT=64
TARGET=""

usage() {
  echo "Usage: $(basename "$0") [-c|--color <1-256>] <file.png|directory/>" >&2
  exit 1
}

while [ "$#" -gt 0 ]; do
  case "$1" in
    -c|--color)
      COLOR_COUNT="${2:-}"
      shift 2 || usage
      ;;
    --color=*)
      COLOR_COUNT="${1#*=}"
      shift
      ;;
    -h|--help)
      usage
      ;;
    *)
      TARGET="$1"
      shift
      ;;
  esac
done

[ -z "${TARGET}" ] && usage

if [ ! -e "${TARGET}" ] && [[ "${TARGET}" != /* ]] && [ -e "${ROOT}/${TARGET}" ]; then
  TARGET="${ROOT}/${TARGET}"
fi

is_int() { [[ "$1" =~ ^[0-9]+$ ]]; }
if ! is_int "${COLOR_COUNT}"; then COLOR_COUNT=64; fi
if [ "${COLOR_COUNT}" -lt 1 ]; then COLOR_COUNT=1; fi
if [ "${COLOR_COUNT}" -gt 256 ]; then COLOR_COUNT=256; fi

MAGICK_BIN="$(command -v magick || command -v convert || true)"
PNGQUANT_BIN="$(command -v pngquant || true)"
OPTIPNG_BIN="$(command -v optipng || true)"
ADVPNG_BIN="$(command -v advpng || true)"
ZOPFLIPNG_BIN="$(command -v zopflipng || true)"
if [ -z "${MAGICK_BIN}" ]; then
  echo "ImageMagick (magick/convert) is required." >&2
  exit 1
fi

optimize_one() {
  local in_path="$1"
  if [ ! -f "${in_path}" ]; then
    echo "Skip (not a file): ${in_path}" >&2
    return
  fi
  if [[ "${in_path}" != *.png && "${in_path}" != *.PNG ]]; then
    echo "Skip (not png): ${in_path}" >&2
    return
  fi

  local dir base tmp tmp_zopf final_tmp
  dir="$(dirname "${in_path}")"
  base="$(basename "${in_path}")"
  tmp="$(mktemp "${dir}/.drawopt.${base}.XXXXXX")"
  tmp_zopf="${tmp}.zopf"
  final_tmp="${tmp}.norm"
  local stem ext out_path
  stem="${base%.*}"
  ext="${base##*.}"
  out_path="${dir}/${stem}_opt.${ext}"

  # Step 1: quantize to N colors with IM (no resize)
  "${MAGICK_BIN}" "${in_path}" \
    -alpha on \
    -dither FloydSteinberg -colors "${COLOR_COUNT}" -type PaletteAlpha \
    -strip \
    -define png:exclude-chunk=all \
    -define png:compression-level=9 \
    png32:"${tmp}"

  # Step 2: if pngquant available, re-quantize & compress
  if [ -n "${PNGQUANT_BIN}" ]; then
    "${PNGQUANT_BIN}" --force --strip --speed 1 --quality 0-100 --output "${tmp}" "${COLOR_COUNT}" "${tmp}"
  fi

  # Step 3: if optipng available, recompress
  if [ -n "${OPTIPNG_BIN}" ]; then
    "${OPTIPNG_BIN}" -o7 "${tmp}" >/dev/null 2>&1 || true
  fi

  # Step 4: if advpng available, recompress further
  if [ -n "${ADVPNG_BIN}" ]; then
    "${ADVPNG_BIN}" -z4 "${tmp}" >/dev/null 2>&1 || true
  fi

  # Step 5: if zopflipng available, final pass
  if [ -n "${ZOPFLIPNG_BIN}" ]; then
    "${ZOPFLIPNG_BIN}" -y --iterations=15 --filters=01234mepb "${tmp}" "${tmp_zopf}" >/dev/null 2>&1 || true
    if [ -f "${tmp_zopf}" ]; then
      mv -f "${tmp_zopf}" "${tmp}"
    fi
  fi

  # Final normalize to clear png:* defines and enforce 8-bit RGBA
  "${MAGICK_BIN}" "${tmp}" \
    -alpha on -type TrueColorAlpha -depth 8 -colorspace sRGB \
    +set png:bit-depth +set png:color-type \
    png32:"${final_tmp}"
  mv -f "${final_tmp}" "${tmp}"

  mv -f "${tmp}" "${out_path}"
  echo "Optimized ${in_path} -> ${out_path} (${COLOR_COUNT} colors)"
}

if [ -d "${TARGET}" ]; then
  # Find PNGs in directory
  while IFS= read -r -d '' file; do
    optimize_one "${file}"
  done < <(find "${TARGET}" -type f \( -iname '*.png' \) -print0)
elif [ -f "${TARGET}" ]; then
  optimize_one "${TARGET}"
else
  echo "Target not found: ${TARGET}" >&2
  exit 1
fi
