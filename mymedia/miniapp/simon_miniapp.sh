#!/usr/bin/env bash
set -euo pipefail

# GoofyDeck "Simon Says" miniapp
#
# - Takes control via paging_daemon stop-control/start-control/load-last-page
# - Reads buttons from ulanzi_d200_daemon (read-buttons)
# - Renders UI via set-buttons-explicit-14 + partial updates for flashes
#
# Layout:
# - Pads (2x2): 1=RED, 2=GREEN, 6=BLUE, 7=YELLOW
# - Reset: 4
# - Life (repeat sequence once): 5
# - Score: 9 (label)
# - Best:  10 (label)
# - Message: 12 (label: READY/PLAY/FAIL)
# - Exit: 14 (wide tile, icon only; no label support)

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"

MINIAPP_NAME="simon"

ULANZI_SOCK="${ULANZI_SOCK:-/tmp/ulanzi_device.sock}"
PAGING_CTRL_SOCK="${PAGING_CTRL_SOCK:-/tmp/goofydeck_paging_control.sock}"

SOUND_DIR="${ROOT}/mymedia/miniapp/.assets/simon"
SOUND_RED="${SOUND_DIR}/red.wav"
SOUND_GREEN="${SOUND_DIR}/green.wav"
SOUND_BLUE="${SOUND_DIR}/blue.wav"
SOUND_YELLOW="${SOUND_DIR}/yellow.wav"
SOUND_FAIL="${SOUND_DIR}/fail.wav"

# Quick knobs
SOUND_ENABLE=1

DISK_CACHE_DIR="${ROOT}/mymedia/miniapp/.cache/${MINIAPP_NAME}"
PREGEN_DIR="${DISK_CACHE_DIR}/pregen"

RAM_BASE="/dev/shm/goofydeck/${MINIAPP_NAME}"
RAM_DIR="${RAM_BASE}/$$"

ICON_SIZE=196

# Timings (ms)
FLASH_ON_MS=260
FLASH_OFF_MS=140

mkdir -p "${PREGEN_DIR}" "${RAM_DIR}/pregen"

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

play_pad_sound() {
  # id: 0=red,1=green,2=blue,3=yellow
  [ "${SOUND_ENABLE}" -eq 1 ] || return 0
  [ "${#SOUND_PLAYER[@]}" -gt 0 ] || return 0
  local id="$1"
  local f=""
  case "${id}" in
    0) f="${SOUND_RED}" ;;
    1) f="${SOUND_GREEN}" ;;
    2) f="${SOUND_BLUE}" ;;
    3) f="${SOUND_YELLOW}" ;;
    *) return 0 ;;
  esac
  [ -f "${f}" ] || return 0
  "${SOUND_PLAYER[@]}" "${f}" >/dev/null 2>&1 &
}

