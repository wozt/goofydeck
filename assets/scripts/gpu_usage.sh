#!/usr/bin/env bash

# Best-effort GPU utilization probe across vendors.
# Outputs an integer percentage (0..100) on stdout.
#
# This is intentionally used as an optional helper. Callers must treat empty output,
# non-numeric output, or "0" as "unknown" and fall back to another method.

sh -c 'for p in /sys/class/drm/card*/device/gpu_busy_percent; do d=$(basename "$(readlink -f "$(dirname "$p")/driver" 2>/dev/null)"); [ "$d" = amdgpu ] && cat "$p" && exit; done; command -v nvidia-smi >/dev/null 2>&1 && nvidia-smi --query-gpu=utilization.gpu --format=csv,noheader,nounits 2>/dev/null | awk "NR==1 && \\$1~/^[0-9]+$/ {print \\$1; exit}"; for p in /sys/class/drm/card*/device/gpu_busy_percent; do d=$(basename "$(readlink -f "$(dirname "$p")/driver" 2>/dev/null)"); [ "$d" = i915 ] && cat "$p" && exit; done; echo 0'
