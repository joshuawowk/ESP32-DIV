# ESP32‑DIV → M5Stack Tab5 (ESP32‑P4) — End‑to‑End Porting Plan

*Companion: `Bruce Port/PORTING_PLAN.md` (the Bruce→Tab5 plan) established and de‑risked the entire Tab5 **platform** layer — toolchain, M5GFX display, A164 keyboard, M5Unified power/touch/SD, and the esp‑hosted C6 Wi‑Fi/BLE recipe. This plan reuses that platform work verbatim where it applies and does **not** re‑derive it. Cross‑references are cited as “Bruce §N.”*

---

## 1. Executive summary

ESP32‑DIV is an **ESP32‑S3 offensive/defensive RF multitool**: Wi‑Fi, BLE, 2.4 GHz (nRF24×3), Sub‑GHz (CC1101), RFID/NFC (PN532), IR, and GPS, presented through a 240×320 TFT_eSPI touch UI with a PCF8574 5‑way keypad. Unlike Bruce (a self‑contained SoC app), **most of ESP32‑DIV’s value lives in external radio silicon** wired to fixed S3 SPI/UART pins.

Per Josh’s direction, the target is **full feature parity** using a **split‑brain architecture**: the **M5Stack Tab5 (ESP32‑P4)** becomes the **host** — big MIPI‑DSI display, A164 keyboard, capacitive touch, and Wi‑Fi/BLE via its onboard **ESP32‑C6** — while a **generic ESP32‑S3 (Josh’s own, N16R8 = 8 MB PSRAM) runs the ESP32‑DIV firmware as an RF co‑processor** (“the companion”). Josh does **not** own a finished ESP32‑DIV board; he will **wire the RF modules (nRF24×3, CC1101, PN532, IR, GPS) himself** to the S3 per the DIV firmware’s existing `shared.h` pin map (§16). The DIV firmware already targets generic ESP32‑S3, so this is a **wiring/assembly task, not a silicon port**. The two boards talk over a **framed binary link** on the Tab5 **M5‑Bus**.

This reframes the effort. We are **not** porting `ESP32-DIV.ino` to RISC‑V. We are:

1. **Writing a new Tab5 host firmware** (PlatformIO, on Launcher’s proven `m5stack-tab5` board + esp‑hosted bundle) that reproduces ESP32‑DIV’s menus/screens on **M5GFX**, takes input from the **A164 + capacitive touch**, provides **connectivity‑class Wi‑Fi/BLE** natively via the C6, and hosts a **link client**.
2. **Lightly extending the ESP32‑DIV firmware** (stays on the S3, keeps its Arduino build) with a **link server** that exposes its radios — and, as the primary route to parity, its already‑working **raw Wi‑Fi/BLE offense** — as remote commands + telemetry streams.

The critical path is therefore **the inter‑processor link + the UI/host rewrite**, not P4 radio bring‑up. This side‑steps the hardest Bruce risks (P4 RMT timing, RF24/CC1101 library ports to RISC‑V, shared‑SPI static‑init) by leaving those drivers on the S3 where they are proven. It introduces one dominant new risk: **the link and the split‑feature model** (§7, §10).

**Decision of record (from Josh):** full parity · **companion = a generic ESP32‑S3 (N16R8) running DIV firmware, with operator‑wired RF modules** · plan saved to the ESP32‑DIV repo root.

---

## 2. Scope & decisions of record

| # | Decision | Rationale |
|---|---|---|
| **DR‑1** | **Two‑MCU split‑brain**: Tab5‑P4 host + **generic ESP32‑S3 (N16R8) companion running DIV firmware**. | Josh’s choice. The DIV firmware is written for generic S3; avoids P4 RF library/timing ports. |
| **DR‑2** | **Radios live on the companion S3**, wired by Josh to the DIV `shared.h` S3 pin map (§16). Not on the Tab5. | Keeps RF24/CC1101/PN532/RMT on the chip class they’re proven on; the Tab5 M5‑Bus carries only the link + power. |
| **DR‑3** | **Wi‑Fi/BLE offense runs on the companion S3 as the parity path**, with the Tab5‑C6 **CustomRpc** path documented as an optional native alternative (§10). | The S3 already does promiscuous RX + `esp_wifi_80211_tx` + NimBLE raw adv today. Fastest parity, lowest new risk. |
| **DR‑4** | **Connectivity‑class Wi‑Fi/BLE runs natively on the Tab5 C6** (scan, STA/ARP, AP/captive‑portal, BLE scan/adv). | These work over esp‑hosted today (Bruce §17.5), keep the big‑screen UX responsive, and don’t tie up the link. |
| **DR‑5** | **Tab5 host is new PlatformIO firmware** on Launcher’s `m5stack-tab5` board + esp‑hosted libs bundle. | Bruce §17.1/§18.1‑B3 already proved this toolchain. ESP32‑DIV’s Arduino‑IDE build is irrelevant to the host. |
| **DR‑6** | **Companion keeps the DIV Arduino build**, extended (not rewritten) with a `DIV_COMPANION` link server + headless mode. | Minimize churn to 42 kLOC of working firmware. |
| **DR‑7** *(revised — PSRAM confirmed)* | **Two‑stage UI, both viable:** (A) **remote‑canvas mirror over the SPI data‑plane** for fast bootstrap parity — **now un‑blocked** because the companion is **N16R8 (8 MB PSRAM)**; then (B) **native M5GFX rendering from telemetry** for high‑value screens. | The N16R8 choice resolves §17.1 BR‑1 (which applied to the DIV’s no‑PSRAM N16). Mirror still needs SPI, not UART (§17.2 HR‑1). |
| **DR‑8** | **Legal/ethics gate unchanged.** Offensive features remain opt‑in and are the operator’s responsibility; the plan does not lower any safety bar. | ESP32‑DIV’s own README warns “educational/research only, authorized targets only.” |

**Out of scope (v1):** porting the DIV firmware to run *on* the P4; re‑homing radios onto the M5‑Bus (kept as a documented fallback in §16.2); the CYD/V1 DIV board variants.

---

## 3. Source recap — ESP32‑DIV architecture (what a port must satisfy)

**Build system.** Arduino IDE project: one 4,587‑line `ESP32-DIV.ino` + companion `.cpp/.h` (wifi 10.7k, bluetooth 9.6k, subghz 4.3k, gps 4.0k, ir 3.2k, utils 3.2k, rfid 2.6k lines). Board selection via `BoardConfig.h` (`BOARD_ESP32_DIV_V2` = ESP32‑S3 default). No PlatformIO, no board‑abstraction layer, no HAL. Vendored `TFT_eSPI` + `SmartRC-CC1101-Driver-Lib` in `Libraries/`.

**UI/render.** Direct `TFT_eSPI` calls (`tft.*`) against a 240×320 panel (`TFT_WIDTH/HEIGHT`), hand‑drawn `icon.h` bitmaps (150 KB), a case‑switch menu in the `.ino`, and an on‑screen keyboard (`KeyboardUI.*`). Old **core‑2.x LEDC API** (`ledcSetup`/`ledcAttachPin`) and `WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG,…)` in `setup()`.

**Input.** PCF8574 I²C expander 5‑way keypad (`BTN_UP/DOWN/LEFT/RIGHT/SELECT`) + XPT2046 resistive touch (`Touchscreen.*`) + optional on‑screen nav bar (`TOUCH_BUTTON_CUE_ENABLED`).

**Radios & fixed S3 wiring (from `shared.h`).**
- **nRF24×3** — `RF24 radio1/2/3(CE,CSN,16 MHz)`; bit‑bang helpers for Scanner/ESB/MouseJack; **BLE “jammer” is nRF24 const‑carrier**, *not* the ESP BT radio (`bluetooth.cpp:3124‑3235`).
- **CC1101** — ELECHOUSE/SmartRC lib; OOK via **RCSwitch + `micros()` bit‑bang** (*not* RMT — §17.3 MR‑1); Sub‑GHz replay/jam/brute/detect (`subghz.cpp`).
- **PN532** — SPI (Adafruit_PN532), RFID/NFC (`rfid.cpp`).
- **IR** — RMT TX/RX record/replay (`ir.cpp`, `.ino`).
- **GPS** — Neo‑6M UART2 + wardriving/wigle (`gps.cpp`).
- **Wi‑Fi (native S3)** — `esp_wifi_set_promiscuous(true)`, `esp_wifi_80211_tx(WIFI_IF_AP,…)` (`wifi.cpp:611,847,1170,1244…`), `WebServer`/`DNSServer` captive portal.
- **BLE (native S3)** — NimBLE via `BleCompat.h` shim; scan/sniff/spoof/AirTag/SourApple/BLE‑HID ducky.

