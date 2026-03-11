# GoofyDeck

GoofyDeck controls an Ulanzi D200 with C daemons and config-driven paging, with optional Home Assistant integration (`ha_daemon`).
~ This project is still under construction /!\ But WAY more stable now as it can be considered as a "pre-release" ~ 

The previous (long) README has been archived to `legacy/README.md`.

## 🚀 Quick Start

```bash
# Clone the repository
git clone <repository-url>
cd GoofyDeck

# Interactive installation (recommended)
./install.sh

# Launch GoofyDeck
./launch_stack.sh --byobu
```

### 🎯 Interactive Installation

The new `install.sh` provides a fully interactive installation experience:

- **🔧 USB Device Permissions**: Automatic udev rules configuration
- **⚙️ Environment Setup**: .env file with USERNAME and Home Assistant integration
- **🎨 MDI Icons**: Choice between complete set (7000+ icons) or configuration-specific
- **📚 Fonts Collection**: Optional font installation
- **🔨 Dependencies**: Automatic system dependencies installation
- **⚡ Compilation**: Binary compilation when needed
- **🚀 Systemd Service**: Optional auto-start service installation
- **⚡ Performance**: RPI Zero 2W optimization

### 🎨 MDI Icons Download Options

The installer offers flexible MDI icons download:

**Option 1: Complete Set (Recommended)**
- Downloads all 7000+ Material Design Icons (~50MB)
- ⚡ **More practical**: No runtime downloads when adding new icons
- ⚡ **Faster**: Icons instantly available for miniapps
- ⚡ **Very useful**: Essential for custom miniapps development
- ⚡ **Future-proof**: Supports any icon without re-running installation

**Option 2: Configuration Only**
- Downloads only icons found in your `configuration.yml`
- Smaller download size
- Runtime downloads for new icons

**Option 3: Skip**
- Icons downloaded on-demand during runtime (slower)

### 🎯 Raspberry Pi Zero 2W Optimization

The installer automatically detects Raspberry Pi Zero 2W and other low-memory devices, offering to:
- Disable wallpapers for better performance
- Use optimized MDI download methods
- Reduce memory footprint

### 🔧 Systemd Service (Optional)

For automatic startup at boot, GoofyDeck can be installed as a systemd service:

**During installation:**
```bash
./install.sh
# The installer will ask if you want to install the service
```

**Service Features:**
- 🔄 **Automatic restart** if processes crash
- 📊 **Process monitoring** with auto-recovery
- 📝 **Integrated logging** via systemd journal
- ⚡ **Optimized for daemon** operation (no interactive terminal needed)

**Service Management:**
```bash
# Check service status
sudo systemctl status goofydeck

# View service logs
sudo journalctl -u goofydeck -f

# Start/stop service
sudo systemctl start goofydeck
# Stop service
sudo systemctl stop goofydeck

# Disable autostart
sudo systemctl disable goofydeck
```

**To run manually instead of service:**
```bash
sudo systemctl stop goofydeck
cd /path/to/GoofyDeck
./launch_stack.sh --byobu
```

## Prerequisites

- **Firmware update**: The Ulanzi D200 must have its firmware updated once using the official Windows software. Using a Windows VM works for this step.
- **System dependencies**: Automatically installed by `install.sh`
- **sudo access**: Required for USB permissions and systemd service

## Build & Installation

### Option 1: Interactive Installation (Recommended)
```bash
./install.sh
```
This handles everything automatically:
- System dependencies
- USB permissions
- Environment configuration
- MDI icons download
- Binary compilation
- Optional systemd service

### Option 2: Manual Build
```bash
# Install dependencies manually (see install.sh for details)
sudo apt install build-essential pkg-config git libhidapi-dev libusb-1.0-0-dev zlib1g-dev libpng-dev libyaml-dev libssl-dev ffmpeg libavformat-dev libavcodec-dev libavutil-dev libswscale-dev imagemagick librsvg2-bin librsvg2-dev libcairo2-dev libusb-dev jq bc netcat-openbsd socat fonts-noto-core fonts-noto-color-emoji

# Build binaries
make all

# Download MDI icons manually
./icons/download_mdi.sh all
```

## Run

### Method 1: Systemd Service (Recommended)
```bash
# Start the service
sudo systemctl start goofydeck

# View logs
sudo journalctl -u goofydeck -f
```

