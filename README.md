# GoofyDeck

GoofyDeck controls an Ulanzi D200 with C daemons and config-driven paging, with optional Home Assistant integration (`ha_demon`).
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
./ulanzi_d200_demon
./lib/pagging_demon
./lib/ha_demon
```

## Sockets

- Ulanzi device daemon: `/tmp/ulanzi_device.sock`
- Paging control socket: `/tmp/goofydeck_pagging_control.sock`
- Home Assistant daemon: `/tmp/goofydeck_ha.sock`

## Configuration

- Main config: `config/configuration.yml` (parsed with `libyaml`, no Python).
- Pages/presets: `presets:` + `pages:` + `system_buttons:`.
- Wallpaper (optional): `wallpaper:` globally and/or per page.

## Paging control commands

Send one line to `/tmp/goofydeck_pagging_control.sock`:

```bash
printf 'stop-control\n' | socat - UNIX-CONNECT:/tmp/goofydeck_pagging_control.sock
printf 'start-control\n' | socat - UNIX-CONNECT:/tmp/goofydeck_pagging_control.sock
printf 'load-last-page\n' | socat - UNIX-CONNECT:/tmp/goofydeck_pagging_control.sock

# Simulate button events (same logic as physical buttons)
printf 'simule-button TAP1\n' | socat - UNIX-CONNECT:/tmp/goofydeck_pagging_control.sock
printf 'simule-button LONGHOLD14\n' | socat - UNIX-CONNECT:/tmp/goofydeck_pagging_control.sock
```

## Home Assistant (`ha_demon`)

Put credentials in `.env` (see `example.env`):

```bash
HA_HOST="ws://localhost:8123"
HA_ACCESS_TOKEN=""
```
- **Buttons**: 13 main buttons + 1 special button
- **Resolution**: 1280x720 pixels for full images
- **Format**: PNG recommended for icons
- **Padding**: Automatically handled to avoid invalid bytes

## 🤝 Contributing

The project is developed in C for optimal performance, with bash scripts for automation. Old Python versions are kept in `legacy/` for reference.

## 📄 License

[To be completed according to your license]