**Feature inventory (parity target):**
- *Wi‑Fi (12):* PacketMonitor, WifiScan, BeaconSpammer, Deauther, DeauthDetect, CaptivePortal, ProbeRequestFlood, HiddenSsidReveal, WpsScanner, ArpScanner, KarmaAttack, FirmwareUpdate.
- *BLE (9):* BleScan, BleSniffer, BleSpoofer, SourApple, BleJammer *(nRF24)*, AirTagSpoofer, AirTagSniffer, BleSkimmer, BLE‑Ducky.
- *2.4 GHz nRF24 (6):* Scanner, ProtoKill, EsbSniffer, EsbReplay, MouseJack, MouseJackInject.
- *Sub‑GHz CC1101 (5):* ReplayAttack, SavedProfile, subjammer, SubBrute, jammingdetector.
- *RFID/NFC (1 module, many ops):* read/write/emulate (PN532).
- *IR (1 module):* record/replay.
- *GPS (1 module):* live GPS + wardriving/wigle export.

---

## 4. Target hardware — M5Stack Tab5 (authoritative essentials)

Full detail in Bruce §3. The essentials this plan depends on, plus the **authoritative expansion pinout extracted from Josh’s `Bruce Port/Docs/Tab5.pdf` datasheet (rev 2026‑08‑05, p.13)**:

**Core.** ESP32‑P4 dual‑RISC‑V @360 MHz, PSRAM (oct), no native Wi‑Fi/BLE. **ESP32‑C6** co‑processor over **SDIO2** (`D0=G11,D1=G10,D2=G9,D3=G8,CMD=G13,CLK=G12,RST=G15`) provides Wi‑Fi 6 + BLE via **esp‑hosted**. MIPI‑DSI 720×1280 panel (M5GFX autodetects ILI9881C/ST7123/ST7121), capacitive touch, microSD via SDMMC 4‑bit (`G39‑44`), audio/IMU/RTC/INA226/IO‑expanders on **system I²C `SDA=G31/SCL=G32`**, A164 keyboard on **`Wire1 SDA=G0/SCL=G1, INT=G50, addr 0x6D`**.

**M5‑Bus expansion header (2×15, back of device) — authoritative:**

| Func | Pin | # | # | Pin | Func |
|---|---|---|---|---|---|
| GND | — | 1 | 2 | **G16** | GPIO |
| GND | — | 3 | 4 | **G17** | PB_IN |
| GND | — | 5 | 6 | RST | EN |
| **MOSI** | **G18** | 7 | 8 | **G45** | GPIO |
| **MISO** | **G19** | 9 | 10 | **G52** | PB_OUT |
| **SCK** | **G5** | 11 | 12 | 3V3 | — |
| RXD0 | G38 | 13 | 14 | G37 | TXD0 |
| **PC_RX** | **G7** | 15 | 16 | **G6** | **PC_TX** |
| Int SDA | G31 | 17 | 18 | G32 | Int SCL |
| GPIO | **G3** | 19 | 20 | **G4** | GPIO |
| GPIO | **G2** | 21 | 22 | **G48** | GPIO |
| GPIO | **G47** | 23 | 24 | **G35** | GPIO |
| HVIN | — | 25 | 26 | **G51** | GPIO |
| HVIN | — | 27 | 28 | 5V | — |
| HVIN | — | 29 | 30 | BAT | — |

Also: **Grove Port.A I²C `G53/G54`** (5 V, EN‑gated), **EXT_5V_BUS** (rear M5‑Bus + side 10P + HY2.0 4P, gated by **`EXT5V_EN` on IO‑expander E1 = PI4IOE5V6408‑1 @0x43, bit E1.P2** — *not* E2; E2/0x44 owns `WLAN_PWR_EN`/`USB5V_EN`/`PWROFF`). **G36 = CAM_MCLK** (camera, not on the M5‑Bus); real ESP32‑P4 straps are **GPIO34 (JTAG‑sel)** and **GPIO2‑5 (default JTAG)** — confirm against the P4 SoC datasheet before using G2–G5 as strap‑sensitive outputs (§17.4 LR‑3). ESP32‑P4 has **no input‑only pins**.

**Why this matters here:** the M5‑Bus already carries **UART0 (G38/G37, console), a spare “PC” UART (G7/G6), a full SPI (G18/G19/G5), Int‑I²C (G31/G32), Grove‑I²C (G53/G54), and 5 V/3V3/BAT power** — i.e. everything needed to attach a companion MCU with plenty of headroom. The link (§7) lives here.

---

## 5. Feature → dependency & execution‑home matrix

“Home” = which MCU runs the feature’s logic. “Link role” = what crosses the wire. **P4‑native** features use the C6 (esp‑hosted); **Companion** features use the S3 + its radios. *Feasible‑P4* = could also run on the Tab5 C6 via CustomRpc (§10) if we later choose to.

| Feature | Radio / HW | Home (recommended) | Link role | Notes / caveats |
|---|---|---|---|---|
| WifiScan | Wi‑Fi | **P4‑native (C6)** | none | `esp_wifi_scan` works over hosted (Bruce §17.5). |
| ArpScanner | Wi‑Fi STA | **P4‑native** | none | Needs STA join → hosted‑OK. |
| CaptivePortal | Wi‑Fi AP+DNS+HTTP **+ raw‑TX deauth** | **Companion** | cmd+stream | ⚠ It’s an Evil‑Twin: fires deauth every 50 ms (`wifi.cpp:3086‑3105`, §17.2 HR‑4). Host‑native only as a *reduced* rogue‑AP (no deauth). |
| WpsScanner | Wi‑Fi scan+parse | **P4‑native** | none | Passive parse of scan IEs. |
| HiddenSsidReveal | Wi‑Fi (probe/deauth) | **Companion** *(feasible‑P4)* | cmd+stream | Needs raw TX; S3 proven. |
| PacketMonitor | Wi‑Fi promiscuous | **Companion** *(feasible‑P4)* | stream (PCAP) | Promiscuous RX; CustomRpc can do it (§10) but S3 is proven + higher pps. |
| DeauthDetect | Wi‑Fi promiscuous RX | **Companion** *(feasible‑P4)* | stream | Same as above. |
| BeaconSpammer | `esp_wifi_80211_tx` | **Companion** *(feasible‑P4)* | cmd | Beacon injection — CustomRpc‑supported frame type. |
| Deauther | raw mgmt/deauth TX | **Companion** | cmd | ⚠ Deauth is blocked by `esp_wifi_80211_tx` sanity‑check; DIV bypasses it + hidden SoftAP (§17.1 BR‑2). *Not* plain‑CustomRpc‑feasible. |
| ProbeRequestFlood | raw probe‑req TX | **Companion** *(feasible‑P4)* | cmd | Probe‑req — CustomRpc‑supported. |
| KarmaAttack | probe listen + AP impersonate | **Companion** | cmd+stream | Promiscuous + AP + raw TX combined; keep on S3. |
| FirmwareUpdate | OTA (Wi‑Fi/SD) | **P4‑native** | none | Rework to update the *Tab5* image; add companion‑OTA passthrough. |
| BleScan / Sniffer | BLE scan | **P4‑native (C6)** | none | NimBLE scan over hosted works (⚠ hosted BLE‑scan stall bug, §14 R‑BLE). |
| BleSpoofer / SourApple / AirTagSpoofer | BLE adv | **P4‑native** *(fallback companion)* | none | GAP adv over hosted HCI should work; validate custom AdvData. |
| AirTagSniffer / BleSkimmer | BLE scan+parse | **P4‑native** | none | Passive. |
| BLE‑Ducky | BLE HID peripheral | **P4‑native** *(fallback companion)* | none | HID‑over‑GATT over hosted — verify; else companion. |
| **BleJammer** | **nRF24** const‑carrier | **Companion** | cmd | *Not* an ESP‑BT feature — pure nRF24. |
| Scanner / ProtoKill | nRF24 | **Companion** | cmd+stream | 2.4 GHz channel scan / protocol kill. |
| EsbSniffer / EsbReplay | nRF24 (ESB) | **Companion** | cmd+stream+blob | Capture buffers cross the link. |
| MouseJack / …Inject | nRF24 | **Companion** | cmd+stream | Keystroke injection payloads over link. |
| SubGHz replay/save/jam/brute/detect | CC1101 (RCSwitch/`micros()`) | **Companion** | cmd+stream+blob | OOK capture blobs + live RSSI stream. Timing‑sensitive bit‑bang — link must not starve its loop. |
| RFID/NFC | PN532 | **Companion** | cmd+blob | Tag UIDs/dumps over link; keep PN532 on S3 SPI. |
| IR record/replay | IR LED/RX + RMT | **Companion** | cmd+blob | RMT symbol arrays over link. |
| GPS + wardriving | Neo‑6M UART | **Companion** | stream | NMEA/fix telemetry; wigle CSV written on whichever holds the SD. |

