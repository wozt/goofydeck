# GoofyDeck

GoofyDeck is a comprehensive project for managing the Ulanzi D200 device, including a C daemon, utilities, and scripts to control the screen and buttons.
/!\ this project is still under construction, not all function are implemented yet /!\

## 📋 Table of Contents

- [Prerequisites](#prerequisites)
- [Installation](#installation)
- [Usage](#usage)
- [Commands](#commands)
- [Utility Scripts](#utility-scripts)
- [Project Structure](#project-structure)
- [Troubleshooting](#troubleshooting)

## 🚀 Prerequisites

### Supported Systems
- **Linux**: Debian/Ubuntu, Arch Linux, RedHat/Fedora
- **macOS**: via Homebrew

### Required Dependencies
The installation script will automatically install:
- **Build Tools**: gcc, make, pkg-config
- **System Libraries**: hidapi, libusb, zlib, libpng, libyaml
- **Crypto/TLS**: OpenSSL (for Home Assistant wss://)
- **Multimedia**: ffmpeg, ImageMagick
- **Graphics**: cairo, librsvg (optional)
- **Utilities**: jq, bc, netcat, socat
- **Fonts**: Noto Fonts

## 🔧 Installation

### Automatic Installation (Recommended)

```bash
# Clone the repository
git clone <repository-url>
cd GoofyDeck

# Run complete installation
./install.sh
```

Automatic installation:
1. Detects your Linux/macOS distribution
2. Installs all system dependencies
3. Compiles necessary binaries
4. Configures fonts

### Installation Options

```bash
# Install dependencies only
./install.sh --deps-only

# Force complete recompilation
./install.sh --compile

# Do not compile binaries
./install.sh --no-compile

# Do not install fonts
./install.sh --no-fonts
```

### Manual Installation

If you prefer manual installation:

#### 1. Install Dependencies

**Debian/Ubuntu:**
```bash
sudo apt-get update
sudo apt-get install -y build-essential pkg-config git \
  libhidapi-dev libusb-1.0-0-dev zlib1g-dev libpng-dev libyaml-dev libssl-dev \
  ffmpeg imagemagick librsvg2-bin librsvg2-dev libcairo2-dev \
  jq bc netcat-openbsd socat fonts-noto-core fonts-noto-color-emoji
```

**Arch Linux:**
```bash
sudo pacman -Sy --noconfirm base-devel pkgconf git \
  hidapi libusb zlib libpng libyaml openssl ffmpeg imagemagick \
  librsvg cairo jq bc openbsd-netcat socat \
  noto-fonts noto-fonts-emoji
```

**RedHat/Fedora:**
```bash
sudo dnf install -y gcc gcc-c++ make pkgconf-pkg-config git \
  hidapi hidapi-devel libusbx-devel zlib zlib-devel libpng-devel libyaml-devel openssl-devel \
  ffmpeg ffmpeg-devel ImageMagick librsvg2-tools \
  librsvg2-devel cairo-devel jq bc nmap-ncat socat \
  google-noto-emoji-color-fonts google-noto-sans-fonts
```

**macOS (Homebrew):**
```bash
brew update
brew install git hidapi libusb zlib libpng libyaml openssl ffmpeg \
  imagemagick librsvg cairo jq bc netcat socat
brew install --cask font-noto-sans font-noto-emoji
```

#### 2. Compile Binaries

```bash
make all
```

## 🎯 Usage

### Start the Stack (Recommended)

```bash
# 2 panes (byobu): ulanzi_d200_demon + pagging_demon
./launch_stack.sh --byobu

# Stop everything started by launch_stack.sh (and kill the byobu session)
./launch_stack.sh --kill
```

### Start Manually

```bash
./ulanzi_d200_demon
./lib/pagging_demon
./lib/ha_demon
```

`pagging_demon`:
- connects to `/tmp/ulanzi_device.sock`
- reads button events (`read-buttons`)
- renders icons from `config/configuration.yml`

### Configuration

Config file: `config/configuration.yml`

High-level structure:
- `system_buttons`: reserves positions for navigation buttons (`$page.back`, `$page.previous`, `$page.next`)
- `presets`: icon/text styles (`default`, `$nav`, and custom presets)
- `pages`: page definitions; each page has `buttons:`

Actions:
- `tap_action.action: "$page.go_to"` + `tap_action.data: "<page_name>"` to enter a page
- Navigation actions are internal/system: `$page_back`, `$page_prev`, `$page_next` (shown automatically when needed)

The YAML is parsed using `libyaml` (no Python).

Home Assistant bindings (optional):
- Button `entity_id: <entity_id>` enables HA state tracking for that button when `ha_demon` is running.
- Button `tap_action.action: <domain>.<service>` calls HA via `ha_demon` (example: `light.toggle`).
  - If `tap_action.data` is omitted, `pagging_demon` sends `{"entity_id":"<entity_id>"}`.
  - If `tap_action.data` is present and is a JSON object/array, it is forwarded as HA service data (and `entity_id` is injected if missing and `entity_id:` is set).
- Script shortcut: `tap_action.action: script.<script_entity>` calls `script.turn_on` with `{"entity_id":"script.<script_entity>"}`.
- Button `states:` can map HA state strings (e.g. `"on"`, `"off"`) to display overrides; when the state matches, the daemon uses a cached icon variant named `itemN_<hash>_<state>.png` and updates the button via partial updates.

### Home Assistant (ha_demon)

`ha_demon` connects to Home Assistant via WebSocket and exposes a local unix socket for commands/events.

Add to `.env`:

```bash
# "wss" = secure websocket, "ws" = normal websocket
HA_HOST="ws://localhost:8123"
# Long-lived access token: https://www.home-assistant.io/docs/authentication
HA_ACCESS_TOKEN=""
```

Start:

```bash
./lib/ha_demon
```

### Start the Ulanzi D200 Daemon

```bash
# Launch the daemon
./ulanzi_d200_demon
```

The daemon:
- Creates a Unix socket: `/tmp/ulanzi_device.sock`
- Maintains connection with the device
- Sends automatic keep-alive every 24 seconds

### Environment Variables

```bash
# Enable debug mode
ULANZI_DEBUG=1 ./ulanzi_d200_demon

# Disable padding (fast mode)
ULANZI_FAST_NOPAD=1 ./ulanzi_d200_demon
```

## 📱 Commands

Commands are sent via the Unix socket:

```bash
# Use netcat
nc -U /tmp/ulanzi_device.sock
```

### Available Commands

| Command | Description | Example |
|---------|-------------|---------|
| `ping` | Manual keep-alive | `ping` |
| `set-brightness` | Brightness (0-100) | `set-brightness 75` |
| `set-small-window` | Small window mode | `set-small-window cpu 1 1 12:30:00 0` |
| `set-label-style` | Label style (JSON) | `set-label-style style.json` |
| `set-buttons` | Send pre-built ZIP | `set-buttons buttons.zip` |
| `set-buttons-explicit` | Explicit buttons | `set-buttons-explicit --button-1=icon.png --label-1=Text` |
| `set-buttons-explicit-14` | Explicit buttons + button 14 | `set-buttons-explicit-14 --button-14=icon14.png` |
| `set-partial-explicit` | Partial update | `set-partial-explicit --button-5=icon.png` |
| `read-buttons` | Listen to button events | `read-buttons` |

### Explicit Command Format

```bash
# Buttons 1-13 with labels
set-buttons-explicit \
  --button-1=cpu.png --label-1=CPU \
  --button-2=mem.png --label-2=RAM \
  --button-3=disk.png --label-3=Disk

# Button 14 (no label)
set-buttons-explicit-14 --button-14=weather.png

# Partial update
set-partial-explicit --button-7=notification.png
```

## 🛠️ Utility Scripts

The project includes many scripts in the `lib/` folder:

### Main Scripts

```bash
# Send an image to a specific button
./lib/send_button.sh 5 icon.png "Label"

# Send a complete page (13 buttons)
./lib/send_page.sh icon1.png icon2.png ... icon13.png

# Send from folder (first 13 images)
./lib/send_page_from_folder.sh /path/to/icons/

# Process an image into 14 tiles
./lib/send_image_page.sh image.jpg

# Control brightness
./lib/set_brightness.sh 75

# Manual ping
./lib/ping_alive.sh
```

### Advanced Scripts

```bash
# Flash random icons
./lib/flash_icons.sh

# Send video page
./lib/send_video_page_wrapper.sh video.mp4
```

## 📁 Project Structure

```
GoofyDeck/
├── ulanzi_d200_demon          # Main C daemon
├── ulanzi_d200.c              # Daemon source code
├── launch_stack.sh            # Local launcher (byobu)
├── install.sh                 # Installation script
├── Makefile                   # Build file
├── lib/                       # Utility scripts
│   ├── pagging_demon           # Paging daemon (config-driven)
│   ├── send_button.sh
│   ├── send_page.sh
│   ├── send_image_page.sh
│   └── ...
├── src/                       # Additional C sources
│   ├── lib/
│   ├── icons/
│   └── standalone/
├── icons/                     # Icons and graphics tools
├── fonts/                     # Collected fonts
├── presetpages/               # Preset pages
├── legacy/                    # Old Python files
└── .env                       # Environment configuration
```

## 🔍 Troubleshooting

### Common Issues

**Device not detected:**
```bash
# Check USB connection
lsusb | grep -i ulanzi

# Check permissions
sudo usermod -a -G plugdev $USER
# Reconnect after this command
```

**Compilation error:**
```bash
# Clean and recompile
make clean
make all

# Check dependencies
pkg-config --exists hidapi
pkg-config --exists libpng
pkg-config --exists yaml-0.1
```

**Socket not responding:**
```bash
# Check if daemon is running
ps aux | grep ulanzi_d200_demon

# Restart daemon
pkill ulanzi_d200_demon
./ulanzi_d200_demon
```

**Font issues:**
```bash
# Reinstall fonts
./install.sh --no-compile --deps-only

# Check system fonts
fc-list | grep -i noto
```

### Advanced Debug

```bash
# Detailed debug mode
ULANZI_DEBUG=1 ./ulanzi_d200_demon 2>&1 | tee debug.log

# Test manual communication
echo "ping" | nc -U /tmp/ulanzi_device.sock

# Monitor button events
echo "read-buttons" | nc -U /tmp/ulanzi_device.sock
```

## 📝 Technical Notes

- **Unix Socket**: `/tmp/ulanzi_device.sock`
- **Keep-alive**: 24 seconds (automatic)
- **Buttons**: 13 main buttons + 1 special button
- **Resolution**: 1280x720 pixels for full images
- **Format**: PNG recommended for icons
- **Padding**: Automatically handled to avoid invalid bytes

## 🤝 Contributing

The project is developed in C for optimal performance, with bash scripts for automation. Old Python versions are kept in `legacy/` for reference.

## 📄 License

[To be completed according to your license]
