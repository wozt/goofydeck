#!/usr/bin/env bash
# Description: reset Ulanzi D200 USB device (unbind/bind) to recover HID.
#
# Notes:
# - This triggers a logical USB disconnect/reconnect (re-enumeration). It does NOT guarantee cutting 5V power.
# - Targets ONLY the first matching device for VID:PID 2207:0019 unless overridden.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
SHOW_HELP="${ROOT}/show_help.sh"

VID="${VID:-2207}"
PID="${PID:-0019}"
WAIT_SEC="${WAIT_SEC:-1}"
WAIT_SOCK="${WAIT_SOCK:-}"

usage() {
  cat >&2 <<EOF
Usage: $(basename "$0") [options]

Reset the Ulanzi D200 USB device by unbinding/binding it from the kernel USB driver.
Requires root (will re-exec via sudo).

Options:
  --vidpid <VID:PID>     Default: ${VID}:${PID}
  --wait <seconds>       Sleep between unbind and bind (default: ${WAIT_SEC})
  --wait-sock <path>     After bind, wait until this unix socket exists (best-effort)
  -h, --help             Show this help

Examples:
  ${0##*/}
  ${0##*/} --wait 2
  ${0##*/} --wait-sock /tmp/ulanzi_device.sock
EOF
}

if [[ "${1:-}" == "-h" || "${1:-}" == "--help" ]]; then
  if [ -x "${SHOW_HELP}" ]; then
    exec "${SHOW_HELP}" "$(basename "$0")"
  fi
  usage
  exit 0
fi

while [ "$#" -gt 0 ]; do
  case "$1" in
    --vidpid)
      vp="${2:-}"; shift 2
      VID="${vp%%:*}"
      PID="${vp##*:}"
      ;;
    --wait)
      WAIT_SEC="${2:-}"; shift 2
      ;;
    --wait-sock)
      WAIT_SOCK="${2:-}"; shift 2
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      echo "Unknown arg: $1" >&2
      usage
      exit 2
      ;;
  esac
done

if [[ ! "${VID}" =~ ^[0-9a-fA-F]{4}$ ]] || [[ ! "${PID}" =~ ^[0-9a-fA-F]{4}$ ]]; then
  echo "Invalid VID:PID (expected 4-hex each): ${VID}:${PID}" >&2
  exit 2
fi

if [ "${EUID}" -ne 0 ]; then
  exec sudo -E "$0" --vidpid "${VID}:${PID}" --wait "${WAIT_SEC}" ${WAIT_SOCK:+--wait-sock "${WAIT_SOCK}"}
fi

driver_dir="/sys/bus/usb/drivers/usb"
if [ ! -d "${driver_dir}" ]; then
  echo "USB driver directory not found: ${driver_dir}" >&2
  exit 1
fi

if [ ! -w "${driver_dir}/unbind" ] || [ ! -w "${driver_dir}/bind" ]; then
  echo "Cannot write to ${driver_dir}/unbind or bind (need root)." >&2
  exit 1
fi

device_path=""
for dev in /sys/bus/usb/devices/*; do
  [ -d "$dev" ] || continue
  base="$(basename "$dev")"
  # Skip interface nodes like "1-1:1.0"
  [[ "${base}" == *:* ]] && continue
  if [ -f "$dev/idVendor" ] && [ -f "$dev/idProduct" ]; then
    v="$(cat "$dev/idVendor" 2>/dev/null || true)"
    p="$(cat "$dev/idProduct" 2>/dev/null || true)"
    if [ "${v}" = "${VID}" ] && [ "${p}" = "${PID}" ]; then
      device_path="$dev"
      break
    fi
  fi
done

if [ -z "${device_path}" ]; then
  echo "Ulanzi device not found in sysfs for ${VID}:${PID}." >&2
  echo "Hint: lsusb | grep -i \"${VID}:${PID}\"" >&2
  exit 1
fi

dev_name="$(basename "${device_path}")"
echo "[reset_usb] Resetting ${VID}:${PID} (${dev_name})..."

echo "${dev_name}" > "${driver_dir}/unbind"
sleep "${WAIT_SEC}"
echo "${dev_name}" > "${driver_dir}/bind"

if [ -n "${WAIT_SOCK}" ]; then
  for _ in $(seq 1 200); do
    [ -S "${WAIT_SOCK}" ] && break
    sleep 0.05
  done
  if [ ! -S "${WAIT_SOCK}" ]; then
    echo "[reset_usb] WARN: socket did not appear: ${WAIT_SOCK}" >&2
  fi
fi

echo "[reset_usb] Done."