play_fail_sound() {
  [ "${SOUND_ENABLE}" -eq 1 ] || return 0
  [ "${#SOUND_PLAYER[@]}" -gt 0 ] || return 0
  [ -f "${SOUND_FAIL}" ] || return 0
  "${SOUND_PLAYER[@]}" "${SOUND_FAIL}" >/dev/null 2>&1 &
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

drain_rb_events() {
  # Best-effort drain buffered button events for a short duration.
  # This avoids consuming the TAP that launched the miniapp as an in-app action.
  local seconds="$1"
  local end
  end="$(awk -v s="$seconds" 'BEGIN{printf "%.3f", s}')"
  local start_ns now_ns
  start_ns="$(date +%s%N)"
  while true; do
    now_ns="$(date +%s%N)"
    if awk -v a="$start_ns" -v b="$now_ns" -v s="$seconds" 'BEGIN{exit ((b-a)/1e9 >= s) ? 0 : 1}'; then
      break
    fi
    local _line=""
    if ! read -r -t 0.05 _line; then
      continue
    fi
  done
}

wait_ms_drain() {
  # Wait while draining any queued button events (ignore input).
  # Requires RB_READ_FD to be set.
  local ms="$1"
  if [ -z "${RB_READ_FD:-}" ]; then
    sleep "$(awk -v ms="$ms" 'BEGIN{printf "%.3f\n", ms/1000.0}')"
    return 0
  fi

  local start_ns now_ns
  start_ns="$(date +%s%N)"
  while true; do
    now_ns="$(date +%s%N)"
    if awk -v a="$start_ns" -v b="$now_ns" -v ms="$ms" 'BEGIN{exit ((b-a)/1e6 >= ms) ? 0 : 1}'; then
      break
    fi
    local _line=""
    read -r -t 0.05 _line <&"${RB_READ_FD}" || true
  done
}

ensure_pregen() {
  # dim / lit variants
  local r0="${PREGEN_DIR}/pad_red_dim.png"
  local r1="${PREGEN_DIR}/pad_red_lit.png"
  local g0="${PREGEN_DIR}/pad_green_dim.png"
  local g1="${PREGEN_DIR}/pad_green_lit.png"
  local b0="${PREGEN_DIR}/pad_blue_dim.png"
  local b1="${PREGEN_DIR}/pad_blue_lit.png"
  local y0="${PREGEN_DIR}/pad_yellow_dim.png"
  local y1="${PREGEN_DIR}/pad_yellow_lit.png"
  local reset="${PREGEN_DIR}/reset.png"
  local life_on="${PREGEN_DIR}/life_on.png"
  local life_off="${PREGEN_DIR}/life_off.png"
  local stat="${PREGEN_DIR}/stat.png"
  local exit14="${PREGEN_DIR}/exit14.png"

  if [ ! -f "${r0}" ]; then "${ROOT}/icons/draw_square" 6B0016 --size="${ICON_SIZE}" "${r0}" >/dev/null; "${ROOT}/icons/draw_optimize" -c=64 "${r0}" >/dev/null 2>&1 || true; fi
  if [ ! -f "${r1}" ]; then "${ROOT}/icons/draw_square" FF1744 --size="${ICON_SIZE}" "${r1}" >/dev/null; "${ROOT}/icons/draw_optimize" -c=64 "${r1}" >/dev/null 2>&1 || true; fi
  if [ ! -f "${g0}" ]; then "${ROOT}/icons/draw_square" 0B3D0B --size="${ICON_SIZE}" "${g0}" >/dev/null; "${ROOT}/icons/draw_optimize" -c=64 "${g0}" >/dev/null 2>&1 || true; fi
  if [ ! -f "${g1}" ]; then "${ROOT}/icons/draw_square" 00FF4C --size="${ICON_SIZE}" "${g1}" >/dev/null; "${ROOT}/icons/draw_optimize" -c=64 "${g1}" >/dev/null 2>&1 || true; fi
  if [ ! -f "${b0}" ]; then "${ROOT}/icons/draw_square" 001A4D --size="${ICON_SIZE}" "${b0}" >/dev/null; "${ROOT}/icons/draw_optimize" -c=64 "${b0}" >/dev/null 2>&1 || true; fi
  if [ ! -f "${b1}" ]; then "${ROOT}/icons/draw_square" 2196F3 --size="${ICON_SIZE}" "${b1}" >/dev/null; "${ROOT}/icons/draw_optimize" -c=64 "${b1}" >/dev/null 2>&1 || true; fi
  if [ ! -f "${y0}" ]; then "${ROOT}/icons/draw_square" 5A4A00 --size="${ICON_SIZE}" "${y0}" >/dev/null; "${ROOT}/icons/draw_optimize" -c=64 "${y0}" >/dev/null 2>&1 || true; fi
  if [ ! -f "${y1}" ]; then "${ROOT}/icons/draw_square" FFD700 --size="${ICON_SIZE}" "${y1}" >/dev/null; "${ROOT}/icons/draw_optimize" -c=64 "${y1}" >/dev/null 2>&1 || true; fi

  if [ ! -f "${reset}" ]; then
    "${ROOT}/icons/draw_square" transparent --size="${ICON_SIZE}" "${reset}" >/dev/null
    "${ROOT}/icons/draw_mdi" mdi:restart FFFFFF --size=160 "${reset}" >/dev/null
    "${ROOT}/icons/draw_optimize" -c=32 "${reset}" >/dev/null 2>&1 || true
  fi
  if [ ! -f "${life_on}" ]; then
    "${ROOT}/icons/draw_square" transparent --size="${ICON_SIZE}" "${life_on}" >/dev/null
    "${ROOT}/icons/draw_mdi" mdi:replay FFFFFF --size=160 "${life_on}" >/dev/null 2>&1 || true
    "${ROOT}/icons/draw_optimize" -c=32 "${life_on}" >/dev/null 2>&1 || true
  fi
  if [ ! -f "${life_off}" ]; then
    "${ROOT}/icons/draw_square" transparent --size="${ICON_SIZE}" "${life_off}" >/dev/null
    "${ROOT}/icons/draw_mdi" mdi:replay 555555 --size=160 "${life_off}" >/dev/null 2>&1 || true
    "${ROOT}/icons/draw_optimize" -c=32 "${life_off}" >/dev/null 2>&1 || true
  fi
  if [ ! -f "${stat}" ]; then
    "${ROOT}/icons/draw_square" transparent --size="${ICON_SIZE}" "${stat}" >/dev/null
    "${ROOT}/icons/draw_optimize" -c=16 "${stat}" >/dev/null 2>&1 || true
  fi

  # Button 14 is a wide tile (442x196). Always (re)generate to ensure it matches the latest tools.
  rm -f "${exit14}" 2>/dev/null || true
  "${ROOT}/icons/draw_rectangle" transparent --size="${ICON_SIZE}" "${exit14}" >/dev/null
  "${ROOT}/icons/draw_mdi_rectangle" mdi:exit-to-app C0C0C0 --size=56 --offset=183,58 "${exit14}" >/dev/null 2>&1 || true
  "${ROOT}/icons/draw_optimize" -c=32 "${exit14}" >/dev/null 2>&1 || true
}

load_pregen_to_ram() {
  cp -f "${PREGEN_DIR}/pad_red_dim.png" "${RAM_DIR}/pregen/pad_red_dim.png"
  cp -f "${PREGEN_DIR}/pad_red_lit.png" "${RAM_DIR}/pregen/pad_red_lit.png"
  cp -f "${PREGEN_DIR}/pad_green_dim.png" "${RAM_DIR}/pregen/pad_green_dim.png"
  cp -f "${PREGEN_DIR}/pad_green_lit.png" "${RAM_DIR}/pregen/pad_green_lit.png"
  cp -f "${PREGEN_DIR}/pad_blue_dim.png" "${RAM_DIR}/pregen/pad_blue_dim.png"
  cp -f "${PREGEN_DIR}/pad_blue_lit.png" "${RAM_DIR}/pregen/pad_blue_lit.png"
  cp -f "${PREGEN_DIR}/pad_yellow_dim.png" "${RAM_DIR}/pregen/pad_yellow_dim.png"
  cp -f "${PREGEN_DIR}/pad_yellow_lit.png" "${RAM_DIR}/pregen/pad_yellow_lit.png"
  cp -f "${PREGEN_DIR}/reset.png" "${RAM_DIR}/pregen/reset.png"
  cp -f "${PREGEN_DIR}/life_on.png" "${RAM_DIR}/pregen/life_on.png"
  cp -f "${PREGEN_DIR}/life_off.png" "${RAM_DIR}/pregen/life_off.png"
  cp -f "${PREGEN_DIR}/stat.png" "${RAM_DIR}/pregen/stat.png"
  cp -f "${PREGEN_DIR}/exit14.png" "${RAM_DIR}/pregen/exit14.png"
}

pad_btns() { echo "1 2 6 7"; }

pad_id_for_btn() {
  # returns 0..3 or -1
  local btn="$1"
  case "${btn}" in
    1) echo 0 ;; # red
    2) echo 1 ;; # green
    6) echo 2 ;; # blue
    7) echo 3 ;; # yellow
    *) echo -1 ;;
  esac
}

