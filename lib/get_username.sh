#!/usr/bin/env bash
# Description: print username from .env or system whoami
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
ENV_FILE="${ROOT}/.env"
SHOW_HELP="${ROOT}/show_help.sh"

if [ "${1:-}" = "-h" ] || [ "${1:-}" = "--help" ]; then
  exec "${SHOW_HELP}" "$(basename "$0")"
fi

if [ "$#" -gt 1 ]; then
  exec "${SHOW_HELP}" "$(basename "$0")"
fi

if [ -f "${ENV_FILE}" ]; then
  # shellcheck disable=SC1090
  source "${ENV_FILE}"
fi

if [ -n "${USERNAME:-}" ]; then
  echo "${USERNAME}"
else
  whoami
fi
