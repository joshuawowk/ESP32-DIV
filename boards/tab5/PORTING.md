# ESP32-DIV → M5Stack Tab5 (ESP32-P4) — Port Plan & Status

Porting **ESP32-DIV** (an Arduino ESP32/ESP32-S3 pentest multitool) to run on the
**M5Stack Tab5** as a **bmorcelli Launcher** app.

A Launcher "app" is a standalone firmware `.bin` that the Launcher flashes to an
app partition and boots. So this port builds ESP32-DIV as a native **ESP32-P4**
firmware. The Tab5 is already a supported Launcher device, so its display/touch/
SD/WiFi stack and 16 MB partition layout are proven.

## Target hardware

| | Classic ESP32-DIV | M5Stack Tab5 |
|---|---|---|
| SoC | ESP32 / ESP32-S3 (Xtensa) | **ESP32-P4** (RISC-V) + **ESP32-C6** |
| WiFi/BT | native radio | via **esp-hosted** over SDIO (C6) |
| Display | SPI TFT via **TFT_eSPI** | **MIPI-DSI 720×1280** via **M5GFX** |
| Touch | XPT2046 resistive | **GT911** capacitive (M5GFX) |
| Buttons | PCF8574 I2C expander | none → on-screen touch nav bar |
| Bluetooth Classic | yes | **not available on P4** |
| SubGHz/nRF24/IR/RFID/GPS | external SPI/UART modules | not attached |

## Strategy

1. **Display** — a `TFT_eSPI`→M5GFX compatibility shim (`boards/tab5/compat/`)
   placed first on the include path resolves every `#include <TFT_eSPI.h>` and
   maps all ~2800 `tft.*` calls onto M5GFX/LovyanGFX (which mirrors the TFT_eSPI
   API). Only `writecommand`/`writedata`/`getTouchRawZ` are added by the shim.
2. **Input** — a `BOARD_TAB5` profile sets `HAS_PCF8574_BUTTONS=0`, so all input
   falls through to the existing on-screen touch nav bar with no feature changes.
   `Touchscreen.cpp` gets a Tab5 backend using `tft.getTouch()` (GT911).
3. **Feature-gate** external radios/modules that the Tab5 lacks; keep the UI,
   settings, SD, and (in later milestones) WiFi scan + BLE scan via the C6.

## Feature feasibility on the Tab5

| Feature group | Verdict | Notes |
|---|---|---|
| Menu / UI / theme / settings | **Works** | via M5GFX shim + touch nav bar |
| Touch input | **Works** | GT911 via M5GFX |
| SD card / file tools | **Works** (SPI pins 42/43/39/44) | avoid C6 SDIO pins 8-13 |
| WiFi scan / STA / AP | **Built (M2a)** — works via C6 | standard esp_wifi over hosted |
| WiFi deauth / beacon / karma | **Gated (M2a)** | raw `esp_wifi_80211_tx` unsupported over hosted → shows "not available" screen |
| WiFi promiscuous / sniff | **Gated (M2a)** | promiscuous unsupported over hosted → "not available" screen |
| BLE scan | **Built (M3)** — IDF NimBLE C API | `hostedInitBLE()` + `nimble_port_init` + active `ble_gap_disc` |
| BLE advertise (spoofer/sourapple/airtag) | **Stubbed** — portable to IDF NimBLE | NimBLE works on P4; needs C-API reimplementation |
| BLE jammer / low-level | **Blocked** | needs raw radio control |
| Bluetooth Classic | **Blocked (platform)** | P4/C6 have no BT Classic |
| SubGHz (CC1101) | **Gated** | external module, not on Tab5 |
| nRF24 / ESB / MouseJack | **Gated** | external module |
| IR / RFID (RC522) / GPS | **Gated** | external modules |
| BadUSB / Ducky | **Deferred** | P4 has USB-OTG; needs TinyUSB HID work |

## Milestones