### Method 2: Manual Launch
```bash
# Interactive terminal (recommended for development)
./launch_stack.sh --byobu

# Background mode
./launch_stack.sh

# Stop all processes
./launch_stack.sh --kill
```

Manual:

```bash
./ulanzi_d200_daemon
./bin/paging_daemon
./bin/ha_daemon
```

## 🛠️ Troubleshooting

### Device Not Found
```bash
# Check USB device access
lsusb | grep 2207:0019

# Check user groups
groups | grep plugdev

# If not in plugdev group:
sudo usermod -a -G plugdev $USER
# Then logout and login again
```

### Service Issues
```bash
# Check service status
sudo systemctl status goofydeck

# View service logs
sudo journalctl -u goofydeck -n 50

# Restart service
sudo systemctl restart goofydeck
```

### MDI Icons Issues
```bash
# Check MDI icons directory
ls -la assets/mdi/ | wc -l

# Re-download all icons
./icons/download_mdi.sh all

# Clear cache and retry
rm -f .cache/mdi_dl_*.once
./install.sh
```

### Performance Issues (RPI Zero 2W)
```bash
# Check if wallpapers are disabled
grep "disable_wallpapers" config/configuration.yml

# Disable wallpapers manually
sed -i 's/disable_wallpapers: false/disable_wallpapers: true/' config/configuration.yml
```

## 🔧 Advanced Features

### Miniapps Development
With complete MDI icons download (7000+ icons), you have instant access to:
- Any Material Design icon for your miniapps
- No runtime download delays
- Complete creative freedom

### Home Assistant Integration
Configure in `.env` file:
```bash
HA_HOST="ws://your-home-assistant:8123"
HA_ACCESS_TOKEN="your_long_lived_token"
```

### Custom Fonts
The installer can download additional fonts for better text rendering in your pages.

## Sockets

- Ulanzi device daemon: `/tmp/ulanzi_device.sock`
- Paging control socket: `/tmp/goofydeck_paging_control.sock`
- Home Assistant daemon: `/tmp/goofydeck_ha.sock`

## Configuration

Main config: `config/configuration.yml` (parsed with `libyaml`, no Python).

### Top-Level Keys

- `brightness`: base backlight brightness (0–100).
- `sleep:`:
  - `dim_brightness`: brightness when dimmed (0–100)
  - `dim_timeout`: seconds of inactivity before dimming
  - `sleep_timeout`: seconds of inactivity before turning backlight off (brightness 0)
- `cmd_timeout_ms`: default timeout for `$cmd.*` executions (default `3000`)
- `wallpaper:` (optional): global wallpaper applied to pages (unless overridden per page).
  - `path`: path to the wallpaper image
  - `quality`: default `30`
  - `magnify`: default `100`
  - `dithering`: default `yes`
- `system_buttons:`: reserves positions (1–13) for navigation buttons.
  - `$page.back.position`
  - `$page.previous.position`
  - `$page.next.position`
- `presets:`: reusable styles for icons/text.
- `pages:`: the page tree; each page has a `buttons:` list.

### Wallpaper notes

When `wallpaper` is enabled, `paging_daemon` renders the wallpaper into **14 tiles** and sends them with the page (buttons 1–13 are composed as `tile + icon`, and button 14 is the raw tile).

Navigation buttons (`$page.previous/$page.next/$page.back`) are also composed against the current page wallpaper and cached per page.

How it works:
- You can enable a **global** `wallpaper:` and optionally override it per page (`pages.<name>.wallpaper:`).
- On first use, the wallpaper image is rendered into a folder next to the image: `<wallpaper filename without .png>/` containing `<name>-1.png` ... `<name>-14.png`.
- At runtime, the daemon copies the needed tiles into `/dev/shm/goofydeck/paging/` (session cache).
- For buttons 1–13, the daemon composes `tile + icon` using `draw_over` and sends a 14-button page update.

Recommendations (tune based on your SBC and the image content):
- Prefer **~360p wallpapers** for responsiveness.
- All image sizes are supported, but you typically need to lower `quality` as the resolution goes up.
  - **720p**: start around `quality: 30` (±10)
  - **360p**: start around `quality: 60` (±10)

Performance warning:
- Wallpaper composition adds CPU work and extra file I/O. It is **not recommended** on very small SBCs (e.g. Raspberry Pi Zero / Banana Pi) if you want fast navigation.
- Wallpaper also increases the amount of data that changes per page render (14 tiles + per-button compositions), which can make page transitions slower on weak hosts.

