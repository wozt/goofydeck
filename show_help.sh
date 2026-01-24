#!/usr/bin/env bash
# Description: central help dispatcher for known scripts by name (also offers completion)
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="${SCRIPT_DIR}"

declare -A USAGES

USAGES["dir_sizes.sh"]="Usage: dir_sizes.sh <file|dir> [more paths...]\nLists each file size (recursive on dirs) and total."
USAGES["test_function.sh"]="Usage: test_function.sh <script-name.sh> [args...]\nSearches in root, lib/, icons/, standalone/ and runs it."
USAGES["test_png_format.sh"]="Usage: test_png_format.sh <suffix> [--interval ms]\nExample: test_png_format.sh 64 uses test_icons_64/."
USAGES["flash_icons.sh"]="Usage: flash_icons.sh [-t ms|--interval ms] <file1.png file2.png ... | directory>\nSends up to 13 icons per flash (>=13 PNGs). Logs to stdout."
USAGES["send_button.sh"]="Usage: send_button.sh --button-1=<file.png> [--button-2=<file.png> ... --button-13=<file.png>] [--label-N=text]\nSends provided files (and optional labels) to specific buttons 1-13 via ulanzi_d200_demon (Unix socket daemon)."
USAGES["send_page.sh"]="Usage: send_page.sh <file1.png [file2.png ...]>\nSend up to 13 explicit files (and optional labels with --label-N=text) to buttons 1..13. Use send_page_from_folder.sh for directories."
USAGES["optimize_png.sh"]="Usage: optimize_png.sh [options] <file|dir> [more files/dirs...]\nOptions: -r, --size <px>, --color <n 2/4/8/16/32/64/128/256>, --bc."
USAGES["make"]="Usage: make [target]\nBuild the C binaries (see Makefile targets: all, daemon, tools, icons, standalone, clean)."
USAGES["draw_optimize_std.sh"]="Usage: draw_optimize_std.sh [-c|--color <1-256>] <file.png|directory>\nWrites optimized _opt.png alongside inputs (finds PNGs in dir)."
USAGES["run_tests_root.sh"]="Usage: run_tests_root.sh [pytest-args...]\nRuns pytest as root after activating venv."
USAGES["draw_border"]="Usage: icons/draw_border <hexcolor|transparent> [--size=128] [--radius=20] <path.png>\nEdits the PNG in place. If path is relative, it is resolved relative to the project root."
USAGES["draw_square"]="Usage: icons/draw_square <hexcolor|transparent> [--size=196] <path.png>\nCreates the PNG exactly at the given path. If path is relative, it is resolved relative to the project root."
USAGES["draw_text"]="Usage: icons/draw_text [--list-ttf] [--text=hello] [--text_color=00FF00] [--text_align=top|center|bottom] [--text_font=Roboto.ttf] [--text_size=16] [--text_offset=x,y] <path.png>\nEdits the PNG in place. If path is relative, it is resolved relative to the project root."
USAGES["draw_optimize"]="Usage: icons/draw_optimize [-c|--color <n<=256>] <path.png>\nEdits the PNG in place. If path is relative, it is resolved relative to the project root."
USAGES["draw_mdi"]="Usage: icons/draw_mdi <mdi:name> <hexcolor|transparent> [--size=128] [--offset=x,y] [--brightness=1..200] <path.png>\nEdits/creates the target PNG. If path is relative, it is resolved relative to the project root."
USAGES["set_brightness.sh"]="Usage: set_brightness.sh [--socket <path>] <0-100>\nSet brightness via ulanzi_d200_demon (Unix socket daemon)."
USAGES["keep_alive.sh"]="Usage: keep_alive.sh --start|--stop [--socket <path>]\nStart/stop a background ping every 24s using ping_alive.sh. Logs in /tmp/ulanzi_keep_alive.log."
USAGES["ping_alive.sh"]="Usage: ping_alive.sh [--socket <path>] [--no-verbose]\nSend a single keep-alive ping to the daemon."
USAGES["test_button_pressed.sh"]="Usage: test_button_pressed.sh [--socket <path>]\nStream button events from the daemon (TAP/HOLD/RELEASE). CTRL+C to stop."
USAGES["pagging_demon"]="Usage: lib/pagging_demon [--config <path>] [--ulanzi-sock <path>] [--cache <dir>]\nSingle C paging daemon: subscribes to read-buttons and renders/sends pages via ulanzi_d200_demon."
USAGES["launch_stack.sh"]="Usage: launch_stack.sh [--config <path>] [--byobu|--kill]\nManual launcher: starts ulanzi_d200_demon + pagging_demon."
USAGES["install.sh"]="Usage: ./install.sh [--compile] [--no-compile] [--deps-only] [--python] [--no-fonts]\nInstalls system deps (Debian/Arch/RedHat/Brew) and optionally compiles via Makefile."
USAGES["get_username.sh"]="Usage: get_username.sh\nPrint username from .env if set, else system."
USAGES["reset_ulanzi.sh"]="Usage: standalone/reset_ulanzi.sh\nReset USB device 2207:0019 via unbind/bind (requires root)."
USAGES["compile_driver.sh"]="Usage: compile_driver.sh\nCompile the C driver ulanzi_ctl (requires libhidapi-dev, zlib1g-dev)."
USAGES["ulanzi_ctl"]="Usage: ulanzi_ctl <command> [options]\n  set-buttons --zip <file.zip> [--device-path <path>]\n  set-brightness <0-100> [--device-path <path>]\n  set-small-window [--mode N] [--cpu N] [--mem N] [--gpu N] [--time HH:MM:SS] [--device-path <path>]\n  set-label-style --json <file> [--device-path <path>]\n  ping | keep-alive [--interval sec] [--device-path <path>]\n  read-buttons [--device-path <path>]"

usage() {
  cat <<'EOF'
Usage:
  show_help.sh -h|--help            Show this help
  show_help.sh --complete           List known script names for completion
  show_help.sh <script-name>        Show help for known script (use base name, not path)
EOF
}

if [ "$#" -eq 0 ]; then
  usage
  exit 0
fi

if [ "$#" -gt 1 ]; then
  usage
  exit 1
fi

if [[ "$1" == "-h" || "$1" == "--help" ]]; then
  usage
  exit 0
fi

if [[ "$1" == "--complete" ]]; then
  printf "%s\n" "${!USAGES[@]}"
  exit 0
fi

TARGET="$1"

if [[ "$TARGET" == */* ]]; then
  echo "Do not provide a path, only the script name (e.g., flash_icons.sh)." >&2
  usage
  exit 1
fi

if [[ -n "${USAGES[$TARGET]:-}" ]]; then
  printf "%b\n" "${USAGES[$TARGET]}"
else
  echo "No help found for ${TARGET}. Known scripts:" >&2
  printf "  %s\n" "${!USAGES[@]}" >&2
  exit 1
fi
