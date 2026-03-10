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
NO_UDEV=0
NO_MDI=0
SETUP_ENV=1
INTERACTIVE=1

usage() {
  cat >&2 <<EOF
Usage: ./install.sh [options]

Installs system dependencies for this project and (by default) builds binaries via Makefile.

Options:
  --compile            Force full rebuild (make clean all)
  --no-compile         Do not build anything
  --deps-only          Install deps only (implies --no-compile)
  --no-fonts           Skip fonts collection/copy
  --no-udev            Skip udev rules setup (for Docker/containers)
  --no-mdi             Skip MDI icons download
  --no-setup-env       Skip interactive .env setup
  --no-interactive     Skip interactive component selection (use defaults)
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
    --no-udev) NO_UDEV=1; shift ;;
    --no-mdi) NO_MDI=1; shift ;;
    --setup-env) SETUP_ENV=1; shift ;;
    --no-setup-env) SETUP_ENV=0; shift ;;
    --interactive) INTERACTIVE=1; shift ;;
    --no-interactive) INTERACTIVE=0; shift ;;
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

setup_udev_rules() {
  if [ "${NO_UDEV}" -eq 1 ]; then
    log "Skipping udev rules setup (--no-udev specified)"
    return 0
  fi

  if [ "$(uname -s)" = "Darwin" ]; then
    log "Skipping udev rules on macOS (not applicable)"
    return 0
  fi

  # Check if udev setup is needed (from early check)
  if [ "${UDEV_NEEDED:-0}" -eq 0 ]; then
    log "USB access is already properly configured - skipping udev setup"
    return 0
  fi

  local udev_file="/etc/udev/rules.d/50-ulanzi.rules"
  local vid="2207"
  local pid="0019"
  local current_user
  current_user="$(load_env_user || whoami)"

  if [ "${INTERACTIVE}" -eq 1 ]; then
    echo
    read -p "Setup USB device permissions (udev rules) for Ulanzi D200? [Y/n]: " setup_udev_confirm
    if [[ "${setup_udev_confirm}" =~ ^[Nn]*$ ]]; then
      log "Skipping udev rules setup (may cause device access issues)"
      return 0
    fi
  fi

  if [ -f "${udev_file}" ]; then
    log "Udev rules already exist at ${udev_file}"
    return 0
  fi

  log "Setting up udev rules for Ulanzi D200 (${vid}:${pid})..."
  cat <<EOF | as_root tee "${udev_file}" >/dev/null
# Ulanzi D200 Stream Deck
SUBSYSTEM=="usb", ATTR{idVendor}=="${vid}", ATTR{idProduct}=="${pid}", MODE="0666", GROUP="plugdev"
SUBSYSTEM=="hidraw", ATTRS{idVendor}=="${vid}", ATTRS{idProduct}=="${pid}", MODE="0666", GROUP="plugdev"
EOF

  log "Reloading udev rules..."
  as_root udevadm control --reload-rules
  as_root udevadm trigger

  # Add user to plugdev group if not already
  if ! groups "${current_user}" 2>/dev/null | grep -q "plugdev"; then
    log "Adding user ${current_user} to plugdev group..."
    as_root usermod -a -G plugdev "${current_user}"
    log "NOTE: You may need to log out and log back in for group changes to take effect"
  fi

  log "Udev rules installed successfully"
}