- **M1 (DONE — builds + links)** — ESP32-DIV compiles and links as an ESP32-P4
  firmware: UI + input + theme + settings + SD, with the 7 hardware-heavy modules
  (`wifi`, `bluetooth`, `subghz`, `ir`, `rfid`, `gps`, `ducky`) excluded from the
  build and their feature entrypoints stubbed (`tab5_stubs.cpp`) showing an
  exitable "not available on Tab5" screen. Flash: ~872 KB (11.9% of the 7 MB app
  slot); RAM 7.4%. Artifacts staged in `dist/` (see below). **On-device boot/UI
  verification is pending** — esp-emu cannot run it (no PSRAM/DSI).
- **M2a (DONE — builds)** — `wifi.cpp` is re-included and compiles/links on the
  P4. WiFi scan + STA/AP/OTA build against the standard esp_wifi APIs (work over
  the C6 hosted link). Raw-injection / promiscuous features (Deauther, Beacon
  Spammer, Karma, Probe Flood, Packet Monitor, Deauth Detector) are **runtime-
  gated**: on Tab5 they show a "needs radio hardware not available on the Tab5"
  screen instead of silently failing (esp_wifi_80211_tx / promiscuous are not
  supported over esp-hosted). Fixes: arduinoFFT pinned to 1.6.x (v2 renamed the
  class), `tcpip_adapter_init`→`esp_netif_init`, legacy `system_event_t` handler
  and 802.11n `wifi_pkt_rx_ctrl_t` HT fields gated (removed in IDF 5 / absent on
  P4), `byte`→`uint8_t`.
- **M2c (DONE — builds)** — **UI scaling**: the 240×320 UI is rendered to an
  offscreen canvas and integer-scaled (3×) + centered onto the 720×1280 panel,
  with touch mapped back to canvas coords (`boards/tab5/compat/TFT_eSPI.h`,
  `-DTAB5_SCALED_UI=1`; set 0 for native top-left drawing). No per-screen
  coordinate changes needed. **On-device visual verification pending.**
- **M2b/M3 (DONE — builds) — BLE scan via IDF NimBLE.** NimBLE-Arduino (`h2zero`)
  does NOT build on the P4 — it bundles its own NimBLE that conflicts with the
  framework's IDF NimBLE (`uxGetCriticalNestingDepth` undeclared,
  `ble_npl_hw_set_isr` conflicting). So `BleScan` is reimplemented against the
  **framework's IDF NimBLE C API** in `ESP32-DIV/tab5_ble_scan.cpp`: bring up the
  C6 BT controller over esp-hosted with the Arduino core's `hostedInitBLE()`
  (the P4 gotcha — `nimble_port_init()` does NOT do this), then `nimble_port_init`
  + a host task; start an **active** `ble_gap_disc()` from the sync callback;
  advertising reports stream into a MAC-deduplicated device table the UI renders
  (name + RSSI + MAC), integrated with the touch nav bar (Rescan/Up/Down/Exit).
  Verified: `ble_gap_disc` / `hostedInitBLE` / `nimble_port_init` are linked into
  the firmware. The other bluetooth.cpp features (jammer, spoofer, SourApple,
  AirTag, sniffer, nRF24/ESB, Bluetooth Classic) remain stubbed. **On-device BLE
  verification pending.**
- **M3+** — on-device verification pass; port the NimBLE advertising features
  (Spoofer/SourApple/AirTag) to the IDF NimBLE C API; dynamic accent theming;
  proper Launcher packaging + catalog entry.

## Build

```sh
cd ESP32-DIV      # repo root
pio run -e tab5   # board m5stack-tab5-p4, produces .pio/build/tab5/firmware.bin
```

> **Shared-toolchain note:** if another PlatformIO project on this machine uses a
> *different* `framework-arduinoespressif32-libs` (e.g. the bmorcelli Launcher
> overrides it) on the same `~/.platformio`, the two will fight over the shared
> package. Give this project its own core dir to isolate it:
> `export PLATFORMIO_CORE_DIR=~/.platformio-div` before `pio`. The platform is
> pinned by URL in `platformio.ini` so a clean core dir resolves pioarduino
> 55.03.39 (not the unrelated official `espressif32`).

