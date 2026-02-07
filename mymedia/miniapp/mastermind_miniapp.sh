#!/usr/bin/env bash
set -euo pipefail

# GoofyDeck "Mastermind" miniapp (5 pegs, 5 colors, duplicates allowed)
#
# - Takes control via paging_daemon stop-control/start-control/load-last-page
# - Reads buttons from ulanzi_d200_daemon (read-buttons)
# - Renders UI via set-buttons-explicit-14 (icons) + labels
#
# Layout (per user request):
# - Guess slots (5): 1,2,3,4,5
# - Palette (5 colors): 6,7,8,9,10
# - Score: 11
# - Reset: 12
# - Submit: 13 (status / feedback label)
# - Exit:  14 (wide tile, icon only; no label support)
#
# Controls:
# - TAP a palette color to select it (and auto-fill next empty slot if any).
# - TAP a slot to set it to the currently selected color.
# - TAP Submit (13) to validate when 5 slots are filled.

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"

MINIAPP_NAME="mastermind"

ULANZI_SOCK="${ULANZI_SOCK:-/tmp/ulanzi_device.sock}"
PAGING_CTRL_SOCK="${PAGING_CTRL_SOCK:-/tmp/goofydeck_paging_control.sock}"

DISK_CACHE_DIR="${ROOT}/mymedia/miniapp/.cache/${MINIAPP_NAME}"
PREGEN_DIR="${DISK_CACHE_DIR}/pregen"

RAM_BASE="/dev/shm/goofydeck/${MINIAPP_NAME}"
RAM_DIR="${RAM_BASE}/$$"

ICON_SIZE=196

mkdir -p "${PREGEN_DIR}" "${RAM_DIR}/pregen" "${RAM_DIR}/tmp"

cleanup() {
  rm -rf "${RAM_DIR}" 2>/dev/null || true
  if [ -S "${ULANZI_SOCK}" ]; then
    # Restore default label style (best effort)
    printf 'set-label-style %s\n' "$(abs_path "${ROOT}/assets/json/default.json")" | nc -w 1 -U "${ULANZI_SOCK}" >/dev/null 2>&1 || true
  fi
  if [ -S "${PAGING_CTRL_SOCK}" ]; then
    printf 'start-control\n' | nc -w 1 -U "${PAGING_CTRL_SOCK}" >/dev/null 2>&1 || true
    sleep 0.05
    printf 'load-last-page\n' | nc -w 1 -U "${PAGING_CTRL_SOCK}" >/dev/null 2>&1 || true
  fi
}
trap cleanup EXIT INT TERM

pg_cmd() {
  local cmd="$1"
  [ -S "${PAGING_CTRL_SOCK}" ] || return 0
  printf '%s\n' "${cmd}" | nc -w 1 -U "${PAGING_CTRL_SOCK}" >/dev/null 2>&1 || true
}

ul_cmd() {
  local cmd="$1"
  printf '%s\n' "${cmd}" | nc -w 1 -U "${ULANZI_SOCK}" 2>/dev/null || true
}

