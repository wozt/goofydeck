#!/usr/bin/env bash
set -euo pipefail

usage() {
  cat >&2 <<'EOF'
Usage:
  clear_wallpaper_cache.sh --text
  clear_wallpaper_cache.sh --clear

Wallpaper cache = rendered tiles folder next to each configured wallpaper image:
  <wallpaper_dir>/<wallpaper_basename_without_ext>/
containing tiles:
  <wallpaper_basename_without_ext>-<tile>.png

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
CFG="${ROOT}/config/configuration.yml"

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

list_wallpaper_paths() {
  # Supports:
  #   wallpaper:
  #     path: "..."
  # (top-level or per-page). Keeps context by tracking indentation.
  awk '
    function indent_len(s){ match(s, /^[ \t]*/); return RLENGTH }
    BEGIN { in_wp=0; wp_indent=-1 }
    {
      line=$0
      if (line ~ /^[ \t]*#/ || line ~ /^[ \t]*$/) next
      if (line ~ /^[ \t]*wallpaper:[ \t]*$/) {
        in_wp=1
        wp_indent=indent_len(line)
        next
      }
      if (in_wp) {
        cur_indent=indent_len(line)
        if (cur_indent <= wp_indent && line !~ /^[ \t]*path:/) {
          in_wp=0
          wp_indent=-1
          # fallthrough to allow a new wallpaper: on same line next record
        } else if (line ~ /^[ \t]*path:[ \t]*"[^"]+"[ \t]*$/) {
          if (cur_indent > wp_indent) {
            sub(/^[ \t]*path:[ \t]*"/, "", line)
            sub(/"[ \t]*$/, "", line)
            print line
          }
          next
        }
      }
    }
  ' "$CFG" 2>/dev/null || true
}

wallpaper_cache_dirs=()
declare -A seen=()

while IFS= read -r wp; do
  [[ -z "$wp" ]] && continue

  # Resolve relative paths against project root.
  if [[ "$wp" != /* ]]; then
    wp="${ROOT}/${wp}"
  fi

  base="${wp##*/}"
  stem="${base%.*}"
  dir="${wp%/*}"
  render_dir="${dir}/${stem}"

  key="${render_dir}"
  if [[ -n "${seen[$key]+x}" ]]; then
    continue
  fi
  seen[$key]=1
  wallpaper_cache_dirs+=("$render_dir")
done < <(list_wallpaper_paths)

ram_wallpaper_dirs=(
  "/dev/shm/goofydeck/paging/wallpaper"
  "/dev/shm/goofydeck_$(id -u)/paging/wallpaper"
  "/tmp/goofydeck_paging_$(id -u)/wallpaper"
)

if [[ "${action}" == "clear" ]]; then
  shopt -s nullglob
  for d in "${wallpaper_cache_dirs[@]}"; do
    [[ -d "$d" ]] || continue
    stem="${d##*/}"
    tiles=( "$d/${stem}-"*.png )
    if (( ${#tiles[@]} == 0 )); then
      continue
    fi
    rm -rf -- "$d" 2>/dev/null || true
  done
  for d in "${ram_wallpaper_dirs[@]}"; do
    rm -rf -- "$d" 2>/dev/null || true
  done
  exit 0
fi

if [[ "${action}" == "text" ]]; then
  total_files=0
  total_bytes=0
  for d in "${wallpaper_cache_dirs[@]}"; do
    mapfile -t _cs < <(count_and_size_dir "$d")
    f="${_cs[0]:-0}"
    b="${_cs[1]:-0}"
    total_files=$((total_files + f))
    total_bytes=$((total_bytes + b))
  done
  for d in "${ram_wallpaper_dirs[@]}"; do
    mapfile -t _cs < <(count_and_size_dir "$d")
    f="${_cs[0]:-0}"
    b="${_cs[1]:-0}"
    total_files=$((total_files + f))
    total_bytes=$((total_bytes + b))
  done
  printf "%s files\n%s\n" "${total_files}" "$(human_size "${total_bytes}")"
  exit 0
fi

usage
exit 2
