#!/usr/bin/env bash
# Description: resize/crop an image to the D200 layout and send via ulanzi_d200_demon (set-buttons-explicit-14)
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

SOCK_PATH="/tmp/ulanzi_device.sock"
SHOW_HELP="${ROOT}/show_help.sh"

if [ "$#" -ne 1 ]; then
  exec "${SHOW_HELP}" "$(basename "$0")"
fi

IMG="$1"
IMG_IN="${IMG}"
if [ ! -f "${IMG}" ] && [[ "${IMG}" != /* ]] && [ -f "${ROOT}/${IMG}" ]; then
  IMG="${ROOT}/${IMG}"
fi
if [ ! -f "${IMG}" ] && [[ "${IMG_IN}" != /* ]] && [ -f "${ROOT}/build/${IMG_IN}" ]; then
  IMG="${ROOT}/build/${IMG_IN}"
fi
if [ ! -f "${IMG}" ]; then
  echo "File not found: ${IMG_IN} (tried as-is and under ${ROOT}/)" >&2
  exit 1
fi

TMP="$(mktemp -d)"
cleanup() { rm -rf "${TMP}"; }
trap cleanup EXIT

# 1) centre-crop 16:9 puis resize 1280x720
convert "${IMG}" -gravity center -resize 1280x720^ -extent 1280x720 "${TMP}/frame.png"

gap=50
btn=196
# marges calculées pour 5 colonnes et 3 lignes (gap constant)
margin_x=$(( (1280 - (5*btn + 4*gap)) / 2 ))
margin_y=$(( (720  - (3*btn + 2*gap)) / 2 ))

read -r -a x <<< "$(for c in 0 1 2 3 4; do printf '%d ' $(( margin_x + c*(btn+gap) )); done)"
read -r -a y <<< "$(for r in 0 1 2; do printf '%d ' $(( margin_y + r*(btn+gap) )); done)"

n=1
for r in 0 1; do
  for c in 0 1 2 3 4; do
    convert "${TMP}/frame.png" -crop ${btn}x${btn}+${x[$c]}+${y[$r]} +repage "${TMP}/b${n}.png"
    n=$((n+1))
  done
done
for c in 0 1 2; do
  convert "${TMP}/frame.png" -crop ${btn}x${btn}+${x[$c]}+${y[2]} +repage "${TMP}/b${n}.png"
  n=$((n+1))
done
# bouton 14 (largeur btn+gap+btn = 442)
b14w=$((btn+gap+btn))
convert "${TMP}/frame.png" -crop ${b14w}x${btn}+${x[3]}+${y[2]} +repage "${TMP}/b14.png"

cmd="set-buttons-explicit-14"
for i in {1..13}; do
  cmd+=" --button-${i}=${TMP}/b${i}.png"
done
cmd+=" --button-14=${TMP}/b14.png"
echo "${cmd}"
resp="$(printf '%s\n' "${cmd}"" \n" | nc -U "${SOCK_PATH}" || true)"
if [ "${resp}" = "ok" ]; then
  echo "Sent page from ${IMG}"
else
  echo "send_image_page.sh failed (response: ${resp:-<no response>})" >&2
  exit 1
fi