Storage warning:
- Wallpaper tiling creates small render files **next to your wallpaper image** (a folder named `<wallpaper filename without .png>/` containing `<name>-1.png` ... `<name>-14.png`).
- During runtime, additional session caches and temporary files are stored under `/dev/shm/goofydeck/paging/` (fallback: `/dev/shm/goofydeck_<uid>/paging/`) and are cleared on daemon start.

### External icons (`local:` / `url:`)

Buttons can use custom icons:
- `icon: local:<path>` for local PNG/SVG
- `icon: url:<url>` for remote PNG/SVG

The daemon normalizes them (crop to square, resize if needed, dither + palette reduction) so they fit the device constraints.
Normalized results are cached on disk under `.cache/external_icons/` and also copied to `/dev/shm/goofydeck/paging/external_icons_session/` for the current session.

Notes:
- If the icon is still too large after normalization, the daemon falls back to `assets/pregen/filetobig.png`.
- SVG generally gives the best quality/size ratio.

### `presets:` (Button Style)

A preset is a set of rendering defaults used by a button (and can also be overridden by a per-state entry).

Common fields:
- `icon`: optional default icon (ex: `mdi:sofa`)
- `icon_size`: icon size (pixels)
- `icon_padding`: padding (pixels)
- `icon_offset`: `x,y` (pixels)
- `icon_border_radius`: radius (pixels)
- `icon_border_width`: width (pixels), clamped to `1..98` if present
- `icon_border_color`: RGB hex, or `transparent`
- `icon_border_size`: border square size (pixels), default `196` (min `98`, max `196`)
- `icon_brightness`: clamped to `0..99` (values >= 99 => 99)
- `icon_color`: RGB hex or `transparent`
- `icon_background_color`: RGB hex or `transparent`
- `text`: optional text rendered inside the icon (different from the label sent to the device)
- `text_color`: default `FFFFFF` if missing
- `text_align`: default `center` if missing (`top`, `center`, `bottom`)
- `text_font`: default `Roboto` if empty
- `text_size`: default `40` if missing
- `text_offset`: `x,y` (pixels)

### `pages:` / `buttons:`

Each page is keyed by its name, for example:

```yml
pages:
  $root:
    buttons:
      - name: "Salon"        # label sent to the device (not rendered in icon unless you set `text:`)
        presets: [room_folder]
        icon: mdi:sofa
        text: "SALON"        # optional, rendered on the icon
        tap_action:
          action: $page.go_to
          data: salon

  salon:
    wallpaper:              # optional per-page override (same schema as global `wallpaper`)
      path: "mymedia/wallpapers/valley.png"
      quality: 30
      magnify: 100
      dithering: yes
    buttons:
      - name: "Lampe droite"
        entity_id: light.lampe_droite
        presets: [base_button]
        tap_action:
          action: light.toggle
        states:
          "on":
            name: "LD ON"
            presets: [light_on]
          "off":
            name: "LD OFF"
            presets: [light_off]
```

Button fields:
- `name`: label sent to the device (and used by HA state overrides via `states: ... name:`).
- `presets`: list of presets to apply (later items override earlier items).
- `icon`:
  - `mdi:<name>` (built-in Material Design Icons)
  - `local:<path>` (local file, PNG or SVG)
  - `url:<url>` (downloaded file, PNG or SVG)
  - empty string to omit
- `text`: optional icon text (static).
- `entity_id`: enables Home Assistant state tracking on that button when HA is enabled.
- `tap_action:` / `hold_action:` / `longhold_action:` / `released_action:`:
  - single action form:
    - `action`: either internal paging actions (start with `$`) or a Home Assistant service like `light.toggle`
    - `data`: optional:
      - for `$page.go_to`: target page name (string)
      - for HA services: JSON string/obj/array forwarded as HA `service_data` (and `entity_id` is injected when possible)
  - macro form (action sequences):
    - `actions:` list of steps, each step being `{action, data}` (executed in order)
- `states:`: map of HA state string → overrides (`name`, `presets`, and optionally `icon`/`text`).

### Action macros (`actions:`)

You can chain multiple actions for a single event using `actions:`:

```yml
longhold_action:
  actions:
    - action: $cmd.exec
      data:
        cmd: "assets/scripts/clear_cache.sh --clear"
    - action: $cmd.text_clear
```

This is useful for “do something, then restore base icon / clear overlay”.

## Host command buttons (`$cmd.*`)

