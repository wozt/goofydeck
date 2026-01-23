#!/usr/bin/env bash
# Foreground button event forwarder:
# - subscribes to ulanzi_d200_demon "read-buttons"
# - prints events to stdout/stderr
# - forwards them to paging socket as: "press N TAP|HOLD|RELEASE"
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

ULANZI_SOCK="/tmp/ulanzi_device.sock"
PAGING_SOCK="/tmp/goofydeck_paging.sock"
SHOW_HELP="${ROOT}/show_help.sh"
FORWARD_TIMEOUT_MS=200

usage() {
  cat >&2 <<EOF
Usage: ./lib/get_button_pressed.sh [options]

Options:
  --ulanzi-sock <path>   (default: ${ULANZI_SOCK})
  --paging-sock <path>   (default: ${PAGING_SOCK})
  --no-forward           (print only; do not forward)
  --forward-timeout-ms N (default: ${FORWARD_TIMEOUT_MS})
  -h, --help             Show help

Output:
  Prints lines like: button N TAP/HOLD/RELEASE
  Forwards to paging.sh as: press N TAP
EOF
}

NO_FORWARD=0

while [ "$#" -gt 0 ]; do
  case "$1" in
    --ulanzi-sock) ULANZI_SOCK="${2:-}"; shift 2 ;;
    --paging-sock) PAGING_SOCK="${2:-}"; shift 2 ;;
    --no-forward) NO_FORWARD=1; shift ;;
    --forward-timeout-ms) FORWARD_TIMEOUT_MS="${2:-200}"; shift 2 ;;
    -h|--help) usage; exit 0 ;;
    *) echo "Unknown argument: $1" >&2; usage; exit 2 ;;
  esac
done

if ! command -v nc >/dev/null 2>&1; then
  echo "[get_button_pressed] ERROR: nc is required" >&2
  exit 1
fi

if [ ! -S "${ULANZI_SOCK}" ]; then
  echo "[get_button_pressed] ERROR: ulanzi socket not found: ${ULANZI_SOCK}" >&2
  exit 1
fi

echo "[get_button_pressed] Subscribing to ${ULANZI_SOCK} (read-buttons)..." >&2

forward_line() {
  local btn="$1" evt="$2"
  if [ "${NO_FORWARD}" -eq 1 ]; then
    return 0
  fi
  # Always forward TAP only (paging navigation is TAP-driven).
  [ "${evt}" = "TAP" ] || return 0
  if [ ! -S "${PAGING_SOCK}" ]; then
    echo "[get_button_pressed] WARN: paging socket missing: ${PAGING_SOCK}" >&2
    return 0
  fi
  if command -v timeout >/dev/null 2>&1; then
    # Avoid blocking this reader loop if paging is busy (e.g. rendering).
    resp="$(
      printf 'press %s %s\n' "${btn}" "${evt}" | timeout "${FORWARD_TIMEOUT_MS}ms" socat - UNIX-CONNECT:"${PAGING_SOCK}",connect-timeout=1 2>/dev/null || true
    )"
    if [ -n "${resp}" ]; then
      echo "[get_button_pressed] fwd press ${btn} ${evt} -> ${resp}" >&2
    else
      echo "[get_button_pressed] WARN: forward timed out/failed" >&2
    fi
  else
    # Best-effort async fallback (can spawn processes if you spam buttons).
    (printf 'press %s %s\n' "${btn}" "${evt}" | socat - UNIX-CONNECT:"${PAGING_SOCK}",connect-timeout=1 >/dev/null 2>&1 || true) &
  fi
}

# Subscribe; keep connection open and read events.
{
  printf 'read-buttons\n'
  # Keep stdin open; daemon only needs initial command but some nc versions close fast without it.
  cat >/dev/null
} | nc -U "${ULANZI_SOCK}" | while IFS= read -r line; do
  # Initial "ok"
  if [ "${line}" = "ok" ]; then
    echo "[get_button_pressed] Subscribed (ok)" >&2
    continue
  fi

  case "${line}" in
    button\ *)
      # daemon emits: "button N TAP" / "button N HOLD (..)" / "button N RELEASED"
      read -r _ btn rest <<<"${line}"
      evt="${rest%% *}"
      if [[ "${btn}" =~ ^[0-9]+$ ]]; then
        if [ "${evt}" = "RELEASED" ]; then evt="RELEASE"; fi
        if [[ "${evt}" =~ ^(TAP|HOLD|RELEASE)$ ]]; then
          echo "button ${btn} ${evt}"
          forward_line "${btn}" "${evt}"
        else
          echo "[get_button_pressed] rx: ${line}" >&2
        fi
      fi
      ;;
    *)
      echo "[get_button_pressed] rx: ${line}" >&2
      ;;
  esac
done
