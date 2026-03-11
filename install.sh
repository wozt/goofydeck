#!/bin/bash

set -euo pipefail

# Constants
readonly ROOT="$(cd "$(dirname "$0")" && pwd)"
readonly ENV_FILE="${ROOT}/.env"
readonly FONT_DIR="${ROOT}/fonts"
readonly COLLECT_FONTS="${ROOT}/icons/collect_fonts.sh"

# Logging functions
log() {
  echo "[install] $*"
}

log_success() {
  echo "[install] ✅ $*"
}

log_warning() {
  echo "[install] ⚠️  $*"
}

log_error() {
  echo "[install] ❌ $*" >&2
}

die() {
  log_error "$*"
  exit 1
}

# Utility functions

need_build() {
  [ ! -f "${ROOT}/ulanzi_d200_daemon" ] || [ ! -f "${ROOT}/bin/send_image_page" ]
}

is_rpi_zero_2w() {
  if grep -q "Raspberry Pi Zero 2 W" /proc/cpuinfo 2>/dev/null; then
    return 0
  fi
  if grep -q "BCM2835" /proc/cpuinfo 2>/dev/null && [ "$(awk '/MemTotal/ {print int($2/1024)}' /proc/meminfo)" -lt 600 ]; then
    return 0
  fi
  return 1
}

detect_platform() {
  if [ "$(uname -s)" = "Darwin" ]; then
    echo "brew"
  elif command -v apt >/dev/null 2>&1; then
    echo "debian"
  elif command -v pacman >/dev/null 2>&1; then
    echo "arch"
  elif command -v dnf >/dev/null 2>&1 || command -v yum >/dev/null 2>&1; then
    echo "redhat"
  else
    echo "unknown"
  fi
}

# Step 1: USB Device Permissions
setup_usb_permissions() {
  log "Step 1: USB Device Permissions"
  echo "================================"
  
  if [ "$(uname -s)" = "Darwin" ]; then
    log "USB permissions: Not applicable on macOS"
    return 0
  fi
  
  echo "USB device permissions will be checked and configured if needed"
  read -p "Setup USB device permissions (udev rules) for Ulanzi D200? [Y/n]: " setup_udev
  
  if [[ "${setup_udev}" =~ ^[Nn]*$ ]]; then
    log "Skipping udev rules setup (may cause device access issues)"
    return 0
  fi
  
  log "Setting up udev rules..."
  local udev_file="/etc/udev/rules.d/50-ulanzi.rules"
  local udev_rule='SUBSYSTEM=="usb", ATTR{idVendor}=="2207", ATTR{idProduct}=="0019", MODE="0666", GROUP="plugdev"'
  
  if [ -f "${udev_file}" ]; then
    log "Udev rules already exist at ${udev_file}"
  else
    echo "${udev_rule}" | sudo tee "${udev_file}" > /dev/null
    log_success "Created udev rules at ${udev_file}"
  fi
  
  sudo udevadm control --reload-rules
  sudo udevadm trigger
  log_success "USB permissions configured"
}

# Step 2: Environment Configuration
setup_environment() {
  log "Step 2: Environment Configuration"
  echo "=================================="
  
  local current_user
  current_user="$(whoami)"
  echo "Current user: ${current_user}"
  
  read -p "Recreate environment file (.env)? [y/N]: " recreate_env
  recreate_env="${recreate_env:-no}"
  
  if [ "${recreate_env}" = "no" ] && [ -f "${ENV_FILE}" ]; then
    log "Environment file already exists, keeping current configuration"
    return 0
  fi
  
  log "Setting up environment file..."
  local example_env="${ROOT}/example.env"
  
  if [ ! -f "${example_env}" ]; then
    die "example.env not found at ${example_env}"
  fi
  
  cp "${example_env}" "${ENV_FILE}"
  log "Created ${ENV_FILE} from example.env"
  
  # Setup USERNAME
  read -p "Set USERNAME in .env? [Y/n]: " setup_username
  if [[ "${setup_username}" =~ ^[Yy]*$ ]] || [ -z "${setup_username}" ]; then
    sed -i "s/USERNAME=\"<username>\"/USERNAME=\"${current_user}\"/" "${ENV_FILE}"
    log_success "Set USERNAME=${current_user}"
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
    
    sed -i "s|HA_HOST=\"ws://localhost:8123\"|HA_HOST=\"${ha_host}\"|" "${ENV_FILE}"
    if [ -n "${ha_token}" ]; then
      sed -i "s|HA_ACCESS_TOKEN=\"\"|HA_ACCESS_TOKEN=\"${ha_token}\"|" "${ENV_FILE}"
      log_success "Set HA_ACCESS_TOKEN=[configured]"
    fi
    log_success "Set HA_HOST=${ha_host}"
  fi
  
  log_success "Environment file configured at ${ENV_FILE}"
}