**Reading of the matrix:** ~9 connectivity‑class features become **Tab5‑native** (nice big‑screen UX, no link pressure); ~22 offense/RF features live on the **companion** and are surfaced through the link. Nothing is *dropped*; the split is about *where code runs*, not *whether it exists*.

---

## 6. Target architecture

### 6.1 System topology

```
        ┌──────────────────────────── M5Stack Tab5 (ESP32‑P4 HOST) ────────────────────────────┐
        │  M5GFX 720×1280 DSI  ·  A164 kbd (Wire1 0x6D)  ·  cap‑touch  ·  microSD  ·  M5Unified  │
        │  ┌───────────────┐  ┌──────────────┐  ┌───────────────┐  ┌───────────────────────┐    │
        │  │ DIV UI (M5GFX)│  │ Input router │  │ Connectivity  │  │  LINK CLIENT          │    │
        │  │ menus/icons/  │  │ A164+touch → │  │ C6 esp‑hosted │  │  framed binary, seq/  │    │
        │  │ screens       │  │ nav/keystroke│  │ scan/STA/AP/  │  │  ack/CRC, streams     │    │
        │  └───────────────┘  └──────────────┘  │ BLE scan/adv  │  └──────────┬────────────┘    │
        │        ▲  presents telemetry / mirrors canvas          │            │  M5‑Bus         │
        └────────┼──────────────────────────────────────────────┼────────────┼─────────────────┘
                 │                                               │   UART @3Mb │  (or SPI)
                 │                                               │            ▼
        ┌────────┴───────────────────── ESP32‑DIV (ESP32‑S3 COMPANION / RF ENGINE) ─────────────┐
        │  LINK SERVER  ·  headless feature runner  ·  existing radio drivers (unchanged)        │
        │  nRF24×3  ·  CC1101+RMT  ·  PN532  ·  IR  ·  GPS  ·  native Wi‑Fi promisc/raw TX  ·  BLE│
        └────────────────────────────────────────────────────────────────────────────────────────┘
```

### 6.2 Division of responsibility

- **Tab5 (host):** everything the *user sees and touches*, plus connectivity‑class Wi‑Fi/BLE. Owns the menu, theming, on‑screen keyboard, settings, SD for host‑side artifacts, and the link client. Renders companion telemetry natively (target) or mirrors the companion canvas (bootstrap).
- **Companion (S3):** everything *radio*. Runs feature logic **headless** (no local TFT/PCF needed) and answers link commands, emitting telemetry/streams/blobs. Keeps its SD for large captures (PCAP/IR/Sub‑GHz) to avoid streaming bulk over the link.

### 6.3 The two‑stage UI strategy (DR‑7)

The DIV feature code is **entangled with `tft.*` and local input** (no headless seam today). Rather than gate parity on disentangling 42 kLOC, stage it:

- **Stage A — Remote‑canvas mirror (bootstrap parity; un‑blocked by the N16R8 companion).** The companion holds a full‑screen `TFT_eSPI` sprite **in its 8 MB PSRAM** and renders each feature to it unmodified; **dirty tiles (RLE)** are pushed over the **SPI data‑plane** (§17.2 HR‑1 — *not* UART, or it’s ~2 fps) and composited by the Tab5, which forwards A164/touch as virtual `BTN_*`/touch events. This is exactly the Bruce `bruce-remote` precedent (8 MB PSRAM + 20 MHz SPI + IRQ), which now applies because Josh’s S3 is N16R8. **Reuses feature draw code as‑is** (except the Serial Terminal hardware scroll, §17.3 MR‑4), giving fast full parity and a link‑under‑load proof.
- **Stage B — Native M5GFX rendering (target UX).** Per‑feature, the companion sends **structured telemetry** (AP records, packet stats, RSSI arrays, NMEA, tag dumps) and the Tab5 draws it **full‑resolution** natively — highest‑value screens first (WifiScan list, PacketMonitor waterfall, BLE list, Sub‑GHz RSSI, GPS). Stage A remains the fallback for the long tail.

This restores the original two‑stage plan: ship parity early via the mirror, then upgrade screens to native without a big‑bang rewrite. **Still required regardless of PSRAM:** the headless‑seam refactor for blocking loops/input (§17.2 HR‑2) and the SPI data‑plane (HR‑1).

### 6.4 Host UI port (TFT_eSPI → M5GFX)

Mechanical where possible (Bruce §18.2 H‑odr, M‑metrics apply):
- Introduce a thin **`gfx` façade** (`tft.` → `M5.Display.`); M5GFX’s LovyanGFX method names line up (`fillScreen/fillRoundRect/drawString/setTextColor/…`). Use **`M5Canvas`** for sprites.
- **Delegate text metrics to M5GFX** (`textWidth/fontHeight/drawString`) — do **not** reuse a `len*6` stub; 720×1280 magnifies misalignment.
- Set `lib_ignore = TFT_eSPI, LovyanGFX, GFX Library for Arduino` so only M5GFX’s `lgfx` links (ODR).
- Recompute layout constants from `M5.Display.width()/height()` and rotation, not the hard‑coded 240×320. Re‑master `icon.h` at 2–3× or draw vector where cheap.
- Brightness/backlight via `M5.Display.setBrightness()` — drop `ledcSetup/ledcAttachPin` and the brown‑out‑register poke (P4/core‑3 don’t use them).

### 6.5 Host input router

- **A164 keyboard** via `M5Unit-KEYBOARD` (Bruce §17.3/§18.2 H‑kbd): `Wire1.begin(0,1)`, `INT=G50`, class `m5::unit::UnitTab5Keyboard`, **Normal mode**. Map arrows→`BTN_UP/DOWN/LEFT/RIGHT`, Enter→`SELECT`, Esc→back; fill printable ASCII for the on‑screen‑keyboard/text fields.
- **Capacitive touch** via `M5.Touch.getDetail()` → the existing `readTouchXY()` contract (throttled ~200 ms), replacing XPT2046. Keep the on‑screen nav‑bar cue as a touch affordance.
- A single **input event type** feeds both native screens and (as virtual button/touch events) the Stage‑A mirror.

---

## 7. The inter‑processor link (critical path)

The link is the make‑or‑break component. It must carry: low‑latency **commands** (start/stop/tune), high‑rate **telemetry streams** (packet stats, RSSI, NMEA), bulk **blobs** (PCAP, IR/Sub‑GHz captures, tag dumps), and — for Stage A — **canvas tiles**. Bruce §18.2 (H‑companion) is explicit that an ASCII CLI is the wrong shape; we define a real protocol.

### 7.1 Physical layer

**Primary: UART @ 3 Mbaud** on the M5‑Bus “PC” UART.

| Signal | Tab5 pin (M5‑Bus) | Companion (S3) | Note |
|---|---|---|---|
| Host TX → Comp RX | **G6 (PC_TX)** | a free S3 UART RX | e.g. DIV `RX_PIN`/spare |
| Host RX ← Comp TX | **G7 (PC_RX)** | a free S3 UART TX | |
| GND | GND (pins 1/3/5) | GND | common ground mandatory |
| Comp power | **5 V pin 28** (EXT5V_EN) / **BAT pin 30** | DIV VIN | budget in §7.6 |
| Reset (opt) | a free GPIO (**G16**) | S3 EN | host‑driven companion reset / boot‑guard |

