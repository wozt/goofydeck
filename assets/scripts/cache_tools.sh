#!/usr/bin/env bash
set -euo pipefail

usage() {
  cat >&2 <<'EOF'
Usage:
  cache_tools.sh --ram|--disk --text
  cache_tools.sh --ram|--disk --clear

--text  prints exactly two lines:
  "<N> files"
  "<SIZE>" (B/KB/MB/GB)

--clear prints nothing (best-effort delete).
EOF
}

mode=""
action=""

for arg in "$@"; do
  case "$arg" in
    --ram|--disk) mode="${arg#--}" ;;
    --text|--clear) action="${arg#--}" ;;
    -h|--help) usage; exit 0 ;;
  esac
done

if [[ -z "${mode}" || -z "${action}" ]]; then
  usage
  exit 2
fi

ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)"

disk_dir="${ROOT}/.cache"
ram_dir="/dev/shm/goofydeck/paging"
ram_dir_uid="/dev/shm/goofydeck_$(id -u)/paging"
tmp_dir="/tmp/goofydeck_paging_$(id -u)"

target=""
case "$mode" in
  disk) target="$disk_dir" ;;
  ram) target="$ram_dir" ;;
  *) usage; exit 2 ;;
esac

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

if [[ "$action" == "clear" ]]; then
  if [[ "$mode" == "disk" ]]; then
    rm -rf -- "$disk_dir" 2>/dev/null || true
    mkdir -p -- "$disk_dir" 2>/dev/null || true
  else
    rm -rf -- "$ram_dir" 2>/dev/null || true
    rm -rf -- "$ram_dir_uid" 2>/dev/null || true
    rm -rf -- "$tmp_dir" 2>/dev/null || true

    mkdir -p -- "/dev/shm/goofydeck" 2>/dev/null || true
    mkdir -p -- "$ram_dir" 2>/dev/null || true
    chmod 0777 "/dev/shm/goofydeck" "$ram_dir" 2>/dev/null || true
  fi
  exit 0
fi

if [[ "$action" == "text" ]]; then
  if [[ "$mode" == "disk" ]]; then
    mapfile -t _cs < <(count_and_size_dir "$disk_dir")
    files="${_cs[0]:-0}"
    bytes="${_cs[1]:-0}"
  else
    mapfile -t _cs < <(count_and_size_dir "$ram_dir")
    files="${_cs[0]:-0}"
    bytes="${_cs[1]:-0}"
    # Also include fallback locations if they exist.
    if [[ -d "$ram_dir_uid" && "$ram_dir_uid" != "$ram_dir" ]]; then
      mapfile -t _cs2 < <(count_and_size_dir "$ram_dir_uid")
      f2="${_cs2[0]:-0}"
      b2="${_cs2[1]:-0}"
      files=$((files + f2))
      bytes=$((bytes + b2))
    fi
    if [[ -d "$tmp_dir" ]]; then
      mapfile -t _cs3 < <(count_and_size_dir "$tmp_dir")
      f3="${_cs3[0]:-0}"
      b3="${_cs3[1]:-0}"
      files=$((files + f3))
      bytes=$((bytes + b3))
    fi
  fi

  printf "%s files\n%s\n" "$files" "$(human_size "$bytes")"
  exit 0
fi

usage
exit 2
