# Touch Calibration Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace the per-board touch coordinate mapping with one unified 3-corner, swap-aware calibration that persists to NVS and re-runs on first boot or when a touch is held during boot.

**Architecture:** A dedicated `src/touch.{h,cpp}` module owns all touch logic. It works in raw touch space, so the only per-board difference is a single `read_raw()` function (`XPT2046_Touchscreen` polling on the dedicated-bus boards, `tft.getTouchRaw()` on the shared-bus Freenove). The shared `TFT_eSPI tft` instance (owned by `main.cpp`) draws the calibration screen on every board; LVGL keeps its own instance for the GUI.

**Tech Stack:** PlatformIO, Arduino-ESP32, TFT_eSPI, LVGL 9, XPT2046_Touchscreen, ESP32 `Preferences` (NVS).

**Spec:** `docs/superpowers/specs/2026-06-07-touch-calibration-design.md`

**Testing note:** No host test harness exists for this firmware and the behavior depends on touch hardware + display. Verification is therefore `pio run` (compile) per task plus explicit on-device checks. Do not invent a unit-test framework — it is out of scope.

---

## File Structure

- **Create `src/touch.h`** — public interface: `touch_init()`, `touch_read()`.
- **Create `src/touch.cpp`** — controller bring-up, per-board `read_raw()`, NVS load/save, 3-corner calibration screen, swap-aware map, hold-at-boot.
- **Modify `src/main.cpp`** — delete inline touch code (object construction, `touch_init`, `touch_get`, `touch_calibration[]`, the conditional XPT2046 include); add `#include "touch.h"`; call `touch_read()` in the LVGL callback. Keep `TFT_eSPI tft = TFT_eSPI();`.
- **Unchanged:** `src/Setup_*.h` (touch `#define`s already correct), `platformio.ini`.

---

## Task 1: Build the touch module and rewire main.cpp

**Files:**
- Create: `src/touch.h`
- Create: `src/touch.cpp`
- Modify: `src/main.cpp`

- [ ] **Step 1: Create `src/touch.h`**

```cpp
#pragma once
#include <stdint.h>

// Initialise touch. Call once in setup(), BEFORE lv_tft_espi_create().
// Brings up the shared TFT instance (to draw the calibration screen) and the
// touch controller. If a touch is held during boot, or no valid calibration is
// stored in NVS, runs the on-screen calibration and saves the result.
void touch_init(void);

// Poll the touch controller. Returns true if pressed and writes calibrated
// screen-pixel coordinates to *x and *y. Call once per LVGL read callback.
bool touch_read(int32_t *x, int32_t *y);
```

- [ ] **Step 2: Create `src/touch.cpp` with the full implementation**

