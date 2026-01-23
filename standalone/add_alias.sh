#!/usr/bin/env bash
# Description: add useful aliases to user shell RC for GoofyDeck helpers
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
GET_USER="${SCRIPT_DIR}/../lib/get_username.sh"
ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
SHOW_HELP="${ROOT}/show_help.sh"

if [[ "${1:-}" == "-h" || "${1:-}" == "--help" ]]; then
  exec "${SHOW_HELP}" "$(basename "$0")"
fi

USERNAME="$("${GET_USER}")"
PROJECT_PATH="${PROJECT_ROOT:-${ROOT}}"

# Determine target shell rc file
TARGET_RC=""
if [ -n "${SHELL:-}" ]; then
  case "$(basename "${SHELL}")" in
    bash) TARGET_RC="${HOME}/.bashrc" ;;
    zsh) TARGET_RC="${HOME}/.zshrc" ;;
    fish) TARGET_RC="${HOME}/.config/fish/config.fish" ;;
  esac
fi

# Fallbacks
if [ -z "${TARGET_RC}" ]; then
  [ -f "${HOME}/.bashrc" ] && TARGET_RC="${HOME}/.bashrc"
fi
if [ -z "${TARGET_RC}" ]; then
  echo "Could not determine shell rc file. Set SHELL or create ~/.bashrc." >&2
  exit 1
fi

aliases=(
  "alias testgoofydeck='cd \"${PROJECT_PATH}\" && ./test_function.sh'"
  "alias helpgoofydeck='cd \"${PROJECT_PATH}\" && ./show_help.sh'"
  "alias t='cd \"${PROJECT_PATH}\" && ./test_function.sh'"
  "alias h='cd \"${PROJECT_PATH}\" && ./show_help.sh'"
)

for entry in "${aliases[@]}"; do
  # Check by alias name to avoid duplicate definitions
  alias_name="$(echo "$entry" | awk '{print $2}' | cut -d'=' -f1)"
  if grep -Eq "^alias[[:space:]]+${alias_name}=" "${TARGET_RC}" 2>/dev/null; then
    echo "Already present in ${TARGET_RC}: ${alias_name}"
    continue
  fi
  echo "$entry" >> "${TARGET_RC}"
  echo "Added: $entry -> ${TARGET_RC}"
done

echo "Reload your shell or run: source ${TARGET_RC}"