# Step 3: MDI Icons Download
setup_mdi_icons() {
  log "Step 3: MDI Icons Download"
  echo "============================="
  
  read -p "Download MDI icons from configuration.yml? [Y/n]: " download_mdi
  if [[ "${download_mdi}" =~ ^[Nn]*$ ]]; then
    log "Skipping MDI icons download"
    return 0
  fi
  
  local config_file="${ROOT}/config/configuration.yml"
  local mdi_dir="${ROOT}/assets/mdi"
  
  if [ ! -f "${config_file}" ]; then
    log_warning "Configuration file not found at ${config_file}"
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
    if [ ! -f "${icon_path}" ]; then
      missing_count=$((missing_count + 1))
      missing_icons="${missing_icons} ${icon}"
    fi
  done
  
  if [ "${missing_count}" -eq 0 ]; then
    log_success "All ${icon_count} MDI icons already present"
    return 0
  fi
  
  log "Downloading ${missing_count} missing MDI icons..."
  mkdir -p "${mdi_dir}"
  
  # Use the external script for download
  if [ -f "${ROOT}/icons/download_mdi.sh" ]; then
    "${ROOT}/icons/download_mdi.sh"
    log_success "MDI icons download completed"
  else
    log_warning "download_mdi.sh not found, skipping icon download"
  fi
}

# Step 4: Fonts Setup
setup_fonts() {
  log "Step 4: Fonts Setup"
  echo "===================="
  
  read -p "Setup fonts collection? [Y/n]: " setup_fonts
  if [[ "${setup_fonts}" =~ ^[Nn]*$ ]]; then
    log "Skipping fonts setup"
    return 0
  fi
  
  if [ ! -f "${COLLECT_FONTS}" ]; then
    log_warning "Font collection script not found at ${COLLECT_FONTS}"
    return 0
  fi
  
  log "Collecting fonts into ${FONT_DIR}..."
  "${COLLECT_FONTS}"
  log_success "Fonts setup completed"
}

# Step 5: Dependencies Installation
install_dependencies() {
  log "Step 5: System Dependencies"
  echo "==========================="
  
  local platform
  platform=$(detect_platform)
  
  case "${platform}" in
    debian)
      log "Installing Debian/Ubuntu dependencies..."
      sudo apt update
      sudo apt install -y build-essential pkg-config git ca-certificates \
        libhidapi-dev libusb-1.0-0-dev zlib1g-dev libpng-dev \
        libyaml-dev libssl-dev ffmpeg libavformat-dev libavcodec-dev \
        libavutil-dev libswscale-dev imagemagick librsvg2-bin \
        librsvg2-dev libcairo2-dev libusb-dev jq bc netcat-openbsd \
        socat fonts-noto-core fonts-noto-color-emoji sox sonic-pi
      ;;
    arch)
      log "Installing Arch Linux dependencies..."
      sudo pacman -S --needed base-devel pkg-config git ca-certificates \
        hidapi libusb zlib libpng libyaml openssl ffmpeg \
        imagemagick librsvg cairo libusb jq bc netcat socat \
        noto-fonts noto-fonts-emoji
      ;;
    *)
      log_warning "Unsupported platform: ${platform}"
      log "Please install dependencies manually"
      return 0
      ;;
  esac
  
  log_success "Dependencies installed"
}

