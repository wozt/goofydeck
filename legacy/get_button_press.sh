#!/usr/bin/env bash
# GoofyDeck button press forwarder daemon.
# Connects to ulanzi_d200_demon (read-buttons) and pushes events to paging.sh socket.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

ULANZI_SOCK="/tmp/ulanzi_device.sock"
PAGING_SOCK="/tmp/goofydeck_paging.sock"
RECONNECT_DELAY_SEC=1
VERBOSE=1
DEBOUNCE_MS=150

usage() {
  cat >&2 <<EOF
Usage:
  ./get_button_press.sh --daemon [options]

Options:
  --ulanzi-sock <path>   (default: ${ULANZI_SOCK})
  --paging-sock <path>   (default: ${PAGING_SOCK})
  --reconnect <sec>      (default: ${RECONNECT_DELAY_SEC})
  --debounce-ms <ms>     (default: ${DEBOUNCE_MS})
  --quiet                (no per-event output)
EOF
}

log() { echo "[get_button_press] $*" >&2; }

have() { command -v "$1" >/dev/null 2>&1; }

now_ms() {
  if command -v date >/dev/null 2>&1 && date +%s%3N >/dev/null 2>&1; then
    date +%s%3N
  else
    echo $(( $(date +%s) * 1000 ))
  fi
}

should_forward() {
  local key="$1"
  local now last
  now="$(now_ms)"
  last="${LAST_MS[$key]:-}"
  if [[ "${last}" =~ ^[0-9]+$ ]] && [ $((now - last)) -lt "${DEBOUNCE_MS}" ]; then
    return 1
  fi
  LAST_MS[$key]="${now}"
  return 0
}

push_event() {
  local btn="$1" evt="$2"
  # Best-effort: one connection per event.
  if [ "${VERBOSE}" -eq 1 ]; then
    resp="$(printf 'press %s %s\n' "${btn}" "${evt}" | nc -U "${PAGING_SOCK}" 2>/dev/null || true)"
    printf "[get_button_press] button %s %s -> %s\n" "${btn}" "${evt}" "${resp:-no_response}" >&2
  else
    printf 'press %s %s\n' "${btn}" "${evt}" | nc -U "${PAGING_SOCK}" >/dev/null 2>&1 || true
  fi
}

run_once() {
  # Keep a single connection open for streaming events.
  coproc NC { nc -U "${ULANZI_SOCK}"; }
  local nc_out="${NC[0]}"
  local nc_in="${NC[1]}"

  # Register for events
  printf 'read-buttons\n' >&"${nc_in}" || return 1

  # Read initial response ("ok")
  local line
  if ! IFS= read -r line <&"${nc_out}"; then
    return 1
  fi
  if [ "${VERBOSE}" -eq 1 ]; then
    log "Subscribed (resp='${line}')"
  fi

  # Stream events: "button N TAP|HOLD|RELEASED"
  while IFS= read -r line <&"${nc_out}"; do
    case "${line}" in
      button\ *)
        # Example: button 3 TAP
        local _ b e
        read -r _ b e <<<"${line}"
        if [[ "${b}" =~ ^[0-9]+$ ]] && [[ "${e}" =~ ^(TAP|HOLD|RELEASED)$ ]]; then
          if [ "${e}" = "RELEASED" ]; then e="RELEASE"; fi
          if should_forward "${b}:${e}"; then
            push_event "${b}" "${e}"
          fi
        fi
        ;;
      *)
        if [ "${VERBOSE}" -eq 1 ]; then
          log "rx: ${line}"
        fi
        ;;
    esac
  done
  return 1
}

daemon_main() {
  if ! have nc; then
    log "nc is required"
    exit 1
  fi

  declare -gA LAST_MS=()
  log "Forwarding events: ulanzi=${ULANZI_SOCK} -> paging=${PAGING_SOCK}"
  while true; do
    if [ ! -S "${ULANZI_SOCK}" ]; then
      sleep "${RECONNECT_DELAY_SEC}"
      continue
    fi
    run_once || true
    sleep "${RECONNECT_DELAY_SEC}"
  done
}

mode=""
while [ "$#" -gt 0 ]; do
  case "$1" in
    --daemon) mode="daemon"; shift ;;
    --ulanzi-sock) ULANZI_SOCK="${2:-}"; shift 2 ;;
    --paging-sock) PAGING_SOCK="${2:-}"; shift 2 ;;
    --reconnect) RECONNECT_DELAY_SEC="${2:-1}"; shift 2 ;;
    --debounce-ms) DEBOUNCE_MS="${2:-150}"; shift 2 ;;
    --quiet) VERBOSE=0; shift ;;
    -h|--help) usage; exit 0 ;;
    *) echo "Unknown arg: $1" >&2; usage; exit 2 ;;
  esac
done

case "${mode}" in
  daemon) daemon_main ;;
  *) usage; exit 2 ;;
esac
