#!/usr/bin/env bash
set -euo pipefail

usage() {
  cat >&2 <<'EOF'
Usage:
  clear_miniapp_cache.sh --text
  clear_miniapp_cache.sh --clear

--text  prints exactly two lines:
  "<N> files"
  "<SIZE>" (B/KB/MB/GB)

--clear prints nothing (best-effort delete).
EOF
}

action=""
for arg in "$@"; do
  case "$arg" in
    --text|--clear) action="${arg#--}" ;;
    -h|--help) usage; exit 0 ;;
  esac
done

if [[ -z "${action}" ]]; then
  usage
  exit 2
fi

ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)"
target="${ROOT}/mymedia/miniapp/.cache"

human_size() {
  local bytes="$1"
  local unit="B"
  local value="$bytes"

  if (( bytes >= 1024 )); then
    unit="KB"
    value="$(awk -v b="$bytes" 'BEGIN{printf "%.1f", b/1024}')"
    if (( bytes >= 1024*1024 )); then
      unit="MB"
      value="$(awk -v b="$bytes" 'BEGIN{printf "%.1f", b/(1024*1024)}')"
      if (( bytes >= 1024*1024*1024 )); then
        unit="GB"
        value="$(awk -v b="$bytes" 'BEGIN{printf "%.1f", b/(1024*1024*1024)}')"
      fi
    fi
  fi

  printf "%s%s" "$value" "$unit"
}

count_and_size_dir() {
  local dir="$1"
  if [[ ! -d "$dir" ]]; then
    printf "0\n0\n"
    return 0
  fi

  local files
  files="$(find "$dir" -type f -printf '.' 2>/dev/null | wc -c | tr -d ' ')"
  if [[ -z "$files" ]]; then files=0; fi

  local bytes
  bytes="$(find "$dir" -type f -printf '%s\n' 2>/dev/null | awk '{s+=$1} END{print s+0}')"
  if [[ -z "$bytes" ]]; then bytes=0; fi

  printf "%s\n%s\n" "$files" "$bytes"
}

if [[ "${action}" == "clear" ]]; then
  rm -rf -- "${target}" 2>/dev/null || true
  mkdir -p -- "${target}" 2>/dev/null || true
  exit 0
fi

if [[ "${action}" == "text" ]]; then
  mapfile -t _cs < <(count_and_size_dir "${target}")
  files="${_cs[0]:-0}"
  bytes="${_cs[1]:-0}"
  printf "%s files\n%s\n" "${files}" "$(human_size "${bytes}")"
  exit 0
fi

usage
exit 2

