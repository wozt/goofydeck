#!/usr/bin/env bash
set -euo pipefail

# GoofyDeck "2048" miniapp (3x3 variant)
#
# - stop-control/start-control/load-last-page via paging control socket
# - read-buttons via ulanzi daemon
# - render via set-buttons-explicit-14 (numbers are labels)
#
# Layout:
# - Grid (3x3): 1,2,3 / 6,7,8 / 11,12,13
# - Reset: 4 (TAP)
# - Life (delete one tile): 5
# - Score: 9 (label)
# - Max:   10 (label)
# - Exit:  14 (wide tile, icon only; no label support)
#
# Controls (tap on grid chooses a direction):
# - Top row (1/2/3): UP
# - Bottom row (11/12/13): DOWN
# - Else left col (1/6/11): LEFT
# - Else right col (3/8/13): RIGHT
# (Corners follow row priority: top->UP, bottom->DOWN)

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"

MINIAPP_NAME="2048"

ULANZI_SOCK="${ULANZI_SOCK:-/tmp/ulanzi_device.sock}"
PAGING_CTRL_SOCK="${PAGING_CTRL_SOCK:-/tmp/goofydeck_paging_control.sock}"

DISK_CACHE_DIR="${ROOT}/mymedia/miniapp/.cache/${MINIAPP_NAME}"
PREGEN_DIR="${DISK_CACHE_DIR}/pregen"

RAM_BASE="/dev/shm/goofydeck/${MINIAPP_NAME}"
RAM_DIR="${RAM_BASE}/$$"
TILES_DIR="${RAM_DIR}/tiles"

ICON_SIZE=196
ARROW_SIZE=120

mkdir -p "${PREGEN_DIR}" "${RAM_DIR}" "${TILES_DIR}"

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
  local reset="${PREGEN_DIR}/reset.png"
  local life_on="${PREGEN_DIR}/life_on.png"
  local life_off="${PREGEN_DIR}/life_off.png"
  local exit14="${PREGEN_DIR}/exit14.png"
  local label_style="${PREGEN_DIR}/label_style.json"

  if [ ! -f "${reset}" ]; then
    "${ROOT}/icons/draw_square" transparent --size="${ICON_SIZE}" "${reset}" >/dev/null
    "${ROOT}/icons/draw_mdi" mdi:restart FFFFFF --size=160 "${reset}" >/dev/null
    "${ROOT}/icons/draw_optimize" -c=16 "${reset}" >/dev/null 2>&1 || true
  fi
  if [ ! -f "${life_on}" ]; then
    "${ROOT}/icons/draw_square" transparent --size="${ICON_SIZE}" "${life_on}" >/dev/null
    "${ROOT}/icons/draw_mdi" mdi:eraser-variant FFFFFF --size=160 "${life_on}" >/dev/null 2>&1 || true
    "${ROOT}/icons/draw_optimize" -c=16 "${life_on}" >/dev/null 2>&1 || true
  fi
  if [ ! -f "${life_off}" ]; then
    "${ROOT}/icons/draw_square" transparent --size="${ICON_SIZE}" "${life_off}" >/dev/null
    "${ROOT}/icons/draw_mdi" mdi:eraser-variant 555555 --size=160 "${life_off}" >/dev/null 2>&1 || true
    "${ROOT}/icons/draw_optimize" -c=16 "${life_off}" >/dev/null 2>&1 || true
  fi
  if [ ! -f "${label_style}" ]; then
    cat >"${label_style}" <<'EOF'
{
  "Align": "center",
  "Color": 13421772,
  "FontName": "Roboto",
  "ShowTitle": true,
  "Size": 26,
  "Weight": 80
}
EOF
  fi

  # Button 14 is a wide tile (442x196). Always (re)generate to ensure it matches the latest tools.
  rm -f "${exit14}" 2>/dev/null || true
  "${ROOT}/icons/draw_rectangle" transparent --size="${ICON_SIZE}" "${exit14}" >/dev/null
  "${ROOT}/icons/draw_mdi_rectangle" mdi:exit-to-app C0C0C0 --size=56 --offset=183,58 "${exit14}" >/dev/null 2>&1 || true
  "${ROOT}/icons/draw_optimize" -c=32 "${exit14}" >/dev/null 2>&1 || true
}

