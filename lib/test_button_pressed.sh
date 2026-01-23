#!/usr/bin/env bash
# Description: loop and print button press events from ulanzi_d200_demon daemon (CTRL+C to stop).
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

SOCK_PATH="/tmp/ulanzi_device.sock"
SHOW_HELP="${ROOT}/show_help.sh"

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
    -h|--help)
      exec "${SHOW_HELP}" "$(basename "$0")"
      ;;
    *)
      echo "Unknown argument: $1" >&2
      exec "${SHOW_HELP}" "$(basename "$0")"
      ;;
  esac
done

echo "Listening for button events... (CTRL+C to exit)"
printf 'read-buttons\n' | nc -U "${SOCK_PATH}"