```cpp
#include "touch.h"
#include <Arduino.h>
#include <Preferences.h>
#include <SPI.h>
#include <TFT_eSPI.h>
#if defined(TOUCH_XPT2046_LIB)
#include <XPT2046_Touchscreen.h>
#endif

extern TFT_eSPI tft;   // owned by main.cpp

// Pressure threshold for the shared-bus (TFT_eSPI) raw read. Matches the value
// TFT_eSPI uses internally for its own touch validation.
#define TOUCH_Z_THRESHOLD 350

// Inset of calibration targets from the screen edges, in pixels.
static const int16_t CAL_MARGIN = 16;

#if defined(TOUCH_XPT2046_LIB)
static XPT2046_Touchscreen ts(XPT2046_CS);   // CS only -> polling, no IRQ
#endif

// Stored calibration: the three raw anchor points. Swap and slopes are
// recomputed at load, so nothing redundant is persisted.
struct TouchCal {
    uint16_t xTL, yTL, xTR, yTR, xBL, yBL;
};
static TouchCal cal = {0, 0, 0, 0, 0, 0};
static bool     cal_loaded = false;

static const char *NVS_NS  = "cydtouch";
static const char *NVS_KEY = "tcal_v1";   // bump to invalidate stored data

// --- per-board raw read (the only #if in the module) ------------------------
static bool read_raw(uint16_t *rx, uint16_t *ry) {
#if defined(TOUCH_XPT2046_LIB)
    if (!ts.touched()) return false;
    TS_Point p = ts.getPoint();
    *rx = (uint16_t)p.x;
    *ry = (uint16_t)p.y;
    return true;
#elif defined(TOUCH_TFT_ESPI)
    if (tft.getTouchRawZ() <= TOUCH_Z_THRESHOLD) return false;  // getTouchRaw has no press check
    tft.getTouchRaw(rx, ry);
    return true;
#endif
}

// --- NVS --------------------------------------------------------------------
static bool load_cal(void) {
    Preferences p;
    if (!p.begin(NVS_NS, /*readOnly=*/true)) return false;
    size_t n = p.getBytesLength(NVS_KEY);
    if (n != sizeof(cal)) { p.end(); return false; }
    p.getBytes(NVS_KEY, &cal, sizeof(cal));
    p.end();
    return true;
}

static void save_cal(void) {
    Preferences p;
    p.begin(NVS_NS, /*readOnly=*/false);
    p.putBytes(NVS_KEY, &cal, sizeof(cal));
    p.end();
}

static bool spans_valid(void) {
    return (cal.xTR != cal.xTL) && (cal.yBL != cal.yTL) &&
           (cal.yTR != cal.yTL) && (cal.xBL != cal.xTL);
}

// --- mapping (signed spans handle inversion; swap flag handles axis swap) ----
static void map_raw_to_screen(uint16_t rx, uint16_t ry, int32_t *sx, int32_t *sy) {
    const int32_t W = tft.width();
    const int32_t H = tft.height();
    const int32_t spanX = W - 1 - 2 * CAL_MARGIN;
    const int32_t spanY = H - 1 - 2 * CAL_MARGIN;

    bool swap = abs((int)cal.yTR - (int)cal.yTL) > abs((int)cal.xTR - (int)cal.xTL);

    int32_t x, y;
    if (!swap) {
        x = CAL_MARGIN + ((int)rx - (int)cal.xTL) * spanX / ((int)cal.xTR - (int)cal.xTL);
        y = CAL_MARGIN + ((int)ry - (int)cal.yTL) * spanY / ((int)cal.yBL - (int)cal.yTL);
    } else {
        x = CAL_MARGIN + ((int)ry - (int)cal.yTL) * spanX / ((int)cal.yTR - (int)cal.yTL);
        y = CAL_MARGIN + ((int)rx - (int)cal.xTL) * spanY / ((int)cal.xBL - (int)cal.xTL);
    }
    if (x < 0) x = 0; else if (x >= W) x = W - 1;
    if (y < 0) y = 0; else if (y >= H) y = H - 1;
    *sx = x;
    *sy = y;
}

// --- calibration screen -----------------------------------------------------
static void draw_target(int16_t cx, int16_t cy) {
    tft.fillScreen(TFT_BLACK);
    tft.drawLine(cx - 8, cy, cx + 8, cy, TFT_WHITE);
    tft.drawLine(cx, cy - 8, cx, cy + 8, TFT_WHITE);
    tft.fillCircle(cx, cy, 3, TFT_RED);
}

static void wait_for_tap(uint16_t *rx, uint16_t *ry) {
    uint16_t tx, ty;
    while (read_raw(&tx, &ty)) delay(10);   // wait for any prior touch to release
    delay(150);
    while (!read_raw(&tx, &ty)) delay(10);  // wait for a new press
    uint32_t ax = 0, ay = 0;
    const int N = 16;
    int got = 0;
    while (got < N) {                        // average N samples for stability
        if (read_raw(&tx, &ty)) { ax += tx; ay += ty; got++; }
        delay(5);
    }
    *rx = (uint16_t)(ax / N);
    *ry = (uint16_t)(ay / N);
    while (read_raw(&tx, &ty)) delay(10);   // wait for release before returning
    delay(150);
}

static void run_calibration(void) {
    const int32_t W = tft.width();
    const int32_t H = tft.height();

    tft.fillScreen(TFT_BLACK);
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.setTextDatum(MC_DATUM);
    tft.drawString("Touch Calibration", W / 2, H / 2 - 10, 2);
    tft.drawString("Tap each target", W / 2, H / 2 + 10, 2);
    delay(1500);

    draw_target(CAL_MARGIN, CAL_MARGIN);
    wait_for_tap(&cal.xTL, &cal.yTL);

    draw_target(W - 1 - CAL_MARGIN, CAL_MARGIN);
    wait_for_tap(&cal.xTR, &cal.yTR);

    draw_target(CAL_MARGIN, H - 1 - CAL_MARGIN);
    wait_for_tap(&cal.xBL, &cal.yBL);

    save_cal();
    cal_loaded = true;

    tft.fillScreen(TFT_BLACK);
    tft.drawString("Calibrated", W / 2, H / 2, 2);
    delay(700);
    tft.fillScreen(TFT_BLACK);
}

// --- hold-at-boot recalibration trigger -------------------------------------
static bool held_at_boot(void) {
    uint16_t tx, ty;
    int pressed = 0;
    const int N = 30;
    for (int i = 0; i < N; i++) {            // ~300 ms window
        if (read_raw(&tx, &ty)) pressed++;
        delay(10);
    }
    return pressed >= 25;                     // solid majority => intentional hold
}

// --- public API -------------------------------------------------------------
void touch_init(void) {
    // The shared TFT instance draws the calibration screen on every board and
    // defines the W/H the map targets. LVGL uses its own instance for the GUI.
    tft.init();
    tft.setRotation(3);   // landscape, matches LV_DISPLAY_ROTATION_270 in main.cpp

#if defined(TOUCH_XPT2046_LIB)
    // Dedicated touch SPI bus, separate from the display.
    SPI.begin(XPT2046_CLK, XPT2046_MISO, XPT2046_MOSI, XPT2046_CS);
    ts.begin();
    ts.setRotation(0);    // capture raw space; orientation handled by the map
#endif

    if (held_at_boot()) {
        run_calibration();
        return;
    }
    if (load_cal() && spans_valid()) {
        cal_loaded = true;
        return;
    }
    run_calibration();
}

bool touch_read(int32_t *x, int32_t *y) {
    if (!cal_loaded) return false;
    uint16_t rx, ry;
    if (!read_raw(&rx, &ry)) return false;
    map_raw_to_screen(rx, ry, x, y);
    return true;
}
```

