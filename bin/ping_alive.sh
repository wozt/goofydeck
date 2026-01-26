#!/usr/bin/env bash
# Description: send a single keep-alive ping to the Ulanzi D200 via ulanzi_d200_daemon daemon.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

SOCK_PATH="/tmp/ulanzi_device.sock"
SHOW_HELP="${ROOT}/show_help.sh"
VERBOSE=1

while [ "$#" -gt 0 ]; do
  case "$1" in
    --socket)
      SOCK_PATH="${2:-}"
      shift 2 || exec "${SHOW_HELP}" "$(basename "$0")"
      ;;
    --socket=*)
      SOCK_PATH="${1#*=}"
      shift
      ;;
    --no-verbose)
      VERBOSE=0
      shift
      ;;
    -h|--help)
      exec "${SHOW_HELP}" "$(basename "$0")"
      ;;
    *)
      echo "Unknown argument: $1" >&2
      exec "${SHOW_HELP}" "$(basename "$0")"
      ;;
  esac
done

resp="$(printf 'ping\n' | nc -U "${SOCK_PATH}" || true)"
if [ "${resp}" = "ok" ]; then
  if [ "${VERBOSE}" -eq 1 ]; then
    echo "[ping_alive] ping sent"
  fi
else
  echo "ping_alive.sh failed (response: ${resp:-<no response>})" >&2
  exit 1
fi