check_usb_access_needs() {
  local vid="2207"
  local pid="0019"
  local current_user
  current_user="$(load_env_user || whoami)"
  
  log "Checking USB device access for Ulanzi D200 (${vid}:${pid})..."
  
  # Check if device is connected
  local device_connected=0
  if command -v lsusb >/dev/null 2>&1; then
    if lsusb -d "${vid}:${pid}" >/dev/null 2>&1; then
      device_connected=1
      log "✓ Ulanzi D200 device found via USB"
    else
      log "⚠ Ulanzi D200 device not currently connected"
    fi
  else
    log "⚠ lsusb not available, cannot check device connection"
  fi
  
  # Check if user is in plugdev group
  local in_plugdev=0
  if groups "${current_user}" 2>/dev/null | grep -q "plugdev"; then
    in_plugdev=1
    log "✓ User ${current_user} is in plugdev group"
  else
    log "✗ User ${current_user} is NOT in plugdev group"
  fi
  
  # Check if udev rules exist
  local udev_file="/etc/udev/rules.d/50-ulanzi.rules"
  local udev_exists=0
  if [ -f "${udev_file}" ]; then
    udev_exists=1
    log "✓ Udev rules already exist"
  else
    log "✗ Udev rules do not exist"
  fi
  
  # Check if device is accessible (if connected)
  local device_accessible=0
  if [ "${device_connected}" -eq 1 ]; then
    # Try to find the correct hidraw device
    local target_hidraw=""
    for hidraw in /dev/hidraw*; do
      if [ -e "${hidraw}" ]; then
        local uevent_path="/sys/class/hidraw/$(basename "${hidraw}")/device/uevent"
        if [ -f "${uevent_path}" ]; then
          local hid_id device_vid device_pid
          hid_id=$(grep "^HID_ID=" "${uevent_path}" 2>/dev/null | cut -d= -f2)
          # HID_ID format: BUS:VID:PID (hex)
          if [ -n "${hid_id}" ]; then
            device_vid=$(echo "${hid_id}" | cut -d: -f2)
            device_pid=$(echo "${hid_id}" | cut -d: -f3)
            # Convert hex to lowercase for comparison
            device_vid=$(echo "${device_vid}" | tr '[:upper:]' '[:lower:]')
            device_pid=$(echo "${device_pid}" | tr '[:upper:]' '[:lower:]')
            # Convert expected VID/PID to hex format (4 digits)
            local expected_vid expected_pid
            expected_vid=$(echo "0000${vid}" | tail -c 5)
            expected_pid=$(echo "0000${pid}" | tail -c 5)
            expected_vid=$(echo "${expected_vid}" | tr '[:upper:]' '[:lower:]')
            expected_pid=$(echo "${expected_pid}" | tr '[:upper:]' '[:lower:]')
            if [ "${device_vid}" = "${expected_vid}" ] && [ "${device_pid}" = "${expected_pid}" ]; then
              target_hidraw="${hidraw}"
              log "✓ Found Ulanzi D200 at ${hidraw}"
              break
            fi
          fi
        fi
      fi
    done
    
    if [ -n "${target_hidraw}" ]; then
      # Test actual access to the device
      if [ -r "${target_hidraw}" ] && [ -w "${target_hidraw}" ]; then
        # Try a simple read test (non-blocking)
        if timeout 2 dd if="${target_hidraw}" of=/dev/null bs=1 count=1 2>/dev/null; then
          device_accessible=1
          log "✓ Device ${target_hidraw} is accessible to user ${current_user}"
        else
          log "✗ Device ${target_hidraw} exists but cannot be accessed (permission denied)"
        fi
      else
        log "✗ Device ${target_hidraw} exists but no read/write permissions"
      fi
    else
      log "✗ Ulanzi D200 hidraw device not found in /dev/hidraw*"
    fi
  fi
  
  # Summary and recommendation
  echo
  log "USB Access Summary:"
  echo "- Device connected: $([ "${device_connected}" -eq 1 ] && echo "Yes" || echo "No")"
  echo "- User in plugdev group: $([ "${in_plugdev}" -eq 1 ] && echo "Yes" || echo "No")"
  echo "- Udev rules exist: $([ "${udev_exists}" -eq 1 ] && echo "Yes" || echo "No")"
  echo "- Device accessible: $([ "${device_accessible}" -eq 1 ] && echo "Yes" || echo "No")"
  echo
  
  # Determine if udev setup is needed
  if [ "${in_plugdev}" -eq 1 ] && [ "${udev_exists}" -eq 1 ] && [ "${device_accessible}" -eq 1 ]; then
    log "✅ USB access is properly configured - no action needed"
    export UDEV_NEEDED=0
  else
    log "🔧 USB access needs configuration"
    if [ "${in_plugdev}" -eq 0 ]; then
      log "   → User needs to be added to plugdev group"
    fi
    if [ "${udev_exists}" -eq 0 ]; then
      log "   → Udev rules need to be created"
    fi
    if [ "${device_connected}" -eq 1 ] && [ "${device_accessible}" -eq 0 ]; then
      log "   → Current permissions are insufficient"
    fi
    export UDEV_NEEDED=1
  fi
}

