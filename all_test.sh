#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "${ROOT}"

COLOR_BORDER="${COLOR_BORDER:-111111}"
COLOR_MDI="${COLOR_MDI:-FFFFFF}"
MDI_ICON="${MDI_ICON:-mdi:dev-to}"
SIZE="${SIZE:-196}"
INNER_SIZE="${INNER_SIZE:-172}"
RADIUS="${RADIUS:-22}"
MDI_SIZE="${MDI_SIZE:-120}"
OPT_COLORS="${OPT_COLORS:-64}"

VIDEO="${VIDEO:-test_video/skrillex_10s.mp4}"
RENDER_DELAY_MS="${RENDER_DELAY_MS:-66}"
PLAY_SECONDS="${PLAY_SECONDS:-5}"

SKIP_VIDEO=0
SKIP_PLAY=0

usage() {
  cat >&2 <<EOF
Usage: ./all_test.sh [options]

Runs a full test battery:
  1) make clean all
  2) standard icon (BASH pipeline)
  3) standard icon (C pipeline)
  4) render video (${VIDEO})
  5) play rendered folder (best-effort)

Options:
  --skip-video     Skip video render
  --skip-play      Skip video playback
  -h, --help       Show this help

Env overrides:
  VIDEO=<path>                 (default: ${VIDEO})
  MDI_ICON=<mdi:name>          (default: ${MDI_ICON})
  COLOR_BORDER=<RRGGBB>        (default: ${COLOR_BORDER})
  COLOR_MDI=<RRGGBB>           (default: ${COLOR_MDI})
  SIZE=<n<=196>                (default: ${SIZE})
  INNER_SIZE=<n<=196>          (default: ${INNER_SIZE})
  RADIUS=<0..50>               (default: ${RADIUS})
  MDI_SIZE=<n<=196>            (default: ${MDI_SIZE})
  OPT_COLORS=<1..256>          (default: ${OPT_COLORS})
  RENDER_DELAY_MS=<ms>         (default: ${RENDER_DELAY_MS})
  PLAY_SECONDS=<seconds>       (default: ${PLAY_SECONDS})
EOF
}

log() { echo "[all_test] $*"; }
warn() { echo "[all_test] WARN: $*" >&2; }

have() { command -v "$1" >/dev/null 2>&1; }

run_step() {
  local name="$1"
  shift
  log "==> ${name}"
  "$@"
}

best_effort() {
  local name="$1"
  shift
  log "==> ${name} (best-effort)"
  set +e
  "$@"
  local rc=$?
  set -e
  if [ $rc -ne 0 ]; then
    warn "${name} failed (rc=${rc})"
  fi
  return 0
}

while [ "$#" -gt 0 ]; do
  case "$1" in
    --skip-video) SKIP_VIDEO=1; shift ;;
    --skip-play) SKIP_PLAY=1; shift ;;
    -h|--help) usage; exit 0 ;;
    *) echo "Unknown argument: $1" >&2; usage; exit 2 ;;
  esac
done

ts="$(date +%Y%m%d_%H%M%S)"
BASH_ICON="./test_std_bash_${ts}.png"
C_ICON="./test_std_c_${ts}.png"

run_step "Compilation (make clean all)" make clean all

run_step "Standard icon (BASH) -> ${BASH_ICON}" bash -c \
  "./icons/draw_square.sh transparent --size=${SIZE} ${BASH_ICON} && \
   ./icons/draw_border.sh ${COLOR_BORDER} --size=${SIZE} --radius=${RADIUS} ${BASH_ICON} && \
   ./icons/draw_border.sh transparent --size=${INNER_SIZE} --radius=${RADIUS} ${BASH_ICON} && \
   ./icons/draw_mdi.sh ${MDI_ICON} ${COLOR_MDI} --size=${MDI_SIZE} ${BASH_ICON} && \
   ./icons/draw_optimize.sh -c ${OPT_COLORS} ${BASH_ICON}"

run_step "Standard icon (C) -> ${C_ICON}" bash -c \
  "./icons/draw_square transparent --size=${SIZE} ${C_ICON} && \
   ./icons/draw_border ${COLOR_BORDER} --size=${SIZE} --radius=${RADIUS} ${C_ICON} && \
   ./icons/draw_border transparent --size=${INNER_SIZE} --radius=${RADIUS} ${C_ICON} && \
   ./icons/draw_mdi ${MDI_ICON} ${COLOR_MDI} --size=${MDI_SIZE} ${C_ICON} && \
   ./icons/draw_optimize.sh -c ${OPT_COLORS} ${C_ICON}"

run_step "Verify icons exist" bash -c "ls -la ${BASH_ICON} ${C_ICON} && file ${BASH_ICON} ${C_ICON}"

if [ "${SKIP_VIDEO}" -eq 0 ]; then
  if [ ! -f "${VIDEO}" ]; then
    warn "Video not found: ${VIDEO} (skipping render)"
  else
    run_step "Render video -> rendered folder" ./lib/send_video_page_wrapper -r "${VIDEO}"
  fi
else
  log "Skipping video render (--skip-video)."
fi

if [ "${SKIP_PLAY}" -eq 0 ]; then
  render_dir="${VIDEO%.*}"
  if [ ! -d "${render_dir}" ]; then
    warn "Rendered folder not found: ${render_dir} (skipping play)"
  else
    if have timeout; then
      best_effort "Play rendered folder (${PLAY_SECONDS}s)" timeout "${PLAY_SECONDS}s" ./lib/play_rendered_video.sh --delay="${RENDER_DELAY_MS}" "${render_dir}"
    else
      warn "'timeout' not found; running play without timeout (CTRL+C to stop)"
      best_effort "Play rendered folder" ./lib/play_rendered_video.sh --delay="${RENDER_DELAY_MS}" "${render_dir}"
    fi
  fi
else
  log "Skipping video playback (--skip-play)."
fi

log "Done."
