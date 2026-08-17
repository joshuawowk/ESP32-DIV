# Installing ESP32-DIV on the M5Stack Tab5 via the bmorcelli Launcher

ESP32-DIV builds as a standalone ESP32-P4 firmware. The bmorcelli
[M5Stack Launcher](https://github.com/bmorcelli/Launcher) installs it as an "app":
at install time the Launcher reads its current partition table, **dynamically
carves a new OTA app partition sized to the image**, flashes the app there, sets
it as the boot partition, and reboots into it. So there is no fixed offset to
target and no special container format — a plain ESP-IDF app image (magic `0xE9`)
is all that is required. **No icon** is needed (the Launcher renders a text tile
from the app name), and **no header/signature**.

> The app's on-screen **name is taken from the filename** (truncated to 20 chars;
> the tile shows the first 5). Keep the SD file named exactly **`ESP32-DIV.bin`**.

## Build artifacts (`dist/`, regenerated on every `pio run -e tab5`)

| File | What it is | Use |
|---|---|---|
| `ESP32-DIV.bin` | app-only image (app partition contents) | **Launcher SD install** (this file) and Launcher external-OTA `source_offset:0` |
| `ESP32-DIV-tab5-full.bin` | merged bootloader + partition table + app | direct `esptool write_flash 0x0` recovery, or external-OTA `source_offset:0x10000` |

(Produced by `boards/tab5/package_launcher.py`, wired via `extra_scripts`.)

## Path A — SD-card install (recommended; works today, no account)

1. `pio run -e tab5` (uses a private core dir if you share `~/.platformio` with
   another project: `export PLATFORMIO_CORE_DIR=~/.platformio-div`).
2. Copy **`dist/ESP32-DIV.bin`** to the root of the Tab5's microSD card, keeping
   the name `ESP32-DIV.bin`.
3. On the Tab5, open the Launcher's file browser, select `ESP32-DIV.bin`, choose
   **Install**. The Launcher makes a new app slot, flashes it, and boots ESP32-DIV.

The merged `ESP32-DIV-tab5-full.bin` also installs from SD (the Launcher parses
its embedded table and extracts only the app slice), but it is larger and
redundant — prefer the app-only `ESP32-DIV.bin`.

### Returning to the Launcher

The Launcher sets ESP32-DIV as the boot partition, so a normal reboot re-launches
ESP32-DIV, not the Launcher. To get back, use the Launcher's boot-time entry
(hold the Launcher's key / touch during boot, per the Launcher docs) or re-select
the Launcher partition. (A future ESP32-DIV enhancement could add an in-app
"Return to Launcher" that clears the OTA boot selection — this is app behavior,
not part of packaging.)

## Path B — Online catalog (M5Burner / LauncherHub)

To appear in the Launcher's **on-device OTA list**, the app must be published to
the M5Stack M5Burner store under the Tab5 category **`tab5`** (the Launcher fetches
`api.launcherhub.net/firmwares?category=tab5`). Publishing requires an M5Stack
account and store acceptance into that category — a process outside this repo, so
it cannot be completed from the build alone.

The publish metadata and the install manifest that drives flashing are templated
in **`catalog-entry.json`** (fill `author`, `github`, `version`, and the
`sources.firmware` download URL; set `install.app.image_size` to the byte size of
`dist/ESP32-DIV.bin`). Use `source_offset: 0` with `ESP32-DIV.bin`, or
`source_offset: 65536` (0x10000) with `ESP32-DIV-tab5-full.bin`.

## Direct flash (bench recovery, bypasses the Launcher)

```sh
esptool --chip esp32p4 write_flash 0x0 dist/ESP32-DIV-tab5-full.bin
```
