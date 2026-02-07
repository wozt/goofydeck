#!/usr/bin/env bash
set -euo pipefail

# GoofyDeck video miniapp (MP4 only)
#
# - Grabs control from paging_daemon via stop-control/start-control
# - Subscribes to ulanzi_d200_daemon button events (read-buttons)
# - Shows a paged list of MP4s from mymedia/videos/ with 64x64 thumbnails
# - TAP: play if rendered exists, else no-op
# - HOLD: render (convert+render with live visualization)
#
# Notes:
# - Menu uses buttons 11/12/13 for exit/prev/next (always reserved)
# - Playback: TAP any button (except 14) shows controls for 100 frames:
#     1=stop, 11=back, 12=play/pause, 13=forward

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"

MINIAPP_NAME="videoplayer"

ULANZI_SOCK="${ULANZI_SOCK:-/tmp/ulanzi_device.sock}"
PAGING_CTRL_SOCK="${PAGING_CTRL_SOCK:-/tmp/goofydeck_paging_control.sock}"

VIDEOS_DIR="${ROOT}/mymedia/videos"
# Miniapp disk cache (thumbs, etc). Keep it out of the global .cache/.
CACHE_DIR="${ROOT}/mymedia/miniapp/.cache/${MINIAPP_NAME}"
# Backward-compat read-only location (older versions wrote here).
CACHE_DIR_OLD="${ROOT}/.cache/${MINIAPP_NAME}"
RAM_BASE="/dev/shm/goofydeck/video_miniapp"
RAM_DIR="${RAM_BASE}/$$"

THUMB_SIZE=64
CTRL_SIZE=60
MENU_PAGE_SIZE=10
MENU_FRAME_BTN_14="${ROOT}/assets/pregen/empty.png"

FRAME_DELAY_MS=66
SEEK_FRAMES=75

TARGET_W=640
TARGET_H=360
TARGET_FPS=15

# 0 = replace buttons with control icons (fast)
# 1 = attempt draw_over overlay (slow, for testing)
USE_DRAW_OVER=0

mkdir -p "${CACHE_DIR}/thumbs" "${RAM_DIR}/tmp" "${RAM_DIR}/ctrl"

