#!/usr/bin/env bash
# GoofyDeck paging daemon: receives button events, renders current page icons from YAML config,
# caches icons in .cache/<page>/, and pushes the page to ulanzi_d200_demon.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

CONFIG_PATH="${ROOT}/config/configuration.yml"
PAGING_SOCK="/tmp/goofydeck_paging.sock"
ULANZI_SOCK="/tmp/ulanzi_device.sock"
CACHE_ROOT="${ROOT}/.cache"
STATE_DIR="${CACHE_ROOT}/paging"
ERROR_ICON="${ROOT}/assets/pregen/error.png"
PARSE_AWK="${ROOT}/lib/config_parse.awk"

START_PAGE=""

usage() {
  cat >&2 <<EOF
Usage:
  ./paging.sh --daemon [options]
  ./paging.sh --handle [options]          (internal; used by server)
  ./paging.sh --cmd "<line>" [options]    (send one command to daemon)

Options:
  --config <path>     (default: ${CONFIG_PATH})
  --sock <path>       (default: ${PAGING_SOCK})
  --ulanzi-sock <p>   (default: ${ULANZI_SOCK})
  --cache <dir>       (default: ${CACHE_ROOT})
  --start-page <name> (default: auto: \$root if present else first page)

Socket protocol (one line per connection):
  press <1-13> <TAP|HOLD|RELEASE>
  go <page>
  render
  state
EOF
}

die() { echo "[paging] ERROR: $*" >&2; exit 1; }
log() { echo "[paging] $*" >&2; }

have() { command -v "$1" >/dev/null 2>&1; }

now_ms() {
  if have date && date +%s%3N >/dev/null 2>&1; then
    date +%s%3N
  else
    echo $(( $(date +%s) * 1000 ))
  fi
}

debounce_ok() {
  # $1=key $2=window_ms ; returns 0 if allowed, 1 if blocked
  local key="$1" window_ms="$2"
  local f; f="$(state_path "debounce_${key}")"
  local now last
  now="$(now_ms)"
  last="$(cat "${f}" 2>/dev/null || true)"
  if [[ "${last}" =~ ^[0-9]+$ ]] && [ $((now - last)) -lt "${window_ms}" ]; then
    return 1
  fi
  printf "%s" "${now}" >"${f}"
  return 0
}

hash_str() {
  local s="$1"
  if have sha256sum; then
    printf "%s" "${s}" | sha256sum | awk '{print $1}'
  elif have shasum; then
    printf "%s" "${s}" | shasum -a 256 | awk '{print $1}'
  else
    die "Missing sha256sum/shasum for cache hashing"
  fi
}

ensure_dirs() {
  mkdir -p "${STATE_DIR}"
}

state_path() { printf "%s/%s" "${STATE_DIR}" "$1"; }
state_get() { local f; f="$(state_path "$1")"; [ -f "$f" ] && cat "$f" || true; }
state_set() { local f; f="$(state_path "$1")"; printf "%s" "$2" >"$f"; }

lock_run() {
  local lockf; lockf="$(state_path lock)"
  if have flock; then
    (
      : >"${lockf}"
      exec 9>"${lockf}"
      flock 9
      "$@"
    )
    return $?
  fi
  "$@"
}

build_config_dump() {
  ensure_dirs
  [ -f "${CONFIG_PATH}" ] || die "Config not found: ${CONFIG_PATH}"
  local dump; dump="$(state_path config.dump)"
  local cfg_mtime; cfg_mtime="$(stat -c %Y "${CONFIG_PATH}" 2>/dev/null || stat -f %m "${CONFIG_PATH}")"
  local old; old="$(state_get config_mtime || true)"
  if [ -f "${dump}" ] && [ "${old:-}" = "${cfg_mtime}" ]; then
    return 0
  fi
  awk -f "${PARSE_AWK}" "${CONFIG_PATH}" >"${dump}"
  state_set config_mtime "${cfg_mtime}"
}

