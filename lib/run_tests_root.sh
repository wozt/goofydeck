#!/usr/bin/env bash
# Description: run pytest as root after activating project venv
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

SHOW_HELP="${ROOT}/show_help.sh"

if [[ "${1:-}" == "-h" || "${1:-}" == "--help" ]]; then
  exec "${SHOW_HELP}" "$(basename "$0")"
fi

if [ "${EUID}" -ne 0 ]; then
  exec sudo -E "$0" "$@"
fi

VENV="${ROOT}/venv"
if [ ! -d "${VENV}" ]; then
  echo "Virtualenv not found at ${VENV}. Install deps first (run ./install.sh --python)." >&2
  exit 1
fi

# shellcheck disable=SC1091
source "${VENV}/bin/activate"
exec pytest "$@"
