#!/usr/bin/env bash
set -euo pipefail

usage() {
  cat >&2 <<'EOF'
Usage: assets/scripts/sonicpi_render_wav.sh --code <sonicpi.rb> --out <out.wav> --duration <seconds>

Renders a short Sonic Pi SFX into a WAV file without the GUI:
- Starts a temporary Sonic Pi server on dynamic ports
- Wraps your code with recording_start/stop/save
- Sends the code over OSC (/run-code)

Requirements:
- sonic-pi installed (server ruby files present)
- scsynth available
- ruby available
EOF
}

CODE_FILE=""
OUT_WAV=""
DURATION=""

while [ "$#" -gt 0 ]; do
  case "$1" in
    --code) CODE_FILE="${2:-}"; shift 2 ;;
    --out) OUT_WAV="${2:-}"; shift 2 ;;
    --duration) DURATION="${2:-}"; shift 2 ;;
    -h|--help) usage; exit 0 ;;
    *) echo "Unknown arg: $1" >&2; usage; exit 2 ;;
  esac
done

[ -n "${CODE_FILE}" ] || { echo "Missing --code" >&2; exit 2; }
[ -n "${OUT_WAV}" ] || { echo "Missing --out" >&2; exit 2; }
[ -n "${DURATION}" ] || { echo "Missing --duration" >&2; exit 2; }
[ -f "${CODE_FILE}" ] || { echo "Code file not found: ${CODE_FILE}" >&2; exit 2; }

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"

code_abs="${CODE_FILE}"
case "${code_abs}" in
  /*) ;;
  *) code_abs="${ROOT}/${code_abs}" ;;
esac

out_abs="${OUT_WAV}"
case "${out_abs}" in
  /*) ;;
  *) out_abs="${ROOT}/${out_abs}" ;;
esac

PORT_DISC="/usr/lib/sonic-pi/app/server/ruby/bin/port-discovery.rb"
INIT_SCRIPT="/usr/lib/sonic-pi/app/server/ruby/bin/init-script.rb"
EXIT_SCRIPT="/usr/lib/sonic-pi/app/server/ruby/bin/exit-script.rb"
SERVER_SCRIPT="/usr/lib/sonic-pi/app/server/ruby/bin/sonic-pi-server.rb"

[ -f "${PORT_DISC}" ] || { echo "Missing Sonic Pi port discovery: ${PORT_DISC}" >&2; exit 1; }
[ -f "${SERVER_SCRIPT}" ] || { echo "Missing Sonic Pi server: ${SERVER_SCRIPT}" >&2; exit 1; }

tmp_dir="$(mktemp -d -t goofydeck-sonicpi-XXXXXX)"
trap 'rm -rf "${tmp_dir}" 2>/dev/null || true' EXIT

ports_file="${tmp_dir}/ports.txt"
if ruby "${PORT_DISC}" >"${ports_file}" 2>/dev/null && [ -s "${ports_file}" ]; then
  :
else
  # Fallback to defaults if port-discovery can't run (e.g. restricted env).
  cat >"${ports_file}" <<'EOF'
server-listen-to-gui: 4557
gui-listen-to-server: 4558
scsynth: 4556
scsynth-send: 4556
osc-midi-out: 4563
osc-midi-in: 4564
server-osc-cues: 4560
erlang-router: 4561
websocket: 4562
EOF
fi

get_port() {
  local key="$1"
  awk -F': ' -v k="$key" '$1==k {print $2}' "${ports_file}"
}

server_port="$(get_port server-listen-to-gui)"
gui_port="$(get_port gui-listen-to-server)"
scsynth_port="$(get_port scsynth)"
scsynth_send_port="$(get_port scsynth-send)"
osc_cues_port="$(get_port server-osc-cues)"
erlang_port="$(get_port erlang-router)"
osc_midi_out_port="$(get_port osc-midi-out)"
osc_midi_in_port="$(get_port osc-midi-in)"
websocket_port="$(get_port websocket)"

for v in server_port gui_port scsynth_port scsynth_send_port osc_cues_port erlang_port osc_midi_out_port osc_midi_in_port websocket_port; do
  [ -n "${!v}" ] || { echo "ERROR: missing port ${v}" >&2; exit 1; }
done

mkdir -p "$(dirname "${OUT_WAV}")"
mkdir -p "$(dirname "${out_abs}")"
rm -f "${out_abs}" 2>/dev/null || true

wrapped="${tmp_dir}/wrapped.rb"
cat >"${wrapped}" <<EOF
use_debug false
recording_start
begin
  load "${code_abs}"
rescue Exception => e
  puts "ERROR: \#{e}"
end
sleep ${DURATION}
recording_stop
recording_save "${out_abs}"
sleep 0.05
EOF

log_file="${tmp_dir}/server.log"

# Force Sonic Pi to use a writable home (avoids ~/.sonic-pi permission issues).
export SONIC_PI_HOME="${tmp_dir}/sonicpi_home"

if [ -f "${INIT_SCRIPT}" ]; then
  ruby "${INIT_SCRIPT}" >/dev/null 2>&1 || true
fi

# Start server (UDP protocol)
ruby "${SERVER_SCRIPT}" -u "${server_port}" "${gui_port}" "${scsynth_port}" "${scsynth_send_port}" "${osc_cues_port}" "${erlang_port}" "${osc_midi_out_port}" "${osc_midi_in_port}" "${websocket_port}" >"${log_file}" 2>&1 &
server_pid="$!"

cleanup_server() {
  if kill -0 "${server_pid}" 2>/dev/null; then
    kill "${server_pid}" 2>/dev/null || true
    sleep 0.2
    kill -9 "${server_pid}" 2>/dev/null || true
  fi
  if [ -f "${EXIT_SCRIPT}" ]; then
    ruby "${EXIT_SCRIPT}" >/dev/null 2>&1 || true
  fi
}
trap cleanup_server EXIT

# Give it a moment to boot
sleep 1.2

# Send code
ruby "${ROOT}/assets/scripts/sonicpi_send_run_code.rb" "${server_port}" "${wrapped}" >/dev/null 2>&1 || true

# Wait for output
for _i in $(seq 1 120); do
  if [ -s "${out_abs}" ]; then
    exit 0
  fi
  sleep 0.05
done

echo "ERROR: timed out waiting for ${out_abs}" >&2
echo "Sonic Pi server log (tail):" >&2
tail -n 80 "${log_file}" >&2 || true
exit 1
