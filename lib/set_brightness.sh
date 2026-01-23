#!/usr/bin/env bash
# Description: set Ulanzi D200 brightness (0-100) via ulanzi_d200_demon daemon
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

SHOW_HELP="${ROOT}/show_help.sh"
SOCK_PATH="/tmp/ulanzi_device.sock"
VALUE=""

while [ "$#" -gt 0 ]; do
  case "$1" in
    -h|--help)
      exec "${SHOW_HELP}" "$(basename "$0")"
      ;;
    --socket)
      SOCK_PATH="${2:-}"
      shift 2 || exec "${SHOW_HELP}" "$(basename "$0")"
      ;;
    --socket=*)
      SOCK_PATH="${1#*=}"
      shift
      ;;
    *)
      if [ -z "${VALUE}" ]; then
        VALUE="$1"
      else
        echo "Unexpected argument: $1" >&2
        exec "${SHOW_HELP}" "$(basename "$0")"
      fi
      shift
      ;;
  esac
done

if [ -z "${VALUE}" ]; then
  exec "${SHOW_HELP}" "$(basename "$0")"
fi

if ! [[ "${VALUE}" =~ ^[0-9]+$ ]]; then
  echo "Brightness must be an integer 0-100." >&2
  exit 1
fi
if [ "${VALUE}" -lt 0 ] || [ "${VALUE}" -gt 100 ]; then
  echo "Brightness must be between 0 and 100." >&2
  exit 1
fi

resp="$(printf 'set-brightness %s\n' "${VALUE}" | nc -U "${SOCK_PATH}" || true)"
if [ "${resp}" = "ok" ]; then
  echo "Set brightness to ${VALUE}%"
else
  echo "set_brightness.sh failed (response: ${resp:-<no response>})" >&2
  exit 1
fi
