#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

ENV_FILE="${ROOT}/.env"
OUT_DIR=""
PROJECT_NAME="$(basename "${ROOT}")"
declare -a EXCLUDES=()

usage() {
  cat >&2 <<EOF
Usage: $(basename "$0") [options]

Creates a zip backup of the entire project directory.

Options:
  --env-file <path>     Path to .env file (default: ${ENV_FILE})
  --out-dir <path>      Output directory (default: user's home from .env USER/USERNAME)
  --name <project>      Override project name (default: ${PROJECT_NAME})
  --exclude <pattern>   Exclude pattern (zip -x). Can be repeated.
  -h, --help            Show this help.

Notes:
  - Reads USER or USERNAME from the .env file.
  - Output filename: backup_<project>_<YYYYmmdd_HHMMSS>.zip
EOF
}

trim() {
  local s="$1"
  s="${s#"${s%%[![:space:]]*}"}"
  s="${s%"${s##*[![:space:]]}"}"
  printf "%s" "$s"
}

strip_quotes() {
  local s="$1"
  if [[ "$s" == \"*\" ]]; then s="${s#\"}"; s="${s%\"}"; fi
  if [[ "$s" == \'*\' ]]; then s="${s#\'}"; s="${s%\'}"; fi
  printf "%s" "$s"
}

read_username_from_env() {
  local file="$1"
  local raw=""
  if [ -f "$file" ]; then
    raw="$(awk -F= '($1=="USER" || $1=="USERNAME"){print $2; exit}' "$file" || true)"
    raw="$(trim "$raw")"
    raw="$(strip_quotes "$raw")"
  fi

  if [ -n "$raw" ]; then
    printf "%s\n" "$raw"
    return 0
  fi

  if [ -n "${USER:-}" ]; then
    printf "%s\n" "${USER}"
    return 0
  fi

  return 1
}

sanitize_name() {
  local s="$1"
  s="${s// /_}"
  s="${s//\//_}"
  s="${s//\\/_}"
  s="$(printf "%s" "$s" | tr -cd 'A-Za-z0-9._-')"
  if [ -z "$s" ]; then s="project"; fi
  printf "%s" "$s"
}

while [ "$#" -gt 0 ]; do
  case "$1" in
    --env-file)
      ENV_FILE="${2:-}"; shift 2 || { usage; exit 2; }
      ;;
    --env-file=*)
      ENV_FILE="${1#*=}"; shift
      ;;
    --out-dir)
      OUT_DIR="${2:-}"; shift 2 || { usage; exit 2; }
      ;;
    --out-dir=*)
      OUT_DIR="${1#*=}"; shift
      ;;
    --name)
      PROJECT_NAME="${2:-}"; shift 2 || { usage; exit 2; }
      ;;
    --name=*)
      PROJECT_NAME="${1#*=}"; shift
      ;;
    --exclude)
      EXCLUDES+=("${2:-}"); shift 2 || { usage; exit 2; }
      ;;
    --exclude=*)
      EXCLUDES+=("${1#*=}"); shift
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      echo "Unknown argument: $1" >&2
      usage
      exit 2
      ;;
  esac
done

if ! command -v zip >/dev/null 2>&1; then
  echo "Error: 'zip' is required (apt install zip)." >&2
  exit 1
fi

PROJECT_NAME="$(sanitize_name "${PROJECT_NAME}")"
timestamp="$(date +%Y%m%d_%H%M%S)"

if [ -z "${OUT_DIR}" ]; then
  username="$(read_username_from_env "${ENV_FILE}" || true)"
  if [ -z "${username:-}" ]; then
    echo "Error: could not read USER/USERNAME from ${ENV_FILE} and \$USER is not set." >&2
    exit 1
  fi

  home_dir="$(getent passwd "${username}" 2>/dev/null | cut -d: -f6 || true)"
  if [ -z "${home_dir:-}" ]; then
    home_dir="/home/${username}"
  fi
  OUT_DIR="${home_dir}"
fi

mkdir -p "${OUT_DIR}"

OUT_FILE="${OUT_DIR}/backup_${PROJECT_NAME}_${timestamp}.zip"

cd "${ROOT}"

zip_args=(-r -y "${OUT_FILE}" .)
for pat in "${EXCLUDES[@]}"; do
  if [ -n "${pat}" ]; then
    zip_args+=(-x "${pat}")
  fi
done

echo "Creating backup: ${OUT_FILE}"
zip "${zip_args[@]}" >/dev/null
ls -lh "${OUT_FILE}"