load_pregen_to_ram() {
  mkdir -p "${RAM_DIR}/pregen"
  cp -f "${PREGEN_DIR}/reset.png" "${RAM_DIR}/pregen/reset.png"
  cp -f "${PREGEN_DIR}/life_on.png" "${RAM_DIR}/pregen/life_on.png"
  cp -f "${PREGEN_DIR}/life_off.png" "${RAM_DIR}/pregen/life_off.png"
  cp -f "${PREGEN_DIR}/exit14.png" "${RAM_DIR}/pregen/exit14.png"
}

tile_bg_color() {
  # Red -> yellow -> green gradient (user-tuned)
  local v="$1"
  case "${v}" in
    0) echo "222222" ;;
    2) echo "7A0019" ;;    # dark red
    4) echo "A3001D" ;;    # red
    8) echo "D12A00" ;;    # red-orange
    16) echo "FF5A00" ;;   # orange
    32) echo "FFD700" ;;   # yellow
    64) echo "B7F000" ;;   # yellow-green
    128) echo "7BE600" ;;  # green
    256) echo "42DC00" ;;  # greener
    512) echo "0AD200" ;;  # vivid green
    1024) echo "00D93A" ;; # vivid green
    2048) echo "00FF4C" ;; # popping green
    *) echo "0B0B0B" ;;
  esac
}

tile_text_color() {
  local v="$1"
  case "${v}" in
    2) echo "FFD1E8" ;;  # light pink
    4) echo "FFE6EE" ;;
    8|16|32|64|128|256|512|1024|2048) echo "FFFFFF" ;;
    *) echo "FFFFFF" ;;
  esac
}

tile_text_size() {
  local v="$1"
  if [ "${v}" -lt 10 ]; then echo 90
  elif [ "${v}" -lt 100 ]; then echo 84
  elif [ "${v}" -lt 1000 ]; then echo 76
  elif [ "${v}" -lt 10000 ]; then echo 66
  else echo 58
  fi
}

is_dir_btn() {
  # Direction controls requested by user
  # 2=UP, 6=LEFT, 8=RIGHT, 12=DOWN
  local btn="$1"
  case "${btn}" in
    2|6|8|12) return 0 ;;
    *) return 1 ;;
  esac
}

ensure_tile_icon() {
  # Produces a ready-to-send tile icon that already includes:
  # - colored background
  # - big centered number (for v>0)
  local v="$1"
  local base="${TILES_DIR}/tile_${v}.png"

  if [ -f "${base}" ]; then
    echo "${base}"
    return 0
  fi

  local bg tc ts
  bg="$(tile_bg_color "${v}")"
  tc="$(tile_text_color "${v}")"
  ts="$(tile_text_size "${v}")"

  "${ROOT}/icons/draw_square" "${bg}" --size="${ICON_SIZE}" "${base}" >/dev/null

  if [ "${v}" -ne 0 ]; then
    "${ROOT}/icons/draw_text" \
      --text="${v}" \
      --text_color="${tc}" \
      --text_align="center" \
      --text_font="DejaVuSans-Bold.ttf" \
      --text_size="${ts}" \
      --text_offset="0,0" \
      "${base}" >/dev/null 2>&1 || true
    "${ROOT}/icons/draw_optimize" -c=128 "${base}" >/dev/null 2>&1 || true
  else
    "${ROOT}/icons/draw_optimize" -c=64 "${base}" >/dev/null 2>&1 || true
  fi

  echo "${base}"
}

render_stat_tile() {
  # Dynamic stat tiles (draw_text), rendered in RAM each time score/max changes.
  # args: kind (score|max), value
  local kind="$1"
  local value="$2"

  local out="${RAM_DIR}/tmp/${kind}.png"
  "${ROOT}/icons/draw_square" 111111 --size="${ICON_SIZE}" "${out}" >/dev/null
  "${ROOT}/icons/draw_text" \
    --text="${value}" \
    --text_color="FFFFFF" \
    --text_align="center" \
    --text_font="DejaVuSans-Bold.ttf" \
    --text_size="72" \
    --text_offset="0,0" \
    "${out}" >/dev/null 2>&1 || true
  "${ROOT}/icons/draw_optimize" -c=128 "${out}" >/dev/null 2>&1 || true
  echo "${out}"
}