- [ ] **Step 3: In `src/main.cpp`, replace the include/object block**

Replace this current block:

```cpp
// include the installed the "XPT2046_Touchscreen" library by Paul Stoffregen to use the Touchscreen - https://github.com/PaulStoffregen/XPT2046_Touchscreen
// Only needed on boards whose touch controller is on a dedicated SPI bus.
#if defined(TOUCH_XPT2046_LIB)
#include <XPT2046_Touchscreen.h>
#endif


// Create a instance of the TFT_eSPI class.
// On boards where touch shares the display SPI bus (TOUCH_TFT_ESPI) this same
// instance also reads the touch controller via tft.getTouch().
TFT_eSPI tft = TFT_eSPI();

// Touch pins / driver are selected per board in the Setup_*.h header.
#if defined(TOUCH_XPT2046_LIB)
// Touch on its own dedicated SPI bus -> standalone XPT2046_Touchscreen library.
XPT2046_Touchscreen touchscreen(XPT2046_CS, XPT2046_IRQ);
#elif defined(TOUCH_TFT_ESPI)
// Touch shares the display SPI bus -> calibration data for tft.getTouch().
// TODO: replace with values printed by TFT_eSPI's "Touch_calibrate" example.
static uint16_t touch_calibration[5] = { 300, 3600, 300, 3600, 7 };
#else
#error "No touch driver selected - define TOUCH_XPT2046_LIB or TOUCH_TFT_ESPI in your Setup_*.h"
#endif
```

with:

```cpp
// Touch handling lives in its own module (touch.h / touch.cpp). The touch
// driver and pins are selected per board in the Setup_*.h headers.
#include "touch.h"

// Create an instance of the TFT_eSPI class. The touch module references this
// same instance (extern) to draw the calibration screen.
TFT_eSPI tft = TFT_eSPI();
```

- [ ] **Step 4: In `src/main.cpp`, replace the touch helper functions**

Replace the current `touch_init()`, `touch_get()`, and `touchscreen_read()` block:

```cpp
// Initialise the touch controller. The body is selected per board; the rest of
// this sketch is identical across all targets.
static void touch_init() {
#if defined(TOUCH_XPT2046_LIB)
  // Touch on its own dedicated SPI bus.
  SPI.begin(XPT2046_CLK, XPT2046_MISO, XPT2046_MOSI, XPT2046_CS);
  touchscreen.begin();
  touchscreen.setRotation(2);
#elif defined(TOUCH_TFT_ESPI)
  // Touch shares the display SPI bus; bring up our TFT_eSPI instance (LVGL uses
  // its own) and apply the calibration that tft.getTouch() maps with.
  // Done before lv_tft_espi_create() so LVGL's begin() ends up owning the display.
  tft.init();
  tft.setRotation(3);                 // match the landscape display orientation
  tft.setTouch(touch_calibration);
#endif
}

// Read the touch controller and return mapped screen coordinates.
// Returns true if the screen is currently pressed. Per-board body only.
static bool touch_get(int32_t *tx, int32_t *ty) {
#if defined(TOUCH_XPT2046_LIB)
  if (touchscreen.tirqTouched() && touchscreen.touched()) {
    TS_Point p = touchscreen.getPoint();
    // Calibrate raw points with map() to the correct width and height
    *tx = map(p.x, 200, 3700, 1, TFT_WIDTH);
    *ty = map(p.y, 240, 3800, 1, TFT_HEIGHT);
    return true;
  }
  return false;
#elif defined(TOUCH_TFT_ESPI)
  uint16_t rawx, rawy;
  if (tft.getTouch(&rawx, &rawy)) {   // shares the display SPI bus, uses TOUCH_CS
    *tx = rawx;
    *ty = rawy;
    return true;
  }
  return false;
#endif
}

// Get the Touchscreen data
void touchscreen_read(lv_indev_t * indev, lv_indev_data_t * data) {
  // Checks if Touchscreen was touched, and prints X, Y
  int32_t tx, ty;
  if (touch_get(&tx, &ty)) {
    x = tx;
    y = ty;

    Serial.printf("touch: (%d,%d)\n", (int)x, (int)y);

    data->state = LV_INDEV_STATE_PRESSED;

    // Set the coordinates
    data->point.x = x;
    data->point.y = y;
  }
  else {
    data->state = LV_INDEV_STATE_RELEASED;
  }
}
```

with (the per-board helpers are gone — they now live in `touch.cpp`):

```cpp
// Get the Touchscreen data (calibration + per-board read live in touch.cpp)
void touchscreen_read(lv_indev_t * indev, lv_indev_data_t * data) {
  int32_t tx, ty;
  if (touch_read(&tx, &ty)) {
    x = tx;
    y = ty;

    Serial.printf("touch: (%d,%d)\n", (int)x, (int)y);

    data->state = LV_INDEV_STATE_PRESSED;
    data->point.x = x;
    data->point.y = y;
  }
  else {
    data->state = LV_INDEV_STATE_RELEASED;
  }
}
```

- [ ] **Step 5: Confirm `setup()` already calls `touch_init()` before `lv_tft_espi_create()`**

