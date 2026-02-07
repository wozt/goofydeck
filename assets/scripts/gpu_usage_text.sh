#!/usr/bin/env bash
set -euo pipefail

# Prints a single line like: "GPU 52%"
# Uses the existing gpu_usage.sh probe.

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
v="$("${root}/assets/scripts/gpu_usage.sh" 2>/dev/null | head -n 1 || true)"
v="${v:-0}"
if [[ ! "${v}" =~ ^[0-9]+$ ]]; then v=0; fi
if [[ "${v}" -gt 99 ]]; then v=99; fi
echo "GPU ${v}%"

