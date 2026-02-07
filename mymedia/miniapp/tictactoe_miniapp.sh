#!/usr/bin/env bash
set -euo pipefail

# GoofyDeck Tic-Tac-Toe miniapp
#
# - Grabs control from paging_daemon via stop-control/start-control
# - Subscribes to ulanzi_d200_daemon button events (read-buttons)
# - Renders the game board via set-buttons-explicit-14 (+ labels for scores / messages)
#
# Layout:
# - Grid (3x3): 1,2,3 / 6,7,8 / 11,12,13
# - Starter select: 5 (toggles who starts next; resets board)
# - Reset: 4
# - Score O: 9 (label)
# - Score X: 10 (label)
# - Exit: 14 (wide tile, icon only; no label support)

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"

MINIAPP_NAME="tictactoe"

ULANZI_SOCK="${ULANZI_SOCK:-/tmp/ulanzi_device.sock}"
PAGING_CTRL_SOCK="${PAGING_CTRL_SOCK:-/tmp/goofydeck_paging_control.sock}"

SOUND_DIR="${ROOT}/mymedia/miniapp/.assets/tictactoe"
SOUND_X="${SOUND_DIR}/x_stone.wav"
SOUND_O="${SOUND_DIR}/o_drop.wav"
SOUND_WIN="${SOUND_DIR}/win_tadaa.wav"
SOUND_RESET="${SOUND_DIR}/reset.wav"

# Quick knob
SOUND_ENABLE=1

DISK_CACHE_DIR="${ROOT}/mymedia/miniapp/.cache/${MINIAPP_NAME}"
PREGEN_DIR="${DISK_CACHE_DIR}/pregen"

RAM_BASE="/dev/shm/goofydeck/${MINIAPP_NAME}"
RAM_DIR="${RAM_BASE}/$$"

ICON_SIZE=196
MARK_SIZE=150

mkdir -p "${PREGEN_DIR}" "${RAM_DIR}"