board_reset() {
  for i in $(seq 0 8); do board[$i]=0; done
  score=0
  msg=""
  spawn_tile
  spawn_tile
}

max_tile() {
  local m=0
  for v in "${board[@]}"; do
    if [ "${v}" -gt "${m}" ]; then m="${v}"; fi
  done
  printf "%s\n" "${m}"
}

empty_positions() {
  # prints indices with zeros
  for i in $(seq 0 8); do
    if [ "${board[$i]}" -eq 0 ]; then
      printf "%s " "$i"
    fi
  done
}

spawn_tile() {
  local empties
  empties="$(empty_positions)"
  if [ -z "${empties// /}" ]; then
    return 1
  fi
  # pick random empty index
  local arr=()
  for x in ${empties}; do arr+=("$x"); done
  local n="${#arr[@]}"
  local idx=$((RANDOM % n))
  local pos="${arr[$idx]}"
  # 90% => 2, 10% => 4
  local r=$((RANDOM % 10))
  if [ "${r}" -eq 0 ]; then
    board[$pos]=4
  else
    board[$pos]=2
  fi
  return 0
}

can_move() {
  # any empty?
  for v in "${board[@]}"; do
    if [ "${v}" -eq 0 ]; then return 0; fi
  done
  # any merge adjacent?
  local b=("${board[@]}")
  local pairs=(
    "0 1" "1 2" "3 4" "4 5" "6 7" "7 8"
    "0 3" "3 6" "1 4" "4 7" "2 5" "5 8"
  )
  for p in "${pairs[@]}"; do
    local a c
    a="$(printf "%s" "${p}" | awk '{print $1}')"
    c="$(printf "%s" "${p}" | awk '{print $2}')"
    if [ "${b[$a]}" -ne 0 ] && [ "${b[$a]}" -eq "${b[$c]}" ]; then
      return 0
    fi
  done
  return 1
}

compress_line() {
  # input: 3 ints; output: "changed out0 out1 out2 gained"
  local a="$1" b="$2" c="$3"
  local in=("$a" "$b" "$c")
  local tmp=()
  for v in "${in[@]}"; do
    if [ "${v}" -ne 0 ]; then tmp+=("$v"); fi
  done
  local out=(0 0 0)
  local gained=0
  local i=0
  while [ "${i}" -lt "${#tmp[@]}" ]; do
    local v="${tmp[$i]}"
    if [ $((i+1)) -lt "${#tmp[@]}" ] && [ "${tmp[$((i+1))]}" -eq "${v}" ]; then
      v=$((v*2))
      gained=$((gained + v))
      i=$((i+2))
    else
      i=$((i+1))
    fi
    local j=0
    while [ "${j}" -lt 3 ]; do
      if [ "${out[$j]}" -eq 0 ]; then out[$j]="${v}"; break; fi
      j=$((j+1))
    done
  done
  local changed=0
  for k in 0 1 2; do
    if [ "${out[$k]}" -ne "${in[$k]}" ]; then changed=1; fi
  done
  printf "%s %s %s %s %s\n" "${changed}" "${out[0]}" "${out[1]}" "${out[2]}" "${gained}"
}

