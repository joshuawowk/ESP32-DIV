#!/usr/bin/env bash
# End-to-end emulated link test: boot the link-emu firmware in esp-emu with
# UART1 bridged to companion_sim.py, and assert the handshake reaches LINK_UP.
set -uo pipefail
HERE="$(cd "$(dirname "$0")/.." && pwd)"
EMU="${ESP_EMU:-/home/jwowk/.local/bin/esp-emu}"
BUILD="$HERE/link-emu/.pio/build/link-emu"
IMG="$BUILD/firmware.factory.bin"
PORT="${1:-5560}"
[ -f "$IMG" ] || { echo "build link-emu first (make link-emu)"; exit 2; }

LOG="$(mktemp)"
echo "[emulate_link] booting esp-emu, UART1 -> tcp:127.0.0.1:$PORT"
"$EMU" --chip esp32p4 --firmware "$IMG" \
       --uart1-tcp "127.0.0.1:$PORT" \
       --exit-on "LINK_UP" --timeout 30s > "$LOG" 2>&1 &
EMU_PID=$!

sleep 1
echo "[emulate_link] starting companion_sim (connect mode)"
python3 "$HERE/tools/companion_sim.py" connect 127.0.0.1 "$PORT" &
SIM_PID=$!

wait "$EMU_PID"; RC=$?
kill "$SIM_PID" 2>/dev/null
echo "=== firmware UART (filtered) ==="
grep -vE "Bus fault|INFO |WARN " "$LOG" | grep -iE "LINK-EMU|HELLO|CAPS|rxOk|LINK_UP"
if grep -q "LINK_UP" "$LOG"; then echo "RESULT: PASS (LINK_UP reached)"; else echo "RESULT: FAIL"; fi
rm -f "$LOG"
exit $RC
