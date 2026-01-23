#!/usr/bin/env bash
# Description: send specific PNGs to explicit buttons (1-13) on the D200 via ulanzi_d200_demon daemon
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

SHOW_HELP="${ROOT}/show_help.sh"
SOCK_PATH="/tmp/ulanzi_device.sock"
declare -A BUTTON_MAP=()
declare -A LABEL_MAP=()

abs_path() {
  local p="$1"
  if command -v realpath >/dev/null 2>&1; then
    realpath -m "$p"
  else
    if [[ "$p" == /* ]]; then
      printf "%s\n" "$p"
    else
      printf "%s/%s\n" "$(pwd -P)" "$p"
    fi
  fi
}

resolve_file() {
  local p="$1"
  if [ -f "$p" ]; then
    abs_path "$p"
    return 0
  fi
  if [[ "$p" != /* ]] && [ -f "${ROOT}/$p" ]; then
    abs_path "${ROOT}/$p"
    return 0
  fi
  if [[ "$p" != /* ]] && [ -f "${ROOT}/build/$p" ]; then
    abs_path "${ROOT}/build/$p"
    return 0
  fi
  return 1
}

while [ "$#" -gt 0 ]; do
  case "$1" in
    -h|--help)
      exec "${SHOW_HELP}" "$(basename "$0")"
      ;;
    --button-[1-9]=*|--button-1[0-3]=*)
      num="${1#--button-}"
      btn="${num%%=*}"
      file="${num#*=}"
      BUTTON_MAP["${btn}"]="${file}"
      shift
      ;;
    --label-[1-9]=*|--label-1[0-3]=*)
      num="${1#--label-}"
      btn="${num%%=*}"
      val="${num#*=}"
      LABEL_MAP["${btn}"]="${val}"
      shift
      ;;
    --button-[1-9]|--button-1[0-3])
      num="${1#--button-}"
      btn="${num}"
      file="${2:-}"
      BUTTON_MAP["${btn}"]="${file}"
      shift 2 || exec "${SHOW_HELP}" "$(basename "$0")"
      ;;
    --label-[1-9]|--label-1[0-3])
      num="${1#--label-}"
      btn="${num}"
      val="${2:-}"
      LABEL_MAP["${btn}"]="${val}"
      shift 2 || exec "${SHOW_HELP}" "$(basename "$0")"
      ;;
    --)
      shift
      break
      ;;
    -*)
      echo "Unknown option: $1" >&2
      exec "${SHOW_HELP}" "$(basename "$0")"
      ;;
    *)
      echo "Unknown positional argument: $1" >&2
      exec "${SHOW_HELP}" "$(basename "$0")"
      shift
      ;;
  esac
done

if [ "${#BUTTON_MAP[@]}" -eq 0 ] || [ "${#BUTTON_MAP[@]}" -gt 13 ]; then
  echo "Specify between 1 and 13 buttons with --button-N=<file.png>" >&2
  exec "${SHOW_HELP}" "$(basename "$0")"
fi

for btn in "${!BUTTON_MAP[@]}"; do
  if ! [[ "${btn}" =~ ^[0-9]+$ ]] || [ "${btn}" -lt 1 ] || [ "${btn}" -gt 13 ]; then
    echo "Invalid button number: ${btn} (must be 1-13)" >&2
    exit 1
  fi
  f="${BUTTON_MAP[$btn]}"
  resolved="$(resolve_file "${f}" || true)"
  if [ -z "${resolved}" ]; then
    echo "Icon file not found for button ${btn}: ${f} (tried as-is and under ${ROOT}/)" >&2
    exit 1
  fi
  BUTTON_MAP["${btn}"]="${resolved}"
done

# Build command for daemon
cmd="set-partial-explicit"
for btn in $(printf "%s\n" "${!BUTTON_MAP[@]}" | sort -n); do
  cmd+=" --button-${btn}=${BUTTON_MAP[$btn]}"
  if [ -n "${LABEL_MAP[$btn]:-}" ]; then
    cmd+=" --label-${btn}=${LABEL_MAP[$btn]}"
  fi
done

resp="$(printf '%s\n' "${cmd}" | nc -U "${SOCK_PATH}" || true)"
if [ "${resp}" = "ok" ]; then
  ordered="$(printf "%s\n" "${!BUTTON_MAP[@]}" | sort -n | tr '\n' ' ')"
  echo "Sent ${#BUTTON_MAP[@]} icon(s) to buttons [${ordered}]"
else
  echo "send_button.sh failed (response: ${resp:-<no response>})" >&2
  exit 1
fi