move_dir() {
  # dir: up/down/left/right ; returns 0 if moved, 1 if no-op
  local dir="$1"
  local moved=0
  local gained_total=0
  local b=("${board[@]}")

  case "${dir}" in
    left)
      for row in 0 3 6; do
        local res changed o0 o1 o2 gained
        res="$(compress_line "${b[$row]}" "${b[$((row+1))]}" "${b[$((row+2))]}")"
        changed="$(printf "%s" "${res}" | awk '{print $1}')"
        o0="$(printf "%s" "${res}" | awk '{print $2}')"
        o1="$(printf "%s" "${res}" | awk '{print $3}')"
        o2="$(printf "%s" "${res}" | awk '{print $4}')"
        gained="$(printf "%s" "${res}" | awk '{print $5}')"
        gained_total=$((gained_total + gained))
        if [ "${changed}" -eq 1 ]; then moved=1; fi
        board[$row]="${o0}"
        board[$((row+1))]="${o1}"
        board[$((row+2))]="${o2}"
      done
      ;;
    right)
      for row in 0 3 6; do
        local res changed o0 o1 o2 gained
        res="$(compress_line "${b[$((row+2))]}" "${b[$((row+1))]}" "${b[$row]}")"
        changed="$(printf "%s" "${res}" | awk '{print $1}')"
        o0="$(printf "%s" "${res}" | awk '{print $2}')"
        o1="$(printf "%s" "${res}" | awk '{print $3}')"
        o2="$(printf "%s" "${res}" | awk '{print $4}')"
        gained="$(printf "%s" "${res}" | awk '{print $5}')"
        gained_total=$((gained_total + gained))
        if [ "${changed}" -eq 1 ]; then moved=1; fi
        board[$row]="${o2}"
        board[$((row+1))]="${o1}"
        board[$((row+2))]="${o0}"
      done
      ;;
    up)
      for col in 0 1 2; do
        local res changed o0 o1 o2 gained
        res="$(compress_line "${b[$col]}" "${b[$((col+3))]}" "${b[$((col+6))]}")"
        changed="$(printf "%s" "${res}" | awk '{print $1}')"
        o0="$(printf "%s" "${res}" | awk '{print $2}')"
        o1="$(printf "%s" "${res}" | awk '{print $3}')"
        o2="$(printf "%s" "${res}" | awk '{print $4}')"
        gained="$(printf "%s" "${res}" | awk '{print $5}')"
        gained_total=$((gained_total + gained))
        if [ "${changed}" -eq 1 ]; then moved=1; fi
        board[$col]="${o0}"
        board[$((col+3))]="${o1}"
        board[$((col+6))]="${o2}"
      done
      ;;
    down)
      for col in 0 1 2; do
        local res changed o0 o1 o2 gained
        res="$(compress_line "${b[$((col+6))]}" "${b[$((col+3))]}" "${b[$col]}")"
        changed="$(printf "%s" "${res}" | awk '{print $1}')"
        o0="$(printf "%s" "${res}" | awk '{print $2}')"
        o1="$(printf "%s" "${res}" | awk '{print $3}')"
        o2="$(printf "%s" "${res}" | awk '{print $4}')"
        gained="$(printf "%s" "${res}" | awk '{print $5}')"
        gained_total=$((gained_total + gained))
        if [ "${changed}" -eq 1 ]; then moved=1; fi
        board[$col]="${o2}"
        board[$((col+3))]="${o1}"
        board[$((col+6))]="${o0}"
      done
      ;;
    *) return 1 ;;
  esac

  if [ "${moved}" -eq 1 ]; then
    score=$((score + gained_total))
    spawn_tile >/dev/null 2>&1 || true
    return 0
  fi
  return 1
}