The current `setup()` contains this (leave it as-is — it now calls the module's `touch_init()`):

```cpp
  // Start LVGL
  lv_init();

  // Initialise the touch controller (implementation is per board).
  // For boards that share the display SPI bus this also brings up the TFT
  // instance, so it must run before lv_tft_espi_create() below.
  touch_init();

  // Create a display object
```

No edit needed if it already matches. If `touch_init();` is missing or the inline SPI/`touchscreen.begin()` lines are still present here, replace them with the single `touch_init();` call.

- [ ] **Step 6: Build all three environments**

Run: `pio run -e freenove_esp32_lcd -e ESP32_2432S028R_ILI9341 -e ESP32_2432S028R_ST7789`
Expected: three `========= [SUCCESS] ...` lines, no errors. (First Freenove build will fetch no new libs; `Preferences` ships with the ESP32 core.)

- [ ] **Step 7: Commit**

```bash
git add src/touch.h src/touch.cpp src/main.cpp
git commit -m "Add unified touch module with 3-corner calibration and NVS persistence"
```

---

## Task 2: On-device first-boot calibration (Freenove)

**Files:** none (verification + any fixes only)

- [ ] **Step 1: Erase NVS so calibration is guaranteed to run**

Run: `pio run -e freenove_esp32_lcd -t erase`
Expected: `Erasing flash... Done`. (This wipes stored calibration so the next boot starts fresh.)

- [ ] **Step 2: Flash and open the serial monitor**

Run: `pio run -e freenove_esp32_lcd -t upload && pio device monitor -b 115200`
Expected: device reboots and shows the "Touch Calibration / Tap each target" screen, then three crosshair targets in turn (top-left, top-right, bottom-left).

- [ ] **Step 3: Tap the three targets**

Tap each crosshair center with a stylus. Expected: after the third tap the screen shows "Calibrated" briefly, then the normal GUI appears.

- [ ] **Step 4: Verify touch accuracy**

Touch several known GUI elements (the toggle button, the switch, the slider). Expected: the serial monitor prints `touch: (x,y)` with coordinates that match where you pressed, and the GUI controls respond at the points you touch (not offset, not inverted).

- [ ] **Step 5: Verify persistence across reboot**

Press the board's reset button (do NOT hold the screen). Expected: the GUI comes straight up with NO calibration screen, and touch is still accurate (calibration loaded from NVS).

- [ ] **Step 6: If accuracy is wrong, record the symptom (no code change yet)**

If touches are consistently offset/scaled, that is normal pre-calibration drift and Step 3 should have fixed it — re-run from Step 1. If touches are rotated 90° or mirrored relative to the GUI (tapping left registers as up, etc.), that is an orientation mismatch between the map's output space and LVGL's input space — note it and proceed to Task 3 Step 4, which addresses it. No commit if no code changed.

---

## Task 3: Cross-board verification, hold-at-boot, and orientation alignment

**Files:** `src/touch.cpp` and/or `src/main.cpp` only if an orientation fix is needed.

- [ ] **Step 1: Verify hold-at-boot recalibration (Freenove)**

Hold a finger/stylus firmly on the screen and press reset, keeping contact for ~1 second. Expected: the calibration screen runs again (held touch overrides stored calibration). Release and complete the three taps; confirm the new calibration takes effect.

- [ ] **Step 2: Verify the ILI9341 board did not regress**

Run: `pio run -e ESP32_2432S028R_ILI9341 -t erase && pio run -e ESP32_2432S028R_ILI9341 -t upload && pio device monitor -b 115200`
Expected: calibration screen on first boot; after tapping the three targets, touch is accurate and the GUI responds correctly; reboot loads calibration silently.

- [ ] **Step 3: Verify the ST7789 board did not regress**

Run: `pio run -e ESP32_2432S028R_ST7789 -t erase && pio run -e ESP32_2432S028R_ST7789 -t upload && pio device monitor -b 115200`
Expected: same as Step 2 — first-boot calibration, accurate touch afterward, silent load on reboot.

- [ ] **Step 4: Fix orientation only if a board showed rotation/mirror in Task 2 Step 6 or Steps 2-3**

The 3-corner calibration already corrects raw axis swap and inversion, so a remaining mismatch is purely between the map's output box and LVGL's expected input box. The single lever is the rotation pairing between the touch map and the LVGL display. If a board is rotated/mirrored:

For that board, try the alternate display rotation in `src/main.cpp` `setup()` — change:

```cpp
  lv_display_set_rotation(disp, LV_DISPLAY_ROTATION_270);
```

to `LV_DISPLAY_ROTATION_90`, and correspondingly change `tft.setRotation(3);` to `tft.setRotation(1);` in `touch_init()` (`src/touch.cpp`). Re-erase, reflash, recalibrate, and re-verify. Keep the pairing consistent (270↔rotation 3, or 90↔rotation 1) so the calibration draw orientation matches the GUI.

- [ ] **Step 5: Commit any fix**

Only if Step 4 changed code:

```bash
git add src/touch.cpp src/main.cpp
git commit -m "Align touch/display rotation for <board>"
```

- [ ] **Step 6: Final full build**

Run: `pio run -e freenove_esp32_lcd -e ESP32_2432S028R_ILI9341 -e ESP32_2432S028R_ST7789`
Expected: three `[SUCCESS]` lines.

---

## Self-Review

**Spec coverage:**
- Unified across all three boards → Task 1 (shared module, per-board `read_raw` only); verified Tasks 2-3.
- 3-corner swap-aware map → Task 1 `map_raw_to_screen` + `run_calibration`.
- Hold-at-boot trigger → Task 1 `held_at_boot`; verified Task 3 Step 1.
- NVS persistence + version key + span validation → Task 1 `load_cal`/`save_cal`/`spans_valid`; verified Task 2 Step 5.
- `main.cpp` cleanup → Task 1 Steps 3-5.
- Edge cases (cal_loaded guard, draw-before-LVGL, zero-span re-cal) → Task 1 code.
- Verification checklist → Tasks 2-3.

**Placeholder scan:** No TBD/TODO; the old `touch_calibration` TODO array is deleted in Task 1 Step 3. All code blocks complete.

**Type consistency:** `touch_init(void)` / `touch_read(int32_t*, int32_t*)` match between `touch.h`, `touch.cpp`, and the `main.cpp` call sites. `TouchCal` field names (`xTL,yTL,xTR,yTR,xBL,yBL`) are consistent across `run_calibration`, `map_raw_to_screen`, `spans_valid`, and NVS I/O. `read_raw(uint16_t*, uint16_t*)` signature consistent at all call sites.
