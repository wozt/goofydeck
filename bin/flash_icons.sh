#!/usr/bin/env bash
# Description: continuously flash up to 13 shuffled icons from files or directory to D200 via send_page.sh
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

SENDER="${ROOT}/bin/send_page.sh"
INTERVAL_MS="40"
TARGETS=()
SHOW_HELP="${ROOT}/show_help.sh"
DEBUG_LOGS=0

while [ "$#" -gt 0 ]; do
  case "$1" in
    --interval|-i|-t)
      INTERVAL_MS="${2:-}"
      shift 2 || { exec "${SHOW_HELP}" "$(basename "$0")"; }
      ;;
    --debug)
      DEBUG_LOGS=1
      shift
      ;;
    --help|-h)
      exec "${SHOW_HELP}" "$(basename "$0")";;
    *)
      if [[ "$1" == -* ]]; then
        echo "Unknown option: $1" >&2
        exec "${SHOW_HELP}" "$(basename "$0")"
      fi
      TARGETS+=("$1"); shift;;
  esac
done

if [ "${#TARGETS[@]}" -eq 0 ]; then
  exec "${SHOW_HELP}" "$(basename "$0")"
fi

if [ -z "${INTERVAL_MS}" ]; then
  INTERVAL_MS=0
elif ! [[ "${INTERVAL_MS}" =~ ^[0-9]+$ ]]; then
  echo "Interval invalid, falling back to 0ms." >&2
  INTERVAL_MS=0
fi

INTERVAL_SEC=$(awk "BEGIN { printf \"%.3f\", ${INTERVAL_MS}/1000 }")

if [ ! -x "${SENDER}" ]; then
  echo "Missing sender script at ${SENDER}" >&2
  exit 1
fi

if [ "${DEBUG_LOGS}" -eq 1 ]; then
  export ULANZI_DEBUG=1
  echo "Debug logging enabled (ULANZI_DEBUG=1)" >&2
fi

echo "Sending random icons to all 13 buttons..."
start_ts=$(date +%s%3N)

while true; do
  icons=()
  if [ "${#TARGETS[@]}" -eq 1 ]; then
    dir="${TARGETS[0]}"
    if [ ! -d "${dir}" ] && [[ "${dir}" != /* ]] && [ -d "${ROOT}/${dir}" ]; then
      dir="${ROOT}/${dir}"
    fi
    if [ -d "${dir}" ]; then
      mapfile -t icons < <(find "${dir}" -maxdepth 1 -type f \( -iname '*.png' -o -iname '*.webp' -o -iname '*.jpg' -o -iname '*.jpeg' \) | shuf | head -n 13)
    fi
  else
    for f in "${TARGETS[@]}"; do
      if [ -f "${f}" ]; then icons+=("${f}"); continue; fi
      if [[ "${f}" != /* ]] && [ -f "${ROOT}/${f}" ]; then icons+=("${ROOT}/${f}"); continue; fi
      echo "File not found: ${f} (tried as-is and under ${ROOT}/)" >&2
      exit 1
    done
    if [ "${#icons[@]}" -gt 13 ]; then
      echo "More than 13 files provided; using first 13." >&2
      icons=("${icons[@]:0:13}")
    fi
  fi

  if [ "${#icons[@]}" -eq 0 ]; then
    exec "${SHOW_HELP}" "$(basename "$0")"
  fi

  mapfile -t icons < <(printf '%s\n' "${icons[@]}" | shuf)
  icons=("${icons[@]:0:13}")
  output="$("${SENDER}" "${icons[@]}")"
  now_runtime=$(date +%s%3N)
  runtime_ms=$((now_runtime - start_ts))
  up_secs=$(LC_ALL=C awk "BEGIN { printf \"%.3f\", ${runtime_ms}/1000 }")
  printf "\r\033[K%s | up:%ss" "${output}" "${up_secs}"
  sleep "${INTERVAL_SEC}"
done