setup_env_file() {
  if [ "${SETUP_ENV}" -eq 0 ] && [ -f "${ENV_FILE}" ]; then
    log "Environment file already exists at ${ENV_FILE} (use --setup-env to recreate)"
    return 0
  fi

  local example_env="${ROOT}/example.env"
  if [ ! -f "${example_env}" ]; then
    log "Example environment file not found at ${example_env}"
    return 0
  fi

  log "Setting up environment file..."
  
  # Copy example.env if .env doesn't exist
  if [ ! -f "${ENV_FILE}" ]; then
    cp "${example_env}" "${ENV_FILE}"
    log "Created ${ENV_FILE} from example.env"
  fi

  # Interactive setup (only if in interactive mode or explicitly requested)
  if [ "${INTERACTIVE}" -eq 1 ] || [ "${SETUP_ENV}" -eq 1 ]; then
    echo
    log "Environment Configuration (optional):"
    echo "=================================="
    
    # Setup USERNAME
    local current_user
    current_user="$(whoami)"
    echo "Current user: ${current_user}"
    read -p "Set USERNAME in .env? [Y/n]: " setup_username
    if [[ "${setup_username}" =~ ^[Yy]*$ ]] || [ -z "${setup_username}" ]; then
      sed -i "s/USERNAME=\"<username>\"/USERNAME=\"${current_user}\"/" "${ENV_FILE}"
      log "Set USERNAME=${current_user}"
    fi

    # Setup Home Assistant
    echo
    read -p "Configure Home Assistant integration? [y/N]: " setup_ha
    if [[ "${setup_ha}" =~ ^[Yy]*$ ]]; then
      echo
      echo "Home Assistant WebSocket Configuration:"
      echo "- Get long-lived access token: https://www.home-assistant.io/docs/authentication"
      echo
      
      read -p "HA Host [ws://localhost:8123]: " ha_host
      ha_host="${ha_host:-ws://localhost:8123}"
      
      read -p "HA Access Token (leave empty if not using HA): " ha_token
      
      # Update .env
      sed -i "s|HA_HOST=\"ws://localhost:8123\"|HA_HOST=\"${ha_host}\"|" "${ENV_FILE}"
      sed -i "s|HA_ACCESS_TOKEN=\"\"|HA_ACCESS_TOKEN=\"${ha_token}\"|" "${ENV_FILE}"
      
      log "Set HA_HOST=${ha_host}"
      if [ -n "${ha_token}" ]; then
        log "Set HA_ACCESS_TOKEN=[configured]"
      else
        log "HA_ACCESS_TOKEN left empty (HA integration disabled)"
      fi
    else
      log "Home Assistant configuration skipped"
    fi

    echo
    log "Environment file configured at ${ENV_FILE}"
    log "You can edit it manually anytime to change settings"
  else
    log "Environment file created with default values"
  fi
}

setup_mdi_icons() {
  if [ "${NO_MDI}" -eq 1 ]; then
    log "Skipping MDI icons download (--no-mdi specified)"
    return 0
  fi

  local config_file="${ROOT}/config/configuration.yml"
  local mdi_dir="${ROOT}/assets/mdi"

  if [ ! -f "${config_file}" ]; then
    log "Configuration file not found at ${config_file}, skipping MDI download"
    return 0
  fi

  # Extract MDI icons from configuration
  local mdi_icons
  mdi_icons=$(grep -o 'mdi:[a-zA-Z0-9_-]*' "${config_file}" | sed 's/mdi://' | sort -u | tr '\n' ' ')
  
  if [ -z "${mdi_icons}" ]; then
    log "No MDI icons found in configuration.yml"
    return 0
  fi

  local icon_count
  icon_count=$(echo "${mdi_icons}" | wc -w)
  log "Found ${icon_count} unique MDI icons in configuration.yml"

  # Check which icons are missing
  local missing_count=0
  local missing_icons=""
  for icon in ${mdi_icons}; do
    local icon_path="${mdi_dir}/${icon}.svg"
    log "Checking ${icon_path}..."
    if [ ! -f "${icon_path}" ]; then
      missing_count=$((missing_count + 1))
      missing_icons="${missing_icons} ${icon}"
      log "Missing: ${icon}"
    else
      log "Found: ${icon}"
    fi
  done

  if [ "${missing_count}" -eq 0 ]; then
    log "All ${icon_count} MDI icons already present in ${mdi_dir}"
    return 0
  fi

  log "Downloading ${missing_count} missing MDI icons..."
  log "Missing icons:${missing_icons}"

  # Download only required icons using individual downloads
  local downloaded=0
  local failed=0
  for icon in ${missing_icons}; do
    local icon_url="https://cdn.jsdelivr.net/npm/@mdi/svg@latest/svg/${icon}.svg"
    local target_file="${mdi_dir}/${icon}.svg"
    
    if command -v curl >/dev/null 2>&1; then
      if curl -s "${icon_url}" -o "${target_file}" 2>/dev/null; then
        downloaded=$((downloaded + 1))
        log "Downloaded ${icon}.svg"
      else
        failed=$((failed + 1))
        log "Failed to download ${icon}.svg"
      fi
    elif command -v wget >/dev/null 2>&1; then
      if wget -q "${icon_url}" -O "${target_file}" 2>/dev/null; then
        downloaded=$((downloaded + 1))
        log "Downloaded ${icon}.svg"
      else
        failed=$((failed + 1))
        log "Failed to download ${icon}.svg"
      fi
    else
      log "Neither wget nor curl available, falling back to full MDI download"
      # Fallback to full download
      "${ROOT}/icons/download_mdi.sh"
      return 0
    fi
  done

  # If some downloads failed, try individual GitHub Raw method for remaining icons
  if [ "${failed}" -gt 0 ]; then
    log "Attempting GitHub Raw method for ${failed} failed downloads..."
    local github_success=0
    for icon in ${missing_icons}; do
      local target_file="${mdi_dir}/${icon}.svg"
      if [ -f "${target_file}" ]; then continue; fi
      
      if "${ROOT}/icons/download_mdi.sh" "${icon}"; then
        github_success=$((github_success + 1))
        failed=$((failed - 1))
      fi
    done
    
    if [ "${github_success}" -gt 0 ]; then
      log "✅ GitHub Raw method succeeded for ${github_success} icons"
    fi
  fi

  log "MDI icons download completed: ${downloaded} downloaded, ${failed} failed"
  
  if [ "${failed}" -gt 0 ]; then
    log "WARNING: ${failed} icons failed to download"
    log "You can run manually: ${ROOT}/icons/download_mdi.sh"
  else
    log "All ${icon_count} MDI icons are now available"
  fi
}