## On-device testing (ESP32-P4)

Flash over the Tab5's USB-Serial-JTAG. **Pin the port** and confirm its identity
first (`ls -l /dev/serial/by-id/`) — never flash a shared bus blindly:

```sh
sudo chmod a+rw /dev/ttyACM<N>            # if your user isn't in the dialout group
pio run -e tab5 -t upload --upload-port /dev/ttyACM<N>
```

Read serial with pyserial (PlatformIO's monitor needs a TTY). Decode any panic
with the RISC-V addr2line against `.pio/build/tab5/firmware.elf`:

```sh
riscv32-esp-elf-addr2line -pfiaC -e .pio/build/tab5/firmware.elf 0x<MEPC> 0x<RA>
```

**First-boot fix found this way:** `readBatteryVoltage()` called
`analogReadMilliVolts(BATTERY_ADC_PIN)` with `BATTERY_ADC_PIN == -1` (→ pin 255),
which faults `__analogInit` on the P4 (a Load access fault crash-loop; harmless on
classic ESP32). Now guarded to return 0.0 when there is no battery-sense pin.

**Boots on real hardware** (serial, after the fix): PSRAM added; M5GFX autodetects
`board_M5Tab5`, the ST7123 DSI panel and the touch controller; input falls to the
touch nav bar; reaches `[boot] ready` and runs with no panic. Remaining on-device
checks (need eyes on the panel / touch): scaled UI geometry, touch mapping, and
exercising WiFi scan + BLE scan from the menu.

Toolchain: pioarduino platform-espressif32 55.03.39, Arduino core 3.3.9
(ESP-IDF 5.5.4), M5GFX, ArduinoJson, arduinoFFT. Partition table:
`boards/tab5/partitions_tab5_16mb.csv` (dual 7 MB app slots).

### Artifacts & install

`pio run -e tab5` produces (also copied to `dist/`):

- `dist/ESP32-DIV-tab5-app.bin` — the app image (goes at app0 / `0x10000`).
  Install via the Launcher (SD or OTA), which flashes it to the app partition.
- `dist/ESP32-DIV-tab5-full.bin` — full merged image (bootloader + partition
  table + app). Flash directly to `0x0` with esptool or web.esphome.io:
  `esptool --chip esp32p4 write_flash 0x0 dist/ESP32-DIV-tab5-full.bin`.

### Files added/changed by the port

- `platformio.ini`, `boards/tab5/partitions_tab5_16mb.csv`
- `boards/tab5/compat/{TFT_eSPI.h, PCF8574.h, XPT2046_Touchscreen.h}` — shims
- `ESP32-DIV/tab5_stubs.cpp` — stub entrypoints for excluded modules
- `ESP32-DIV/{BoardConfig.h, shared.h, config.h, Touchscreen.cpp, ESP32-DIV.ino}`
  — `BOARD_TAB5` profile, include/pin gating, Tab5 touch backend, setup() init.
  All changes are `#if defined(BOARD_TAB5)`-gated; other boards are unaffected.

## Emulator note (validation without hardware)

The local `esp-emu` (`espressif/esp-emulator`) can emulate the ESP32-P4, but it
**models no PSRAM and no MIPI-DSI** and is ESP-IDF-oriented. Arduino P4 firmware
will not boot in it (the prebuilt bootloader inits Octal PSRAM → faults), and the
real app needs PSRAM anyway (1.8 MB framebuffer). The emulator is only usable as
a headless CI harness for no-PSRAM logic tests. **On-device flashing is the
primary validation.** (Espressif's QEMU has no esp32p4 machine at all.)

## Key coupling metrics (why the shim is the big lever)

`tft.*` calls: 2836 · PCF8574 button reads: 75 · XPT2046 refs: 50 · raw
`esp_wifi_80211_tx`: 10 · classic-BT refs: 5 · CC1101: 212 · nRF24: 108 ·
`TFT_eSprite` uses: 1.