btn_for_pad_id() {
  local id="$1"
  case "${id}" in
    0) echo 1 ;;
    1) echo 2 ;;
    2) echo 6 ;;
    3) echo 7 ;;
  esac
}

pad_icon_dim() {
  local id="$1"
  case "${id}" in
    0) echo "${RAM_DIR}/pregen/pad_red_dim.png" ;;
    1) echo "${RAM_DIR}/pregen/pad_green_dim.png" ;;
    2) echo "${RAM_DIR}/pregen/pad_blue_dim.png" ;;
    3) echo "${RAM_DIR}/pregen/pad_yellow_dim.png" ;;
  esac
}

pad_icon_lit() {
  local id="$1"
  case "${id}" in
    0) echo "${RAM_DIR}/pregen/pad_red_lit.png" ;;
    1) echo "${RAM_DIR}/pregen/pad_green_lit.png" ;;
    2) echo "${RAM_DIR}/pregen/pad_blue_lit.png" ;;
    3) echo "${RAM_DIR}/pregen/pad_yellow_lit.png" ;;
  esac
}

set_partial_btn_icon() {
  local btn="$1"
  local icon_path="$2"
  local cmd="set-partial-explicit --button-${btn}=$(abs_path "${icon_path}")"
  ul_cmd "${cmd}" >/dev/null
}

