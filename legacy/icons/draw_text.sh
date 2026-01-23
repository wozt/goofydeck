#!/usr/bin/env bash
# Add text onto an existing PNG with configurable font/color/position.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
FONT_DIR="${ROOT}/assets/fonts"

TEXT=""
TEXT_COLOR="00FF00"
TEXT_ALIGN="center"
TEXT_FONT=""
TEXT_SIZE=16
TEXT_OFFSET="0,0"
FILENAME=""
LIST_TTF=0

is_int() { [[ "$1" =~ ^-?[0-9]+$ ]]; }

while [ "$#" -gt 0 ]; do
  case "$1" in
    --list-ttf) LIST_TTF=1; shift ;;
    --text=*) TEXT="${1#*=}"; shift ;;
    --text_color=*) TEXT_COLOR="${1#*=}"; shift ;;
    --text_align=*) TEXT_ALIGN="${1#*=}"; shift ;;
    --text_font=*) TEXT_FONT="${1#*=}"; shift ;;
    --text_size=*) TEXT_SIZE="${1#*=}"; shift ;;
    --text_offset=*) TEXT_OFFSET="${1#*=}"; shift ;;
    -*) echo "Unknown option: $1" >&2; exit 1 ;;
    *) 
      if [ -z "${FILENAME}" ]; then
        FILENAME="$1"
      else
        echo "Unexpected argument: $1" >&2
        exit 1
      fi
      shift ;;
  esac
done

MAGICK_BIN="$(command -v magick || command -v convert || true)"
if [ -z "${MAGICK_BIN}" ]; then
  echo "ImageMagick is required." >&2
  exit 1
fi

if [ "${LIST_TTF}" -eq 1 ]; then
  if ! command -v fc-list >/dev/null 2>&1; then
    echo "fc-list not found; install fontconfig to list fonts." >&2
    exit 1
  fi
  # Project fonts (absolute)
  find "${FONT_DIR}" -maxdepth 1 -type f -iname "*.ttf" 2>/dev/null | sort -f
  # System fonts (absolute, filtered)
  fc-list : file | awk -F: '{print $1}' | \
    grep -Ei '\.ttf$' | grep -Evi 'ding|symbol|wing' | sort -u
  exit 0
fi

if [ -z "${FILENAME}" ]; then
  echo "Usage: $0 [--text=hello] [--text_color=00FF00|transparent] [--text_align=top|center|bottom] [--text_font=Roboto.ttf] [--text_size=16] [--text_offset=x,y] <filename.png>" >&2
  exit 1
fi

if [[ "${FILENAME}" != *.png ]]; then
  echo "Filename must end with .png" >&2
  exit 1
fi

IS_TRANSPARENT=0
if [[ "${TEXT_COLOR,,}" == "transparent" ]]; then
  IS_TRANSPARENT=1
elif ! [[ "${TEXT_COLOR}" =~ ^[0-9A-Fa-f]{6}$ ]]; then
  echo "Invalid text_color: ${TEXT_COLOR} (expected 6-digit hex or 'transparent')" >&2
  exit 1
fi

case "${TEXT_ALIGN}" in
  top) GRAVITY="North" ;;
  bottom) GRAVITY="South" ;;
  center|"") GRAVITY="Center" ;;
  *) echo "Invalid text_align: ${TEXT_ALIGN} (use top|center|bottom)" >&2; exit 1 ;;
esac

if ! is_int "${TEXT_SIZE}"; then TEXT_SIZE=16; fi
read -r OFF_X OFF_Y <<<"$(echo "${TEXT_OFFSET}" | awk -F, '{print $1" "($2==""?0:$2)}')"
if ! is_int "${OFF_X}"; then OFF_X=0; fi
if ! is_int "${OFF_Y}"; then OFF_Y=0; fi