- **Throughput:** 3 Mbaud ≈ 300 KB/s raw. Ample for telemetry/commands and moderate blobs; **bulk captures are written to the companion SD** and transferred lazily, not streamed live.
- **Upgrade path — SPI:** if canvas‑mirror or PCAP rates saturate UART, move the data plane to the **M5‑Bus SPI (G18/G19/G5)** with the Tab5 as master and a `DATA_RDY` GPIO (companion→host) for slave‑initiated frames. Keep UART as the control plane. (Bruce measured SPI‑FD ~25 Mbps vs SDIO; here SPI is only host↔companion and is plenty.) SPI is deferred until a measured need.

*Reconcile against other M5‑Bus users:* G6/G7 “PC” UART is free; avoid G37/G38 (UART0 console/programming). SPI pins G18/G19/G5 are free unless a future user wires an M5‑Bus SPI module.

### 7.2 Frame format

Length‑prefixed, CRC‑checked, sequence/ack, with a channel/type tag:

```
[ver 1B][type 1B][chan 1B][seq 2B][len 2B][payload …len][crc16 2B]
```
then **COBS‑encoded and 0x00‑delimited** on the wire (no SOF byte — COBS is self‑synchronising; a receiver always resyncs on the next 0x00). `type: HELLO|CAPS|HEARTBEAT|CMD|ACK|NAK|EVT|STREAM|BLOB|CANVAS|INPUT_EV|LOG`; `chan` = feature/session id. CRC‑16/CCITT‑FALSE over header+payload. **This is implemented and validated** in `tab5-port/divlink/` (COBS + CRC + streaming parser), with 88 native tests + an on‑P4 emulator self‑test. *(Supersedes the earlier `[SOF 0xA5]…` sketch — review L1.)*
- **Reliability:** CMD/ACK are reliable (retry w/ seq + timeout, bounded; server de‑dups by seq so a retransmit never double‑executes). STREAM/CANVAS are **best‑effort** with drop‑counters (never block the radio loop). BLOB is windowed/chunked with per‑chunk ack + resume.
- **Backpressure:** host advertises a credit window per channel; companion honors it and drops STREAM frames (incrementing a loss counter surfaced in the UI) rather than stalling capture. *Implementation status: the companion sink is non‑blocking (drop + count) today; the full credit‑window handshake is a remaining item (see `tab5-port/README.md`).*

### 7.3 Command/telemetry model

A compact registry mirroring the DIV feature set. Examples:

| Class | Command | Payload | Response / stream |
|---|---|---|---|
| Session | `FEATURE_START(id, params)` / `FEATURE_STOP` | feature enum + tuning | ACK + EVT “running” |
| Wi‑Fi off. | `WIFI_DEAUTH(bssid, ch, count)` | 6B+1B+2B | ACK + EVT tx‑count |
| Wi‑Fi mon. | `WIFI_PROMISC(ch, filter)` | 1B+1B | STREAM pkt‑meta; BLOB pcap‑on‑SD |
| nRF24 | `NRF_SCAN(range)` / `ESB_SNIFF(addr)` | — | STREAM channel‑RSSI / capture blobs |
| Sub‑GHz | `SUBGHZ_RX(freq,mod)` / `TX(blob)` | 4B+1B / blob | STREAM RSSI + BLOB OOK capture |
| RFID | `NFC_READ` / `NFC_EMULATE(uid)` | — / dump | BLOB tag data |
| IR | `IR_RECORD` / `IR_SEND(blob)` | — / symbols | BLOB rmt symbols |
| GPS | `GPS_STREAM(on)` | 1B | EVT fix (lat/lon/sats/…) |

For **Stage A**, an additional `CANVAS_CFG(w,h,scale)` + `CANVAS(tiles)` channel and an `INPUT(evt)` reverse channel carry the mirror.

### 7.4 Companion “headless” seam

Minimal change to the DIV firmware:
- Compile‑time `DIV_COMPANION` mode: **replace the panel target** — `tft` becomes an off‑screen `M5Canvas`/`TFT_eSprite` (Stage A) or is compiled out for native features (Stage B), and **input comes from the link** (`INPUT` events → the existing `BTN_*`/touch globals) instead of PCF8574/XPT2046.
- A **link task** on a dedicated core services frames; feature `setup()/loop()` run unmodified. This is the smallest possible diff to reach parity and is why the companion keeps its Arduino build (DR‑6).

### 7.5 Boot‑guard & recovery

- Host **owns companion reset** (G16→S3 EN). On link‑timeout, host power‑cycles/reset‑pulses the companion and re‑handshakes (`HELLO/CAPS` exchange advertising firmware version + feature bitmap).
- Companion runs a **watchdog**; a wedged feature returns it to idle without dropping the link.
- Mirror the Bruce §17.5 **NVS “attempting” flag** pattern for any C6/hosted init on the host so a bad connectivity bring‑up can’t brick host boot.

### 7.6 Power budget (must verify on bench)

- nRF24 **with PA/LNA** draws current spikes that brown out weak 3V3 LDOs; CC1101 TX + PN532 field add load. Since this is a **DIY companion (generic S3 devboard + wired modules)**, **power the S3 from Tab5 EXT_5V (pin 28) or BAT (pin 30) into the board’s 5 V/VIN** and feed each radio from the **S3 devboard’s own 3V3 regulator with proper bulk/decoupling caps** (or a dedicated 3V3 LDO for the nRF24‑PA) — do **not** feed radios from Tab5 3V3. Budget the S3 devkit’s onboard regulator headroom; add a local cap array at the nRF24.
- Confirm **EXT5V_EN** is asserted at host boot before expecting the companion to enumerate — it is **E1 (PI4IOE5V6408‑1 @0x43) bit P2** (corrected per §17.2 HR‑5), asserted via the M5Unified expander API. Measure worst‑case (nRF24 PA + CC1101 TX + GPS + PN532) against the Tab5 battery/USB budget.

---

## 8. Wi‑Fi / BLE strategy (the design fork)

Two viable homes for **raw‑injection** Wi‑Fi/BLE. We choose **companion‑primary** (DR‑3) and keep CustomRpc documented so the host can later do it natively.

### 8.1 Companion‑native (primary, proven)
The DIV firmware **already** runs promiscuous RX, `esp_wifi_80211_tx`, and NimBLE raw adv on the S3. Surfacing these over the link is **zero new radio risk** — only protocol work. All 802.11 offense + BLE spoof/AirTag/SourApple/BLE‑jammer(nRF) reach parity here first.

### 8.2 Tab5‑C6 via esp‑hosted **CustomRpc** (documented alternative)
Raw injection **is** achievable on P4+C6. The **CustomRpc** mechanism (RPC ID 388, esp‑hosted ≥ v2.8.1) exposes, via a rebuilt C6 **slave** firmware + host‑side lib, exactly the primitives DIV needs:

| API | Direct hosted RPC | Via CustomRpc |
|---|---|---|
| `esp_wifi_set_channel` | OK | OK |
| `esp_wifi_80211_tx` | **not supported** | **OK** |
| `esp_wifi_set_promiscuous` | **not supported** | **OK** |
| `esp_wifi_set_promiscuous_filter` | **not supported** | **OK** |

Injectable frame types: **beacon, probe‑req/resp, action, non‑QoS data** — covers **BeaconSpammer (`0x80`), ProbeFlood (`0x40`)**, and promiscuous PacketMonitor/DeauthDetect. **⚠ Deauth/disassoc are NOT in this set** — `esp_wifi_80211_tx`’s `ieee80211_raw_frame_sanity_check` rejects them (§17.1 BR‑2). Deauther/HiddenSsidReveal(force)/CaptivePortal/Karma need the C6 slave *additionally* rebuilt to override that check + inject from an active SoftAP; absent that, they stay **companion‑primary**. **Constraints:** channel‑locked while monitoring; ~2000 raw‑TX frames/s ceiling (RPC round‑trip); single‑threaded command API (needs a host mutex); promiscuous RX competes with STA throughput; **requires re‑flashing the C6 slave (OTA from the P4) with the extension** + `CONFIG_ESP_HOSTED_ENABLE_PEER_DATA_TRANSFER=y`, `CONFIG_ESP_HOSTED_MAX_CUSTOM_MSG_HANDLERS=8`.
*Reference implementation:* `r4d10n/esp32p4-c6-wifi-test` (host `wifi_raw.c/h` + slave `wifi_raw_slave.c`, message IDs `0x0100‑0x0200`).

**Why not primary:** re‑flashing the C6 carries a boot‑hang/brick risk (Bruce §17.5 R‑C), the ~2000 fps cap and channel‑lock are tighter than the S3, and it duplicates capability the companion already has. Keep it as the **single‑board‑later** option (host offense without the companion) and validate opportunistically.