render() {
  local reset="${RAM_DIR}/pregen/reset.png"
  local life_on="${RAM_DIR}/pregen/life_on.png"
  local life_off="${RAM_DIR}/pregen/life_off.png"
  local exit14="${RAM_DIR}/pregen/exit14.png"
  local empty="${ROOT}/assets/pregen/empty.png"
  local tile0
  tile0="$(ensure_tile_icon 0)"

  local b1 b2 b3 b4 b5 b6 b7 b8 b9 b10 b11 b12 b13 b14
  b1="${tile0}" ; b2="${tile0}" ; b3="${tile0}"
  b4="${reset}"
  if [ "${life_used}" -eq 0 ]; then b5="${life_on}"; else b5="${life_off}"; fi
  b6="${tile0}" ; b7="${tile0}" ; b8="${tile0}"
  b9="${tile0}" ; b10="${tile0}"
  b11="${tile0}" ; b12="${tile0}" ; b13="${tile0}"
  b14="${exit14}"

  # per-tile icons (numbers are rendered inside the icon for sizing + per-tile colors)
  # map board idx -> button
  local map=(1 2 3 6 7 8 11 12 13)
  local idxs=(0 1 2 3 4 5 6 7 8)
  # Always show direction labels on control buttons, even when a tile number is present.
  local dir_label_2="1"
  local dir_label_6="1"
  local dir_label_8="1"
  local dir_label_12="1"
  if [ "${life_select}" -eq 1 ]; then
    dir_label_2=""
    dir_label_6=""
    dir_label_8=""
    dir_label_12=""
  fi
  local i=0
  while [ "${i}" -lt 9 ]; do
    local btn="${map[$i]}"
    local v="${board[${idxs[$i]}]}"
    local icon_path
    icon_path="$(ensure_tile_icon "${v}")"
    case "${btn}" in
      1) b1="${icon_path}" ;;
      2) b2="${icon_path}" ;;
      3) b3="${icon_path}" ;;
      6) b6="${icon_path}" ;;
      7) b7="${icon_path}" ;;
      8) b8="${icon_path}" ;;
      11) b11="${icon_path}" ;;
      12) b12="${icon_path}" ;;
      13) b13="${icon_path}" ;;
    esac
    i=$((i+1))
  done

  b9="$(render_stat_tile score "${score}")"
  b10="$(render_stat_tile max "$(max_tile)")"

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

  # labels (no spaces; ulanzi daemon argv splitting is space-based)
  if [ "${life_used}" -eq 0 ]; then
    if [ "${life_select}" -eq 1 ]; then cmd+=" --label-5=PICK"; else cmd+=" --label-5=DEL"; fi
  else
    cmd+=" --label-5="
  fi
  cmd+=" --label-9=S"
  cmd+=" --label-10=M"
  if [ -n "${dir_label_2}" ]; then cmd+=" --label-2=↑"; else cmd+=" --label-2="; fi
  if [ -n "${dir_label_6}" ]; then cmd+=" --label-6=←"; else cmd+=" --label-6="; fi
  if [ -n "${dir_label_8}" ]; then cmd+=" --label-8=→"; else cmd+=" --label-8="; fi
  if [ -n "${dir_label_12}" ]; then cmd+=" --label-12=↓"; else cmd+=" --label-12="; fi

  if [ -n "${msg}" ]; then
    cmd+=" --label-7=${msg}"
  fi

  ul_cmd "${cmd}" >/dev/null
}

main() {
  [ -S "${ULANZI_SOCK}" ] || { echo "Missing ulanzi socket: ${ULANZI_SOCK}" >&2; exit 1; }

  ensure_pregen
  load_pregen_to_ram

  # Bigger/centered labels for score/message (best effort)
  ul_cmd "set-label-style $(abs_path "${PREGEN_DIR}/label_style.json")" >/dev/null 2>&1 || true

  pg_cmd "stop-control"

  declare -a board
  score=0
  msg=""
  life_used=0
  life_select=0
  board_reset
  render

  coproc RB { socat - UNIX-CONNECT:"${ULANZI_SOCK}"; }
  printf 'read-buttons\n' >&"${RB[1]}"
  read -r _okline <&"${RB[0]}" || true

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

    msg=""
    if [ "${btn}" -eq 14 ]; then
      return 0
    fi
    if [ "${btn}" -eq 4 ]; then
      board_reset
      life_used=0
      life_select=0
      render
      continue
    fi

    if [ "${btn}" -eq 5 ]; then
      if [ "${life_used}" -eq 0 ]; then
        if [ "${life_select}" -eq 0 ]; then
          life_select=1
          msg="DEL"
        else
          life_select=0
          msg=""
        fi
        render
      fi
      continue
    fi

    if [ "${life_select}" -eq 1 ]; then
      # In delete-select mode: ignore direction controls; use the tapped grid tile to clear it.
      case "${btn}" in
        1) idx=0 ;;
        2) idx=1 ;;
        3) idx=2 ;;
        6) idx=3 ;;
        7) idx=4 ;;
        8) idx=5 ;;
        11) idx=6 ;;
        12) idx=7 ;;
        13) idx=8 ;;
        *) idx=-1 ;;
      esac
      if [ "${idx}" -ge 0 ]; then
        if [ "${board[$idx]}" -ne 0 ]; then
          board[$idx]=0
          life_used=1
          life_select=0
          msg=""
        else
          msg="EMPTY"
        fi
        render
      fi
      continue
    fi

    # Direction controls:
    # 2=UP, 6=LEFT, 8=RIGHT, 12=DOWN
    case "${btn}" in
      2) move_dir "up" || true ;;
      6) move_dir "left" || true ;;
      8) move_dir "right" || true ;;
      12) move_dir "down" || true ;;
      *) continue ;;
    esac

    if ! can_move; then
      msg="GAMEOVER"
      render
      sleep 0.8
      board_reset
    fi
    render
  done
}

main "$@"