abs_path() {
  if command -v realpath >/dev/null 2>&1; then
    realpath -m "$1"
  else
    if [[ "$1" == /* ]]; then printf "%s\n" "$1"; else printf "%s/%s\n" "$(pwd -P)" "$1"; fi
  fi
}

parse_btn_evt() {
  local line="$1"
  local btn evt
  btn="$(printf "%s" "${line}" | awk '{print $2}')"
  evt="$(printf "%s" "${line}" | awk '{print $3}')"
  if [[ "${btn}" =~ ^[0-9]+$ ]] && [[ "${evt}" =~ ^(TAP|HOLD|LONGHOLD|RELEASED)$ ]]; then
    printf "%s %s\n" "${btn}" "${evt}"
    return 0
  fi
  return 1
}

ensure_pregen() {
  local empty="${PREGEN_DIR}/slot_empty.png"
  local reset="${PREGEN_DIR}/reset.png"
  local score="${PREGEN_DIR}/score.png"
  local submit="${PREGEN_DIR}/submit.png"
  local exit14="${PREGEN_DIR}/exit14.png"
  local label_style="${PREGEN_DIR}/label_style.json"

  # palette colors (5): R, G, B, Y, P
  local c0="${PREGEN_DIR}/c0.png"
  local c1="${PREGEN_DIR}/c1.png"
  local c2="${PREGEN_DIR}/c2.png"
  local c3="${PREGEN_DIR}/c3.png"
  local c4="${PREGEN_DIR}/c4.png"

  if [ ! -f "${empty}" ]; then
    "${ROOT}/icons/draw_square" 111111 --size="${ICON_SIZE}" "${empty}" >/dev/null
    "${ROOT}/icons/draw_optimize" -c=32 "${empty}" >/dev/null 2>&1 || true
  fi

  if [ ! -f "${c0}" ]; then "${ROOT}/icons/draw_square" FF1744 --size="${ICON_SIZE}" "${c0}" >/dev/null; "${ROOT}/icons/draw_optimize" -c=64 "${c0}" >/dev/null 2>&1 || true; fi
  if [ ! -f "${c1}" ]; then "${ROOT}/icons/draw_square" 00FF4C --size="${ICON_SIZE}" "${c1}" >/dev/null; "${ROOT}/icons/draw_optimize" -c=64 "${c1}" >/dev/null 2>&1 || true; fi
  if [ ! -f "${c2}" ]; then "${ROOT}/icons/draw_square" 2196F3 --size="${ICON_SIZE}" "${c2}" >/dev/null; "${ROOT}/icons/draw_optimize" -c=64 "${c2}" >/dev/null 2>&1 || true; fi
  if [ ! -f "${c3}" ]; then "${ROOT}/icons/draw_square" FFD700 --size="${ICON_SIZE}" "${c3}" >/dev/null; "${ROOT}/icons/draw_optimize" -c=64 "${c3}" >/dev/null 2>&1 || true; fi
  if [ ! -f "${c4}" ]; then "${ROOT}/icons/draw_square" B000FF --size="${ICON_SIZE}" "${c4}" >/dev/null; "${ROOT}/icons/draw_optimize" -c=64 "${c4}" >/dev/null 2>&1 || true; fi

  if [ ! -f "${reset}" ]; then
    "${ROOT}/icons/draw_square" transparent --size="${ICON_SIZE}" "${reset}" >/dev/null
    "${ROOT}/icons/draw_mdi" mdi:restart FFFFFF --size=160 "${reset}" >/dev/null
    "${ROOT}/icons/draw_optimize" -c=32 "${reset}" >/dev/null 2>&1 || true
  fi
  if [ ! -f "${score}" ]; then
    "${ROOT}/icons/draw_square" transparent --size="${ICON_SIZE}" "${score}" >/dev/null
    "${ROOT}/icons/draw_mdi" mdi:trophy-outline C0C0C0 --size=120 "${score}" >/dev/null 2>&1 || true
    "${ROOT}/icons/draw_optimize" -c=32 "${score}" >/dev/null 2>&1 || true
  fi
  if [ ! -f "${submit}" ]; then
    "${ROOT}/icons/draw_square" transparent --size="${ICON_SIZE}" "${submit}" >/dev/null
    "${ROOT}/icons/draw_mdi" mdi:check-bold FFFFFF --size=160 "${submit}" >/dev/null
    "${ROOT}/icons/draw_optimize" -c=32 "${submit}" >/dev/null 2>&1 || true
  fi
  # Button 14 is a wide tile (442x196). Always (re)generate to ensure it matches the latest tools.
  rm -f "${exit14}" 2>/dev/null || true
  "${ROOT}/icons/draw_rectangle" transparent --size="${ICON_SIZE}" "${exit14}" >/dev/null
  # Small exit icon in bottom-right corner (tap on button 14 exits).
  # draw_mdi places at: (tw-size)/2 + off_x, so choose offsets to land at bottom-right with a margin.
  "${ROOT}/icons/draw_mdi_rectangle" mdi:exit-to-app C0C0C0 --size=56 --offset=183,58 "${exit14}" >/dev/null 2>&1 || true
  "${ROOT}/icons/draw_optimize" -c=32 "${exit14}" >/dev/null 2>&1 || true

  if [ ! -f "${label_style}" ]; then
    cat >"${label_style}" <<'EOF'
{
  "Align": "center",
  "Color": 16777215,
  "FontName": "Roboto",
  "ShowTitle": true,
  "Size": 22,
  "Weight": 80
}
EOF
  fi
}

load_pregen_to_ram() {
  cp -f "${PREGEN_DIR}/slot_empty.png" "${RAM_DIR}/pregen/slot_empty.png"
  cp -f "${PREGEN_DIR}/reset.png" "${RAM_DIR}/pregen/reset.png"
  cp -f "${PREGEN_DIR}/score.png" "${RAM_DIR}/pregen/score.png"
  cp -f "${PREGEN_DIR}/submit.png" "${RAM_DIR}/pregen/submit.png"
  cp -f "${PREGEN_DIR}/exit14.png" "${RAM_DIR}/pregen/exit14.png"
  cp -f "${PREGEN_DIR}/c0.png" "${RAM_DIR}/pregen/c0.png"
  cp -f "${PREGEN_DIR}/c1.png" "${RAM_DIR}/pregen/c1.png"
  cp -f "${PREGEN_DIR}/c2.png" "${RAM_DIR}/pregen/c2.png"
  cp -f "${PREGEN_DIR}/c3.png" "${RAM_DIR}/pregen/c3.png"
  cp -f "${PREGEN_DIR}/c4.png" "${RAM_DIR}/pregen/c4.png"
}

icon_for_color() {
  local c="$1"
  printf "%s\n" "${RAM_DIR}/pregen/c${c}.png"
}

slot_btns() { echo "1 2 3 4 5"; }
palette_btns() { echo "6 7 8 9 10"; }

slot_idx_for_btn() {
  local btn="$1"
  case "${btn}" in
    1) echo 0 ;;
    2) echo 1 ;;
    3) echo 2 ;;
    4) echo 3 ;;
    5) echo 4 ;;
    *) echo -1 ;;
  esac
}

color_for_palette_btn() {
  local btn="$1"
  case "${btn}" in
    6) echo 0 ;;
    7) echo 1 ;;
    8) echo 2 ;;
    9) echo 3 ;;
    10) echo 4 ;;
    *) echo -1 ;;
  esac
}

next_empty_slot() {
  local i=0
  while [ "${i}" -lt 5 ]; do
    if [ "${guess[$i]}" -lt 0 ]; then echo "${i}"; return 0; fi
    i=$((i+1))
  done
  echo -1
}

guess_complete() {
  local i=0
  while [ "${i}" -lt 5 ]; do
    if [ "${guess[$i]}" -lt 0 ]; then return 1; fi
    i=$((i+1))
  done
  return 0
}

new_secret() {
  local i=0
  while [ "${i}" -lt 5 ]; do
    secret[$i]=$((RANDOM % 5))
    i=$((i+1))
  done
}

reset_round() {
  for i in 0 1 2 3 4; do guess[$i]=-1; done
  selected=0
  tries=0
  status="READY"
  feedback=""
  new_secret
}

score_guess() {
  # sets globals: black, white
  local s_used=(0 0 0 0 0)
  local g_used=(0 0 0 0 0)
  black=0
  white=0

  # black pegs
  for i in 0 1 2 3 4; do
    if [ "${guess[$i]}" -eq "${secret[$i]}" ]; then
      black=$((black + 1))
      s_used[$i]=1
      g_used[$i]=1
    fi
  done

  # white pegs
  for gi in 0 1 2 3 4; do
    [ "${g_used[$gi]}" -eq 1 ] && continue
    for si in 0 1 2 3 4; do
      [ "${s_used[$si]}" -eq 1 ] && continue
      if [ "${guess[$gi]}" -eq "${secret[$si]}" ]; then
        white=$((white + 1))
        s_used[$si]=1
        break
      fi
    done
  done
}

render_ui() {
  local slot_empty="${RAM_DIR}/pregen/slot_empty.png"
  local reset="${RAM_DIR}/pregen/reset.png"
  local score="${RAM_DIR}/pregen/score.png"
  local submit="${RAM_DIR}/pregen/submit.png"
  local exit14="${RAM_DIR}/pregen/exit14.png"
  local score_label="W${wins}L${losses}"

  local b1 b2 b3 b4 b5 b6 b7 b8 b9 b10 b11 b12 b13 b14
  # guess slots
  b1="${slot_empty}" ; b2="${slot_empty}" ; b3="${slot_empty}" ; b4="${slot_empty}" ; b5="${slot_empty}"
  # palette
  b6="$(icon_for_color 0)" ; b7="$(icon_for_color 1)" ; b8="$(icon_for_color 2)" ; b9="$(icon_for_color 3)" ; b10="$(icon_for_color 4)"
  # score/reset/submit/exit14
  b11="${score}" ; b12="${reset}" ; b13="${submit}" ; b14="${exit14}"

  # apply guess icons
  for i in 0 1 2 3 4; do
    local v="${guess[$i]}"
    local icon="${slot_empty}"
    if [ "${v}" -ge 0 ]; then icon="$(icon_for_color "${v}")"; fi
    case "${i}" in
      0) b1="${icon}" ;;
      1) b2="${icon}" ;;
      2) b3="${icon}" ;;
      3) b4="${icon}" ;;
      4) b5="${icon}" ;;
    esac
  done

  local cmd="set-buttons-explicit-14"
  cmd+=" --button-1=$(abs_path "${b1}")"
  cmd+=" --button-2=$(abs_path "${b2}")"
  cmd+=" --button-3=$(abs_path "${b3}")"
  cmd+=" --button-4=$(abs_path "${b4}")"
  cmd+=" --button-5=$(abs_path "${b5}")"
  cmd+=" --button-6=$(abs_path "${b6}")"
  cmd+=" --button-7=$(abs_path "${b7}")"
  cmd+=" --button-8=$(abs_path "${b8}")"
  cmd+=" --button-9=$(abs_path "${b9}")"
  cmd+=" --button-10=$(abs_path "${b10}")"
  cmd+=" --button-11=$(abs_path "${b11}")"
  cmd+=" --button-12=$(abs_path "${b12}")"
  cmd+=" --button-13=$(abs_path "${b13}")"
  cmd+=" --button-14=$(abs_path "${b14}")"

  # labels (no spaces)
  cmd+=" --label-11=${score_label}"
  cmd+=" --label-12=RST"
  # palette selection marker
  cmd+=" --label-6="; cmd+=" --label-7="; cmd+=" --label-8="; cmd+=" --label-9="; cmd+=" --label-10="
  case "${selected}" in
    0) cmd+=" --label-6=●" ;;
    1) cmd+=" --label-7=●" ;;
    2) cmd+=" --label-8=●" ;;
    3) cmd+=" --label-9=●" ;;
    4) cmd+=" --label-10=●" ;;
  esac

  # Submit/status line
  if [ -n "${feedback}" ]; then
    cmd+=" --label-13=${feedback}"
  else
    cmd+=" --label-13=${status}"
  fi

  ul_cmd "${cmd}" >/dev/null
}

main() {
  [ -S "${ULANZI_SOCK}" ] || { echo "Missing ulanzi socket: ${ULANZI_SOCK}" >&2; exit 1; }

  ensure_pregen
  load_pregen_to_ram

  # Bigger/centered labels (best effort)
  ul_cmd "set-label-style $(abs_path "${PREGEN_DIR}/label_style.json")" >/dev/null 2>&1 || true

  pg_cmd "stop-control"

  declare -a secret guess
  selected=0
  tries=0
  wins=0
  losses=0
  status=""
  feedback=""
  reset_round
  render_ui

  # Button stream (persistent)
  coproc RB { socat - UNIX-CONNECT:"${ULANZI_SOCK}"; }
  printf 'read-buttons\n' >&"${RB[1]}"
  read -r _okline <&"${RB[0]}" || true
  # drain buffered events (ignore launch tap)
  while read -r -t 0.15 _line <&"${RB[0]}"; do :; done

  while true; do
    local line=""
    if ! read -r line <&"${RB[0]}"; then
      sleep 0.05
      continue
    fi
    local parsed btn evt
    if ! parsed="$(parse_btn_evt "${line}")"; then
      continue
    fi
    btn="$(printf "%s" "${parsed}" | awk '{print $1}')"
    evt="$(printf "%s" "${parsed}" | awk '{print $2}')"
    [ "${evt}" = "TAP" ] || continue

    if [ "${btn}" -eq 14 ]; then
      return 0
    fi
    if [ "${btn}" -eq 12 ]; then
      reset_round
      render_ui
      continue
    fi

    # palette selection
    local c
    c="$(color_for_palette_btn "${btn}")"
    if [ "${c}" -ge 0 ]; then
      selected="${c}"
      # auto-fill next empty slot if available
      local ns
      ns="$(next_empty_slot)"
      if [ "${ns}" -ge 0 ]; then
        guess[$ns]="${selected}"
      fi
      status="PICK"
      render_ui
      continue
    fi

    # slot set
    local si
    si="$(slot_idx_for_btn "${btn}")"
    if [ "${si}" -ge 0 ]; then
      guess[$si]="${selected}"
      status="PICK"
      render_ui
      continue
    fi

    # submit
    if [ "${btn}" -eq 13 ]; then
      if ! guess_complete; then
        status="FILL"
        feedback=""
        render_ui
        continue
      fi
      score_guess
      tries=$((tries + 1))
      feedback="${black}B${white}W"
      if [ "${black}" -eq 5 ]; then
        wins=$((wins + 1))
        status="WIN"
        render_ui
        sleep 1.5
        reset_round
        render_ui
        continue
      fi
      if [ "${tries}" -ge 10 ]; then
        losses=$((losses + 1))
        status="LOSE"
        render_ui
        sleep 2.5
        reset_round
        render_ui
        continue
      fi
      # next attempt
      for i in 0 1 2 3 4; do guess[$i]=-1; done
      status="NEXT"
      render_ui
      continue
    fi
  done
}

main "$@"