# Step 6: Compilation
compile_binaries() {
  log "Step 6: Compile Binaries"
  echo "========================"
  
  if ! need_build; then
    log "Binaries already exist, skipping compilation"
    return 0
  fi
  
  read -p "Compile binaries? [Y/n]: " compile
  if [[ "${compile}" =~ ^[Nn]*$ ]]; then
    log "Skipping compilation"
    return 0
  fi
  
  log "Building (make all)..."
  make -C "${ROOT}" all
  log_success "Compilation completed"
}

# Step 7: Systemd Service
setup_systemd_service() {
  log "Step 7: Systemd Service"
  echo "======================="
  
  if [ "$(uname -s)" != "Linux" ]; then
    log "Systemd service: Not applicable on this platform"
    return 0
  fi
  
  read -p "Install GoofyDeck as systemd service (auto-start at boot)? [y/N]: " install_service
  if [[ "${install_service}" =~ ^[Nn]*$ ]] || [ -z "${install_service}" ]; then
    log "Skipping systemd service installation"
    return 0
  fi
  
  log "Installing systemd service..."
  
  local service_file="/etc/systemd/system/goofydeck.service"
  local service_content="[Unit]
Description=GoofyDeck Service
After=network.target

[Service]
Type=simple
User=$(whoami)
WorkingDirectory=${ROOT}
Environment=HOME=$(dirname "${ROOT}")
ExecStart=${ROOT}/launch_stack.sh --byobu
ExecReload=/bin/kill -HUP \$MAINPID
KillMode=mixed
TimeoutStopSec=5
PrivateTmp=yes
Restart=on-failure
RestartSec=10

[Install]
WantedBy=multi-user.target"

  echo "${service_content}" | sudo tee "${service_file}" > /dev/null
  sudo systemctl daemon-reload
  sudo systemctl enable goofydeck
  log_success "Systemd service installed and enabled"
  log "To start the service: sudo systemctl start goofydeck"
}

# Step 8: Performance Optimization
optimize_performance() {
  log "Step 8: Performance Optimization"
  echo "==============================="
  
  if ! is_rpi_zero_2w; then
    log "Performance optimization not needed for this device"
    return 0
  fi
  
  log_warning "⚠️  Raspberry Pi Zero 2W detected with limited RAM"
  log_warning "Wallpapers can impact performance on this device."
  
  read -p "Disable wallpapers in configuration for better performance? [Y/n]: " disable_wallpapers
  if [[ "${disable_wallpapers}" =~ ^[Yy]*$ ]] || [ -z "${disable_wallpapers}" ]; then
    local config_file="${ROOT}/config/configuration.yml"
    if [ -f "${config_file}" ]; then
      if grep -q "^global:" "${config_file}"; then
        if grep -q "disable_wallpapers:" "${config_file}"; then
          sed -i 's/^  disable_wallpapers:.*$/  disable_wallpapers: true/' "${config_file}"
        else
          sed -i '/^global:/a\  disable_wallpapers: true' "${config_file}"
        fi
      else
        sed -i '1i\global:\n  disable_wallpapers: true' "${config_file}"
      fi
      log_success "Wallpapers disabled in configuration.yml"
    fi
  fi
}

# Main installation flow
main() {
  log "GoofyDeck Interactive Installation"
  echo "================================="
  echo
  
  # Run all setup steps
  setup_usb_permissions
  echo
  setup_environment
  echo
  setup_mdi_icons
  echo
  setup_fonts
  echo
  install_dependencies
  echo
  compile_binaries
  echo
  setup_systemd_service
  echo
  optimize_performance
  echo
  
  # Final summary
  log_success "Installation completed!"
  echo
  log "Next steps:"
  log "1. Connect your Ulanzi D200 device"
  log "2. Run: ./launch_stack.sh --byobu"
  echo
  log "If device is not found, try:"
  log "- Unplug/replug the USB device"
  log "- Run: groups | grep plugdev (should contain your username)"
  log "- If needed: newgrp plugdev or log out/in"
}

# Run main function
main
