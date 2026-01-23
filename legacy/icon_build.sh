#!/usr/bin/env bash
# Description: build a 196x196 PNG icon (background -> optional border -> icon) using ImageMagick.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

MAGICK_BIN="$(command -v magick || true)"
if [ -z "${MAGICK_BIN}" ]; then
  echo "ImageMagick v7 (magick) is required." >&2
  exit 1
fi

CANVAS=196

FILENAME=""
ICON_SPEC=""
ICON_SIZE=128
ICON_PADDING=0
ICON_OFFSET="0,0"
ICON_BORDER_RADIUS=10
ICON_BORDER_WIDTH=0
ICON_BORDER_COLOR="FFFFFF"
ICON_BRIGHTNESS=100
ICON_COLOR="FFFFFF"
ICON_BACKGROUND_COLOR="transparent"

usage() {
  cat >&2 <<EOF
Usage: $0 --filename=NAME.png --icon=mdi:dev-to [options]

Output:
  Writes \${ROOT}/cache/NAME.png (creates cache/ if needed).

Required:
  --filename=NAME.png
  --icon=mdi:<name>     (uses \${ROOT}/mdi/<name>.svg) or a file path (.svg/.png)

Options (defaults shown):
  --icon_size=${ICON_SIZE}              (1..${CANVAS})
  --icon_padding=${ICON_PADDING}        (px, 0..${CANVAS}/2; reduces max icon size)
  --icon_offset=${ICON_OFFSET}          (x,y px; e.g. -4,8)
  --icon_border_radius=${ICON_BORDER_RADIUS}    (% of canvas, 0..50; 50 = circle)
  --icon_border_width=${ICON_BORDER_WIDTH}      (px, 0..${CANVAS}/2; 0 disables)
  --icon_border_color=${ICON_BORDER_COLOR}      (hex RRGGBB or "transparent")
  --icon_brightness=${ICON_BRIGHTNESS}          (0..100; affects icon only)
  --icon_color=${ICON_COLOR}                    (hex RRGGBB; affects icon only)
  --icon_background_color=${ICON_BACKGROUND_COLOR} (hex RRGGBB or "transparent")

Stack order:
  background -> border (optional) -> icon
EOF
}

is_int() { [[ "${1:-}" =~ ^-?[0-9]+$ ]]; }

normalize_hex_or_transparent() {
  local v="${1:-}"
  if [ -z "${v}" ]; then
    echo ""
    return 0
  fi
  if [[ "${v}" == "transparent" || "${v}" == "none" ]]; then
    echo "transparent"
    return 0
  fi
  if ! [[ "${v}" =~ ^[0-9A-Fa-f]{6}$ ]]; then
    echo ""
    return 1
  fi
  echo "${v^^}"
}

clamp_int() {
  local v="$1" lo="$2" hi="$3"
  if [ "$v" -lt "$lo" ]; then echo "$lo"; return 0; fi
  if [ "$v" -gt "$hi" ]; then echo "$hi"; return 0; fi
  echo "$v"
}

while [ "$#" -gt 0 ]; do
  case "$1" in
    -h|--help) usage; exit 0 ;;
    --filename=*) FILENAME="${1#*=}"; shift ;;
    --filename) FILENAME="${2:-}"; shift 2 ;;
    --icon=*) ICON_SPEC="${1#*=}"; shift ;;
    --icon) ICON_SPEC="${2:-}"; shift 2 ;;
    --icon_size=*) ICON_SIZE="${1#*=}"; shift ;;
    --icon_size) ICON_SIZE="${2:-}"; shift 2 ;;
    --icon_padding=*) ICON_PADDING="${1#*=}"; shift ;;
    --icon_padding) ICON_PADDING="${2:-}"; shift 2 ;;
    --icon_offset=*) ICON_OFFSET="${1#*=}"; shift ;;
    --icon_offset) ICON_OFFSET="${2:-}"; shift 2 ;;
    --icon_border_radius=*) ICON_BORDER_RADIUS="${1#*=}"; shift ;;
    --icon_border_radius) ICON_BORDER_RADIUS="${2:-}"; shift 2 ;;
    --icon_border_width=*) ICON_BORDER_WIDTH="${1#*=}"; shift ;;
    --icon_border_width) ICON_BORDER_WIDTH="${2:-}"; shift 2 ;;
    --icon_border_color=*) ICON_BORDER_COLOR="${1#*=}"; shift ;;
    --icon_border_color) ICON_BORDER_COLOR="${2:-}"; shift 2 ;;
    --icon_brightness=*) ICON_BRIGHTNESS="${1#*=}"; shift ;;
    --icon_brightness) ICON_BRIGHTNESS="${2:-}"; shift 2 ;;
    --icon_color=*) ICON_COLOR="${1#*=}"; shift ;;
    --icon_color) ICON_COLOR="${2:-}"; shift 2 ;;
    --icon_background_color=*) ICON_BACKGROUND_COLOR="${1#*=}"; shift ;;
    --icon_background_color) ICON_BACKGROUND_COLOR="${2:-}"; shift 2 ;;
    *) echo "Unknown argument: $1" >&2; usage; exit 2 ;;
  esac
