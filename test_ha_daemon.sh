#!/usr/bin/env bash
# Interactive test helper for bin/ha_daemon.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ENV_FILE="${ROOT}/.env"

SOCK_PATH_DEFAULT="/tmp/goofydeck_ha.sock"
SOCK_PATH="${SOCK_PATH_DEFAULT}"

PID_FILE="/dev/shm/goofydeck_ha_daemon.pid"
FIFO_DIR="/tmp/goofydeck_ha_test"
FIFO_IN="${FIFO_DIR}/in.fifo"
MON_PID_FILE="${FIFO_DIR}/monitor.pid"
HA_PID_FILE="${FIFO_DIR}/ha.pid"

have() { command -v "$1" >/dev/null 2>&1; }

die() { echo "[test_ha] ERROR: $*" >&2; exit 1; }

trim() {
  local s="$1"
  s="${s#"${s%%[![:space:]]*}"}"
  s="${s%"${s##*[![:space:]]}"}"
  printf "%s" "$s"
}

get_env_value() {
  local key="$1"
  [ -f "${ENV_FILE}" ] || return 1
  # Accept: KEY=value or KEY="value"
  local line
  line="$(grep -E "^${key}=" "${ENV_FILE}" 2>/dev/null | tail -n 1 || true)"
  [ -n "${line}" ] || return 1
  local val="${line#*=}"
  val="$(trim "${val}")"
  # strip single/double quotes if present
  if [[ "${val}" == \"*\" && "${val}" == *\" ]]; then
    val="${val:1:${#val}-2}"
  elif [[ "${val}" == \'*\' && "${val}" == *\' ]]; then
    val="${val:1:${#val}-2}"
  fi
  printf "%s" "${val}"
  return 0
}

set_env_value() {
  local key="$1"
  local val="$2"
  mkdir -p "$(dirname "${ENV_FILE}")"
  touch "${ENV_FILE}"
  if grep -qE "^${key}=" "${ENV_FILE}"; then
    # Replace in-place
    # shellcheck disable=SC2001
    sed -i -E "s|^${key}=.*$|${key}=\"${val}\"|g" "${ENV_FILE}"
  else
    printf '%s="%s"\n' "${key}" "${val}" >>"${ENV_FILE}"
  fi
}

ensure_env() {
  local host token
  host="$(get_env_value HA_HOST || true)"
  token="$(get_env_value HA_ACCESS_TOKEN || true)"

  if [ -z "${host}" ]; then
    echo "HA_HOST is missing in ${ENV_FILE}."
    read -r -p "Enter HA_HOST (example: ws://localhost:8123): " host
    host="$(trim "${host}")"
    [ -n "${host}" ] || die "HA_HOST is required"
    set_env_value "HA_HOST" "${host}"
  fi

  if [ -z "${token}" ]; then
    echo "HA_ACCESS_TOKEN is missing in ${ENV_FILE}."
    read -r -p "Enter HA_ACCESS_TOKEN (long-lived token): " token
    token="$(trim "${token}")"
    [ -n "${token}" ] || die "HA_ACCESS_TOKEN is required"
    set_env_value "HA_ACCESS_TOKEN" "${token}"
  fi

  echo "[test_ha] Using HA_HOST='${host}'"
}

ensure_deps() {
  [ -x "${ROOT}/bin/ha_daemon" ] || {
    echo "[test_ha] Building bin/ha_daemon..."
    make -C "${ROOT}" -B bin/ha_daemon >/dev/null
  }
  have nc || die "nc is required (netcat)"
}

is_running_pid() {
  local pid="$1"
  [[ "${pid}" =~ ^[0-9]+$ ]] || return 1
  kill -0 "${pid}" 2>/dev/null
}

start_daemon() {
  mkdir -p "${FIFO_DIR}"
  rm -f "${SOCK_PATH}" 2>/dev/null || true

  echo "[test_ha] Starting ha_daemon on ${SOCK_PATH}..."
  ("${ROOT}/bin/ha_daemon" --sock "${SOCK_PATH}" >"${FIFO_DIR}/ha_daemon.log" 2>&1) &
  echo $! >"${HA_PID_FILE}"
  echo $! >"${PID_FILE}" 2>/dev/null || true

  # wait for socket
  for _ in $(seq 1 100); do
    [ -S "${SOCK_PATH}" ] && break
    sleep 0.05
  done
  [ -S "${SOCK_PATH}" ] || die "Socket not created: ${SOCK_PATH} (see ${FIFO_DIR}/ha_daemon.log)"
  echo "[test_ha] ha_daemon started (pid=$(cat "${HA_PID_FILE}"))."
}

stop_daemon() {
  local pid=""
  if [ -f "${HA_PID_FILE}" ]; then pid="$(cat "${HA_PID_FILE}" 2>/dev/null || true)"; fi
  if [ -z "${pid}" ] && [ -f "${PID_FILE}" ]; then pid="$(cat "${PID_FILE}" 2>/dev/null || true)"; fi
  if [ -n "${pid}" ] && is_running_pid "${pid}"; then
    echo "[test_ha] Stopping ha_daemon pid=${pid}..."
    kill -TERM "${pid}" 2>/dev/null || true
    sleep 0.2
    kill -KILL "${pid}" 2>/dev/null || true
  fi
  rm -f "${HA_PID_FILE}" "${PID_FILE}" 2>/dev/null || true
  rm -f "${SOCK_PATH}" 2>/dev/null || true
}

monitor_start() {
  mkdir -p "${FIFO_DIR}"
  rm -f "${FIFO_IN}" 2>/dev/null || true
  mkfifo "${FIFO_IN}"

  echo "[test_ha] Starting monitor session (persistent client)..."
  echo "[test_ha] Type menu commands to send requests; events will print here."
  # Keep a single persistent session open. Commands are fed through FIFO.
  (
    # Keep stdin open even if no writers (tail -f)
    # shellcheck disable=SC2002
    cat "${FIFO_IN}" | nc -U "${SOCK_PATH}"
  ) | while IFS= read -r line; do
    printf "[ha_daemon] %s\n" "${line}"
  done &

  echo $! >"${MON_PID_FILE}"
  sleep 0.1
  echo "[test_ha] Monitor started (pid=$(cat "${MON_PID_FILE}"))."
}

monitor_stop() {
  local pid=""
  if [ -f "${MON_PID_FILE}" ]; then pid="$(cat "${MON_PID_FILE}" 2>/dev/null || true)"; fi
  if [ -n "${pid}" ] && is_running_pid "${pid}"; then
    echo "[test_ha] Stopping monitor pid=${pid}..."
    kill -TERM "${pid}" 2>/dev/null || true
    sleep 0.1
    kill -KILL "${pid}" 2>/dev/null || true
  fi
  rm -f "${MON_PID_FILE}" 2>/dev/null || true
  rm -f "${FIFO_IN}" 2>/dev/null || true
}

send_cmd() {
  local cmd="$1"
  [ -S "${SOCK_PATH}" ] || die "ha_daemon socket missing: ${SOCK_PATH} (start daemon first)"
  if [ -p "${FIFO_IN}" ]; then
    printf "%s\n" "${cmd}" >"${FIFO_IN}"
    return 0
  fi
  # One-shot client (no events): ensure we exit even though ha_daemon keeps the socket open.
  if have timeout; then
    resp="$(printf "%s\n" "${cmd}" | timeout 1s nc -U "${SOCK_PATH}" 2>/dev/null || true)"
    [ -n "${resp}" ] && printf "%s\n" "${resp}"
    return 0
  fi
  if nc -h 2>&1 | grep -q -- ' -N '; then
    resp="$(printf "%s\n" "${cmd}" | nc -U "${SOCK_PATH}" -N -w 1 2>/dev/null || true)"
    [ -n "${resp}" ] && printf "%s\n" "${resp}"
    return 0
  fi
  # Best-effort fallback (may hang depending on nc implementation).
  resp="$(printf "%s\n" "${cmd}" | nc -U "${SOCK_PATH}" 2>/dev/null || true)"
  [ -n "${resp}" ] && printf "%s\n" "${resp}"
}

pause_menu() {
  read -r -p "[test_ha] Press ENTER to return to menu... " _ || true
}

cleanup() {
  monitor_stop || true
  stop_daemon || true
  rmdir "${FIFO_DIR}" 2>/dev/null || true
}
trap cleanup EXIT

ensure_deps
ensure_env

main_menu() {
  cat <<EOF

HA test menu:
  1) Start ha_daemon
  2) Stop ha_daemon
  3) Start monitor session (persistent)
  4) Stop monitor session
  5) ping
  6) info
  7) subs
  8) sub-state <entity_id>
  9) unsub <sub_id>
 10) get <entity_id>
 11) call <domain> <service> <json>
 12) Show ha_daemon log tail
  q) Quit