cleanup() {
  rm -rf "${RAM_DIR}" 2>/dev/null || true
  if [ -S "${PAGING_CTRL_SOCK}" ]; then
    # paging control socket handles 1 command per connection
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

set_partial_btn_icon() {
  local btn="$1"
  local icon_path="$2"
  local cmd="set-partial-explicit --button-${btn}=$(abs_path "${icon_path}")"
  ul_cmd "${cmd}" >/dev/null
}

abs_path() {
  if command -v realpath >/dev/null 2>&1; then
    realpath -m "$1"
  else
    if [[ "$1" == /* ]]; then printf "%s\n" "$1"; else printf "%s/%s\n" "$(pwd -P)" "$1"; fi
  fi
}

ensure_ctrl_icons() {
  local stop="${RAM_DIR}/ctrl/stop.png"
  local back="${RAM_DIR}/ctrl/back.png"
  local play="${RAM_DIR}/ctrl/play.png"
  local pause="${RAM_DIR}/ctrl/pause.png"
  local fwd="${RAM_DIR}/ctrl/fwd.png"

  if [ -f "${stop}" ] && [ -f "${back}" ] && [ -f "${play}" ] && [ -f "${pause}" ] && [ -f "${fwd}" ]; then
    return 0
  fi

  "${ROOT}/icons/draw_square" transparent --size="${CTRL_SIZE}" "${stop}" >/dev/null
  "${ROOT}/icons/draw_mdi" mdi:stop FFFFFF --size="$((CTRL_SIZE - 10))" "${stop}" >/dev/null

  "${ROOT}/icons/draw_square" transparent --size="${CTRL_SIZE}" "${back}" >/dev/null
  "${ROOT}/icons/draw_mdi" mdi:rewind FFFFFF --size="$((CTRL_SIZE - 10))" "${back}" >/dev/null

  "${ROOT}/icons/draw_square" transparent --size="${CTRL_SIZE}" "${play}" >/dev/null
  "${ROOT}/icons/draw_mdi" mdi:play FFFFFF --size="$((CTRL_SIZE - 10))" "${play}" >/dev/null

  "${ROOT}/icons/draw_square" transparent --size="${CTRL_SIZE}" "${pause}" >/dev/null
  "${ROOT}/icons/draw_mdi" mdi:pause FFFFFF --size="$((CTRL_SIZE - 10))" "${pause}" >/dev/null

  "${ROOT}/icons/draw_square" transparent --size="${CTRL_SIZE}" "${fwd}" >/dev/null
  "${ROOT}/icons/draw_mdi" mdi:fast-forward FFFFFF --size="$((CTRL_SIZE - 10))" "${fwd}" >/dev/null
}

parse_btn_evt() {
  # input: line like "button 5 TAP" or "button 5 HOLD (0.77s)" or "button 5 LONGHOLD (5.02s)" or "button 5 RELEASED"
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

video_basename() {
  basename "$1" .mp4
}

ffprobe_val() {
  local video="$1"
  local key="$2"
  ffprobe -v error -select_streams v:0 -show_entries "stream=${key}" -of csv=p=0 "${video}" 2>/dev/null | head -n 1 || true
}

is_video_target_format() {
  local video="$1"
  [ -f "${video}" ] || return 1
  local w h fr
  w="$(ffprobe_val "${video}" width | tr -d '\r\n' || true)"
  h="$(ffprobe_val "${video}" height | tr -d '\r\n' || true)"
  fr="$(ffprobe_val "${video}" avg_frame_rate | tr -d '\r\n' || true)"
  [ "${w}" = "${TARGET_W}" ] || return 1
  [ "${h}" = "${TARGET_H}" ] || return 1
  local fps
  fps="$(printf "%s" "${fr}" | awk -F/ 'NF==2 && $2>0 {printf "%.3f", $1/$2}' 2>/dev/null || true)"
  awk -v fps="${fps:-0}" -v want="${TARGET_FPS}" 'BEGIN{exit !(fps>=want-0.05 && fps<=want+0.05)}'
}

convert_in_place_if_needed() {
  # Returns 0 if converted, 1 if no conversion needed, >1 on error.
  local video="$1"
  [ -f "${video}" ] || return 2
  if is_video_target_format "${video}"; then
    return 1
  fi

  local in_dir in_base in_name out
  in_dir="$(dirname "${video}")"
  in_base="$(basename "${video}")"
  in_name="${in_base%.mp4}"
  out="${in_dir}/${in_name}_converted_360p_15fps.mp4"

  "${ROOT}/bin/convert_video.sh" --size=360 --fps=15 "${video}" >/dev/null 2>&1 || return 3
  [ -f "${out}" ] || return 4
  mv -f "${out}" "${video}"
  return 0
}

make_status_tile() {
  local text="$1"
  local out="$2"
  "${ROOT}/icons/draw_square" "111111" --size="${THUMB_SIZE}" "${out}" >/dev/null
  "${ROOT}/icons/draw_text" \
    --text="${text}" \
    --text_color="FFFFFF" \
    --text_align="center" \
    --text_font="dustismo_bold.ttf" \
    --text_size="16" \
    --text_offset="0,0" \
    "${out}" >/dev/null 2>&1 || true
}

drain_rb_events() {
  # Drain buffered button events after a long render so we don't trigger actions immediately.
  local seconds="$1"
  local start now elapsed
  start="$(date +%s.%N)"
  while true; do
    now="$(date +%s.%N)"
    elapsed="$(awk -v a="${now}" -v b="${start}" 'BEGIN{print a-b}')"
    awk -v e="${elapsed}" -v end="${seconds}" 'BEGIN{exit !(e<end)}' || break
    local _line=""
    if ! read -r -t 0.05 _line; then
      continue
    fi
  done
}

render_dir_for_video() {
  local video="$1"
  local base
  base="$(video_basename "${video}")"
  printf "%s/%s\n" "$(dirname "${video}")" "${base}"
}

has_rendered_video() {
  local video="$1"
  local rd
  rd="$(render_dir_for_video "${video}")"
  [ -d "${rd}/1" ]
}

thumb_path_disk() {
  local video="$1"
  local base
  base="$(video_basename "${video}")"
  local p="${CACHE_DIR}/thumbs/${base}.png"
  if [ -f "${p}" ]; then
    printf "%s\n" "${p}"
    return 0
  fi
  local old="${CACHE_DIR_OLD}/thumbs/${base}.png"
  if [ -f "${old}" ]; then
    printf "%s\n" "${old}"
    return 0
  fi
  printf "%s\n" "${p}"
}

thumb_render_path_disk() {
  local video="$1"
  local base
  base="$(video_basename "${video}")"
  local p="${CACHE_DIR}/thumbs/${base}_render.png"
  if [ -f "${p}" ]; then
    printf "%s\n" "${p}"
    return 0
  fi
  local old="${CACHE_DIR_OLD}/thumbs/${base}_render.png"
  if [ -f "${old}" ]; then
    printf "%s\n" "${old}"
    return 0
  fi
  printf "%s\n" "${p}"
}

ensure_thumb() {
  local video="$1"
  local out
  out="$(thumb_path_disk "${video}")"
  if [ -f "${out}" ]; then
    return 0
  fi
  mkdir -p "${CACHE_DIR}/thumbs"
  ffmpeg -hide_banner -loglevel error -y \
    -ss 00:00:01 -i "${video}" \
    -frames:v 1 \
    -vf "scale=${THUMB_SIZE}:${THUMB_SIZE}:force_original_aspect_ratio=increase,crop=${THUMB_SIZE}:${THUMB_SIZE}" \
    "${out}"
}

ensure_thumb_render_variant() {
  local video="$1"
  local base_thumb render_thumb
  base_thumb="$(thumb_path_disk "${video}")"
  render_thumb="$(thumb_render_path_disk "${video}")"
  ensure_thumb "${video}"
  if [ -f "${render_thumb}" ]; then
    return 0
  fi
  cp -f "${base_thumb}" "${render_thumb}"
  "${ROOT}/icons/draw_text" \
    --text="render" \
    --text_color="FFFFFF" \
    --text_align="bottom" \
    --text_size="18" \
    --text_offset="0,0" \
    "${render_thumb}" >/dev/null 2>&1 || true
}

send_page_14() {
  local b1="$1" b2="$2" b3="$3" b4="$4" b5="$5" b6="$6" b7="$7" b8="$8" b9="$9" b10="${10}" b11="${11}" b12="${12}" b13="${13}" b14="${14}"
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
  ul_cmd "${cmd}" >/dev/null
}

menu_render() {
  local -n _videos_ref="$1"
  local offset="$2"
  local total="${#_videos_ref[@]}"

  local outdir="${RAM_DIR}/menu"
  rm -rf "${outdir}" 2>/dev/null || true
  mkdir -p "${outdir}"

  local empty="${ROOT}/assets/pregen/empty.png"
  local back="${ROOT}/assets/pregen/page_back.png"
  local prev="${ROOT}/assets/pregen/page_prev.png"
  local next="${ROOT}/assets/pregen/page_next.png"

  local btn_paths=()
  for _ in $(seq 1 14); do btn_paths+=("${empty}"); done

  # content buttons 1..10
  for b in $(seq 1 "${MENU_PAGE_SIZE}"); do
    local idx=$((offset + b - 1))
    if [ "${idx}" -ge "${total}" ]; then
      btn_paths[$((b-1))]="${empty}"
      continue
    fi
    local v="${_videos_ref[${idx}]}"
    if has_rendered_video "${v}"; then
      ensure_thumb "${v}"
      btn_paths[$((b-1))]="$(thumb_path_disk "${v}")"
    else
      ensure_thumb_render_variant "${v}"
      btn_paths[$((b-1))]="$(thumb_render_path_disk "${v}")"
    fi
  done

  # nav buttons
  btn_paths[10]="${back}" # 11
  if [ "${offset}" -gt 0 ]; then
    btn_paths[11]="${prev}" # 12
  fi
  if [ $((offset + MENU_PAGE_SIZE)) -lt "${total}" ]; then
    btn_paths[12]="${next}" # 13
  fi
  btn_paths[13]="${MENU_FRAME_BTN_14}"

  send_page_14 \
    "${btn_paths[0]}" "${btn_paths[1]}" "${btn_paths[2]}" "${btn_paths[3]}" "${btn_paths[4]}" "${btn_paths[5]}" "${btn_paths[6]}" \
    "${btn_paths[7]}" "${btn_paths[8]}" "${btn_paths[9]}" "${btn_paths[10]}" "${btn_paths[11]}" "${btn_paths[12]}" "${btn_paths[13]}"
}

get_frame_format() {
  local max_frames="$1"
  if [ "${max_frames}" -lt 10 ]; then echo "%d"
  elif [ "${max_frames}" -lt 100 ]; then echo "%02d"
  elif [ "${max_frames}" -lt 1000 ]; then echo "%03d"
  elif [ "${max_frames}" -lt 10000 ]; then echo "%04d"
  elif [ "${max_frames}" -lt 100000 ]; then echo "%05d"
  else echo "%06d"
  fi
}

frame_path() {
  local mount="$1" btn="$2" fmt="$3" frame="$4"
  printf "%s/%d/b%d_%s.png\n" "${mount}" "${btn}" "${btn}" "$(printf "${fmt}" "${frame}")"
}

playback_loop() {
  local video="$1"
  local rd
  rd="$(render_dir_for_video "${video}")"
  local max_frame
  max_frame="$(find "${rd}/1" -maxdepth 1 -type f -name 'b1_*.png' | wc -l | awk '{print $1}')"
  if [ "${max_frame}" -le 0 ]; then
    return 1
  fi
  max_frame=$((max_frame - 1))
  local fmt
  fmt="$(get_frame_format "${max_frame}")"

  ensure_ctrl_icons
  local ctrl_stop="${RAM_DIR}/ctrl/stop.png"
  local ctrl_back="${RAM_DIR}/ctrl/back.png"
  local ctrl_play="${RAM_DIR}/ctrl/play.png"
  local ctrl_pause="${RAM_DIR}/ctrl/pause.png"
  local ctrl_fwd="${RAM_DIR}/ctrl/fwd.png"

  local playing=1
  local paused=0
  local frame=0
  local controls_left=0
  local controls_shown_at_s=0
  local need_redraw=1

  while true; do
    local timeout
    timeout="$(awk -v ms="${FRAME_DELAY_MS}" 'BEGIN{printf "%.3f", ms/1000.0}')"

    local line=""
    if read -r -t "${timeout}" line; then
      local parsed btn evt
      if parsed="$(parse_btn_evt "${line}")"; then
        btn="$(printf "%s" "${parsed}" | awk '{print $1}')"
        evt="$(printf "%s" "${parsed}" | awk '{print $2}')"
        if [ "${evt}" = "TAP" ]; then
          local now_s
          now_s="$(date +%s)"

          # Any TAP (except button 14) can request showing controls, but the TAP that
          # triggers the overlay must NOT also trigger the control action.
          if [ "${btn}" -ne 14 ] && [ "${controls_left}" -le 0 ]; then
            controls_left=100
            controls_shown_at_s="${now_s}"
            need_redraw=1
            if [ "${paused}" -eq 0 ]; then
              continue
            fi
          fi

          if [ "${btn}" -ne 14 ] && [ "${controls_left}" -gt 0 ]; then
            controls_shown_at_s="${now_s}"
          fi

          # Controls are visible: only then apply actions.
          if [ "${btn}" -eq 1 ]; then
            return 0
          elif [ "${btn}" -eq 11 ]; then
            frame=$((frame - SEEK_FRAMES))
            if [ "${frame}" -lt 0 ]; then frame=0; fi
            need_redraw=1
          elif [ "${btn}" -eq 12 ]; then
            if [ "${paused}" -eq 1 ]; then paused=0; else paused=1; fi
            need_redraw=1
          elif [ "${btn}" -eq 13 ]; then
            frame=$((frame + SEEK_FRAMES))
            if [ "${frame}" -gt "${max_frame}" ]; then frame="${max_frame}"; fi
            need_redraw=1
          fi
        fi
      fi
    fi

    local advance_frame=1
    if [ "${paused}" -eq 1 ]; then
      advance_frame=0
      if [ "${controls_left}" -gt 0 ] && [ "${controls_shown_at_s}" -gt 0 ]; then
        local now_s
        now_s="$(date +%s)"
        if [ $((now_s - controls_shown_at_s)) -ge 5 ]; then
          controls_left=0
          need_redraw=1
        fi
      fi

      if [ "${need_redraw}" -le 0 ]; then
        continue
      fi
    fi

    if [ "${frame}" -gt "${max_frame}" ]; then
      return 0
    fi

    local b1 b2 b3 b4 b5 b6 b7 b8 b9 b10 b11 b12 b13 b14
    b1="$(frame_path "${rd}" 1 "${fmt}" "${frame}")"
    b2="$(frame_path "${rd}" 2 "${fmt}" "${frame}")"
    b3="$(frame_path "${rd}" 3 "${fmt}" "${frame}")"
    b4="$(frame_path "${rd}" 4 "${fmt}" "${frame}")"
    b5="$(frame_path "${rd}" 5 "${fmt}" "${frame}")"
    b6="$(frame_path "${rd}" 6 "${fmt}" "${frame}")"
    b7="$(frame_path "${rd}" 7 "${fmt}" "${frame}")"
    b8="$(frame_path "${rd}" 8 "${fmt}" "${frame}")"
    b9="$(frame_path "${rd}" 9 "${fmt}" "${frame}")"
    b10="$(frame_path "${rd}" 10 "${fmt}" "${frame}")"
    b11="$(frame_path "${rd}" 11 "${fmt}" "${frame}")"
    b12="$(frame_path "${rd}" 12 "${fmt}" "${frame}")"
    b13="$(frame_path "${rd}" 13 "${fmt}" "${frame}")"
    b14="$(frame_path "${rd}" 14 "${fmt}" "${frame}")"

    if [ "${controls_left}" -gt 0 ]; then
      if [ "${USE_DRAW_OVER}" -eq 1 ]; then
        local tmp="${RAM_DIR}/tmp"
        mkdir -p "${tmp}"
        cp -f "${b1}" "${tmp}/b1.png"
        cp -f "${b11}" "${tmp}/b11.png"
        cp -f "${b12}" "${tmp}/b12.png"
        cp -f "${b13}" "${tmp}/b13.png"
        "${ROOT}/icons/draw_over" "${ctrl_stop}" "${tmp}/b1.png" >/dev/null 2>&1 || true
        "${ROOT}/icons/draw_over" "${ctrl_back}" "${tmp}/b11.png" >/dev/null 2>&1 || true
        if [ "${paused}" -eq 1 ]; then
          "${ROOT}/icons/draw_over" "${ctrl_play}" "${tmp}/b12.png" >/dev/null 2>&1 || true
        else
          "${ROOT}/icons/draw_over" "${ctrl_pause}" "${tmp}/b12.png" >/dev/null 2>&1 || true
        fi
        "${ROOT}/icons/draw_over" "${ctrl_fwd}" "${tmp}/b13.png" >/dev/null 2>&1 || true
        b1="${tmp}/b1.png"
        b11="${tmp}/b11.png"
        b12="${tmp}/b12.png"
        b13="${tmp}/b13.png"
      else
        b1="${ctrl_stop}"
        b11="${ctrl_back}"
        if [ "${paused}" -eq 1 ]; then b12="${ctrl_play}"; else b12="${ctrl_pause}"; fi
        b13="${ctrl_fwd}"
      fi
      if [ "${paused}" -eq 0 ]; then
        controls_left=$((controls_left - 1))
      fi
    fi

    send_page_14 "${b1}" "${b2}" "${b3}" "${b4}" "${b5}" "${b6}" "${b7}" "${b8}" "${b9}" "${b10}" "${b11}" "${b12}" "${b13}" "${b14}"
    if [ "${advance_frame}" -eq 1 ]; then
      frame=$((frame + 1))
    fi
    need_redraw=0
  done
}

main() {
  [ -S "${ULANZI_SOCK}" ] || { echo "Missing ulanzi socket: ${ULANZI_SOCK}" >&2; exit 1; }
  [ -d "${VIDEOS_DIR}" ] || { echo "Missing videos dir: ${VIDEOS_DIR}" >&2; exit 1; }

  ensure_ctrl_icons

  pg_cmd "stop-control"

  mapfile -t videos < <(find "${VIDEOS_DIR}" -maxdepth 1 -type f -iname '*.mp4' | sort)
  if [ "${#videos[@]}" -eq 0 ]; then
    echo "No MP4 found in ${VIDEOS_DIR}" >&2
  fi

  # Button stream (persistent)
  coproc RB { socat - UNIX-CONNECT:"${ULANZI_SOCK}"; }
  printf 'read-buttons\n' >&"${RB[1]}"
  read -r okline <&"${RB[0]}" || true

  local offset=0
  menu_render videos "${offset}"

  while true; do
    local line=""
    if ! read -r line <&"${RB[0]}"; then
      sleep 0.1
      continue
    fi
    local parsed btn evt
    if ! parsed="$(parse_btn_evt "${line}")"; then
      continue
    fi
    btn="$(printf "%s" "${parsed}" | awk '{print $1}')"
    evt="$(printf "%s" "${parsed}" | awk '{print $2}')"

    if [ "${evt}" = "TAP" ]; then
      if [ "${btn}" -eq 11 ]; then
        return 0
      elif [ "${btn}" -eq 12 ]; then
        if [ "${offset}" -ge "${MENU_PAGE_SIZE}" ]; then
          offset=$((offset - MENU_PAGE_SIZE))
          menu_render videos "${offset}"
        fi
      elif [ "${btn}" -eq 13 ]; then
        if [ $((offset + MENU_PAGE_SIZE)) -lt "${#videos[@]}" ]; then
          offset=$((offset + MENU_PAGE_SIZE))
          menu_render videos "${offset}"
        fi
      elif [ "${btn}" -ge 1 ] && [ "${btn}" -le "${MENU_PAGE_SIZE}" ]; then
        local idx=$((offset + btn - 1))
        if [ "${idx}" -lt "${#videos[@]}" ]; then
          local v="${videos[${idx}]}"
          if has_rendered_video "${v}"; then
            playback_loop "${v}" <&"${RB[0]}"
            menu_render videos "${offset}"
          fi
        fi
      fi
    elif [ "${evt}" = "HOLD" ] || [ "${evt}" = "LONGHOLD" ]; then
      if [ "${btn}" -ge 1 ] && [ "${btn}" -le "${MENU_PAGE_SIZE}" ]; then
        local idx=$((offset + btn - 1))
        if [ "${idx}" -lt "${#videos[@]}" ]; then
          local v="${videos[${idx}]}"
          if [ -f "${v}" ]; then
            if ! is_video_target_format "${v}"; then
              local st="${RAM_DIR}/tmp/convert_${btn}.png"
              make_status_tile "convert..." "${st}"
              set_partial_btn_icon "${btn}" "${st}"
              convert_in_place_if_needed "${v}" >/dev/null 2>&1 || true
            fi

            local st2="${RAM_DIR}/tmp/render_${btn}.png"
            make_status_tile "render..." "${st2}"
            set_partial_btn_icon "${btn}" "${st2}"

            "${ROOT}/bin/send_video_page_wrapper" -d -q=60 -r "${v}" || true

            # Restore UI
            drain_rb_events 0.3 <&"${RB[0]}" || true
          fi
          menu_render videos "${offset}"
        fi
      fi
    fi
  done
}

main "$@"