done

if [ -z "${FILENAME}" ] || [ -z "${ICON_SPEC}" ]; then
  usage
  exit 2
fi

if [[ "${FILENAME}" != *.png ]]; then
  FILENAME="${FILENAME}.png"
fi

if ! is_int "${ICON_SIZE}"; then ICON_SIZE=128; fi
if ! is_int "${ICON_PADDING}"; then ICON_PADDING=0; fi
if ! is_int "${ICON_BORDER_RADIUS}"; then ICON_BORDER_RADIUS=10; fi
if ! is_int "${ICON_BORDER_WIDTH}"; then ICON_BORDER_WIDTH=0; fi
if ! is_int "${ICON_BRIGHTNESS}"; then ICON_BRIGHTNESS=100; fi

ICON_SIZE="$(clamp_int "${ICON_SIZE}" 1 "${CANVAS}")"
ICON_PADDING="$(clamp_int "${ICON_PADDING}" 0 "$((CANVAS / 2))")"
ICON_BORDER_RADIUS="$(clamp_int "${ICON_BORDER_RADIUS}" 0 50)"
ICON_BORDER_WIDTH="$(clamp_int "${ICON_BORDER_WIDTH}" 0 "$((CANVAS / 2))")"
ICON_BRIGHTNESS="$(clamp_int "${ICON_BRIGHTNESS}" 0 100)"

ICON_COLOR="$(normalize_hex_or_transparent "${ICON_COLOR}")" || { echo "Invalid --icon_color (expected RRGGBB)" >&2; exit 2; }
if [ "${ICON_COLOR}" = "transparent" ]; then
  echo "Invalid --icon_color: transparent (expected RRGGBB)" >&2
  exit 2
fi

ICON_BACKGROUND_COLOR="$(normalize_hex_or_transparent "${ICON_BACKGROUND_COLOR}")" || { echo "Invalid --icon_background_color (expected RRGGBB or transparent)" >&2; exit 2; }
ICON_BORDER_COLOR="$(normalize_hex_or_transparent "${ICON_BORDER_COLOR}")" || { echo "Invalid --icon_border_color (expected RRGGBB or transparent)" >&2; exit 2; }

if ! [[ "${ICON_OFFSET}" =~ ^-?[0-9]+,-?[0-9]+$ ]]; then
  echo "Invalid --icon_offset: ${ICON_OFFSET} (expected x,y)" >&2
  exit 2
fi
OFF_X="${ICON_OFFSET%,*}"
OFF_Y="${ICON_OFFSET#*,}"

if [ "${ICON_PADDING}" -gt 0 ]; then
  max_icon="$((CANVAS - 2 * ICON_PADDING))"
  if [ "${ICON_SIZE}" -gt "${max_icon}" ]; then
    ICON_SIZE="${max_icon}"
  fi
  if [ "${ICON_SIZE}" -lt 1 ]; then
    ICON_SIZE=1
  fi
fi

OUT_DIR="${ROOT}/cache"
OUT="${OUT_DIR}/${FILENAME}"
mkdir -p "${OUT_DIR}"

tmp_tag="${FILENAME//[^A-Za-z0-9_.-]/_}"
TMP_BASE="/dev/shm/.icon_build_base_${tmp_tag}"
TMP_MASK="/dev/shm/.icon_build_mask_${tmp_tag}"
TMP_BORDER="/dev/shm/.icon_build_border_${tmp_tag}"
TMP_ICON="/dev/shm/.icon_build_icon_${tmp_tag}"
TMP_ICON_MASK="/dev/shm/.icon_build_icon_mask_${tmp_tag}"
TMP_ICON_OVERLAY="/dev/shm/.icon_build_icon_overlay_${tmp_tag}"
TMP_OUT="/dev/shm/.icon_build_out_${tmp_tag}"

trap 'rm -f "${TMP_BASE}" "${TMP_MASK}" "${TMP_BORDER}" "${TMP_ICON}" "${TMP_ICON_MASK}" "${TMP_ICON_OVERLAY}" "${TMP_OUT}"' EXIT

radius_px="$((CANVAS * ICON_BORDER_RADIUS / 100))"
max_r="$((CANVAS / 2))"
if [ "${radius_px}" -gt "${max_r}" ]; then radius_px="${max_r}"; fi