### 8.3 Connectivity‑class (always Tab5‑native)
WifiScan/ArpScanner/CaptivePortal/WPS + BLE scan/adv run on the **C6 over stock esp‑hosted** (Bruce §17.5). ⚠ Known hosted **BLE‑scan stall after ~90 s** (esp‑hosted‑mcu issue #180) — add a scan‑restart watchdog (§14 R‑BLE).

---

## 9. Driver‑by‑driver mapping

| DIV module | Depends on | Port action |
|---|---|---|
| `Touchscreen.*` (XPT2046) | resistive touch | **Host:** replace with `M5.Touch`; keep the `readTouchXY` API contract. **Companion:** compiled out (input via link). |
| PCF8574 keypad | I²C buttons | **Host:** replace with A164 router. **Companion:** compiled out. |
| `TFT_eSPI`/`icon.h`/`KeyboardUI` | 240×320 panel | **Host:** M5GFX façade + re‑layout + re‑master icons; on‑screen kbd on cap‑touch. **Companion (Stage A):** render to off‑screen sprite. |
| LEDC backlight / brown‑out reg | core‑2 API | **Host:** `M5.Display.setBrightness()`; delete brown‑out poke. |
| `wifi.cpp` scan/AP/portal | esp_wifi/WebServer | **Host‑native (C6).** Port HTML assets; verify `WebServer/DNSServer` over hosted. |
| `wifi.cpp` promiscuous/raw‑tx | 802.11 low‑level | **Companion** (primary) / CustomRpc (alt). Surface via link. |
| `bluetooth.cpp` scan/adv/HID | NimBLE | **Host‑native (C6)** with companion fallback; validate custom AdvData + HID‑over‑GATT over hosted. |
| `bluetooth.cpp` BleJammer + nRF sections | **nRF24** | **Companion** (SoC‑agnostic). |
| `subghz.cpp` | CC1101 + RCSwitch/`micros()` | **Companion** (unchanged; avoids porting `micros()` bit‑bang timing + shared‑SPI to P4 — §17.3 MR‑1). |
| `rfid.cpp` | PN532 SPI | **Companion.** |
| `ir.cpp` | IR + RMT | **Companion.** |
| `gps.cpp` | Neo‑6M UART | **Companion** (its own UART; link streams fixes). |
| `SettingsStore` / SD | LittleFS/SD JSON | **Split:** host settings on Tab5 SD; capture artifacts on companion SD; sync summaries over link. |

---

## 10. Companion firmware changes (keep the S3 build)

1. Add `DIV_COMPANION` build flag (new `BoardConfig` option) that:
   - retargets `tft` to an off‑screen sprite (Stage A) / no‑ops draws (Stage B native features),
   - sources input from the link,
   - starts the **link server task** in `setup()`.
2. Add `link/` (framing, channels, command dispatch, blob/stream helpers) — new, ~1–2 kLOC, no change to radio drivers.
3. Add a **capabilities handshake** (firmware ver + feature bitmap) and a **watchdog**.
4. Route bulk captures to companion SD; expose `BLOB_FETCH(path,range)` for lazy transfer.
5. Everything else — nRF24/CC1101/PN532/IR/GPS/Wi‑Fi/BLE logic — **unchanged**.

---

---

## 11. Build system & toolchain

**Host (Tab5) — new PlatformIO project** (Bruce §17.1/§18.1‑B3 proven recipe):
- **Pin** `platform = https://github.com/pioarduino/platform-espressif32/…/55.03.39/…`; `board = m5stack-tab5`; **`platform_packages` override →** Launcher’s active P4 + esp‑hosted bundle `launcher_esp32-arduino-libs-20260807-031549.tar.gz` (root `[env]`, `platformio.ini:99‑100`). Do **not** use the commented‑out `55.03.32`/Tab5‑specific tarball, and do **not** use M5’s stock `esp32-p4-evboard` quick‑start (pioarduino 54.03.21, **no esp‑hosted**). Resolves Bruce’s open V1/B3 reconcile to **55.03.39**.
- Flash offsets P4: bootloader `0x2000`, part‑table `0x8000`, app `0x10000` (Bruce §17.1/§18.1‑B1). Provide a P4 partition CSV sized for the host app + LittleFS (host settings/assets).
- `lib_deps`: `m5stack/M5Unified@^0.2.17`, `m5stack/M5Unit-KEYBOARD`; **no** TFT_eSPI/RF24/CC1101/PN532 libs on the host.
- `lib_ignore = TFT_eSPI, LovyanGFX, GFX Library for Arduino, SensorLib, XPowersLib` (full ODR set, Bruce §18.2 H‑odr — all five, so only M5GFX’s `lgfx` links).
- `-DUSE_M5GFX=1 -DROTATION=? -DBOARD_HAS_PSRAM -DARDUINO_USB_CDC_ON_BOOT=1`.

**Companion (ESP32‑DIV) — keep Arduino build**, add `-DDIV_COMPANION`. No P4 toolchain, no RF library ports, no RMT re‑timing. This is the single biggest risk reduction versus a native P4 port.

**No merged exFAT bundle needed on the host** (unlike Bruce §18.1‑B3): the host app is small and doesn’t require Bruce’s crypto/exFAT lib set. If host‑side exFAT SD is wanted, that’s an additive bundle concern, not a gate.

---

## 12. Phased implementation plan

Two tracks run in parallel after a shared bring‑up. **“Green” = compiles, flashes, and the acceptance check passes on hardware.**

```
P0 host scaffold ─▶ P1 display ─▶ P2 input ─▶ P3 link bring‑up ─┬─▶ HOST track:  P4 connectivity(C6) ─▶ P8 native screens ─▶ P10 polish
                                                                └─▶ COMPANION track: P5 headless+mirror ─▶ P6 RF cmds ─▶ P7 offense ─▶ P9 blobs
```

- **P0 — Host scaffold (no device‑radio).** PlatformIO builds an empty M5Unified app on `m5stack-tab5`; `M5.begin()`; screen on; serial log. *Accept:* boots to a “DIV‑Tab5” splash, backlight via M5GFX.
- **P1 — Display/UI façade.** `gfx` façade; port menu + `icon.h` + theming; re‑layout for the real resolution; M5GFX text metrics. *Accept:* main menu + one submenu render correctly at native res.
- **P2 — Input.** A164 (`M5Unit-KEYBOARD`, Normal mode) + cap‑touch → unified input; on‑screen keyboard works. *Accept:* full menu navigation + text entry by keyboard and touch.
- **P3 — Link bring‑up.** UART @3 Mbaud on G6/G7; frame/CRC/seq/ack; `HELLO/CAPS` handshake; companion reset via G16. *Accept:* host enumerates companion, round‑trips CMD/ACK, sustains a 100 KB/s telemetry stream with loss counters.
- **P5 — Companion headless + Stage‑A mirror.** `DIV_COMPANION` sprite target + link input; `CANVAS` channel. *Accept:* one DIV feature (WifiScan) fully usable **mirrored** on the Tab5 — instant parity proof.
- **P4 — Host connectivity (C6).** esp‑hosted bring‑up (Bruce §17.5 guarded recipe); native WifiScan/ArpScanner/CaptivePortal/WPS + BLE scan/adv with the scan‑restart watchdog. *Accept:* the 9 connectivity‑class features run natively.
- **P6 — Companion RF commands.** nRF24 (Scanner/ESB/MouseJack/ProtoKill/BleJammer), CC1101 Sub‑GHz, PN532, IR, GPS exposed as CMD/STREAM. *Accept:* each RF feature drivable from the Tab5 (mirrored UI acceptable).
- **P7 — Offense.** Companion Wi‑Fi promiscuous/raw‑TX + BLE spoof surfaced over link (Deauth/Beacon/Probe/Karma/PacketMonitor/DeauthDetect/SourApple/AirTag). *Accept:* offense parity with the standalone DIV, operator‑gated.
- **P8 — Native screens (Stage B).** Migrate WifiScan, PacketMonitor waterfall, BLE list, Sub‑GHz RSSI, GPS to native M5GFX + telemetry. *Accept:* those screens use full resolution; Stage‑A retained for the tail.
- **P9 — Blobs & artifacts.** PCAP/IR/Sub‑GHz/NFC capture on companion SD + `BLOB_FETCH`; wigle CSV; host FirmwareUpdate + companion‑OTA passthrough. *Accept:* captures land as files; both images updatable.
- **P10 — Parity sweep & polish.** Feature‑by‑feature checklist vs standalone DIV; latency/UX tuning; optional SPI data‑plane if UART saturates; optional Tab5‑C6 CustomRpc validation (§8.2).

---

## 13. Risks & mitigations

| # | Risk | Sev | Mitigation |
|---|---|---|---|
| **R‑link** | Link is the whole game; wrong protocol shape stalls sniffers/monitors. | **High** | Framed binary w/ credits + drop counters (§7); bulk→companion SD; SPI data‑plane upgrade path; Bruce §18.2 H‑companion precedent. |
| **R‑UI** | Disentangling 42 kLOC of `tft.*`/input for native rendering is large. | **High** | Stage‑A mirror reuses 100 % of feature code for parity; Stage‑B migrates only high‑value screens. |
| **R‑power** | nRF24‑PA/CC1101‑TX/PN532 brownouts over M5‑Bus power. | **Med** | Power companion from EXT_5V/BAT and let DIV regulators feed radios (§7.6); bench worst‑case; confirm EXT5V_EN. |
| **R‑BLE** | Hosted BLE scan stalls ~90 s (esp‑hosted‑mcu #180); adv/HID over hosted unproven for DIV payloads. | **Med** | Scan‑restart watchdog; validate custom AdvData/HID early; **fallback = run BLE on the companion**. |
| **R‑latency** | Mirror + input round‑trip feels laggy for fast UIs. | **Med** | Dirty‑tile RLE canvas; prioritize native (Stage B) for interactive screens; SPI upgrade. |
| **R‑icons** | 240×320 assets look poor at 720×1280. | **Low** | Re‑master `icon.h` at scale / vectorize; layout from `Display.width()/height()`. |
| **R‑C6rpc** | (Only if §8.2 pursued) C6 re‑flash boot‑hang. | **Med** | Keep offense on companion (primary); NVS attempting‑flag guard (Bruce §17.5 R‑C); OTA‑flasher pattern from reference repo. |
| **R‑time** | Two firmwares + protocol is real engineering. | **Med** | Parity via Stage‑A lands early; native/polish is incremental and non‑blocking. |

---

## 14. Test & validation strategy

- **Link soak:** sustained STREAM + concurrent CMD/ACK for hours; assert bounded loss counters, zero deadlocks, clean recovery after forced companion reset.
- **Per‑feature parity checklist:** every feature in §3’s inventory exercised on the Tab5 vs a standalone DIV as reference oracle (scan lists match; deauth observed on a monitor; Sub‑GHz replay actuates a known target; NFC UID matches; IR replays; GPS fix matches).
- **RF correctness:** capture DIV‑via‑Tab5 output on external SDR/2nd receiver (802.11 frames, CC1101 OOK, nRF24 ESB) to confirm the link didn’t corrupt payloads/timing.
- **Latency budget:** measure command→actuation and capture→display for interactive features; set Stage‑A vs Stage‑B thresholds from data.
- **Power:** instrument worst‑case current; verify no brownout/reset under nRF24‑PA + CC1101‑TX bursts.
- **Boot‑guard:** kill the companion mid‑feature; assert host stays responsive and re‑handshakes. Kill the link; assert companion watchdog returns to idle.
- **Regression on the companion:** `DIV_COMPANION` off must still build/flash a stock ESP32‑DIV (don’t fork the firmware).
- **High‑stakes verification:** run the final parity + link‑soak review through an independent subagent before sign‑off.

---

## 15. Open items to resolve during execution

- **O1 — Companion UART pins:** pick the S3 UART for the link (avoid GPS UART2, SD‑SPI, and radio CE/CSN pins in `shared.h`); confirm G6/G7 direction/naming on a scope.
- **O2 — Canvas transport sizing:** measure mirror bandwidth for the busiest screen (PacketMonitor); decide UART‑vs‑SPI data plane at P5, not on paper.
- **O3 — BLE home:** confirm whether custom BLE **adv** (SourApple/AirTag) and **HID** work over hosted on the current C6 firmware; if not, move all BLE to the companion.
- **O4 — CaptivePortal over hosted:** verify SoftAP + `DNSServer` + `WebServer` behave on esp‑hosted (client redirect, DNS hijack) before committing it to host‑native.
- **O5 — SD ownership:** finalize which artifacts live on which SD; define the sync/summary payloads.
- **O6 — FirmwareUpdate semantics:** define host‑image OTA + companion‑image passthrough (and optional C6 slave OTA if §8.2 is pursued).
- **O7 — Rotation/portrait:** DIV is portrait‑native (240×320). Decide Tab5 portrait vs landscape and re‑layout accordingly.
- **O8 — Icon re‑master pipeline:** script `icon.h` upscale/vectorization.

---

## 16. Companion wiring (radios on the S3) + Tab5‑side fallback

### 16.1 Companion S3 pin map (PRIMARY — what Josh wires)

Josh wires the RF modules to his generic **ESP32‑S3 (N16R8)** using the DIV firmware’s **existing, proven `shared.h` S3 defaults** (the `#else`/`BOARD_ESP32_DIV_V2` branch). Because the companion is **headless** (the Tab5 is the display), the TFT (35/36/37), XPT2046 touch, and PCF8574 pins are **freed** — only the radio + link pins are wired.

| Module | S3 pins (`shared.h` V2) | Bus / note |
|---|---|---|
| **Shared SPI bus** | **MOSI 11 · SCK 12 · MISO 13** | One bus, **modal** — SD + CC1101 + PN532 + nRF24 share it with distinct CS/CE (§17.2 HR‑3). |
| microSD | CS 10, CD 38 | Local capture store (PCAP/IR/Sub‑GHz/NFC). |
| **CC1101** | CS 5 · GDO0 6 · GDO2 3 | OOK via RCSwitch/`micros()` (§17.3 MR‑1). |
| **nRF24 ×3** | CE/CSN = 15/4 · 47/48 · 14/21 | 2.4 GHz Scanner/ESB/MouseJack/BleJammer. |
| **PN532** | SS 5 (SPI) *or* move to I²C | RFID/NFC. Consider I²C to reduce shared‑SPI contention. |
| **IR** | RX 21 · TX 14 | ⚠ Overlaps nRF24#3 (CSN 21 / CE 14) by design — firmware runs them modally; if you want IR **and** nRF24#3 concurrently, reassign IR to two free S3 GPIOs. |
| **GPS (Neo‑6M)** | RX 5 · TX 6 | ⚠ Its own UART, but pins overlap CC1101 CS(5)/GDO0(6) in the defaults — reassign GPS to a free UART pair (e.g. 17/18, freed by headless) so GPS + Sub‑GHz coexist. |
| **Link to Tab5** | pick a **verified‑free** S3 UART (e.g. **8/9**) + reset‑in from Tab5 G16 | §17.3 MR‑6: do **not** reuse 3/6 (CC1101/GPS). SPI data‑plane can use the freed TFT‑HSPI pins. |
| Power in | 5 V/VIN from Tab5 EXT_5V(pin 28)/BAT(pin 30) | §7.6; local 3V3 + caps for nRF24‑PA. |

> These are the DIV’s **current, working** S3 assignments — start here, then resolve the two documented overlaps (IR↔nRF24#3, GPS↔CC1101) and the link‑UART pin (MR‑6) using pins freed by headless mode. Confirm each against `shared.h` at build time; they’re `#ifndef`‑overridable in `BoardConfig.h`.

### 16.2 Tab5‑side fallback bus map (only if radios ever move onto the Tab5)

If the companion approach is later abandoned for a single‑board build, the datasheet‑authoritative M5‑Bus assignments (from §4) are:

| Signal | Tab5 pin | Note |
|---|---|---|
| Aux SPI SCK / MISO / MOSI | **G5 / G19 / G18** | M5‑Bus; shared by CC1101 + nRF24 (identical SCK/MISO/MOSI, distinct CS/CE, modal use — Bruce §18.3 M‑spi). |
| CC1101 CS | **G16** (pin 2) | free GPIO. |
| CC1101 GDO0 (TX bit‑bang + RX IRQ) | **G45** (pin 8) | free GPIO; re‑time RMT for P4 (Bruce §18.3 M‑rmt). |
| nRF24 CE / CSN | **G2 / G48** (pins 21/22) | free GPIO. |
| nRF24 IRQ (opt) | **G47** (pin 23) | free GPIO. |
| PN532 (I²C) | **Grove Port.A G53 / G54** | cleanest external I²C. |
| GPS UART | **G7 / G6** (PC_RX/PC_TX) | free M5‑Bus UART. |
| IR TX / RX | **G3 / G4** (pins 19/20) | free GPIO; P4 RMT. |
| Power | **5 V pin 28 / 3V3 pin 12 / BAT pin 30** | 5 V is EXT5V_EN‑gated. |

> Avoid **G37/G38** (UART0 console). Note SCK=G5, IR=G3/G4, nRF24 CE=G2 land on the P4 **default‑JTAG group (GPIO2‑5)** — usable as GPIO (M5 route M5‑Bus SPI there) but confirm boot/JTAG straps first (§17.4 LR‑3). This table is the fallback only; **DR‑2 keeps radios on the companion.**

---

---

## 17. Adversarial‑review corrections (authoritative — overrides conflicting text above)

Three independent adversarial reviewers (link/architecture lens · Wi‑Fi/BLE/radio lens · toolchain/UI/pins lens) read this plan **and the ESP32‑DIV source, the Launcher board, and Josh’s Tab5 datasheet**. Accepted findings below **override any conflicting earlier text**, each with evidence and fix. Paths are repo‑relative under `ESP32-DIV/ESP32-DIV/` unless noted.

### 17.1 Blocking

- **BR‑1 — ~~The companion has no PSRAM~~ → RESOLVED by hardware choice (Josh runs a generic S3 **N16R8, 8 MB PSRAM**, not the DIV’s N16 module).** *Original finding (still valid for an actual ESP32‑DIV V2 board):* that product uses **ESP32‑S3‑WROOM‑1U‑N16** (`Schematic/v2/main-BOM.xls`) with **no PSRAM** — TFT on GPIO35/36/37 (the OPI‑PSRAM pins, `Libraries/User_Setup v2.h`), zero PSRAM APIs, and it already fights to hold one ~140 KB sprite (`gps.cpp:56‑78, 1301`). A 240×320×16 frame is 153.6 KB and can’t stay resident in DRAM there. **But Josh is not using that board** — his N16R8 devkit has 8 MB PSRAM, so the **full‑frame Stage‑A mirror is viable** (DR‑7 revised; §6.3 restored). **Residual constraints that still apply regardless of PSRAM:** the mirror must run over the **SPI data‑plane** (§17.2 HR‑1), and the **headless‑seam refactor** (§17.2 HR‑2) is still net‑new work. Set `-DBOARD_HAS_PSRAM`, verify `esp_psram` at boot, and place the canvas + PCAP pools in PSRAM.

- **BR‑2 — Deauth/disassoc are NOT injectable through the cited CustomRpc path; §8.2 and the §5 “feasible‑P4” flags overstate coverage.** Espressif’s `esp_wifi_80211_tx()` runs `ieee80211_raw_frame_sanity_check` which **rejects deauth (`0xC0`)/disassoc (`0xA0`)**; the reference slave calls the stock API from `WIFI_IF_STA` with no bypass. The DIV’s deauth only works because it **overrides the check** (`extern "C" int ieee80211_raw_frame_sanity_check(...){return 0;}`, `wifi.cpp:4295`) **and runs a hidden SoftAP** for `WIFI_IF_AP` raw TX (`wifi.cpp:5856‑5869`, `1420‑1421`). **Fix:** BeaconSpammer (`0x80`) and ProbeFlood (`0x40`) are genuinely CustomRpc‑injectable; **Deauther, HiddenSsidReveal(force), CaptivePortal, Karma are not** without *also* rebuilding the C6 slave to override the sanity check and inject from an active SoftAP — extra work with its own brick risk (R‑C6rpc). Remove deauth‑class features from the CustomRpc‑covered set; they stay **companion‑primary** (DR‑3 already covers them). **Foot‑gun:** any headless refactor (§7.4/§10) that drops the SoftAP setup or the sanity‑check TU **silently kills deauth** — keep both.

### 17.2 High

- **HR‑1 — SPI is the data‑plane baseline for any canvas/tile mirror, not a deferral (§7.1).** 240×320×16 = 153.6 KB → **~2 fps at 3 Mbaud** before framing; PacketMonitor’s FFT waterfall (`wifi.cpp:410‑470`) and Sub‑GHz RSSI (`subghz.cpp:848/2476/2508`) redraw large regions every loop, and live PCAP (`wifi.cpp:521`, 512‑B snap, 3‑slot pool) can alone hit ~300 KB/s. The only working precedent runs **20 MHz SPI + 50 Hz band‑diff w/ IRQ** (Bruce `bruce_remote_link.h`, `REMOTE_LINK_HZ 20000000`). **Fix:** keep UART @3 Mbaud as the **control plane**; make **M5‑Bus SPI (G18/G19/G5) master + a `DATA_RDY` IRQ line** the **baseline** transport for tiles/PCAP. SPI cannot use the companion’s radio bus — it uses the display‑HSPI pins freed only in headless mode. Set a waterfall fps/latency acceptance at P5 (O2).

- **HR‑2 — “feature setup()/loop() run unchanged” is false; budget a headless refactor.** Features are modal/blocking: `delay()` appears ~194× in the `.ino`, 94× in `wifi.cpp`; empty `while(isButtonPressed(BTN_SELECT)){}` spins appear ~40× in the `.ino` (e.g. `1227/1261/1295`) with no yield. Headless, these spin waiting for a link event and can starve the deliverer. The *input‑read* seam **is** narrow (redirect `isPhysicalButtonPressed`/`isTouchNavButtonPressed`/`touchSampleOk`). **Fix:** reword §7.4/§10 to include a real pass replacing empty button‑spins with link‑fed edge events + `vTaskDelay`, and de‑`delay()`‑ing hot loops. Not a footnote — a companion work‑item.

- **HR‑3 — No free core, and radios share ONE modal SPI bus; “dedicated core”/“concurrent features” over‑promise (§7.2/§7.4).** All background tasks pin to **core 0** (`wifi.cpp:2084‑2090`, `bluetooth.cpp:4003‑4011`, `utils.cpp:757‑765`), as do the Wi‑Fi/BT stacks + promiscuous cb; feature loops run core 1. And SD/CC1101/PN532/nRF24 all share **GPIO 11/12/13** (`shared.h`; IR even overlaps nRF24#3 pins) — only **one SPI radio active at a time**; the DIV is single‑feature‑modal by design. **Fix:** drop “dedicated core” (pin the link to core 1 with the feature loop, or measure core‑0 contention). State that `chan` multiplexes **control + telemetry + lazy blob for the one active feature**, and the link **scheduler serializes SPI‑radio sessions** and reports “busy” — it is not parallel‑radio licence.

- **HR‑4 — CaptivePortal is mis‑homed: it is an Evil‑Twin with active raw‑TX deauth, not “AP+DNS over hosted” (§5).** `cpSendDeauthFrame()` → `Deauther::wsl_bypasser_send_raw_frame()` every 50 ms (`wifi.cpp:3086‑3105, 4195‑4197`; UI “Evil Twin: %u pkts” `:3695`). **Fix:** home CaptivePortal on the **companion** (or scope the host‑native build as a *reduced* rogue‑AP with no deauth and remove it from the “9 connectivity‑class native” acceptance). Update O4 to include the deauth engine, not just SoftAP/DNS/WebServer.

- **HR‑5 — EXT5V_EN is on IO‑expander E1 (0x43) bit E1.P2, NOT E2 (§4, §7.6).** Datasheet `Tab5.pdf` p.13: `PI4IOE5V6408‑1 (0x43) … EXT_5V_BUS → EXT5V_EN`; E2 (0x44) owns `WLAN_PWR_EN`(P0)/`USB5V_EN`/`PWROFF_PULSE`. Following §7.6 as written pokes the **wrong chip** and never asserts the 5 V gate feeding the companion. **Fix:** correct §4 and §7.6 to **E1 (0x43) P2**; the host must assert it (M5Unified expander write) at boot before the companion enumerates. *(Applied inline.)*

- **HR‑6 — Flip the BLE‑offense default to the companion, matching DR‑3’s own logic (§8.3/DR‑4).** The DIV already does custom 31‑B adv (`bluetooth.cpp:1524‑1537`, Samsung/Google/SourApple/AirTag templates `:252‑265`) and HID‑over‑GATT (`ducky.cpp:1117‑1159`) natively on the S3. Over hosted C6 these are **plausible but unproven** (TX path shares the #180 stall subsystem; HID needs SMP/bonding over hosted, lightly exercised — esp‑hosted‑mcu #115). **Fix:** default **SourApple/AirTagSpoofer/BleSpoofer/BLE‑Ducky to the companion**; treat host‑native C6 BLE adv/HID as the opportunistic upgrade (O3’s default flips).

### 17.3 Medium

- **MR‑1 — Sub‑GHz uses RCSwitch + `micros()` busy‑wait, NOT RMT; the §3/§5/§9 “CC1101 + RMT” rationale is wrong (conclusion still right).** `subghz.cpp` has **zero** RMT refs: OOK TX via `mySwitch.send()`/`enableTransmit()` (`:999‑1010`), raw capture/replay via `micros()`/`delayMicroseconds()` (`:1036‑1050, 3353, 4128‑4141`), CC1101 async transparent `setCCMode(0)` (`:1397`). **Fix:** keep it on the companion for the **real** reason — RCSwitch/`micros()` bit‑bang timing + vendored SmartRC‑CC1101 lib + shared SPI bus. The Bruce §18.3 “M‑rmt” cross‑ref belongs to **IR** (`ir.cpp` uses `IRremoteESP8266`, which does use RMT) — §3’s “IR — RMT” is fine. Link task must not starve the timing‑sensitive Sub‑GHz loop.

- **MR‑2 — Pin the toolchain to 55.03.39 + the 20260807 bundle (§11).** Launcher’s **active** build is root `[env]` `platformio.ini:99‑100` = `…/55.03.39/…` + `launcher_esp32-arduino-libs-20260807-031549.tar.gz`; the tab5 board’s `55.03.32` + Tab5‑specific tarball lines are **commented out** (`boards/m5stack-tab5/platformio.ini:13‑14`). esp‑hosted is confirmed present in the active bundle. **Fix:** pin `platform=…/55.03.39/…` and that `platform_packages` tarball explicitly; this resolves Bruce’s open V1/B3 reconcile to **55.03.39**. *(Applied inline.)*

- **MR‑3 — `lib_ignore` must be the full 5‑entry H‑odr list (§11).** The plan cites Bruce §18.2 H‑odr but lists only 3. **Fix:** `lib_ignore = TFT_eSPI, LovyanGFX, GFX Library for Arduino, SensorLib, XPowersLib`. *(Applied inline.)*

- **MR‑4 — Serial Terminal hardware scroll has no M5GFX/DSI equivalent; “100 % reuse / mechanical façade” is falsified (§6.3/§6.4).** `utils.cpp:1478‑1505` issues raw ILI9341 scroll commands `tft.writecommand(ILI9341_VSCRSADD/VSCRDEF)` (Serial Terminal, `.ino:3555`). The Tab5 DSI panel has no such register set and `M5.Display` exposes no `writecommand`; on a headless sprite it’s a no‑op. Total `tft.` call‑sites ≈ **2,836** — a large mechanical pass, not a thin façade. **Fix:** rewrite the terminal as **software scroll** (scroll an `M5Canvas`/redraw); flag it as the one non‑mechanical feature. (Credit: touch/sprite/DMA seams are otherwise clean — 0 `tft.getTouch` in features, 1 functional sprite, no DMA/pushImage/custom fonts.)

- **MR‑5 — Escalate the hosted BLE‑scan‑stall mitigation (§8.3 R‑BLE).** esp‑hosted‑mcu #180: reports stop ~60‑90 s and a HCI stop/start does **not** resume them (active scan worse than passive). A scan‑restart watchdog is likely insufficient. **Fix:** recovery = **C6 BT‑controller deinit/reinit or C6 reset**; force **passive** scan where possible. Reinforces HR‑6 (BLE on companion).

- **MR‑6 — The example link UART pins collide on the S3 (§7.1/O1).** DIV defaults `RX_PIN 6`/`TX_PIN 3`, but **G6=`GPS_UART_TX`/`SUBGHZ_TX`/`CC1101_GDO0`** and **G3=`SUBGHZ_RX`/`CC1101_GDO2`** (`shared.h`). **Fix:** remove the “e.g. `RX_PIN`” hint; assign the link UART to a **verified‑free S3 GPIO** (e.g. 8/9), or repurpose UART0 (43/44) if the console is sacrificed, or use freed TFT‑HSPI pins in headless mode. Elevate O1.

- **MR‑7 — Reset‑guard details (§7.5).** Driving S3 `EN`/`CHIP_PU` low is a valid reset (EN isn’t a strap), but ① confirm the **DIV V2 PCB breaks out EN** to an accessible pad (schematics are images — unverified) and ② the Tab5 must hold **G16 high/high‑Z at boot** so it doesn’t pin the companion in reset; note G16 is reused for CC1101 CS in the §16 fallback (non‑concurrent, but flag). 

### 17.4 Low

- **LR‑1 — `icon.h` is ~22 KB of pixel data, not 150 KB (§3).** 143 mono 1‑bpp XBitmaps (131 @16×16, 10 @100×120, 1 @150×150) drawn via `tft.drawBitmap` (M5GFX‑compatible). The 131 glyphs are scriptable upscales; the **11 large bespoke images need manual redraw** for 720×1280 — that’s the real art task (O8).
- **LR‑2 — Second legacy‑LEDC site: buzzer at `subghz.cpp:588‑589`** (besides the backlight). Harmless while the companion stays core‑2 (DR‑6); if ever bumped to core‑3 it needs `ledcAttach()`. Host (new code) avoids all of this by construction.
- **LR‑3 — Strap note is imprecise (§4/§16).** Datasheet: **G36 = CAM_MCLK** (not on the M5‑Bus); no datasheet line maps G35/G36 to BOOT_MODE. Real ESP32‑P4 straps: **GPIO34 (JTAG‑sel)**, **GPIO2‑5 default JTAG**. The §16 fallback puts SPI SCK=G5, IR=G3/G4, nRF24 CE=G2 on the JTAG group — usable as GPIO (M5 route M5‑Bus SPI there) but **note it** and confirm true straps against the ESP32‑P4 SoC datasheet before any single‑board re‑home.
- **LR‑4 — nRF24 SPI @16 MHz (`bluetooth.cpp:3124‑3126`) exceeds the ~10 MHz part spec** — fine on the S3’s short traces today; only a concern for the §16 fallback’s longer M5‑Bus traces. **Guardrail:** never split a per‑packet RX→decide→TX loop (e.g. MouseJack) across the link — keep those loops wholly on the companion (the plan already does).

### 17.5 Affirmed by review (do not re‑litigate)

M5‑Bus 30‑pin table matches the datasheet exactly; esp‑hosted present in the 55.03.39 bundle; flash offsets `0x2000/0x8000/0x10000`; SDIO2/microSD/system‑I²C/A164 pins; M5Unified@^0.2.17 + M5Unit‑KEYBOARD; **BleJammer = nRF24 const‑carrier** (not ESP‑BT); best‑effort STREAM + drop‑counters matches the firmware’s lossy monitors; the input‑read seam (`readTouchXY`/`isPhysicalButtonPressed`) is genuinely narrow; WifiScan/ArpScanner/WpsScanner/HiddenSsidReveal/KarmaAttack homing correct; keeping radios on the S3 to avoid P4 RF/RMT ports is the right trade.

### 17.6 Net effect on the plan

The **split‑brain topology, companion‑primary offense (DR‑3), and datasheet bus map survive**. The two claims that did **not** survive contact with the source are **“Stage‑A full‑frame mirror reuses 100 % of code”** (BR‑1: no PSRAM) and **“UART primary, SPI later”** (HR‑1). Re‑baseline: **Stage‑B native telemetry rendering is the primary parity path; SPI is the mirror/PCAP data‑plane baseline; deauth‑class + BLE offense stay on the companion.** Confirm companion PSRAM, the free link‑UART pin, and the DIV EN breakout on hardware before P3/P5.

---

*End of plan (v1 — Launcher/datasheet‑validated, adversarially reviewed). Execution gates: confirm companion PSRAM (BR‑1) and link transport (HR‑1) at P3/P5. Parity rides connectivity‑native (P4) + companion‑over‑link (P5‑P7); native screens (P8) and blobs (P9) are incremental.*


