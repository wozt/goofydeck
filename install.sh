#!/usr/bin/env bash
# Description: install system deps for this project (Debian/Arch/RedHat/Brew) and compile via Makefile.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

ENV_FILE="${ROOT}/.env"
FONT_DIR="${ROOT}/fonts"
COLLECT_FONTS="${ROOT}/icons/collect_fonts.sh"

FORCE_COMPILE=0
NO_COMPILE=0
DEPS_ONLY=0
NO_FONTS=0

usage() {
  cat >&2 <<EOF
Usage: ./install.sh [options]

Installs system dependencies for this project and (by default) builds binaries via Makefile.

Options:
  --compile            Force full rebuild (make clean all)
  --no-compile         Do not build anything
  --deps-only          Install deps only (implies --no-compile)
  --no-fonts           Skip fonts collection/copy
  --env-file <path>    Use a specific .env file (default: ${ENV_FILE})
  -h, --help           Show this help

Notes:
  - USER or USERNAME can be defined in .env for font ownership.
  - This script does not move binaries; it relies on the Makefile outputs.
  - This is a C-only project; Python dependencies are not installed.
EOF
}

if [[ "${1:-}" == "-h" || "${1:-}" == "--help" ]]; then
  usage
  exit 0
fi

while [ "$#" -gt 0 ]; do
  case "$1" in
    --compile) FORCE_COMPILE=1; shift ;;
    --no-compile) NO_COMPILE=1; shift ;;
    --deps-only) DEPS_ONLY=1; NO_COMPILE=1; shift ;;
    --no-fonts) NO_FONTS=1; shift ;;
    --env-file) ENV_FILE="${2:-}"; shift 2 || { usage; exit 2; } ;;
    --env-file=*) ENV_FILE="${1#*=}"; shift ;;
    *) echo "Unknown argument: $1" >&2; usage; exit 2 ;;
  esac
done

log() { echo "[install] $*"; }
die() { echo "[install] ERROR: $*" >&2; exit 1; }

as_root() {
  if [ "${EUID}" -eq 0 ]; then
    "$@"
  else
    command -v sudo >/dev/null 2>&1 || die "sudo not found; run as root."
    sudo -E "$@"
  fi
}

load_env_user() {
  if [ -f "${ENV_FILE}" ]; then
    # shellcheck disable=SC1090
    . "${ENV_FILE}"
  fi
  printf "%s\n" "${USER:-${USERNAME:-}}"
}

detect_platform() {
  if [ "$(uname -s)" = "Darwin" ]; then
    echo "brew"
    return 0
  fi

if [ -f /etc/os-release ]; then
    # shellcheck disable=SC1091
    . /etc/os-release
    id="${ID:-}"
    like="${ID_LIKE:-}"
  else
    id=""
    like=""
  fi

  if command -v apt-get >/dev/null 2>&1 || [[ "${id} ${like}" == *"debian"* ]] || [[ "${id}" == "ubuntu" ]]; then
    echo "debian"
    return 0
  fi
  if command -v pacman >/dev/null 2>&1 || [[ "${id} ${like}" == *"arch"* ]]; then
    echo "arch"
    return 0
  fi
  if command -v dnf >/dev/null 2>&1 || command -v yum >/dev/null 2>&1 || [[ "${id} ${like}" == *"rhel"* ]] || [[ "${id} ${like}" == *"fedora"* ]]; then
    echo "redhat"
    return 0
  fi
  if command -v brew >/dev/null 2>&1; then
    echo "brew"
    return 0
  fi
  echo "unknown"
}

install_deps_debian() {
  local -a pkgs=(
    build-essential pkg-config git ca-certificates
    libhidapi-dev libusb-1.0-0-dev zlib1g-dev libpng-dev libyaml-dev libssl-dev
    ffmpeg libavformat-dev libavcodec-dev libavutil-dev libswscale-dev
    imagemagick librsvg2-bin librsvg2-dev libcairo2-dev
    jq bc
    netcat-openbsd socat
    fonts-noto-core fonts-noto-color-emoji
  )
  log "Installing apt dependencies..."
  as_root apt-get update
  as_root apt-get install -y "${pkgs[@]}"
}

install_deps_arch() {
  local -a pkgs=(
    base-devel pkgconf pkg-config git ca-certificates
    hidapi libusb zlib libpng libyaml openssl
    ffmpeg ffmpeg-devel
    imagemagick librsvg cairo
    jq bc
    openbsd-netcat socat
    noto-fonts noto-fonts-emoji
  )
  log "Installing pacman dependencies..."
  as_root pacman -Sy --noconfirm "${pkgs[@]}"
}

