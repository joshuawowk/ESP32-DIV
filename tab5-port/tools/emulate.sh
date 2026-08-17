#!/usr/bin/env bash
# Merge a PlatformIO P4 build into a flash image and boot it under esp-emu.
# Usage: emulate.sh <pio_env_dir> <exit_on_string> [extra esp-emu args...]
#   e.g. emulate.sh .../.pio/build/tab5-smoke "SMOKE_RESULT:PASS"
set -euo pipefail

BUILD="${1:?build dir}"; EXIT_ON="${2:?exit-on string}"; shift 2 || true
EMU="${ESP_EMU:-/home/jwowk/.local/bin/esp-emu}"
ESPTOOL="${ESPTOOL:-/home/jwowk/.local/bin/esptool}"
OUT="$BUILD/merged-flash.bin"

# Prefer PlatformIO's own combined image — it already includes otadata at 0xe000
# and matches the exact offsets used to build (Review M2).
if [ -f "$BUILD/firmware.factory.bin" ]; then
  OUT="$BUILD/firmware.factory.bin"
  echo "[emulate] using PlatformIO combined image: $OUT"
else
  APP="$BUILD/firmware.bin"; BL="$BUILD/bootloader.bin"; PT="$BUILD/partitions.bin"
  [ -f "$APP" ] || { echo "missing $APP (build first)"; exit 2; }
  BOOTAPP0="$BUILD/boot_app0.bin"
  ARGS=( 0x2000 "$BL" 0x8000 "$PT" 0x10000 "$APP" )
  [ -f "$BOOTAPP0" ] && ARGS=( 0x2000 "$BL" 0x8000 "$PT" 0xe000 "$BOOTAPP0" 0x10000 "$APP" )
  # Use PlatformIO's esptool if a standalone one isn't on PATH.
  command -v "$ESPTOOL" >/dev/null 2>&1 || ESPTOOL="$(ls ~/.platformio/packages/tool-esptoolpy/esptool.py 2>/dev/null | head -1)"
  echo "[emulate] merging -> $OUT (esptool: $ESPTOOL)"
  "$ESPTOOL" --chip esp32p4 merge_bin -o "$OUT" --flash_size 16MB "${ARGS[@]}"
fi

echo "[emulate] booting esp-emu (exit-on: '$EXIT_ON')"
exec "$EMU" --chip esp32p4 --firmware "$OUT" --exit-on "$EXIT_ON" --timeout 30s "$@"