`paging_daemon` can execute host commands (bash/scripts) and optionally render their output as dynamic text on the button.

Notes:
- Commands are executed through `/bin/sh -lc "<cmd>"` so you can use shell syntax (pipes, redirects, env vars, etc).
- For text output, stdout is used; if stdout is empty, stderr is used.
- On failure/timeout, the rendered text becomes `ERR`.
- If a button has no `icon:` (or an empty icon) and wallpaper is disabled, the daemon renders text on a generated 196×196 base (using the preset background/border) instead of `assets/pregen/empty.png` (which is intentionally 1×1 to keep ZIPs small).
- All temporary files are created under `/dev/shm/goofydeck/paging/` (fallback: `/dev/shm/goofydeck_<uid>/paging/`, then `/tmp/`).

### `$cmd.exec` (no text, fire-and-forget)

```yml
tap_action:
  action: $cmd.exec
  data:
    cmd: "notify-send 'hello'"
```

### `$cmd.exec_text` + `$cmd.text_clear` (render text once)

```yml
tap_action:
  action: $cmd.exec_text
  data:
    cmd: "date '+%H:%M:%S'"
    trim: yes
    max_len: 32
hold_action:
  action: $cmd.text_clear   # clears the text and re-sends the base icon
```

Text defaults (when missing in presets):
- `text_color: FFFFFF`
- `text_align: center`
- `text_font: Roboto`
- `text_size: 40`

Font note:
- Bold fonts tend to look much better on the D200 (thin fonts can become hard to read after palette reduction / dithering).

### `$cmd.poll_*` (manual start/stop)

Define what the poll does in `poll:`, and bind start/stop to actions:

```yml
tap_action:   { action: $cmd.poll_start }
hold_action:  { action: $cmd.poll_stop }   # also clears text + re-sends base icon
longhold_action: { action: $cmd.exec_stop } # stop everything for this button (poll + state), clear text/state

poll:
  every_ms: 1000
  action:
    action: $cmd.exec_text
    data:
      cmd: "/home/wozt/GoofyDeck/mymedia/scripts/cmd_demo_counter.sh"
      trim: yes
      max_len: 32
```

### Polling/loops warning (may be a device limitation)

The D200 can become slow or unstable if you refresh icons too frequently for a long time while staying on the same page (for example when using `$cmd.poll_*` or `state_cmd` for “live” widgets).

This appears to be a **device-side caching/RAM limitation**: it accumulates data internally and we do not have a reliable software way to purge it from the host (ADB is not available).

Recommendations:
- Prefer **manual refresh** when possible (one-shot update on tap), like the `RAM cache` example in `settings`.
- If you use polling for “visual” widgets, keep the interval reasonable and **stop it when you don’t need it** (otherwise it will eventually lag).
- If the device is heavily lagging after long runs, a full USB power cycle (unplug/replug the 5V power) clears the device-side state.

### `state_cmd` + `states:` (auto polling per page)

If a button defines `state_cmd`, `paging_daemon`:
- starts state polling automatically when you enter the page
- stops it automatically when you leave the page

You can use a separate tap action to toggle a state (optional):

```yml
tap_action:
  action: $cmd.exec
  data:
    cmd: "/home/wozt/GoofyDeck/mymedia/scripts/cmd_state_toggle.sh"

state_cmd:
  cmd: "/home/wozt/GoofyDeck/mymedia/scripts/cmd_state_get.sh"
  every_ms: 1500

states:
  "on":
    name: "ON"
    presets: [light_on]
  "off":
    name: "OFF"
    presets: [light_off]
```

### Cache management example (manual refresh)

The repository includes helper scripts to inspect/clear caches:
- `assets/scripts/clear_cache_ram.sh`
- `assets/scripts/clear_cache.sh`

They support:
- `--text` (prints 2 lines: `<N> files` then `<SIZE>`)
- `--clear` (no output)

Example button (manual refresh on tap, clear on longhold):

```yml
- name: "DISK cache"
  presets: [cmd_button]
  icon: mdi:database
  tap_action:
    action: $cmd.exec_text
    data:
      cmd: "assets/scripts/clear_cache.sh --text"
      trim: no
      max_len: 32
  longhold_action:
    actions:
      - action: $cmd.exec
        data:
          cmd: "assets/scripts/clear_cache.sh --clear"
      - action: $cmd.text_clear
```

## Paging control commands

Send one line to `/tmp/goofydeck_paging_control.sock`:

```bash
printf 'stop-control\n' | socat - UNIX-CONNECT:/tmp/goofydeck_paging_control.sock
printf 'start-control\n' | socat - UNIX-CONNECT:/tmp/goofydeck_paging_control.sock
printf 'load-last-page\n' | socat - UNIX-CONNECT:/tmp/goofydeck_paging_control.sock

# Simulate button events (same logic as physical buttons)
printf 'simule-button TAP1\n' | socat - UNIX-CONNECT:/tmp/goofydeck_paging_control.sock
printf 'simule-button LONGHOLD14\n' | socat - UNIX-CONNECT:/tmp/goofydeck_paging_control.sock
```

## Miniapps

Miniapps are small interactive programs (usually bash) that temporarily take control of the deck UI:
- They tell `paging_daemon` to stop reacting to physical buttons (`stop-control`).
- They subscribe to button events directly from `ulanzi_d200_daemon` (`read-buttons`).
- They render their own UI by sending full pages (`set-buttons-explicit-14`) and/or partial updates (`set-partial-explicit`).
- On exit, they return control to `paging_daemon` (`start-control`) and typically ask it to reload the last page (`load-last-page`).

Examples in this repo:
- `mymedia/miniapp/video_miniapp.sh` (video browser + player)
- `mymedia/miniapp/tictactoe_miniapp.sh` (tic-tac-toe)

### Miniapp skeleton (bash)

```bash
#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
ULANZI_SOCK="${ULANZI_SOCK:-/tmp/ulanzi_device.sock}"
PAGING_CTRL_SOCK="${PAGING_CTRL_SOCK:-/tmp/goofydeck_paging_control.sock}"

RAM_DIR="/dev/shm/goofydeck/myminiapp/$$"
mkdir -p "${RAM_DIR}"

cleanup() {
  rm -rf "${RAM_DIR}" 2>/dev/null || true
  printf 'start-control\n' | nc -w 1 -U "${PAGING_CTRL_SOCK}" >/dev/null 2>&1 || true
  printf 'load-last-page\n' | nc -w 1 -U "${PAGING_CTRL_SOCK}" >/dev/null 2>&1 || true
}
trap cleanup EXIT INT TERM

printf 'stop-control\n' | nc -w 1 -U "${PAGING_CTRL_SOCK}" >/dev/null 2>&1 || true

coproc RB { socat - UNIX-CONNECT:"${ULANZI_SOCK}"; }
printf 'read-buttons\n' >&"${RB[1]}"
read -r _ok <&"${RB[0]}" || true

# TODO: send UI (set-buttons-explicit-14)
# TODO: loop read events from "${RB[0]}" and react
```

### Reusing existing binaries (recommended)

Miniapps should reuse project tools instead of reinventing pipelines:
- Icon/image tools: `icons/draw_square`, `icons/draw_border`, `icons/draw_mdi`, `icons/draw_text`, `icons/draw_over`, `icons/draw_normalize`, `icons/draw_optimize`
- Video render/player helpers: `bin/send_video_page_wrapper`, `bin/play_rendered_video.sh`, `bin/convert_video.sh`

### Caching advice

For performance and to avoid disk churn:
- Disk cache for miniapps: `mymedia/miniapp/.cache/<miniappname>/` (persistent between runs)
- RAM cache for the current session: `/dev/shm/goofydeck/<miniappname>/<pid>/` (delete on exit)
- Pregen expensive assets once on disk, then copy them into RAM at startup.

### Adding a miniapp to the YAML config

Miniapps are typically started with a host command button:

```yml
pages:
  apps:
    buttons:
      - name: "My MiniApp"
        presets: [cmd_button]
        icon: mdi:gamepad-variant
        tap_action:
          action: $cmd.exec
          data:
            cmd: "mymedia/miniapp/my_miniapp.sh"
```

### Example LLM prompt (when generating a new miniapp)

Use a prompt like:

> Implement a GoofyDeck miniapp in bash at `mymedia/miniapp/<name>_miniapp.sh`.  
> Requirements: use `stop-control`/`start-control`/`load-last-page` via `/tmp/goofydeck_paging_control.sock`, subscribe to `read-buttons` from `/tmp/ulanzi_device.sock`, render UI with `set-buttons-explicit-14`, store persistent cache in `mymedia/miniapp/.cache/<name>/`, store session temp files in `/dev/shm/goofydeck/<name>/$$` and clean them on exit.

## Home Assistant (`ha_daemon`)

