# tab5-port — ESP32-DIV → M5Stack Tab5 (ESP32-P4) port

Execution of `../PORTING_PLAN.md`. Split-brain architecture: the **Tab5 (ESP32-P4)**
is the host (M5GFX UI, A164/touch, Wi-Fi/BLE via its C6); a **generic ESP32-S3
(N16R8) running the ESP32-DIV firmware** is the RF co-processor ("companion").
They talk over **`divlink`**, a framed binary link on the M5-Bus.

## Layout
```
divlink/         SoC-agnostic link library (shared by host + companion)
  divlink.{h,cpp}    COBS framing + CRC-16 + streaming parser
  divproto.h         app protocol: Feature/Cmd enums, packed telemetry structs
  link_client.{h,cpp} host endpoint: reliable CMD/ACK + retry + heartbeat
  link_server.{h,cpp} companion endpoint: HELLO->CAPS, ACK/NAK, seq de-dup
  library.json
tab5-host/       Host firmware (PlatformIO, board m5stack-tab5-p4, M5Unified)
  src/main.cpp       menu + status bar; routes features to C6 or companion
  src/gfx.h          TFT_eSPI -> M5GFX façade
  src/input_router.* capacitive touch (A164 seam behind HOST_HAVE_A164)
tab5-host-smoke/ Minimal P4 divlink self-test (board esp32-p4_r3, emulatable)
link-emu/        On-target LinkClient <-> companion_sim handshake (esp32-p4_r3)
companion/       DIV_COMPANION seam for the ESP32-DIV firmware
  div_companion.{h,cpp}   link server glue: feature start/stop, input, telemetry
  div_companion_globals.cpp
  INTEGRATION.md          exact ESP32-DIV.ino edits
test/            Host-native unit tests (g++)
tools/           emulate.sh, emulate_link.sh, companion_sim.py
Makefile         test / emu / host / emu-link targets
```

## Status (validated)
- **`divlink` protocol** — 88 native tests (COBS edge cases incl. ≥254-runs,
  CRC vector 0x29B1, frame round-trip to 4 KB, corruption rejection, resync,
  bounds/overflow) + 12 end-to-end host↔companion tests (HELLO→CAPS, CMD/ACK,
  NAK, EVT, INPUT, **seq de-dup**). All pass under ASan/UBSan.
- **On P4 (esp-emu):** the smoke firmware boots on emulated ESP32-P4 and runs
  the divlink self-test → `SMOKE_RESULT:PASS` (8/8), PSRAM 16 MB detected.
- **On P4 (esp-emu), end-to-end link:** the real `LinkClient` (compiled for P4)
  completes HELLO→CAPS + a reliable CMD/ACK against `companion_sim.py` over the
  emulator's UART1 bridge → `LINK_UP` (rxOk=3, acks=2, rxErr=0).
- **Full host firmware compiles** for the real Tab5 target (m5stack-tab5-p4,
  17.7 MB ELF): M5Unified + M5GFX + M5Unit-KEYBOARD + divlink + link client.
- **`companion_sim.py`** is byte-for-byte wire-compatible with the C++ encoder
  (verified across 300+ differential-fuzz vectors).

## Build & test
```
make core        # one-time: hardlink-snapshot ~/.platformio into ./.pio-core
                 # (isolates from a concurrent PlatformIO session; no re-download)
make test        # native unit tests + P4 smoke in esp-emu (asserts PASS)
make emu-link    # end-to-end LinkClient<->companion_sim handshake in esp-emu
make host        # compile the full Tab5 host firmware (real target)
```
Requires PlatformIO and `esp-emu` on PATH (`/home/jwowk/.local/bin`).

## Silicon-rev note (important)
esp-emu models **production** ESP32-P4 (`esp32p4-eco5`). The real Tab5 board
(`m5stack-tab5-p4`) targets **`chip_variant: esp32p4_es`** (engineering sample),
which **boot-loops under the emulator** — empirically confirming the plan's risk
**R-B**. So the smoke/link-emu envs use `esp32-p4_r3` (production) to exercise
divlink + the link on-target, while the **host** build stays on `esp32p4_es` to
match Josh's hardware (compile-verified, not emulator-booted). Re-test the `_es`
target when esp-emu adds ES support.

## Adversarial-review fixes applied
Three review agents audited the firmware; accepted fixes folded in:
- **B1** host build (lib structure) — `symlink://` local lib instead of a
  project-scanning `lib_extra_dirs`; added `library.json`.
- **B1** companion globals now defined in a compiled unit (`div_companion_globals.cpp`).
- **cobs_encode** hardened with an `out_cap` bound (proven contract overflow).
- **H2** server-side **seq de-dup** (retransmit re-ACKs, never re-dispatches).
- **H3** companion **reentrancy**: feature start/stop is latched and applied at
  the top level, never torn down inside the link pump.
- **H1** companion sink is **non-blocking** (drop + count), so a stream can't
  stall the radio loop.
- **M1** reliable-CMD downgrade is now signaled (returns 0), not silent.
- **L1/L3/M2/M3/M4** proto_ver gate, macro-collision guards, `link`→`g_link`
  rename (POSIX collision), link heartbeat, `firmware.factory.bin` in the
  emulator, BLOB no longer dropped, feature-home routing (C6-native vs companion).

## Remaining work (next phases)
- **Credit-window backpressure** (plan §7.2) — only the non-blocking-drop half
  is done; add the per-channel credit handshake + surfaced loss counters.
- **Connectivity track (C6):** implement the HOME_P4 features (Wi-Fi scan/ARP/
  captive-portal, BLE scan/adv) natively via esp-hosted; wire `esp_wifi`/NimBLE.
- **Companion integration:** apply `companion/INTEGRATION.md` to ESP32-DIV.ino,
  resolve the Serial1-vs-GPS UART, and de-block feature loops (plan §17 HR-2).
- **Stage-A mirror** over the SPI data-plane (PSRAM sprite) + **Stage-B** native
  telemetry rendering for high-value screens.
- **A164 keyboard:** enable `HOST_HAVE_A164` and verify the M5Unit-KEYBOARD API
  (note: M5UnitUnified prints "ESP32-P4 not supported" for its GPIO/pin adapter).
- **SET_PARAM / BLOB_FETCH** command handlers + windowed BLOB reassembly.
- Refactor `LinkClient` to inject sink+clock so its retry path is native-testable.