interactive_setup() {
  echo
  log "GoofyDeck Interactive Setup"
  echo "=========================="
  echo
  
  # Ask for components to install
  local setup_udev="yes"
  local setup_env="auto"
  local setup_mdi="yes"
  local setup_fonts="yes"
  local setup_compile="yes"
  
  echo "Select components to install:"
  echo
  
  # Udev rules
  if [ "$(uname -s)" != "Darwin" ]; then
    echo "USB device permissions will be checked and configured if needed"
    setup_udev="checked"
  else
    echo "USB permissions: Not applicable on macOS"
    setup_udev="no"
  fi
  
  # Environment file
  if [ -f "${ENV_FILE}" ]; then
    read -p "Recreate environment file (.env)? [y/N]: " setup_env
    setup_env="${setup_env:-no}"
  else
    echo "Environment file (.env): Not found, will create"
    setup_env="yes"
  fi
  
  # MDI icons
  read -p "Download MDI icons from configuration.yml? [Y/n]: " setup_mdi
  setup_mdi="${setup_mdi:-yes}"
  
  # Fonts
  read -p "Setup fonts collection? [Y/n]: " setup_fonts
  setup_fonts="${setup_fonts:-yes}"
  
  # Compilation
  if need_build; then
    read -p "Compile binaries? [Y/n]: " setup_compile
    setup_compile="${setup_compile:-yes}"
  else
    echo "Compilation: Binaries already exist, will skip"
    setup_compile="no"
  fi
  
  echo
  log "Configuration Summary:"
  echo "- USB permissions (udev): ${setup_udev}"
  echo "- Environment file: ${setup_env}"
  echo "- MDI icons download: ${setup_mdi}"
  echo "- Fonts setup: ${setup_fonts}"
  echo "- Compilation: ${setup_compile}"
  echo
  
  read -p "Proceed with this configuration? [Y/n]: " proceed
  if [[ "${proceed}" =~ ^[Nn]*$ ]]; then
    log "Setup cancelled by user"
    exit 0
  fi
  
  # Set global flags based on user choices
  if [ "${setup_udev}" = "no" ]; then
    NO_UDEV=1
  fi
  
  if [ "${setup_env}" = "yes" ]; then
    SETUP_ENV=1
  fi
  
  if [ "${setup_mdi}" = "no" ]; then
    NO_MDI=1
  fi
  
  if [ "${setup_fonts}" = "no" ]; then
    NO_FONTS=1
  fi
  
  if [ "${setup_compile}" = "no" ]; then
    NO_COMPILE=1
  fi
  
  log "Interactive configuration applied"
}

is_rpi_zero_2w() {
  # Detection via /proc/cpuinfo
  if grep -q "Raspberry Pi Zero 2 W" /proc/cpuinfo 2>/dev/null; then
    return 0
  fi
  
  # Alternative detection: BCM2835 + low RAM
  if [ "$(awk '/Hardware/ {print $3}' /proc/cpuinfo 2>/dev/null)" = "BCM2835" ]; then
    local ram_mb=$(awk '/MemTotal/ {print int($2/1024)}' /proc/meminfo 2>/dev/null)
    if [ "${ram_mb}" -le 2048 ]; then
      return 0
    fi
  fi
  
  return 1
}

