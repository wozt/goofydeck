#!/usr/bin/env bash
set -euo pipefail

# Prints a single line like: "CPU 12%"
# Uses /proc/stat with a small persisted state in /dev/shm for a real usage percentage.

state_dir=""
for d in "/dev/shm/goofydeck/paging/cmd_state" "/tmp/goofydeck_paging_cmd_state_${UID:-0}"; do
  mkdir -p "${d}" 2>/dev/null || true
  if [[ -d "${d}" && -w "${d}" ]]; then
    state_dir="${d}"
    break
  fi
done
state_file=""
if [[ -n "${state_dir}" ]]; then
  state_file="${state_dir}/cpu_prev"
fi

read -r _ user nice system idle iowait irq softirq steal _rest < /proc/stat || true
user=${user:-0}; nice=${nice:-0}; system=${system:-0}; idle=${idle:-0}; iowait=${iowait:-0}; irq=${irq:-0}; softirq=${softirq:-0}; steal=${steal:-0}

idle_all=$((idle + iowait))
non_idle=$((user + nice + system + irq + softirq + steal))
total=$((idle_all + non_idle))

prev_total=0
prev_idle=0
if [[ -n "${state_file}" ]]; then
  if [[ -r "${state_file}" ]]; then
    read -r prev_total prev_idle < "${state_file}" || true
  fi
  printf '%s %s\n' "${total}" "${idle_all}" > "${state_file}.tmp" 2>/dev/null || true
  mv -f "${state_file}.tmp" "${state_file}" 2>/dev/null || true
fi

if [[ "${prev_total}" -le 0 || "${total}" -le "${prev_total}" ]]; then
  echo "CPU 0%"
  exit 0
fi

dt=$((total - prev_total))
didle=$((idle_all - prev_idle))
if [[ "${dt}" -le 0 ]]; then
  echo "CPU 0%"
  exit 0
fi

usage=$(( ( (dt - didle) * 100 + dt/2 ) / dt ))
if [[ "${usage}" -lt 0 ]]; then usage=0; fi
if [[ "${usage}" -gt 99 ]]; then usage=99; fi

echo "CPU ${usage}%"
