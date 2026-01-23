#!/usr/bin/env bash
# Description: send up to 13 icons (explicit files) across buttons 1-13 using ulanzi_d200_demon daemon
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

SHOW_HELP="${ROOT}/show_help.sh"
SOCK_PATH="/tmp/ulanzi_device.sock"
TARGETS=()
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
    --label-[1-9]=*|--label-1[0-3]=*)
      num="${1#--label-}"
      btn="${num%%=*}"
      val="${num#*=}"
      LABEL_MAP["${btn}"]="${val}"
      shift
      ;;
    --label-[1-9]|--label-1[0-3])
      num="${1#--label-}"
      btn="${num}"
      val="${2:-}"
      LABEL_MAP["${btn}"]="${val}"
      shift 2 || exec "${SHOW_HELP}" "$(basename "$0")"
      ;;
    *)
      TARGETS+=("$1")
      shift
      ;;
  esac
done

if [ "${#TARGETS[@]}" -eq 0 ]; then
  exec "${SHOW_HELP}" "$(basename "$0")"
fi

# Resolve list of icons (explicit only)
icons=()
for f in "${TARGETS[@]}"; do
  resolved="$(resolve_file "${f}" || true)"
  if [ -z "${resolved}" ]; then
    echo "File not found: ${f} (tried as-is and under ${ROOT}/)" >&2
    exit 1
  fi
  icons+=("${resolved}")
done

if [ "${#icons[@]}" -eq 0 ]; then
  echo "No PNG files found to send." >&2
  exit 1
fi
if [ "${#icons[@]}" -gt 13 ]; then
  icons=("${icons[@]:0:13}")
fi

cmd="set-buttons-explicit"
for i in "${!icons[@]}"; do
  btn=$((i + 1))
  cmd+=" --button-${btn}=${icons[$i]}"
  if [ -n "${LABEL_MAP[$btn]:-}" ]; then
    cmd+=" --label-${btn}=${LABEL_MAP[$btn]}"
  fi
done

resp="$(printf '%s\n' "${cmd}" | nc -U "${SOCK_PATH}" || true)"
if [ "${resp}" = "ok" ]; then
  echo "Sent ${#icons[@]} icon(s) to buttons 1-${#icons[@]}"
else
  echo "send_page.sh failed (response: ${resp:-<no response>})" >&2
  exit 1
fi