# Rounded-rect mask for clipping (also used by background).
"${MAGICK_BIN}" -size "${CANVAS}x${CANVAS}" xc:none \
  -fill white -stroke none \
  -draw "roundrectangle 0,0 $((CANVAS - 1)),$((CANVAS - 1)) ${radius_px},${radius_px}" \
  png8:"${TMP_MASK}"

# Background (transparent or solid) clipped by mask.
bg_arg="none"
if [ "${ICON_BACKGROUND_COLOR}" != "transparent" ]; then
  bg_arg="#${ICON_BACKGROUND_COLOR}"
fi
"${MAGICK_BIN}" -size "${CANVAS}x${CANVAS}" "xc:${bg_arg}" -alpha set \
  png8:"${TMP_MASK}" -compose CopyOpacity -composite \
  png32:"${TMP_BASE}"

# Border overlay (optional).
if [ "${ICON_BORDER_WIDTH}" -gt 0 ] && [ "${ICON_BORDER_COLOR}" != "transparent" ]; then
  inset="$(((ICON_BORDER_WIDTH + 1) / 2))"
  r2="$((radius_px - inset))"
  if [ "${r2}" -lt 0 ]; then r2=0; fi
  x0="${inset}"; y0="${inset}"
  x1="$((CANVAS - 1 - inset))"; y1="$((CANVAS - 1 - inset))"
  if [ "${x1}" -lt "${x0}" ]; then x0=0; y0=0; x1="$((CANVAS - 1))"; y1="$((CANVAS - 1))"; fi
  "${MAGICK_BIN}" -size "${CANVAS}x${CANVAS}" xc:none \
    -fill none -stroke "#${ICON_BORDER_COLOR}" -strokewidth "${ICON_BORDER_WIDTH}" \
    -draw "roundrectangle ${x0},${y0} ${x1},${y1} ${r2},${r2}" \
    png32:"${TMP_BORDER}"
  "${MAGICK_BIN}" png32:"${TMP_BASE}" png32:"${TMP_BORDER}" -compose over -composite png32:"${TMP_BASE}"
fi

# Resolve icon input (mdi:<name> or file path).
ICON_INPUT=""
if [[ "${ICON_SPEC}" == mdi:* ]]; then
  icon_name="${ICON_SPEC#mdi:}"
  ICON_INPUT="${ROOT}/mdi/${icon_name}.svg"
else
  ICON_INPUT="${ICON_SPEC}"
  if [[ "${ICON_INPUT}" != /* ]]; then
    ICON_INPUT="${ROOT}/${ICON_INPUT}"
  fi
fi

if [ ! -f "${ICON_INPUT}" ]; then
  echo "Icon source not found: ${ICON_INPUT}" >&2
  exit 1
fi

# Render -> alpha mask -> solid-color overlay (so we can apply --icon_color and --icon_brightness).
case "${ICON_INPUT,,}" in
  *.svg)
    "${MAGICK_BIN}" -background none -density 512 -define svg:xml-parse-huge=true \
      "svg:${ICON_INPUT}" -resize "${ICON_SIZE}x${ICON_SIZE}" -alpha on \
      png32:"${TMP_ICON}"
    ;;
  *.png)
    "${MAGICK_BIN}" "${ICON_INPUT}" -alpha on -resize "${ICON_SIZE}x${ICON_SIZE}" \
      png32:"${TMP_ICON}"
    ;;
  *)
    echo "Unsupported icon file type (expected .svg or .png): ${ICON_INPUT}" >&2
    exit 2
    ;;
esac

"${MAGICK_BIN}" "${TMP_ICON}" -alpha extract png8:"${TMP_ICON_MASK}"
"${MAGICK_BIN}" -size "${ICON_SIZE}x${ICON_SIZE}" "xc:#${ICON_COLOR}" -alpha set \
  "${TMP_ICON_MASK}" -compose CopyOpacity -composite \
  -modulate "${ICON_BRIGHTNESS},100,100" \
  png32:"${TMP_ICON_OVERLAY}"

geom_x="${OFF_X}"; geom_y="${OFF_Y}"
if [ "${geom_x}" -ge 0 ]; then geom_x="+${geom_x}"; fi
if [ "${geom_y}" -ge 0 ]; then geom_y="+${geom_y}"; fi
GEOM="${geom_x}${geom_y}"

"${MAGICK_BIN}" png32:"${TMP_BASE}" png32:"${TMP_ICON_OVERLAY}" \
  -gravity center -geometry "${GEOM}" -compose over -composite \
  png32:"${TMP_OUT}"
mv -f "${TMP_OUT}" "${OUT}"

echo "${OUT}"