config_pages_order() {
  local dump; dump="$(state_path config.dump)"
  awk -F $'\t' '$1=="PAGE"{print $2}' "${dump}"
}

config_sys_pos() {
  local action="$1"
  local dump; dump="$(state_path config.dump)"
  awk -F $'\t' -v a="${action}" '$1=="SYS" && $2==a{print $3}' "${dump}" | head -n1
}

config_btn_field() {
  # $1=page $2=index $3=field(name|icon|presets)
  local page="$1" idx="$2" field="$3"
  local dump; dump="$(state_path config.dump)"
  case "${field}" in
    name) awk -F $'\t' -v p="${page}" -v i="${idx}" '$1=="BTN" && $2==p && $3==i{print $4; exit}' "${dump}" ;;
    icon) awk -F $'\t' -v p="${page}" -v i="${idx}" '$1=="BTN" && $2==p && $3==i{print $5; exit}' "${dump}" ;;
    presets) awk -F $'\t' -v p="${page}" -v i="${idx}" '$1=="BTN" && $2==p && $3==i{print $6; exit}' "${dump}" ;;
    *) return 1 ;;
  esac
}

config_preset_get() {
  local preset="$1" key="$2"
  local dump; dump="$(state_path config.dump)"
  awk -F $'\t' -v p="${preset}" -v k="${key}" '$1=="PRESET" && $2==p && $3==k{print $4; exit}' "${dump}"
}

pick_start_page() {
  build_config_dump
  if [ -n "${START_PAGE}" ]; then
    echo "${START_PAGE}"
    return 0
  fi
  local pages; pages="$(config_pages_order)"
  if printf "%s\n" "${pages}" | grep -qx '\$root'; then
    echo "\$root"
    return 0
  fi
  printf "%s\n" "${pages}" | head -n1
}

ordered_pages() {
  # Keep $root, and any page not starting with '$' (skip other special pages).
  config_pages_order | awk '$0=="$root" || $0 !~ /^\$/ {print}'
}

page_next() {
  local cur="$1"
  local -a pages=()
  while IFS= read -r p; do pages+=("$p"); done < <(ordered_pages)
  if [ "${#pages[@]}" -eq 0 ]; then echo "${cur}"; return 0; fi
  local i
  for i in "${!pages[@]}"; do
    if [ "${pages[$i]}" = "${cur}" ]; then
      echo "${pages[$(((i+1)%${#pages[@]}))]}"
      return 0
    fi
  done
  echo "${pages[0]}"
}