install_deps_redhat() {
  local -a pkgs=(
    gcc gcc-c++ make pkgconf-pkg-config pkg-config git ca-certificates
    hidapi hidapi-devel libusbx-devel zlib zlib-devel libpng-devel libyaml-devel openssl-devel
    ffmpeg ffmpeg-devel
    ImageMagick ImageMagick-devel librsvg2-tools librsvg2-devel cairo-devel
    jq bc
    nmap-ncat socat
    google-noto-emoji-color-fonts google-noto-sans-fonts
  )
  if command -v dnf >/dev/null 2>&1; then
    log "Installing dnf dependencies..."
    as_root dnf install -y "${pkgs[@]}"
  elif command -v yum >/dev/null 2>&1; then
    log "Installing yum dependencies..."
    as_root yum install -y "${pkgs[@]}"
  else
    die "dnf/yum not found."
  fi
}

install_deps_brew() {
  command -v brew >/dev/null 2>&1 || die "brew not found."
  local -a pkgs=(
    git
    pkg-config
    hidapi libusb zlib libpng libyaml openssl
    ffmpeg
    imagemagick librsvg cairo
    jq bc
    netcat socat
  )
  local -a font_casks=(font-noto-sans font-noto-emoji)
  log "Installing Homebrew dependencies..."
  brew update
  brew install "${pkgs[@]}"
  
  # Install ffmpeg development headers if needed
  if ! brew list ffmpeg >/dev/null 2>&1 || [ ! -d "$(brew --prefix)/include/libavformat" ]; then
    log "Installing ffmpeg with development support..."
    brew reinstall ffmpeg --with-libvpx --with-libx264 --with-libx265 || true
  fi
  
  if brew tap | grep -q "^homebrew/cask-fonts\$"; then
    :
  else
    brew tap homebrew/cask-fonts
  fi
  log "Installing Homebrew font casks..."
  brew install --cask "${font_casks[@]}" || true
}

ensure_fonts() {
  if [ "${NO_FONTS}" -eq 1 ]; then
    return 0
  fi

  mkdir -p "${FONT_DIR}"
  shopt -s nullglob
  local have=("${FONT_DIR}"/*.ttf)
  shopt -u nullglob
  if [ ${#have[@]} -eq 0 ] && [ -x "${COLLECT_FONTS}" ]; then
    log "Collecting fonts into ${FONT_DIR}..."
    "${COLLECT_FONTS}" || true
  fi

  local target_user
  target_user="$(load_env_user || true)"
  if [ -n "${target_user}" ]; then
    as_root chown -R "${target_user}:${target_user}" "${FONT_DIR}" 2>/dev/null || true
  fi
}


need_build() {
  local -a expected=(
    "${ROOT}/ulanzi_d200_demon"
    "${ROOT}/lib/send_image_page"
    "${ROOT}/lib/send_video_page_wrapper"
    "${ROOT}/icons/draw_square"
    "${ROOT}/icons/draw_border"
    "${ROOT}/icons/draw_optimize"
    "${ROOT}/icons/draw_over"
    "${ROOT}/icons/draw_text"
    "${ROOT}/standalone/draw_optimize_std"
  )
  for f in "${expected[@]}"; do
    if [ ! -x "${f}" ]; then
      return 0
    fi
  done
  return 1
}

build_all() {
  if [ "${NO_COMPILE}" -eq 1 ]; then
    return 0
  fi
  command -v make >/dev/null 2>&1 || die "make is required to compile"

  if [ "${FORCE_COMPILE}" -eq 1 ]; then
    log "Rebuilding (make clean all)..."
    make -C "${ROOT}" clean all
    return 0
  fi

  if need_build; then
    log "Building (make all)..."
    make -C "${ROOT}" all
  else
    log "Binaries already present; skipping build (use --compile to rebuild)."
  fi
}

main() {
  platform="$(detect_platform)"
  case "${platform}" in
    debian) install_deps_debian ;;
    arch) install_deps_arch ;;
    redhat) install_deps_redhat ;;
    brew) install_deps_brew ;;
    *) die "Unsupported platform. Install deps manually: gcc/make/pkg-config, hidapi+libusb, zlib+libpng, ffmpeg, ImageMagick, (optional) cairo+librsvg, jq, bc, netcat, socat." ;;
  esac

  ensure_fonts
  build_all

  log "Done."
}

main
