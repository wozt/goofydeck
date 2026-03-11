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
  
  # First, offer the complete download option
  echo "MDI Icons Download Options:"
  echo "1) Download ALL Material Design Icons (7000+ files, ~50MB)"
  echo "2) Download only icons from configuration.yml"
  echo "3) Skip MDI icons download"
  echo
  
  # Information about complete download benefits
  echo "📖 Why download ALL MDI icons?"
  echo "   ⚡ More practical: No runtime downloads when adding new icons"
  echo "   ⚡ Faster: Icons are instantly available for miniapps"
  echo "   ⚡ Very useful: Essential for custom miniapps development"
  echo "   ⚡ Future-proof: Supports any icon without re-running installation"
  echo "   ⚡ Complete access: 7000+ icons for unlimited creativity"
  echo
  
  while true; do
    read -p "Choose option [1/2/3]: " mdi_option
    case "${mdi_option}" in
      1)
        log "Downloading ALL Material Design Icons..."
        log "⚠️  This will download 7000+ SVG files (~50MB)"
        log "⚠️  This may take several minutes depending on your connection"
        echo
        read -p "Confirm download ALL MDI icons? [y/N]: " confirm_all
        if [[ "${confirm_all}" =~ ^[Yy]*$ ]]; then
          download_all_mdi_icons
          log_success "All MDI icons downloaded successfully"
          log "🎉 You now have instant access to 7000+ icons for your miniapps!"
          return 0
        else
          echo "Cancelled. Choose another option."
          continue
        fi
        ;;
      2)
        download_config_mdi_icons
        return 0
        ;;
      3)
        log "Skipping MDI icons download"
        log "⚠️  Note: Icons will be downloaded on-demand during runtime (slower)"
        return 0
        ;;
      *)
        echo "Invalid option. Please choose 1, 2, or 3."
        ;;
    esac
  done
}

