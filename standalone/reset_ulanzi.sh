#!/usr/bin/env bash
# Description: reset Ulanzi D200 USB device (unbind/bind) to recover HID
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

VID="2207"
PID="0019"

usage() {
  echo "Usage: $(basename "$0")"
  echo "Resets USB device ${VID}:${PID} by unbinding/binding the USB driver (requires root)."
}

if [[ "${1:-}" == "-h" || "${1:-}" == "--help" ]]; then
  usage
  exit 0
fi

# Require root
if [ "${EUID}" -ne 0 ]; then
  exec sudo -E "$0" "$@"
fi

driver_dir="/sys/bus/usb/drivers/usb"
if [ ! -d "${driver_dir}" ]; then
  echo "USB driver directory not found: ${driver_dir}" >&2
  exit 1
fi

device_path=""
for dev in /sys/bus/usb/devices/*; do
  [ -d "$dev" ] || continue
  if [ -f "$dev/idVendor" ] && [ -f "$dev/idProduct" ]; then
    v=$(cat "$dev/idVendor")
    p=$(cat "$dev/idProduct")
    if [ "$v" = "$VID" ] && [ "$p" = "$PID" ]; then
      device_path="$dev"
      break
    fi
  fi
done

if [ -z "${device_path}" ]; then
  echo "Ulanzi device ${VID}:${PID} not found." >&2
  exit 1
fi

dev_name="$(basename "${device_path}")"

if [ ! -w "${driver_dir}/unbind" ] || [ ! -w "${driver_dir}/bind" ]; then
  echo "Cannot write to ${driver_dir}/unbind or bind (need root)." >&2
  exit 1
fi

echo "Resetting device ${VID}:${PID} (${dev_name})..."
echo "${dev_name}" > "${driver_dir}/unbind"
sleep 1
echo "${dev_name}" > "${driver_dir}/bind"
echo "Reset complete."