Create a Home Assistant **long-lived access token** (Profile → Security → Long-Lived Access Tokens), then put credentials in `.env` (see `example.env`):

```bash
HA_HOST="ws://localhost:8123"
HA_ACCESS_TOKEN=""
```

Optional:
- `USERNAME`: used by helper scripts like `bin/get_username.sh` (falls back to `whoami` if unset).

Home Assistant UI note:
- If a button has `entity_id` but no `states:` mapping, the daemon does **not** display raw on/off states for toggle-like entities (e.g. `script.*`). For value-like entities (`sensor.*`, `number.*`, `input_number.*`), the raw value is rendered as text by default.

### Home Assistant config examples

#### 1) Room navigation (`$page.go_to`)

```yml
pages:
  $root:
    buttons:
      - name: "Salon"
        presets: [room_folder]
        icon: mdi:sofa
        tap_action:
          action: $page.go_to
          data: salon

  salon:
    buttons: []
```

#### 2) Light toggle with UI states (`states:`)

```yml
presets:
  base_button:
    icon_background_color: "transparent"
    icon_border_radius: 12

  light_off:
    icon: mdi:lightbulb-outline
    icon_background_color: "transparent"
    icon_color: "333333"

  light_on:
    icon: mdi:lightbulb
    icon_background_color: "transparent"
    icon_color: "FFD700"

pages:
  salon:
    buttons:
      - name: "Lampe droite"
        entity_id: light.lampe_droite
        presets: [base_button]
        tap_action:
          action: light.toggle
        states:
          "on":  { name: "ON",  presets: [light_on] }
          "off": { name: "OFF", presets: [light_off] }
```

#### 3) Script “fire and forget” (no state text)

If you just want to trigger a Home Assistant script and you **don’t** want a state-driven UI, omit `entity_id`.

```yml
pages:
  debian_pc:
    buttons:
      - name: "Reboot"
        presets: [base_button]
        icon: mdi:restart
        tap_action:
          action: script.reboot_debian_pc
```

#### 4) Sensor value as dynamic text (no `states:`)

```yml
pages:
  salon:
    buttons:
      - name: "Temperature"
        entity_id: sensor.tz3000_utwgoauk_snzb_02_temperature
        presets: [cmd_button, base_button]  # text_* comes from cmd_button
```

#### 5) Service data (`data:`) forwarded as `service_data`

`data:` can be a JSON object/array string. If the button has an `entity_id`, it is injected automatically when possible.

```yml
pages:
  salon:
    buttons:
      - name: "Light 50%"
        entity_id: light.lampe_droite
        presets: [base_button]
        tap_action:
          action: light.turn_on
          data: '{"brightness_pct":50}'
```

### Subscriptions / updates behavior

- When you enter a page, `paging_daemon` subscribes to the `entity_id` present on that page (via `ha_daemon`).
- When you leave a page, it unsubscribes entities from the old page.
- On state changes, it uses partial updates (only the affected button is refreshed).

## Ulanzi device daemon commands

The Ulanzi daemon accepts simple text commands on `/tmp/ulanzi_device.sock`:
- `ping` → `ok` / `err no_device`
- `read-buttons` → subscribe to button events (push)
- `set-buttons-explicit`, `set-buttons-explicit-14`, `set-partial-explicit`, `set-label-style`, `set-brightness`, `set-small-window`
- `device-info` → returns the last captured `IN_DEVICE_INFO (0x0303)` packet as `ok {json...}` (if available)

### Small window (CPU/RAM/GPU)

`ulanzi_d200_daemon` periodically sends a keep-alive `set-small-window` (protocol `0x0006`). When the small window is in **STATS** mode (`mode=0`), it fills `cpu/mem/gpu` with **real host values** (clamped to `0..99`).

Note: the keep-alive interval is `24s`, and it can take **two cycles (~48s)** for CPU/RAM/GPU to visibly refresh (and only if button 14 is on the correct mode).

GPU is best-effort across vendors. The daemon first tries an optional helper script `assets/scripts/gpu_usage.sh` (you can override the path with `ULANZI_GPU_SCRIPT`). If the script is missing, outputs nothing/non-numeric, or outputs `0`, the daemon falls back to internal sysfs-based probing and optional `nvidia-smi` if available.

## Contributing

The project is developed in C for optimal performance, with bash scripts for automation. Old Python versions are kept in `legacy/` for reference.

## License

[To be completed according to your license]