page_prev() {
  local cur="$1"
  local -a pages=()
  while IFS= read -r p; do pages+=("$p"); done < <(ordered_pages)
  if [ "${#pages[@]}" -eq 0 ]; then echo "${cur}"; return 0; fi
  local i
  for i in "${!pages[@]}"; do
    if [ "${pages[$i]}" = "${cur}" ]; then
      local j=$((i-1))
      if [ $j -lt 0 ]; then j=$((${#pages[@]}-1)); fi
      echo "${pages[$j]}"
      return 0
    fi
  done
  echo "${pages[0]}"
}

hist_push() { local p="$1"; printf "%s\n" "${p}" >>"$(state_path history)"; }
hist_pop() {
  local f; f="$(state_path history)"
  [ -f "${f}" ] || return 1
  local last
  last="$(tail -n1 "${f}" || true)"
  if [ -z "${last}" ]; then return 1; fi
  # truncate last line
  if have sed; then
    sed -i '$d' "${f}" 2>/dev/null || true
  else
    # fallback: rewrite
    head -n -1 "${f}" >"${f}.tmp" 2>/dev/null && mv -f "${f}.tmp" "${f}" || true
  fi
  printf "%s" "${last}"
}

ensure_mdi_icon() {
  local icon_spec="$1" # mdi:name
  local name="${icon_spec#mdi:}"
  local svg="${ROOT}/assets/mdi/${name}.svg"
  if [ -f "${svg}" ]; then
    return 0
  fi
  log "MDI icon missing: ${svg} (attempting download via icons/download_mdi.sh)"
  "${ROOT}/icons/download_mdi.sh" >/dev/null 2>&1 || return 1
  [ -f "${svg}" ]
}

render_button_icon() {
  local page="$1" idx="$2" name="$3" icon="$4" presets_csv="$5"
  local page_dir="${CACHE_ROOT}/${page}"
  mkdir -p "${page_dir}"

  local h; h="$(hash_str "${page}\n${name}\n")"
  local out="${page_dir}/btn${idx}-${h}.png"
  if [ -f "${out}" ]; then
    echo "${out}"
    return 0
  fi

  local tmp="${out}.tmp"
  rm -f "${tmp}" || true

  # Aggregate preset properties (first one wins for now).
  local icon_bg="" icon_fg="" icon_size="" icon_radius="" text_color="" text_size=""
  IFS=',' read -r -a preset_list <<<"${presets_csv:-}"
  for p in "${preset_list[@]}"; do
    [ -n "${p}" ] || continue
    [ -z "${icon_bg}" ] && icon_bg="$(config_preset_get "${p}" "icon_background_color" || true)"
    [ -z "${icon_fg}" ] && icon_fg="$(config_preset_get "${p}" "icon_color" || true)"
    [ -z "${icon_size}" ] && icon_size="$(config_preset_get "${p}" "icon_size" || true)"
    [ -z "${icon_radius}" ] && icon_radius="$(config_preset_get "${p}" "icon_border_radius" || true)"
    [ -z "${text_color}" ] && text_color="$(config_preset_get "${p}" "text_color" || true)"
    [ -z "${text_size}" ] && text_size="$(config_preset_get "${p}" "text_size" || true)"
  done

  # Base: transparent 196x196
  if ! "${ROOT}/icons/draw_square" transparent --size=196 "${tmp}" >/dev/null 2>&1; then
    cp -f "${ERROR_ICON}" "${out}"
    echo "${out}"
    return 0
  fi

  # Background fill (rounded)
  if [ -n "${icon_bg}" ]; then
    if ! "${ROOT}/icons/draw_border" "${icon_bg}" --size=196 --radius="${icon_radius:-0}" "${tmp}" >/dev/null 2>&1; then
      cp -f "${ERROR_ICON}" "${out}"
      echo "${out}"
      return 0
    fi
  fi

  # Main icon
  if [[ "${icon}" == mdi:* ]]; then
    if ! ensure_mdi_icon "${icon}"; then
      cp -f "${ERROR_ICON}" "${out}"
      echo "${out}"
      return 0
    fi
    local size="${icon_size:-128}"
    if ! [[ "${size}" =~ ^[0-9]+$ ]]; then size=128; fi
    local color="${icon_fg:-FFFFFF}"
    if [ -x "${ROOT}/icons/draw_mdi" ]; then
      "${ROOT}/icons/draw_mdi" "${icon}" "${color}" --size="${size}" "${tmp}" >/dev/null 2>&1 || true
    else
      "${ROOT}/legacy/icons/draw_mdi.sh" "${icon}" "${color}" --size="${size}" "${tmp}" >/dev/null 2>&1 || true
    fi
  elif [ -n "${icon}" ]; then
    # Treat as file path (png/jpg/webp). Render by overlaying it (best-effort).
    local p="${icon}"
    if [ ! -f "${p}" ] && [[ "${p}" != /* ]] && [ -f "${ROOT}/${p}" ]; then p="${ROOT}/${p}"; fi
    if [ -f "${p}" ]; then
      "${ROOT}/legacy/icons/draw_over.sh" "${p}" "${tmp}" >/dev/null 2>&1 || true
    fi
  fi

  # Text label rendered into icon (optional)
  if [ -n "${name}" ]; then
    local tc="${text_color:-}"
    local ts="${text_size:-}"
    if [ -z "${tc}" ]; then tc="FFFFFF"; fi
    if [ -z "${ts}" ] || ! [[ "${ts}" =~ ^[0-9]+$ ]]; then ts=16; fi
    "${ROOT}/legacy/icons/draw_text.sh" --text="${name}" --text_color="${tc}" --text_align=bottom --text_size="${ts}" "${tmp}" >/dev/null 2>&1 || true
  fi

  # Finalize
  mv -f "${tmp}" "${out}" 2>/dev/null || cp -f "${tmp}" "${out}"
  echo "${out}"
}

render_and_send_page() {
  build_config_dump
  local page; page="$(state_get current_page || true)"
  if [ -z "${page}" ]; then
    page="$(pick_start_page)"
    [ -n "${page}" ] || die "No pages found in config"
    state_set current_page "${page}"
  fi

  log "render page='${page}'"

  local -a paths=()
  local i
  for i in $(seq 1 13); do
    local name icon presets
    name="$(config_btn_field "${page}" "${i}" name || true)"
    icon="$(config_btn_field "${page}" "${i}" icon || true)"
    presets="$(config_btn_field "${page}" "${i}" presets || true)"
    local p
    p="$(render_button_icon "${page}" "${i}" "${name}" "${icon}" "${presets}")"
    paths+=("${p}")
  done

  # Build command for ulanzi daemon
  local cmd="set-buttons-explicit"
  for i in "${!paths[@]}"; do
    local btn=$((i+1))
    cmd+=" --button-${btn}=${paths[$i]}"
  done

  local resp
  resp="$(printf '%s\n' "${cmd}" | nc -U "${ULANZI_SOCK}" 2>/dev/null || true)"
  if [ "${resp}" != "ok" ]; then
    log "send failed (ulanzi resp='${resp:-<no response>}')"
    return 1
  fi
  log "send ok"
  return 0
}

handle_line() {
  local line="$1"
  build_config_dump
  local cur; cur="$(state_get current_page || true)"
  if [ -z "${cur}" ]; then
    cur="$(pick_start_page)"
    state_set current_page "${cur}"
  fi

  local back_pos next_pos prev_pos
  back_pos="$(config_sys_pos "\$page.back" || true)"
  next_pos="$(config_sys_pos "\$page.next" || true)"
  prev_pos="$(config_sys_pos "\$page.previous" || true)"

  case "${line}" in
    press\ *)
      local _ pnum pevt
      read -r _ pnum pevt <<<"${line}"
      log "rx press button=${pnum} event=${pevt}"
      if ! [[ "${pnum}" =~ ^[0-9]+$ ]] || [ "${pnum}" -lt 1 ] || [ "${pnum}" -gt 13 ]; then
        echo "err invalid_button"
        return 0
      fi
      case "${pevt}" in TAP|HOLD|RELEASE) ;; *) echo "err invalid_event"; return 0 ;; esac
      if [ "${pevt}" = "TAP" ] && [ -n "${back_pos}" ] && [ "${pnum}" = "${back_pos}" ]; then
        if ! debounce_ok "nav_back" 250; then echo "ok"; return 0; fi
        log "nav back"
        local prev
        prev="$(hist_pop || true)"
        if [ -n "${prev}" ]; then
          state_set current_page "${prev}"
          render_and_send_page && echo "ok" || echo "err send_failed"
        else
          echo "ok"
        fi
        return 0
      fi
      if [ "${pevt}" = "TAP" ] && [ -n "${next_pos}" ] && [ "${pnum}" = "${next_pos}" ]; then
        if ! debounce_ok "nav_next" 250; then echo "ok"; return 0; fi
        log "nav next"
        hist_push "${cur}"
        state_set current_page "$(page_next "${cur}")"
        render_and_send_page && echo "ok" || echo "err send_failed"
        return 0
      fi
      if [ "${pevt}" = "TAP" ] && [ -n "${prev_pos}" ] && [ "${pnum}" = "${prev_pos}" ]; then
        if ! debounce_ok "nav_prev" 250; then echo "ok"; return 0; fi
        log "nav prev"
        hist_push "${cur}"
        state_set current_page "$(page_prev "${cur}")"
        render_and_send_page && echo "ok" || echo "err send_failed"
        return 0
      fi
      echo "ok"
      return 0
      ;;
    go\ *)
      local _ target
      read -r _ target <<<"${line}"
      if [ -z "${target}" ]; then echo "err missing_page"; return 0; fi
      hist_push "${cur}"
      state_set current_page "${target}"
      render_and_send_page && echo "ok" || echo "err send_failed"
      return 0
      ;;
    next|prev|back)
      # Deprecated manual commands: keep as no-op to avoid accidental loops/spam.
      log "ignored cmd ${line}"
      echo "ok"
      return 0
      ;;
    render)
      log "cmd render"
      render_and_send_page && echo "ok" || echo "err send_failed"
      return 0
      ;;
    state)
      printf "ok page=%s\n" "$(state_get current_page || true)"
      return 0
      ;;
    *)
      echo "err unknown_cmd"
      return 0
      ;;
  esac
}

daemon_main() {
  ensure_dirs
  if ! have socat && ! have nc; then
    die "Need socat or nc for Unix socket server"
  fi

  # Initialize current page if unset (does not render).
  build_config_dump
  if [ -z "$(state_get current_page || true)" ]; then
    local p; p="$(pick_start_page)"
    [ -n "${p}" ] && state_set current_page "${p}"
  fi

  log "Initial render..."
  if ! lock_run render_and_send_page; then
    log "Initial render failed (continuing)"
  fi

  if have socat; then
    log "Listening on ${PAGING_SOCK} (socat, fork per request)"
    exec socat UNIX-LISTEN:"${PAGING_SOCK}",fork,unlink-early EXEC:"${ROOT}/lib/paging.sh --handle --config \"${CONFIG_PATH}\" --sock \"${PAGING_SOCK}\" --ulanzi-sock \"${ULANZI_SOCK}\" --cache \"${CACHE_ROOT}\" --start-page \"${START_PAGE}\""
  fi

  die "socat not found (nc server mode not implemented)"
}

handle_main() {
  ensure_dirs
  local line
  IFS= read -r line || true
  line="${line:-}"
  log "handle line='${line}'"
  lock_run handle_line "${line}"
}

cmd_send() {
  local line="$1"
  [ -n "${line}" ] || die "--cmd requires a line"
  local resp
  resp="$(printf '%s\n' "${line}" | nc -U "${PAGING_SOCK}" 2>/dev/null || true)"
  printf "%s\n" "${resp}"
}

mode=""
cmd_line=""

while [ "$#" -gt 0 ]; do
  case "$1" in
    --daemon) mode="daemon"; shift ;;
    --handle) mode="handle"; shift ;;
    --cmd) mode="cmd"; cmd_line="${2:-}"; shift 2 ;;
    --config) CONFIG_PATH="${2:-}"; shift 2 ;;
    --sock) PAGING_SOCK="${2:-}"; shift 2 ;;
    --ulanzi-sock) ULANZI_SOCK="${2:-}"; shift 2 ;;
    --cache) CACHE_ROOT="${2:-}"; STATE_DIR="${CACHE_ROOT}/paging"; shift 2 ;;
    --start-page) START_PAGE="${2:-}"; shift 2 ;;
    -h|--help) usage; exit 0 ;;
    *) echo "Unknown arg: $1" >&2; usage; exit 2 ;;
  esac
done

case "${mode}" in
  daemon) daemon_main ;;
  handle) handle_main ;;
  cmd) cmd_send "${cmd_line}" ;;
  *) usage; exit 2 ;;
esac
