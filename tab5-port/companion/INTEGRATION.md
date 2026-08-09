# Companion integration — wiring `DIV_COMPANION` into ESP32-DIV.ino

The companion is the **stock ESP32-DIV firmware** (on Josh's generic ESP32-S3,
N16R8) compiled with `-DDIV_COMPANION`, turning it headless and driving its
radios from the Tab5 host over `divlink`. These are the minimal edits; the goal
is the *smallest diff* that keeps a normal (non-companion) build byte-identical.

## 1. Build flag & files
- Add `tab5-port/companion/div_companion.{h,cpp}` and `tab5-port/divlink/*` to the
  DIV sketch (Arduino: copy into the sketch folder or a `src/` lib; PlatformIO:
  `lib_extra_dirs`).
- Define `DIV_COMPANION` (and optionally `DIVCOMP_RX`/`DIVCOMP_TX`, default 8/9).

## 2. Input globals (shared.h)
Add link-sourced input globals the companion writes and features read:
```cpp
volatile int  g_linkBtn = 0;      volatile bool g_linkBtnDown = false;
volatile int  g_linkTouchX = 0, g_linkTouchY = 0; volatile bool g_linkTouch = false;
```
Then make the existing input readers fall back to them under `DIV_COMPANION`:
- `isPhysicalButtonPressed()` → also return true when `g_linkBtnDown && g_linkBtn==<btn>`.
- `readTouchXY(x,y)` (Touchscreen.cpp) → when `DIV_COMPANION`, return `g_linkTouch`
  with `g_linkTouchX/Y` instead of sampling XPT2046.

## 3. Headless display (Stage-A mirror, PSRAM sprite)
The companion is N16R8, so a full-frame mirror is viable (plan §17.1 BR-1).
Retarget the global `tft` to an off-screen PSRAM sprite:
```cpp
#ifdef DIV_COMPANION
  #include <TFT_eSPI.h>
  static TFT_eSPI real_tft;                 // not attached to a panel
  static TFT_eSprite tft = TFT_eSprite(&real_tft);  // 240x320x16 in PSRAM
  // in setup(): tft.setColorDepth(16); tft.createSprite(240, 320);
#else
  TFT_eSPI tft;                             // unchanged for standalone builds
#endif
```
A low-priority task diffs dirty 16-row bands and calls `divcompSendCanvas()`
over the SPI data-plane (plan §17.2 HR-1). *(Stage B replaces per-feature draws
with `divcompSendEvt()` telemetry; both can coexist.)*

## 4. setup() / loop()
```cpp
void setup() {
  Serial.begin(115200);
  // ... existing radio/SD init (keep) ...
#ifdef DIV_COMPANION
  static const FeatureEntry FEATURES[] = {
    { Feature::NRF_SCANNER,   []{ Scanner::scannerSetup(); },   []{ Scanner::exit(); } },
    { Feature::SUBGHZ_REPLAY, []{ replayat::ReplayAttackSetup();}, []{ /*stop*/ } },
    { Feature::RFID,          []{ /* rfid setup */ },           []{ /* rfid exit */ } },
    // ... one row per DIV feature ...
  };
  divcompRegister(FEATURES, sizeof(FEATURES)/sizeof(FEATURES[0]));
  divcompSetup();
  return;                                   // skip local UI bring-up
#endif
  // ... existing standalone UI setup ...
}

void loop() {
#ifdef DIV_COMPANION
  divcompLoop();                            // service link
  // active feature's own loop() runs when started; it should periodically
  // check divcompFeatureActive() and push telemetry via divcompSendEvt().
  return;
#endif
  // ... existing standalone loop ...
}
```

## 5. Per-feature loop cooperation (plan §17.2 HR-2)
Replace blocking `while(isButtonPressed(...)) {}` spins in feature loops with
edge checks + `vTaskDelay(1)` so the link task is never starved. Features that
render should send structured telemetry (`divcompSendEvt`) for Stage-B, and/or
draw to the PSRAM sprite for Stage-A.

## 6. Verify
`DIV_COMPANION` **off** must still build a byte-identical standalone DIV
(regression guard, plan §14). With it **on**, the host's `link.linked()` goes
true and menu selections start/stop radios.
