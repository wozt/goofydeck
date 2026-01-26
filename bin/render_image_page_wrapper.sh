#!/usr/bin/env bash
# Description: render an image into a 14-tile folder using send_image_page (--no-send + -k auto)
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

SEND_BIN="${ROOT}/bin/send_image_page"
SHOW_HELP="${ROOT}/show_help.sh"

usage() {
  cat >&2 <<EOF
Usage: ./bin/render_image_page_wrapper.sh [send_image_page options...] <image.png>

This wrapper forces:
  --no-send
  -k=<folder>=<prefix>

Where:
  folder = <image path without .png>
  prefix = <basename without .png>

Example:
  ./bin/render_image_page_wrapper.sh -q=80 mymedia/wallpapers/valley.png
  # renders into: mymedia/wallpapers/valley/ (tiles: valley-1.png..valley-14.png)
EOF
}

if [ "$#" -lt 1 ]; then
  usage
  exit 2
fi

orig=("$@")
img=""

for ((i=${#orig[@]}-1; i>=0; i--)); do
  a="${orig[$i]}"
  if [[ "${a}" != -* ]] && [[ "${a}" == *.png ]]; then
    img="${a}"
    unset 'orig[i]'
    break
  fi
done

if [ -z "${img}" ]; then
  # fallback: if the last arg is non-option, treat as image even if not .png (send_image_page will error)
  last="${orig[-1]:-}"
  if [ -n "${last}" ] && [[ "${last}" != -* ]]; then
    img="${last}"
    unset 'orig[-1]'
  fi
fi

args=()
skip_next=0
for a in "${orig[@]}"; do
  [ -z "${a}" ] && continue
  if [ "${skip_next}" -eq 1 ]; then
    skip_next=0
    continue
  fi
  case "${a}" in
    -h|--help)
      usage
      exit 0
      ;;
    --no-send)
      # wrapper enforces it
      ;;
    -k|--keep-icons)
      # wrapper enforces its own -k; skip value too
      skip_next=1
      ;;
    -k=*|--keep-icons=*)
      # wrapper enforces its own -k
      ;;
    *)
      args+=("${a}")
      ;;
  esac
done

if [ -z "${img}" ]; then
  usage
  exit 2
fi

# Resolve image path like other scripts: allow relative to repo root.
img_in="${img}"
if [ ! -f "${img}" ] && [[ "${img}" != /* ]] && [ -f "${ROOT}/${img}" ]; then
  img="${ROOT}/${img}"
fi
if [ ! -f "${img}" ]; then
  echo "File not found: ${img_in}" >&2
  exit 1
fi

base="$(basename -- "${img}")"
dir="$(dirname -- "${img}")"

case "${base}" in
  *.png) ;;
  *)
    echo "Expected a .png file: ${img_in}" >&2
    exit 1
    ;;
esac

name="${base%.png}"
out_dir="${dir}/${name}"

exec "${SEND_BIN}" --no-send --no-tile-optimize "-k=${out_dir}=${name}" "${args[@]}" "${img}"
