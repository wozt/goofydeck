#!/usr/bin/env bash
set -euo pipefail

# Prints a single line like: "RAM 42%"
# Uses MemTotal/MemAvailable from /proc/meminfo.

mem_total_kb="$(awk '/^MemTotal:/ {print $2; exit}' /proc/meminfo 2>/dev/null || true)"
mem_avail_kb="$(awk '/^MemAvailable:/ {print $2; exit}' /proc/meminfo 2>/dev/null || true)"

mem_total_kb="${mem_total_kb:-0}"
mem_avail_kb="${mem_avail_kb:-0}"

if [[ "${mem_total_kb}" -le 0 ]]; then
  echo "RAM 0%"
  exit 0
fi

if [[ "${mem_avail_kb}" -lt 0 ]]; then mem_avail_kb=0; fi
if [[ "${mem_avail_kb}" -gt "${mem_total_kb}" ]]; then mem_avail_kb="${mem_total_kb}"; fi

used_kb=$((mem_total_kb - mem_avail_kb))
pct=$(( (used_kb * 100 + mem_total_kb/2) / mem_total_kb ))
if [[ "${pct}" -lt 0 ]]; then pct=0; fi
if [[ "${pct}" -gt 99 ]]; then pct=99; fi

echo "RAM ${pct}%"