suggest_disable_wallpapers() {
  # Vérifier si les wallpapers sont déjà désactivés dans la config
  local config_file="${ROOT}/config/configuration.yml"
  local already_disabled=false
  
  if [ -f "${config_file}" ]; then
    if grep -q "disable_wallpapers: true" "${config_file}"; then
      already_disabled=true
    fi
  fi
  
  # Ne proposer que si c'est un Raspberry Pi Zero 2W ET que les wallpapers ne sont pas déjà désactivés
  if ! is_rpi_zero_2w || [ "${already_disabled}" = true ]; then
    return 0
  fi
  
  echo
  log "⚠️  Raspberry Pi Zero 2W detected with limited RAM"
  log "Wallpapers can impact performance on this device."
  echo
  
  read -p "Disable wallpapers in configuration for better performance? [Y/n]: " disable_wallpapers
  if [[ "${disable_wallpapers}" =~ ^[Yy]*$ ]] || [ -z "${disable_wallpapers}" ]; then
    if [ -f "${config_file}" ]; then
      # Check if global section already exists
      if grep -q "^global:" "${config_file}"; then
        # Update existing disable_wallpapers value or add it if not present
        if grep -q "disable_wallpapers:" "${config_file}"; then
          sed -i 's/^  disable_wallpapers:.*$/  disable_wallpapers: true/' "${config_file}"
        else
          sed -i '/^global:/a\  disable_wallpapers: true' "${config_file}"
        fi
      else
        sed -i '1i\global:\n  disable_wallpapers: true' "${config_file}"
      fi
      log "✅ Wallpapers disabled in configuration.yml"
    else
      log "Configuration file not found, cannot disable wallpapers"
    fi
  else
    log "Keeping wallpapers enabled"
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
    imagemagick librsvg2-bin librsvg2-dev libcairo2-dev libusb-dev
    jq bc
    netcat-openbsd socat
    fonts-noto-core fonts-noto-color-emoji
    sox
    sonic-pi
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
    sox
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
    sox
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
    sox
  )
  local -a font_casks=(font-noto-sans font-noto-emoji)
  local -a app_casks=(sonic-pi)
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

  log "Installing Homebrew app casks..."
  brew install --cask "${app_casks[@]}" || true
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
    "${ROOT}/ulanzi_d200_daemon"
    "${ROOT}/bin/paging_daemon"
    "${ROOT}/bin/ha_daemon"
    "${ROOT}/bin/send_image_page"
    "${ROOT}/bin/send_video_page_wrapper"
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
  # Early USB device access check (before dependencies)
  if [ "$(uname -s)" != "Darwin" ] && [ "${NO_UDEV}" -eq 0 ]; then
    log "Early USB device access check..."
    check_usb_access_needs
    echo
  fi

  # Suggest wallpaper optimization for low-end devices
  suggest_disable_wallpapers
  echo

  # Check if sudo is available
  if ! command -v sudo >/dev/null 2>&1; then
    die "sudo is required for installation."
  fi

  # Interactive setup by default (can be disabled with --no-interactive)
  if [ "${INTERACTIVE}" -eq 1 ]; then
    interactive_setup
  fi

  platform="$(detect_platform)"
  case "${platform}" in
    debian) install_deps_debian ;;
    arch) install_deps_arch ;;
    redhat) install_deps_redhat ;;
    brew) install_deps_brew ;;
    *) die "Unsupported platform. Install deps manually: gcc/make/pkg-config, hidapi+libusb, zlib+libpng, ffmpeg, ImageMagick, (optional) cairo+librsvg, jq, bc, netcat, socat." ;;
  esac

  # Setup udev rules for USB device access (only if needed)
  setup_udev_rules

  # Setup environment file (interactive by default)
  setup_env_file

  # Download required MDI icons
  setup_mdi_icons

  # Setup fonts
  ensure_fonts

  # Build binaries
  build_all

  log "Done."
  log ""
  log "Next steps:"
  log "1. Connect your Ulanzi D200 device"
  log "2. Run: ./launch_stack.sh --byobu"
  log ""
  log "If device is not found, try:"
  log "- Unplug/replug the USB device"
  log "- Run: groups | grep plugdev (should contain your username)"
  log "- If needed: newgrp plugdev or log out/in"
}

main