# Download ALL MDI icons (complete set)
download_all_mdi_icons() {
  local mdi_dir="${ROOT}/assets/mdi"
  mkdir -p "${mdi_dir}"
  
  log "Starting complete MDI icons download..."
  log "This may take several minutes..."
  
  # Use the external script for complete download
  if [ -f "${ROOT}/icons/download_mdi.sh" ]; then
    log "Using download_mdi.sh for complete download..."
    "${ROOT}/icons/download_mdi.sh" all
    local total_count
    total_count=$(ls -1 "${mdi_dir}"/*.svg 2>/dev/null | wc -l)
    log_success "Complete download finished: ${total_count} icons downloaded"
  else
    log_error "download_mdi.sh not found. Cannot download complete set."
    return 1
  fi
}

# Download only icons from configuration (original behavior)
download_config_mdi_icons() {
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
  
  # Fast individual download approach
  local downloaded=0
  local failed=0
  local github_url="https://raw.githubusercontent.com/Templarian/MaterialDesign/master/svg"
  
  for icon in ${missing_icons}; do
    local icon_url="${github_url}/${icon}.svg"
    local target_file="${mdi_dir}/${icon}.svg"
    
    if [ -f "${target_file}" ]; then
      continue
    fi
    
    log "Downloading ${icon}.svg..."
    
    if command -v curl >/dev/null 2>&1; then
      if curl -s --connect-timeout 10 --max-time 30 "${icon_url}" -o "${target_file}" 2>/dev/null && [ -s "${target_file}" ]; then
        downloaded=$((downloaded + 1))
        log_success "Downloaded ${icon}.svg"
      else
        failed=$((failed + 1))
        rm -f "${target_file}" 2>/dev/null
        log_warning "Failed to download ${icon}.svg"
      fi
    elif command -v wget >/dev/null 2>&1; then
      if wget -q --timeout=30 --tries=3 "${icon_url}" -O "${target_file}" 2>/dev/null && [ -s "${target_file}" ]; then
        downloaded=$((downloaded + 1))
        log_success "Downloaded ${icon}.svg"
      else
        failed=$((failed + 1))
        rm -f "${target_file}" 2>/dev/null
        log_warning "Failed to download ${icon}.svg"
      fi
    else
      log_warning "Neither curl nor wget available"
      failed=${missing_count}
      break
    fi
  done
  
  if [ "${downloaded}" -gt 0 ]; then
    log_success "MDI icons download completed: ${downloaded} downloaded, ${failed} failed"
  fi
  
  if [ "${failed}" -gt 0 ]; then
    log_warning "Some icons failed to download. You can run manually:"
    log_warning "${ROOT}/icons/download_mdi.sh"
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
    export ENABLE_SYSTEMD="false"
    return 0
  fi
  
  log "Installing systemd service..."
  export ENABLE_SYSTEMD="true"
  
  # Create a daemon script that keeps processes running
  local daemon_script="${ROOT}/goofydeck-daemon.sh"
  cat > "${daemon_script}" << 'EOF'
#!/bin/bash

set -euo pipefail

ROOT="$(cd "$(dirname "$0")" && pwd)"
PIDS_DIR="${ROOT}/pids"

# Create pids directory
mkdir -p "${PIDS_DIR}"

# Function to start a daemon
start_daemon() {
    local name="$1"
    local cmd="$2"
    local pid_file="${PIDS_DIR}/${name}.pid"
    
    if [ -f "${pid_file}" ] && kill -0 "$(cat "${pid_file}")" 2>/dev/null; then
        echo "${name} already running (PID: $(cat "${pid_file}"))"
        return 0
    fi
    
    echo "Starting ${name}..."
    nohup bash -c "${cmd}" > /dev/null 2>&1 &
    echo $! > "${pid_file}"
    echo "${name} started (PID: $(cat "${pid_file}"))"
}

# Function to stop a daemon
stop_daemon() {
    local name="$1"
    local pid_file="${PIDS_DIR}/${name}.pid"
    
    if [ -f "${pid_file}" ]; then
        local pid="$(cat "${pid_file}")"
        if kill -0 "${pid}" 2>/dev/null; then
            echo "Stopping ${name} (PID: ${pid})..."
            kill "${pid}"
            # Wait for process to stop
            for i in {1..10}; do
                if ! kill -0 "${pid}" 2>/dev/null; then
                    break
                fi
                sleep 1
            done
            # Force kill if still running
            if kill -0 "${pid}" 2>/dev/null; then
                kill -9 "${pid}"
            fi
        fi
        rm -f "${pid_file}"
        echo "${name} stopped"
    fi
}

# Function to check daemon status
check_daemon() {
    local name="$1"
    local pid_file="${PIDS_DIR}/${name}.pid"
    
    if [ -f "${pid_file}" ] && kill -0 "$(cat "${pid_file}")" 2>/dev/null; then
        echo "${name} is running (PID: $(cat "${pid_file}"))"
        return 0
    else
        echo "${name} is not running"
        return 1
    fi
}

# Handle signals
cleanup() {
    echo "Stopping all daemons..."
    stop_daemon "ulanzi_d200_daemon"
    stop_daemon "ha_daemon"
    stop_daemon "paging_daemon"
    exit 0
}

trap cleanup SIGTERM SIGINT

# Start all daemons
start_daemon "ulanzi_d200_daemon" "cd '${ROOT}' && ./ulanzi_d200_daemon"
sleep 2
start_daemon "ha_daemon" "cd '${ROOT}' && ./bin/ha_daemon"
sleep 2
start_daemon "paging_daemon" "cd '${ROOT}' && ./bin/paging_daemon"

echo "All daemons started. Monitoring..."

# Keep running and monitor daemons
while true; do
    sleep 30
    
    # Check if daemons are still running, restart if needed
    if ! check_daemon "ulanzi_d200_daemon"; then
        echo "Restarting ulanzi_d200_daemon..."
        start_daemon "ulanzi_d200_daemon" "cd '${ROOT}' && ./ulanzi_d200_daemon"
    fi
    
    if ! check_daemon "ha_daemon"; then
        echo "Restarting ha_daemon..."
        start_daemon "ha_daemon" "cd '${ROOT}' && ./bin/ha_daemon"
    fi
    
    if ! check_daemon "paging_daemon"; then
        echo "Restarting paging_daemon..."
        start_daemon "paging_daemon" "cd '${ROOT}' && ./bin/paging_daemon"
    fi
done
EOF

  chmod +x "${daemon_script}"
  
  local service_file="/etc/systemd/system/goofydeck.service"
  local service_content="[Unit]
Description=GoofyDeck Service
After=network.target

[Service]
Type=simple
User=$(whoami)
WorkingDirectory=${ROOT}
Environment=HOME=$(dirname "${ROOT}")
ExecStart=${daemon_script}
ExecReload=/bin/kill -HUP \$MAINPID
KillMode=mixed
TimeoutStopSec=10
Restart=on-failure
RestartSec=10
StandardOutput=journal
StandardError=journal

[Install]
WantedBy=multi-user.target"

  echo "${service_content}" | sudo tee "${service_file}" > /dev/null
  sudo systemctl daemon-reload
  sudo systemctl enable goofydeck
  log_success "Systemd service installed and enabled"
  log "To start the service: sudo systemctl start goofydeck"
  log "To stop the service: sudo systemctl stop goofydeck"
  log "To view logs: sudo journalctl -u goofydeck.service -f"
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
  
  # Initialize systemd service flag
  export ENABLE_SYSTEMD="false"
  
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
  log "1. Connect your Ulanzi D200 device if not done already"
  log "2a. Run: ./launch_stack.sh --byobu to launch manually"
  log "2b. Restart your host device OR start the service with: sudo systemctl start goofydeck" 
  if [ "${ENABLE_SYSTEMD}" = "true" ]; then
    log "   (systemd service was enabled during installation)"
  fi
  echo
  log "If device is not found, try:"
  log "- Unplug/replug the USB device"
  log "- Run: groups | grep plugdev (should contain your username)"
  log "- If needed: newgrp plugdev or log out/in"
  # Offer to start the service if it was installed
  if [ "${ENABLE_SYSTEMD}" = "true" ]; then
    echo "🚀 Quick Start Option:"
    read -p "Start goofydeck service now? [y/N]: " start_service
    if [[ "${start_service}" =~ ^[Yy]*$ ]]; then
      log "Starting goofydeck service..."
      if sudo systemctl start goofydeck.service; then
        sleep 2
        if sudo systemctl is-active --quiet goofydeck.service; then
          log_success "✅ goofydeck service is now running!"
          log "View logs with: sudo journalctl -u goofydeck.service -f"
          log "Stop service with: sudo systemctl stop goofydeck.service"
        else
          log_warning "⚠️  Service started but may not be active yet"
          log "Check status with: sudo systemctl status goofydeck.service"
        fi
      else
        log_error "❌ Failed to start goofydeck service"
        log "You can start it manually with: sudo systemctl start goofydeck.service"
      fi
    else
      log "Service not started. You can start it later with:"
      log "sudo systemctl start goofydeck.service"
    fi
    echo
  fi
}

# Run main function
main