TARGET="${FILENAME}"
if [[ "${TARGET}" != /* ]]; then
  TARGET="${ROOT}/${TARGET}"
fi
if [ ! -f "${TARGET}" ]; then
  echo "Input not found: ${TARGET}" >&2
  exit 1
fi

# Enforce max dimensions 196x196
read -r W H <<<"$("${MAGICK_BIN}" identify -format "%w %h" "${TARGET}")"
if [ "${W}" -gt 196 ] || [ "${H}" -gt 196 ]; then
  echo "Input exceeds 196x196 (got ${W}x${H})" >&2
  exit 1
fi

TMP_OUT="${TARGET}.texttmp"
TMP_TEXT="${TARGET}.texttmp.png"
TMP_MASK="${TARGET}.textmask.png"
trap 'rm -f "${TMP_OUT}" "${TMP_TEXT}" "${TMP_MASK}"' EXIT

FONT_OPT=()
if [ -n "${TEXT_FONT}" ]; then
  FONT_CAND=""
  if [ -f "${TEXT_FONT}" ]; then
    FONT_CAND="${TEXT_FONT}"
  elif [ -f "${FONT_DIR}/${TEXT_FONT}" ]; then
    FONT_CAND="${FONT_DIR}/${TEXT_FONT}"
  fi
  if [ -z "${FONT_CAND}" ] && command -v fc-list >/dev/null 2>&1; then
    FONT_CAND="$(fc-list "${TEXT_FONT}" file | awk -F: '{print $1}' | head -n1)"
  fi
fi
# Validate chosen font; fallback to first available if invalid
validate_font() {
  local f="$1"
  [ -z "$f" ] && return 1
  local tmp="${TMP_OUT}.fontcheck"
  if "${MAGICK_BIN}" -size 1x1 xc:none -font "$f" -pointsize 10 -annotate 0 "a" png:"${tmp}" >/dev/null 2>&1; then
    rm -f "${tmp}"
    return 0
  fi
  rm -f "${tmp}"
  return 1
}

if ! validate_font "${FONT_CAND:-}"; then
  FONT_CAND=""
  if [ -d "${FONT_DIR}" ]; then
    first_font="$(find "${FONT_DIR}" -maxdepth 1 -type f -iname "*.ttf" | sort -f | head -n1 || true)"
    if validate_font "${first_font}"; then
      FONT_CAND="${first_font}"
    fi
  fi
fi

if [ -n "${FONT_CAND:-}" ]; then
  FONT_OPT=(-font "${FONT_CAND}")
fi

if [ "${IS_TRANSPARENT}" -eq 1 ]; then
  # For transparent mode: create text mask and use DstOut to cut out text shape
  # 1) Create text as white on transparent background
  "${MAGICK_BIN}" -size "${W}x${H}" xc:none \
    -gravity "${GRAVITY}" "${FONT_OPT[@]}" \
    -pointsize "${TEXT_SIZE}" \
    -fill white \
    -annotate +${OFF_X}+${OFF_Y} "${TEXT}" \
    png32:"${TMP_TEXT}"
  
  # 2) Apply DstOut composition directly (the text image already has proper alpha)
  "${MAGICK_BIN}" png32:"${TARGET}" \
    png32:"${TMP_TEXT}" \
    -gravity "${GRAVITY}" -compose DstOut -composite \
    png32:"${TMP_OUT}"
else
  # Normal mode: standard text overlay
  "${MAGICK_BIN}" png32:"${TARGET}" \
    -gravity "${GRAVITY}" "${FONT_OPT[@]}" \
    -pointsize "${TEXT_SIZE}" \
    -fill "#${TEXT_COLOR}" \
    -annotate +${OFF_X}+${OFF_Y} "${TEXT}" \
    png32:"${TMP_OUT}"
fi

mv -f "${TMP_OUT}" "${TARGET}"

if [ "${IS_TRANSPARENT}" -eq 1 ]; then
  echo "Updated ${TARGET} with text '${TEXT}' (transparent mask, align ${TEXT_ALIGN})"
else
  echo "Updated ${TARGET} with text '${TEXT}' (color #${TEXT_COLOR}, align ${TEXT_ALIGN})"
fi
