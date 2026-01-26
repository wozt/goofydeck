# GoofyDeck

GoofyDeck controls an Ulanzi D200 with C daemons and config-driven paging, with optional Home Assistant integration (`ha_daemon`).
This project is still under construction.

The previous (long) README has been archived to `legacy/README.md`.

## Build

```bash
./install.sh
make all
```

## Run

Recommended (byobu):

```bash
./launch_stack.sh --byobu
./launch_stack.sh --kill
```

Manual:

```bash
./ulanzi_d200_daemon
./bin/paging_daemon
./bin/ha_daemon
```

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
- `icon`: `mdi:<name>` or empty.
- `text`: optional icon text (static).
- `entity_id`: enables Home Assistant state tracking on that button when HA is enabled.
- `tap_action:`:
  - `action`: either internal paging actions (start with `$`) or a Home Assistant service like `light.toggle`
  - `data`: optional:
    - for `$page.go_to`: target page name (string)
    - for HA services: JSON string/obj/array forwarded as HA `service_data` (and `entity_id` is injected when possible)
- `states:`: map of HA state string → overrides (`name`, `presets`, and optionally `icon`/`text`).

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

## Home Assistant (`ha_daemon`)

Put credentials in `.env` (see `example.env`):

```bash
HA_HOST="ws://localhost:8123"
HA_ACCESS_TOKEN=""
```

Optional:
- `USERNAME`: used by helper scripts like `bin/get_username.sh` (falls back to `whoami` if unset).
- **Buttons**: 13 main buttons + 1 special button
- **Resolution**: 1280x720 pixels for full images
- **Format**: PNG recommended for icons
- **Padding**: Automatically handled to avoid invalid bytes

## 🤝 Contributing

The project is developed in C for optimal performance, with bash scripts for automation. Old Python versions are kept in `legacy/` for reference.

## 📄 License

[To be completed according to your license]