cleanup() {
  rm -rf "${RAM_DIR}" 2>/dev/null || true
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

sound_player_init() {
  SOUND_PLAYER=()
  if [ "${SOUND_ENABLE}" -ne 1 ]; then
    return 0
  fi
  if command -v aplay >/dev/null 2>&1; then
    SOUND_PLAYER=(aplay -q)
    return 0
  fi
  if command -v paplay >/dev/null 2>&1; then
    SOUND_PLAYER=(paplay)
    return 0
  fi
  if command -v ffplay >/dev/null 2>&1; then
    SOUND_PLAYER=(ffplay -nodisp -autoexit -loglevel error)
    return 0
  fi
  SOUND_ENABLE=0
}

play_sound_file() {
  [ "${SOUND_ENABLE}" -eq 1 ] || return 0
  [ "${#SOUND_PLAYER[@]}" -gt 0 ] || return 0
  local f="$1"
  [ -f "${f}" ] || return 0
  "${SOUND_PLAYER[@]}" "${f}" >/dev/null 2>&1 &
}

abs_path() {
  if command -v realpath >/dev/null 2>&1; then
    realpath -m "$1"
  else
    if [[ "$1" == /* ]]; then printf "%s\n" "$1"; else printf "%s/%s\n" "$(pwd -P)" "$1"; fi
  fi
}

parse_btn_evt() {
  # input: line like "button 5 TAP" (or HOLD/LONGHOLD/RELEASED)
  # output: prints "BTN EVT" (e.g. "5 TAP") and returns 0, else returns 1
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
  local cell_empty="${PREGEN_DIR}/cell_empty.png"
  local cell_x="${PREGEN_DIR}/cell_x.png"
  local cell_o="${PREGEN_DIR}/cell_o.png"
  local label_bg="${PREGEN_DIR}/label_bg.png"
  local btn_app="${PREGEN_DIR}/btn_app.png"
  local btn_reset="${PREGEN_DIR}/btn_reset.png"
  local exit14="${PREGEN_DIR}/exit14.png"

  if [ ! -f "${cell_empty}" ]; then
    "${ROOT}/icons/draw_square" 111111 --size="${ICON_SIZE}" "${cell_empty}" >/dev/null
    "${ROOT}/icons/draw_optimize" -c=8 "${cell_empty}" >/dev/null 2>&1 || true
  fi
  if [ ! -f "${cell_x}" ]; then
    cp -f "${cell_empty}" "${cell_x}"
    "${ROOT}/icons/draw_mdi" mdi:close 1E90FF --size="${MARK_SIZE}" "${cell_x}" >/dev/null
    "${ROOT}/icons/draw_optimize" -c=16 "${cell_x}" >/dev/null 2>&1 || true
  fi
  if [ ! -f "${cell_o}" ]; then
    cp -f "${cell_empty}" "${cell_o}"
    "${ROOT}/icons/draw_mdi" mdi:circle-outline FF3333 --size="${MARK_SIZE}" "${cell_o}" >/dev/null
    "${ROOT}/icons/draw_optimize" -c=16 "${cell_o}" >/dev/null 2>&1 || true
  fi
  if [ ! -f "${label_bg}" ]; then
    "${ROOT}/icons/draw_square" transparent --size="${ICON_SIZE}" "${label_bg}" >/dev/null
    "${ROOT}/icons/draw_optimize" -c=8 "${label_bg}" >/dev/null 2>&1 || true
  fi
  if [ ! -f "${btn_app}" ]; then
    "${ROOT}/icons/draw_square" transparent --size="${ICON_SIZE}" "${btn_app}" >/dev/null
    "${ROOT}/icons/draw_mdi" mdi:gamepad-variant-outline C0C0C0 --size=140 "${btn_app}" >/dev/null 2>&1 || true
    "${ROOT}/icons/draw_optimize" -c=16 "${btn_app}" >/dev/null 2>&1 || true
  fi
  if [ ! -f "${btn_reset}" ]; then
    "${ROOT}/icons/draw_square" transparent --size="${ICON_SIZE}" "${btn_reset}" >/dev/null
    "${ROOT}/icons/draw_mdi" mdi:restart FFFFFF --size=160 "${btn_reset}" >/dev/null
    "${ROOT}/icons/draw_optimize" -c=16 "${btn_reset}" >/dev/null 2>&1 || true
  fi

  # Button 14 is a wide tile (442x196). Always (re)generate to ensure it matches the latest tools.
  rm -f "${exit14}" 2>/dev/null || true
  "${ROOT}/icons/draw_rectangle" transparent --size="${ICON_SIZE}" "${exit14}" >/dev/null
  "${ROOT}/icons/draw_mdi_rectangle" mdi:exit-to-app C0C0C0 --size=56 --offset=183,58 "${exit14}" >/dev/null 2>&1 || true
  "${ROOT}/icons/draw_optimize" -c=32 "${exit14}" >/dev/null 2>&1 || true
}

load_pregen_to_ram() {
  mkdir -p "${RAM_DIR}/pregen"
  cp -f "${PREGEN_DIR}/cell_empty.png" "${RAM_DIR}/pregen/cell_empty.png"
  cp -f "${PREGEN_DIR}/cell_x.png" "${RAM_DIR}/pregen/cell_x.png"
  cp -f "${PREGEN_DIR}/cell_o.png" "${RAM_DIR}/pregen/cell_o.png"
  cp -f "${PREGEN_DIR}/label_bg.png" "${RAM_DIR}/pregen/label_bg.png"
  cp -f "${PREGEN_DIR}/btn_app.png" "${RAM_DIR}/pregen/btn_app.png"
  cp -f "${PREGEN_DIR}/btn_reset.png" "${RAM_DIR}/pregen/btn_reset.png"
  cp -f "${PREGEN_DIR}/exit14.png" "${RAM_DIR}/pregen/exit14.png"
}

board_reset() {
  for i in $(seq 0 8); do board[$i]=0; done
  cur_player="${start_player}" # 1=X, 2=O
  msg_label=""
}

board_winner() {
  # prints: 0 (none), 1 (X), 2 (O), 3 (draw)
  local b=("${board[@]}")
  local lines=(
    "0 1 2" "3 4 5" "6 7 8"
    "0 3 6" "1 4 7" "2 5 8"
    "0 4 8" "2 4 6"
  )
  for ln in "${lines[@]}"; do
    local a c d
    a="$(printf "%s" "${ln}" | awk '{print $1}')"
    c="$(printf "%s" "${ln}" | awk '{print $2}')"
    d="$(printf "%s" "${ln}" | awk '{print $3}')"
    if [ "${b[$a]}" -ne 0 ] && [ "${b[$a]}" -eq "${b[$c]}" ] && [ "${b[$a]}" -eq "${b[$d]}" ]; then
      printf "%s\n" "${b[$a]}"
      return 0
    fi
  done
  for v in "${b[@]}"; do
    if [ "${v}" -eq 0 ]; then
      printf "0\n"
      return 0
    fi
  done
  printf "3\n"
}

idx_from_btn() {
  local btn="$1"
  case "${btn}" in
    1) echo 0 ;;
    2) echo 1 ;;
    3) echo 2 ;;
    6) echo 3 ;;
    7) echo 4 ;;
    8) echo 5 ;;
    11) echo 6 ;;
    12) echo 7 ;;
    13) echo 8 ;;
    *) echo -1 ;;
  esac
}

cell_icon_for_value() {
  local v="$1"
  if [ "${v}" -eq 1 ]; then echo "${RAM_DIR}/pregen/cell_x.png"
  elif [ "${v}" -eq 2 ]; then echo "${RAM_DIR}/pregen/cell_o.png"
  else echo "${RAM_DIR}/pregen/cell_empty.png"
  fi
}

render() {
  local empty="${ROOT}/assets/pregen/empty.png"
  local label_bg="${RAM_DIR}/pregen/label_bg.png"
  local btn_app="${RAM_DIR}/pregen/btn_app.png"
  local btn_reset="${RAM_DIR}/pregen/btn_reset.png"
  local exit14="${RAM_DIR}/pregen/exit14.png"
  local starter_label="X1ST"
  if [ "${start_player}" -eq 2 ]; then starter_label="O1ST"; fi

  local b1 b2 b3 b4 b5 b6 b7 b8 b9 b10 b11 b12 b13 b14
  b1="$(cell_icon_for_value "${board[0]}")"
  b2="$(cell_icon_for_value "${board[1]}")"
  b3="$(cell_icon_for_value "${board[2]}")"
  b4="${btn_reset}"
  b5="${btn_app}"
  b6="$(cell_icon_for_value "${board[3]}")"
  b7="$(cell_icon_for_value "${board[4]}")"
  b8="$(cell_icon_for_value "${board[5]}")"
  b9="${label_bg}"
  b10="${label_bg}"
  b11="$(cell_icon_for_value "${board[6]}")"
  b12="$(cell_icon_for_value "${board[7]}")"
  b13="$(cell_icon_for_value "${board[8]}")"
  b14="${exit14}"

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
  cmd+=" --label-5=${starter_label}"
  cmd+=" --label-9=O:${score_o}"
  cmd+=" --label-10=X:${score_x}"
  if [ -n "${msg_label}" ]; then
    cmd+=" --label-7=${msg_label}"
  else
    cmd+=" --label-7="
  fi
  ul_cmd "${cmd}" >/dev/null
}

main() {
  [ -S "${ULANZI_SOCK}" ] || { echo "Missing ulanzi socket: ${ULANZI_SOCK}" >&2; exit 1; }

  sound_player_init

  ensure_pregen
  load_pregen_to_ram

  pg_cmd "stop-control"

  # Button stream (persistent)
  coproc RB { socat - UNIX-CONNECT:"${ULANZI_SOCK}"; }
  printf 'read-buttons\n' >&"${RB[1]}"
  read -r _okline <&"${RB[0]}" || true

  score_x=0
  score_o=0
  msg_label=""
  declare -a board
  start_player=1
  cur_player=1
  board_reset
  render

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
    elif [ "${btn}" -eq 4 ]; then
      play_sound_file "${SOUND_RESET}"
      board_reset
      render
      continue
    elif [ "${btn}" -eq 5 ]; then
      if [ "${start_player}" -eq 1 ]; then start_player=2; else start_player=1; fi
      play_sound_file "${SOUND_RESET}"
      board_reset
      render
      continue
    fi

    local idx
    idx="$(idx_from_btn "${btn}")"
    if [ "${idx}" -lt 0 ]; then
      continue
    fi
    if [ "${board[${idx}]}" -ne 0 ]; then
      continue
    fi

    if [ "${cur_player}" -eq 1 ]; then
      play_sound_file "${SOUND_X}"
    else
      play_sound_file "${SOUND_O}"
    fi
    board[${idx}]="${cur_player}"
    if [ "${cur_player}" -eq 1 ]; then cur_player=2; else cur_player=1; fi

    local w
    w="$(board_winner)"
    if [ "${w}" -eq 0 ]; then
      msg_label=""
      render
      continue
    fi

    if [ "${w}" -eq 1 ]; then
      score_x=$((score_x + 1))
      msg_label="XWIN"
      play_sound_file "${SOUND_WIN}"
    elif [ "${w}" -eq 2 ]; then
      score_o=$((score_o + 1))
      msg_label="OWIN"
      play_sound_file "${SOUND_WIN}"
    else
      msg_label="DRAW"
    fi
    render
    sleep 0.8

    board_reset
    render
  done
}

main "$@"
