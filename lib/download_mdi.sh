#!/usr/bin/env bash
# Description: download all MDI SVG icons into ./mdi using git sparse clone; skip existing files.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

DEST="${ROOT}/mdi"
REPO_URL="https://github.com/Templarian/MaterialDesign.git"

command -v git >/dev/null 2>&1 || { echo "git is required." >&2; exit 1; }

mkdir -p "${DEST}"

TMP_DIR="$(mktemp -d)"
cleanup() { rm -rf "${TMP_DIR}"; }
trap cleanup EXIT

echo "Cloning MaterialDesign repo (sparse, svg only)..." >&2
git -c advice.detachedHead=false clone --depth 1 --filter=blob:none --sparse "${REPO_URL}" "${TMP_DIR}/repo" >&2
git -C "${TMP_DIR}/repo" sparse-checkout set svg >&2

SVG_DIR="${TMP_DIR}/repo/svg"
if [ ! -d "${SVG_DIR}" ]; then
  echo "SVG directory not found in clone." >&2
  exit 1
fi

downloaded=0
skipped=0

for file in "${SVG_DIR}"/*.svg; do
  [ -e "${file}" ] || continue
  name="$(basename "${file}")"
  target="${DEST}/${name}"
  if [ -f "${target}" ]; then
    skipped=$((skipped + 1))
    continue
  fi
  cp "${file}" "${target}"
  downloaded=$((downloaded + 1))
  echo "Downloaded ${name}" >&2
done

echo "Downloaded ${downloaded} new file(s); ${skipped} already present in ${DEST}"
