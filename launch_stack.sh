#!/usr/bin/env bash
# Manual launcher for local testing (no systemd).
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

CONFIG_PATH="${ROOT}/config/configuration.yml"
ULANZI_SOCK="/tmp/ulanzi_device.sock"
PAGING_SOCK="/tmp/goofydeck_paging.sock"
HA_SOCK="/tmp/goofydeck_ha.sock"
TMUX_BASE="goofydeck"
TMUX_SESSION="${TMUX_BASE}"

PID_DIR_DEFAULT="/dev/shm/goofydeck/pids"
PID_DIR="${PID_DIR:-${PID_DIR_DEFAULT}}"
if ! mkdir -p "${PID_DIR}" 2>/dev/null; then
  PID_DIR="${ROOT}/.cache/pids"
  mkdir -p "${PID_DIR}"
  echo "[launch] WARN: unable to use ${PID_DIR_DEFAULT}; using ${PID_DIR} instead" >&2
fi

kill_all() {
  self_pid="$$"
  mux_bin="$(command -v byobu-tmux 2>/dev/null || command -v tmux 2>/dev/null || true)"

  # Kill tmux session if present.
  if [ -n "${mux_bin}" ]; then
    # Also kills legacy sessions from older launcher versions.
    for s in "${TMUX_SESSION}" "${TMUX_BASE}-monitor" "${TMUX_BASE}-paging" "${TMUX_BASE}-ulanzi"; do
      if "${mux_bin}" has-session -t "${s}" >/dev/null 2>&1; then
        echo "[launch] Killing tmux session ${s}..."
        "${mux_bin}" kill-session -t "${s}" || true
      fi
    done
  fi

  # Kill background PIDs (non-tmux launch).
  if ls "${PID_DIR}"/*.pid >/dev/null 2>&1; then
    echo "[launch] Stopping daemons from ${PID_DIR}..."
    pids=()
    for f in "${PID_DIR}"/*.pid; do
      [ -f "${f}" ] || continue
      pid="$(cat "${f}" 2>/dev/null || true)"
      if [[ "${pid}" =~ ^[0-9]+$ ]]; then
        pids+=("${pid}")
      fi
    done
    if [ "${#pids[@]}" -gt 0 ]; then
      kill -TERM "${pids[@]}" 2>/dev/null || true
      # give them a moment
      sleep 0.3
      kill -KILL "${pids[@]}" 2>/dev/null || true
    fi
    rm -f "${PID_DIR}"/*.pid 2>/dev/null || true
  fi

  # Fallback: kill by command line (covers manual runs and tmux runs that didn't write pid files).
  if command -v pgrep >/dev/null 2>&1; then
    extra_pids=()
    while IFS= read -r p; do extra_pids+=("$p"); done < <(pgrep -f "${ROOT}/ulanzi_d200_demon" 2>/dev/null || true)
    while IFS= read -r p; do extra_pids+=("$p"); done < <(pgrep -f "${ROOT}/pagging_demon" 2>/dev/null || true)
    while IFS= read -r p; do extra_pids+=("$p"); done < <(pgrep -f "${ROOT}/ha_demon" 2>/dev/null || true)

    # De-dup + exclude ourselves
    uniq_pids=()
    for pid in "${extra_pids[@]}"; do
      [[ "${pid}" =~ ^[0-9]+$ ]] || continue
      [ "${pid}" = "${self_pid}" ] && continue
      found=0
      for up in "${uniq_pids[@]}"; do
        [ "${up}" = "${pid}" ] && found=1 && break
      done
      [ "${found}" -eq 0 ] && uniq_pids+=("${pid}")
    done

    if [ "${#uniq_pids[@]}" -gt 0 ]; then
      echo "[launch] Stopping daemons by pattern..."
      kill -TERM "${uniq_pids[@]}" 2>/dev/null || true
      sleep 0.3
      kill -KILL "${uniq_pids[@]}" 2>/dev/null || true
    fi
  fi

  # Cleanup sockets (best-effort).
  rm -f "${ULANZI_SOCK}" 2>/dev/null || true
  echo "[launch] Done."
}

start_background() {
  [ -x "${ROOT}/ulanzi_d200_demon" ] || { echo "Missing ${ROOT}/ulanzi_d200_demon (run: make all)" >&2; exit 1; }
  [ -x "${ROOT}/lib/pagging_demon" ] || { echo "Missing ${ROOT}/lib/pagging_demon (run: make all)" >&2; exit 1; }
  [ -x "${ROOT}/lib/ha_demon" ] || { echo "Missing ${ROOT}/lib/ha_demon (run: make all)" >&2; exit 1; }
  [ -f "${CONFIG_PATH}" ] || { echo "Missing config: ${CONFIG_PATH}" >&2; exit 1; }

  echo "[launch] Starting ulanzi_d200_demon..."
  rm -f "${ULANZI_SOCK}" 2>/dev/null || true
  ("${ROOT}/ulanzi_d200_demon") &
  echo $! >"${PID_DIR}/ulanzi_d200_demon.pid"

  echo "[launch] Starting ha_demon..."
  rm -f "${HA_SOCK}" 2>/dev/null || true
  ("${ROOT}/lib/ha_demon") &
  echo $! >"${PID_DIR}/ha_demon.pid"

  echo "[launch] Sleeping 10s before starting paging..."
  sleep 10

  echo "[launch] Starting pagging_demon..."
  ("${ROOT}/lib/pagging_demon") &
  echo $! >"${PID_DIR}/pagging_demon.pid"

  echo "[launch] Running."
  echo "[launch] PIDs:"
  ls -la "${PID_DIR}"
}

start_byobu() {
  mux_bin="$(command -v byobu-tmux 2>/dev/null || true)"
  [ -n "${mux_bin}" ] || { echo "byobu-tmux not found (install byobu)" >&2; exit 1; }
  [ -x "${ROOT}/ulanzi_d200_demon" ] || { echo "Missing ${ROOT}/ulanzi_d200_demon (run: make all)" >&2; exit 1; }
  [ -x "${ROOT}/lib/pagging_demon" ] || { echo "Missing ${ROOT}/lib/pagging_demon (run: make all)" >&2; exit 1; }
  [ -x "${ROOT}/lib/ha_demon" ] || { echo "Missing ${ROOT}/lib/ha_demon (run: make all)" >&2; exit 1; }
  [ -f "${CONFIG_PATH}" ] || { echo "Missing config: ${CONFIG_PATH}" >&2; exit 1; }

  if "${mux_bin}" has-session -t "${TMUX_SESSION}" >/dev/null 2>&1; then
    echo "[launch] byobu/tmux session ${TMUX_SESSION} already exists; attach with: byobu-tmux attach -t ${TMUX_SESSION}" >&2
    exit 2
  fi

  rm -f "${ULANZI_SOCK}" 2>/dev/null || true
  rm -f "${HA_SOCK}" 2>/dev/null || true

  echo "[launch] Starting byobu session ${TMUX_SESSION} (3 panes)..."
  "${mux_bin}" new-session -d -s "${TMUX_SESSION}" -n stack
  "${mux_bin}" set-option -t "${TMUX_SESSION}" -g mouse on >/dev/null 2>&1 || true

  # Pane 0: ulanzi daemon
  "${mux_bin}" send-keys -t "${TMUX_SESSION}:stack.0" "cd \"${ROOT}\"; echo \\$\\$ >\"${PID_DIR}/ulanzi_d200_demon.pid\"; exec ./ulanzi_d200_demon" C-m

  # Pane 1: pagging daemon (delayed)
  "${mux_bin}" split-window -t "${TMUX_SESSION}:stack.0" -h
  "${mux_bin}" send-keys -t "${TMUX_SESSION}:stack.1" "cd \"${ROOT}\"; echo \\$\\$ >\"${PID_DIR}/pagging_demon.pid\"; echo '[launch] sleep 10s before paging...' >&2; sleep 10; exec ./lib/pagging_demon" C-m

  # Pane 2: ha_demon
  "${mux_bin}" split-window -t "${TMUX_SESSION}:stack.1" -h
  "${mux_bin}" send-keys -t "${TMUX_SESSION}:stack.2" "cd \"${ROOT}\"; echo \\$\\$ >\"${PID_DIR}/ha_demon.pid\"; exec ./lib/ha_demon" C-m

  "${mux_bin}" select-layout -t "${TMUX_SESSION}:stack" even-horizontal
  "${mux_bin}" select-pane -t "${TMUX_SESSION}:stack.0"
  echo "[launch] Attaching..."
  exec byobu-tmux attach -t "${TMUX_SESSION}"
}

usage() {
  cat >&2 <<EOF
Usage: ./launch_stack.sh [options]

Options:
  --kill               Stop all daemons started by launch_stack.sh (also kills byobu sessions ${TMUX_BASE} and ${TMUX_BASE}-*)
  --byobu              Run daemons inside byobu-tmux (session: ${TMUX_SESSION})
  --tmux               Alias for --byobu
  --config <path>       (default: ${CONFIG_PATH})
  --ulanzi-sock <path>  (default: ${ULANZI_SOCK})
  --paging-sock <path>  (default: ${PAGING_SOCK})

Starts:
  1) ./ulanzi_d200_demon
  2) ./lib/ha_demon
  3) ./lib/pagging_demon

Stop:
  ./launch_stack.sh --kill
EOF
}

MODE="start"
while [ "$#" -gt 0 ]; do
  case "$1" in
    --kill) MODE="kill"; shift ;;
    --byobu) MODE="byobu"; shift ;;
    --tmux) MODE="byobu"; shift ;;
    --config) CONFIG_PATH="${2:-}"; shift 2 ;;
    --ulanzi-sock) ULANZI_SOCK="${2:-}"; shift 2 ;;
    --paging-sock) PAGING_SOCK="${2:-}"; shift 2 ;;
    -h|--help) usage; exit 0 ;;
    *) echo "Unknown arg: $1" >&2; usage; exit 2 ;;
  esac
done

case "${MODE}" in
  kill) kill_all ;;
  byobu) start_byobu ;;
  start) start_background ;;
  *) usage; exit 2 ;;
esac
