#!/bin/sh
# Project-local Arduino environment when present (company Mac); falls back to
# the Arduino IDE CLI on a home Mac. Credentials stay in ignored config.h.
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cd "$ROOT"
CLI="$ROOT/.local/bin/arduino-cli"
CFG="$ROOT/.local/arduino-cli.yaml"
IDE_CLI="/Applications/Arduino IDE.app/Contents/Resources/app/lib/backend/resources/arduino-cli"
IDE_CLI_HOME="$HOME/Applications/Arduino IDE.app/Contents/Resources/app/lib/backend/resources/arduino-cli"
# no_fs: two 2MB OTA slots. The project stores no filesystem data; the extra
# headroom keeps OTA pushes viable as the firmware grows. Switching scheme
# requires one USB flash; every later update can go over the air.
PROJECT_FQBN=esp32:esp32:esp32:UploadSpeed=460800,PartitionScheme=no_fs
HOME_FQBN=esp32:esp32:esp32:PartitionScheme=no_fs
PORT=${2:-/dev/cu.usbserial-10}
if [ -x "$CLI" ] && [ -f "$CFG" ]; then
  # company / project-local environment
  cli_run() { "$CLI" --config-file "$CFG" "$@"; }
  COMPILE_FQBN=$PROJECT_FQBN
  BUILD_PATH="$ROOT/.local/firmware-build"
  LIB_ARGS=""
else
  # home fallback: Arduino IDE CLI, libraries in ~/Documents/Arduino/libraries
  if [ -x "$IDE_CLI" ]; then CLI=$IDE_CLI
  elif [ -x "$IDE_CLI_HOME" ]; then CLI=$IDE_CLI_HOME
  else echo "No Arduino CLI found (.local/bin/arduino-cli or Arduino IDE). See README.md."; exit 1
  fi
  cli_run() { "$CLI" "$@"; }
  COMPILE_FQBN=$HOME_FQBN
  BUILD_PATH="$ROOT/.local/home-build"
  LIB_ARGS="--libraries $HOME/Documents/Arduino/libraries"
fi

build() {
  # ROOT CAUSE (2026-11-20 22:40, library source verified): the HUB75 library
  # picks its CIE brightness LUT from the COMPILE-TIME PIXEL_COLOR_DEPTH_BITS
  # macro (defaults to 8), but the sketch sets the real plane count at runtime
  # via cfg.setPixelColorDepthBits(6). With the 8-bit LUT, values >63 lose
  # their top bits to the 6-plane write (mod-64 wraparound): mid-range colors
  # collapsed (mint green -> purple, gray-blue -> pink, orange -> white) while
  # saturated 0/255 channels stayed exact. Define the macro to 6 so the 6-bit
  # LUT is compiled in and hues are preserved.
  # To re-run panel diagnostics, also add -DDESK_COLOR_TEST.
  # shellcheck disable=SC2086
  cli_run compile --fqbn "$COMPILE_FQBN" $LIB_ARGS \
    --build-property "compiler.cpp.extra_flags=-DPIXEL_COLOR_DEPTH_BITS=6" \
    --build-path "$BUILD_PATH" \
    --output-dir "$ROOT/.local/firmware" "$ROOT/firmware/desk_display"
}

# OTA host: explicit argument wins, then .local/esp32-host, then DESK_ESP32_HOST.
ota_host() {
  if [ -n "${2:-}" ]; then echo "$2"
  elif [ -f "$ROOT/.local/esp32-host" ]; then tr -d ' \n' < "$ROOT/.local/esp32-host"
  else echo "${DESK_ESP32_HOST:-}"
  fi
}
case ${1:-build} in
  build) build ;;
  flash)
    if [ ! -f firmware/desk_display/config.h ]; then
      echo "Configure firmware/desk_display/config.h before flashing Wi-Fi settings."
      exit 1
    fi
    build
    cli_run upload --fqbn "$COMPILE_FQBN" --port "$PORT" \
      --input-dir "$ROOT/.local/firmware" "$ROOT/firmware/desk_display"
    ;;
  deploy)
    # Wireless firmware push. Requires the device already running a build with
    # the OTA endpoint and the same token as .local/token (config.h
    # DESK_API_TOKEN). tools/ota.py negotiates the authenticated single-stream
    # protocol with 16 KB receive/write/ACK frames and resumes a saved prefix.
    # Older firmware falls back to the existing HTTP slice endpoint.
    build
    HOST=$(ota_host "${2:-}")
    TOKEN_FILE="$ROOT/.local/token"
    if [ -z "$HOST" ]; then
      echo "Need a device host: $0 deploy <ip-or-host>, or put it in .local/esp32-host."
      exit 1
    fi
    if [ ! -f "$TOKEN_FILE" ]; then
      echo "Missing $TOKEN_FILE (must match DESK_API_TOKEN in config.h)."
      exit 1
    fi
    BIN="$ROOT/.local/firmware/desk_display.ino.bin"
    if [ ! -f "$BIN" ]; then
      echo "Firmware image missing: $BIN"
      exit 1
    fi
    PY=${PYTHON:-python3}
    if ! command -v "$PY" >/dev/null 2>&1; then
      echo "python3 not found (set PYTHON=... or use $0 deploy-legacy <host>)."
      exit 1
    fi
    echo "OTA pushing $(basename "$BIN") to $HOST ..."
    "$PY" "$ROOT/tools/ota.py" "$HOST" --bin "$BIN" --token-file "$TOKEN_FILE"
    ;;
  deploy-legacy)
    # One-shot multipart POST to /update, kept for firmware that predates
    # /ota. On this link it usually dies with "Broken pipe" -- it exists to
    # recover a device whose /ota endpoint is missing, not for daily pushes.
    build
    HOST=$(ota_host "${2:-}")
    TOKEN_FILE="$ROOT/.local/token"
    if [ -z "$HOST" ] || [ ! -f "$TOKEN_FILE" ]; then
      echo "Usage: $0 deploy-legacy <ip-or-host> (needs .local/token)"
      exit 1
    fi
    BIN="$ROOT/.local/firmware/desk_display.ino.bin"
    echo "OTA (legacy one-shot) pushing $(basename "$BIN") to $HOST ..."
    curl -sS --noproxy '*' --max-time 300 --fail -u "admin:$(cat "$TOKEN_FILE")" \
      -H "Origin: http://$HOST" \
      -F "firmware=@$BIN" "http://$HOST/update"
    echo
    echo "Device reboots on success; give it ~10 s to reconnect."
    ;;
  monitor) cli_run monitor --port "$PORT" --config baudrate=115200 ;;
  info) cli_run board list ;;
  *) echo "Usage: $0 build|flash|deploy|deploy-legacy|monitor|info [port-or-host]"; exit 1 ;;
esac
