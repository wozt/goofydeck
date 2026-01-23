#!/usr/bin/env bash
# Description: render an MDI SVG at a given color/size (max 196),
# build a colored icon (icon colored, outside transparent),
# then center-composite it onto a PNG.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
MDI_DIR="${ROOT}/assets/mdi"
ICON_SPEC="${1:-}"
COLOR="${2:-}"
shift 2 || true

SIZE=196
FILENAME=""

while [ "$#" -gt 0 ]; do
  case "$1" in
    --size=*) SIZE="${1#*=}"; shift ;;
    *) FILENAME="$1"; shift ;;
  esac
done

if [ -z "${ICON_SPEC}" ] || [ -z "${COLOR}" ] || [ -z "${FILENAME}" ]; then
  echo "Usage: $0 <mdi:name> <hexcolor|transparent> [--size=128] <filename.png>" >&2
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

MAGICK_BIN="$(command -v magick || true)"
if [ -z "${MAGICK_BIN}" ]; then
  echo "ImageMagick v7 (magick) is required." >&2
  exit 1
fi

ICON_NAME="${ICON_SPEC#mdi:}"
ICON_PATH="${MDI_DIR}/${ICON_NAME}.svg"
if [ ! -f "${ICON_PATH}" ]; then
  echo "SVG not found: ${ICON_PATH}" >&2
  exit 1
fi

TARGET="${FILENAME}"
if [[ "${TARGET}" != /* ]]; then
  TARGET="${ROOT}/${TARGET}"
fi
mkdir -p "$(dirname "${TARGET}")"
if [ ! -f "${TARGET}" ]; then
  "${MAGICK_BIN}" -size 196x196 xc:none "${TARGET}"
fi

tmp_tag="${TARGET##*/}"
TMP_ICON="/dev/shm/.mdi_icon_${tmp_tag}"
TMP_MASK="/dev/shm/.mdi_mask_${tmp_tag}"
TMP_OVERLAY="/dev/shm/.mdi_overlay_${tmp_tag}"
TMP_OUT="/dev/shm/.mdi_out_${tmp_tag}"

trap 'rm -f "${TMP_ICON}" "${TMP_MASK}" "${TMP_OVERLAY}" "${TMP_OUT}"' EXIT

# 1) Render SVG -> PNG (robust ordering + explicit svg: coder)
"${MAGICK_BIN}" \
  -background none \
  -density 512 -define svg:xml-parse-huge=true \
  "svg:${ICON_PATH}" \
  -resize "${SIZE}x${SIZE}" \
  -alpha on \
  png32:"${TMP_ICON}"
#cp "${TMP_ICON}" "${DBG_ICON}"

# 2) Extract alpha mask from rendered icon
"${MAGICK_BIN}" "${TMP_ICON}" -alpha extract png8:"${TMP_MASK}"
#cp "${TMP_MASK}" "${DBG_MASK}"

# 3) Build overlay (different logic for transparent mode)
if [ "${IS_TRANSPARENT}" -eq 1 ]; then
  # For transparent mode: create pure alpha mask (black parts opaque, white parts transparent)
  # Create a transparent image, then use the mask as alpha channel
  "${MAGICK_BIN}" -size "${SIZE}x${SIZE}" xc:none -alpha set \
    "${TMP_MASK}" -alpha off -compose CopyOpacity -composite \
    png32:"${TMP_OVERLAY}"
else
  # Normal mode: solid color + icon mask as opacity
  "${MAGICK_BIN}" -size "${SIZE}x${SIZE}" "xc:#${COLOR}" -alpha set \
    "${TMP_MASK}" -compose CopyOpacity -composite \
    png32:"${TMP_OVERLAY}"
fi
#cp "${TMP_OVERLAY}" "${DBG_OVERLAY}"

# 4) Composite overlay onto target (fix target colortype/alpha first)
"${MAGICK_BIN}" "${TARGET}" \
  -alpha on -channel A -depth 8 +channel \
  -colorspace sRGB -type TrueColorAlpha \
  png32:"${TMP_OUT}"
mv -f "${TMP_OUT}" "${TARGET}"

# Now composite (force png32 on both inputs)
if [ "${IS_TRANSPARENT}" -eq 1 ]; then
  # For transparent mode: use DstOut to cut out the icon shape from target
  "${MAGICK_BIN}" \
    png32:"${TARGET}" \
    png32:"${TMP_OVERLAY}" \
    -gravity center -compose DstOut -composite \
    png32:"${TMP_OUT}"
else
  # Normal mode: standard overlay
  "${MAGICK_BIN}" \
    png32:"${TARGET}" \
    png32:"${TMP_OVERLAY}" \
    -gravity center -compose over -composite \
    png32:"${TMP_OUT}"
fi
#cp "${TMP_OUT}" "${DBG_COMPOSITED}"
mv -f "${TMP_OUT}" "${TARGET}"

if [ "${IS_TRANSPARENT}" -eq 1 ]; then
  echo "Updated ${TARGET} with mdi:${ICON_NAME} at size ${SIZE} (transparent mask)"
else
  echo "Updated ${TARGET} with mdi:${ICON_NAME} at size ${SIZE} color #${COLOR}"
fi
# echo "Debug written:"
# echo " - ${DBG_ICON}"
# echo " - ${DBG_MASK}"
# echo " - ${DBG_OVERLAY}"
# echo " - ${DBG_COMPOSITED}"