EOF
}

while true; do
  main_menu
  read -r -p "> " choice
  choice="$(trim "${choice}")"
  case "${choice}" in
    1)
      start_daemon
      ;;
    2)
      stop_daemon
      ;;
    3)
      [ -S "${SOCK_PATH}" ] || die "start ha_daemon first"
      monitor_start
      ;;
    4)
      monitor_stop
      ;;
    5)
      send_cmd "ping"
      pause_menu
      ;;
    6)
      send_cmd "info"
      pause_menu
      ;;
    7)
      send_cmd "subs"
      pause_menu
      ;;
    8)
      read -r -p "entity_id (ex: light.salon): " ent
      ent="$(trim "${ent}")"
      [ -n "${ent}" ] || die "entity_id required"
      send_cmd "sub-state ${ent}"
      pause_menu
      ;;
    9)
      read -r -p "sub_id: " sid
      sid="$(trim "${sid}")"
      [ -n "${sid}" ] || die "sub_id required"
      send_cmd "unsub ${sid}"
      pause_menu
      ;;
    10)
      read -r -p "entity_id (ex: light.salon): " ent
      ent="$(trim "${ent}")"
      [ -n "${ent}" ] || die "entity_id required"
      send_cmd "get ${ent}"
      pause_menu
      ;;
    11)
      read -r -p "domain (ex: light): " domain
      domain="$(trim "${domain}")"
      read -r -p "service (ex: turn_on): " service
      service="$(trim "${service}")"
      read -r -p "json (single line, ex: {\"entity_id\":\"light.salon\"}): " json
      json="$(trim "${json}")"
      [ -n "${domain}" ] || die "domain required"
      [ -n "${service}" ] || die "service required"
      [ -n "${json}" ] || die "json required"
      send_cmd "call ${domain} ${service} ${json}"
      pause_menu
      ;;
    12)
      if [ -f "${FIFO_DIR}/ha_daemon.log" ]; then
        tail -n 50 "${FIFO_DIR}/ha_daemon.log"
      else
        echo "No log yet (${FIFO_DIR}/ha_daemon.log)"
      fi
      pause_menu
      ;;
    q|quit|exit)
      exit 0
      ;;
    *)
      echo "Unknown choice: ${choice}" >&2
      ;;
  esac
done