sleep_ms() {
  local ms="$1"
  sleep "$(awk -v ms="$ms" 'BEGIN{printf "%.3f\n", ms/1000.0}')"
}

flash_pad() {
  local id="$1"
  local btn
  play_pad_sound "${id}"
  btn="$(btn_for_pad_id "${id}")"
  set_partial_btn_icon "${btn}" "$(pad_icon_lit "${id}")"
  sleep_ms "${FLASH_ON_MS}"
  set_partial_btn_icon "${btn}" "$(pad_icon_dim "${id}")"
  sleep_ms "${FLASH_OFF_MS}"
}

show_sequence() {
  render_ui "PLAY"
  sleep_ms 200
  for id in "${seq[@]}"; do
    flash_pad "${id}"
  done
  phase="input"
  input_pos=0
  render_ui "GO"
}

render_ui() {
  local msg="$1"

  local empty="${ROOT}/assets/pregen/empty.png"
  local reset="${RAM_DIR}/pregen/reset.png"
  local life_on="${RAM_DIR}/pregen/life_on.png"
  local life_off="${RAM_DIR}/pregen/life_off.png"
  local stat="${RAM_DIR}/pregen/stat.png"
  local exit14="${RAM_DIR}/pregen/exit14.png"

  local b1 b2 b3 b4 b5 b6 b7 b8 b9 b10 b11 b12 b13 b14
  b1="$(pad_icon_dim 0)"
  b2="$(pad_icon_dim 1)"
  b3="${empty}"
  b4="${reset}"
  if [ "${life_used}" -eq 0 ]; then b5="${life_on}"; else b5="${life_off}"; fi
  b6="$(pad_icon_dim 2)"
  b7="$(pad_icon_dim 3)"
  b8="${empty}"
  b9="${stat}"
  b10="${stat}"
  b11="${empty}"
  b12="${stat}"
  b13="${empty}"
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

  # labels (no spaces)
  cmd+=" --label-4=RST"
  if [ "${life_used}" -eq 0 ]; then cmd+=" --label-5=LIFE"; else cmd+=" --label-5="; fi
  cmd+=" --label-9=S:${score}"
  cmd+=" --label-10=B:${best}"
  cmd+=" --label-12=${msg}"

  ul_cmd "${cmd}" >/dev/null
}

new_round() {
  seq+=("$((RANDOM % 4))")
  phase="show"
  input_pos=0
}

countdown_go() {
  render_ui "3"
  wait_ms_drain 400
  render_ui "2"
  wait_ms_drain 400
  render_ui "1"
  wait_ms_drain 400
  render_ui "GO"
  wait_ms_drain 300
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
  RB_READ_FD="${RB[0]}"
  drain_rb_events 0.25 <&"${RB[0]}" || true

  score=0
  best=0
  life_used=0
  phase="show"
  input_pos=0
  seq=()
  new_round
  countdown_go

  while true; do
    if [ "${phase}" = "show" ]; then
      show_sequence
    fi
    if [ "${phase}" = "show_repeat" ]; then
      show_sequence
    fi

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
    if [ "${btn}" -eq 4 ]; then
      score=0
      life_used=0
      seq=()
      new_round
      phase="show"
      continue
    fi
    if [ "${btn}" -eq 5 ]; then
      if [ "${life_used}" -eq 0 ] && [ "${phase}" = "input" ]; then
        life_used=1
        phase="show_repeat"
        render_ui "HELP"
        continue
      fi
      continue
    fi

    if [ "${phase}" != "input" ]; then
      continue
    fi

    local pid
    pid="$(pad_id_for_btn "${btn}")"
    if [ "${pid}" -lt 0 ]; then
      continue
    fi

    # user feedback flash
    flash_pad "${pid}"

    local expected="${seq[$input_pos]}"
    if [ "${pid}" -ne "${expected}" ]; then
      play_fail_sound
      render_ui "FAIL"
      wait_ms_drain 3000
      score=0
      life_used=0
      seq=()
      new_round
      countdown_go
      phase="show"
      continue
    fi

    input_pos=$((input_pos + 1))
    if [ "${input_pos}" -ge "${#seq[@]}" ]; then
      score=$((score + 1))
      if [ "${score}" -gt "${best}" ]; then best="${score}"; fi
      new_round
      phase="show"
    fi
  done
}

main "$@"
