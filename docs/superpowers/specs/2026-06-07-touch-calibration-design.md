# Touch Calibration — Design Spec

**Date:** 2026-06-07
**Status:** Approved design, pending implementation plan

## Problem

Touch coordinates are mapped to screen pixels differently per board and none of
it is calibrated per unit:

- The two `ESP32_2432S028R_*` boards use a hardcoded `map(p.x, 200, 3700, …)` in
  `main.cpp`.
- The `freenove_esp32_lcd` board uses a placeholder `tft.setTouch()` calibration
  array, so its touches land in the wrong place.

We want a single, unified startup-calibration system: on first boot (or on
demand) the device shows a calibration screen, the result is saved to flash, and
every subsequent boot loads it. The same flow must work on all three boards
despite their different touch hardware topologies.

## Requirements (locked)

1. **Scope — all three boards (unified).** Replace both the hardcoded `map()`
   and the `setTouch()` placeholder with one shared calibration + persistence
   flow that operates in raw touch space.
2. **Calibration math — 3-corner, swap- and inversion-aware.** Must correct for
   the Freenove's likely landscape axis swap, which a 2-point map cannot.
3. **Recalibration trigger — hold touch at boot.** Calibration auto-runs when
   stored data is absent or the format version changed; holding a touch during
   boot forces a re-run. No serial command.
4. **Storage — ESP32 NVS via `Preferences`.** Survives normal reflashes.

## Architecture (Approach A: dedicated touch module)

New `src/touch.h` / `src/touch.cpp` own everything touch-related. `main.cpp`
keeps only the `TFT_eSPI tft` instance (now referenced `extern` by the module)
and two calls: `touch_init()` in `setup()` and `touch_read()` in the LVGL read
callback. All five touch `#define`s remain in the per-board `Setup_*.h` headers.

### Public interface — `touch.h`

```cpp
#pragma once
#include <stdint.h>

// Initialise touch. Must be called AFTER tft.init()/setRotation() in setup().
// Detects a held touch at boot -> forces calibration. Otherwise loads cal from
// NVS; if absent or the format version changed, runs the calibration screen.
void touch_init(void);

// Returns true if currently pressed; writes calibrated screen pixels to *x,*y.
// Called once per LVGL read callback.
bool touch_read(int32_t *x, int32_t *y);
```

### Per-board raw read — the only `#if` in the module

`touch.cpp` owns the `XPT2046_Touchscreen ts(XPT2046_CS)` object (CS only, no
IRQ — polling) and references `extern TFT_eSPI tft`. A single private function
is the entire per-board surface; everything else sees only raw 0–4095 values.

```cpp
// Returns true if pressed; writes RAW controller coords (pre-calibration).
static bool read_raw(uint16_t *rx, uint16_t *ry) {
#if defined(TOUCH_XPT2046_LIB)
  if (!ts.touched()) return false;          // polling, no IRQ
  TS_Point p = ts.getPoint();
  *rx = p.x; *ry = p.y;
  return true;
#elif defined(TOUCH_TFT_ESPI)
  if (tft.getTouchRawZ() <= TOUCH_Z_THRESHOLD) return false;  // getTouchRaw has no press check
  tft.getTouchRaw(rx, ry);
  return true;
#endif
}
```

Rationale: `getTouchRaw()` always returns true (no pressure check), so the
Freenove press test must come from `getTouchRawZ()`. The XPT2046 path drops the
IRQ and polls `ts.touched()`, consistent with the earlier polling decision.

`TOUCH_Z_THRESHOLD` is a new module constant in `touch.cpp` (default ~350, the
same value TFT_eSPI uses internally for its own touch validation).

## Calibration flow

`run_calibration()` is board-agnostic. It draws crosshair targets at three
corners inset by a fixed margin `M` (≈ 16 px) and captures a clean tap at each
(release → press → average ~16 raw samples → release):

- **TL** at `(M, M)` → raw `(xTL, yTL)`
- **TR** at `(W-1-M, M)` → raw `(xTR, yTR)`
- **BL** at `(M, H-1-M)` → raw `(xBL, yBL)`

`W`/`H` come from `tft.width()/height()` *after* rotation is set, so targets and
clamps always match the active orientation on every board.

### Swap detection

Moving TL→TR changes screen X only; TL→BL changes screen Y only. Compare which
raw axis responds to motion along screen X:

```cpp
bool swap = abs(yTR - yTL) > abs(xTR - xTL);  // raw-Y tracks screen-X ⇒ axes swapped
```

### Mapping (signed spans handle inversion for free)

```cpp
// no swap: raw-X→screen-X (anchors TL..TR), raw-Y→screen-Y (anchors TL..BL)
sx = M + (int32_t)(rx - xTL) * (W-1 - 2*M) / (xTR - xTL);
sy = M + (int32_t)(ry - yTL) * (H-1 - 2*M) / (yBL - yTL);
// swap: raw-Y→screen-X, raw-X→screen-Y
sx = M + (int32_t)(ry - yTL) * (W-1 - 2*M) / (yTR - yTL);
sy = M + (int32_t)(rx - xTL) * (H-1 - 2*M) / (xBL - xTL);
// clamp sx∈[0,W-1], sy∈[0,H-1]
```

## Storage

ESP32 NVS via `Preferences`:

- Namespace `"cydtouch"`, key `"tcal_v1"`.
- Payload: the three raw anchor points only — swap and slopes are recomputed at
  load, so nothing redundant is persisted.

```cpp
struct TouchCal { uint16_t xTL, yTL, xTR, yTR, xBL, yBL; };
```

- `load_cal()`: open read-only; return false if the key is missing **or** its
  byte length ≠ `sizeof(TouchCal)` (covers first boot and a format-version bump —
  later bumping the key to `tcal_v2` auto-invalidates old data).
- `save_cal()`: open read-write, `putBytes`, close.
- After load, validate every span (`xTR-xTL`, `yBL-yTL`, `yTR-yTL`, `xBL-xTL`)
  is non-zero. If any is zero, treat the stored cal as invalid → re-run.

Because the stored value is the raw anchor points (not derived slopes), the blob
stays valid across future changes to the mapping math.

## LVGL native-frame input transform

LVGL rotates every input point itself (`lv_indev.c` → `lv_display_rotate_point`)
and expects coordinates in the display's **native (unrotated) frame**, then maps
them onto the rotated landscape widgets. `main.cpp` uses
`LV_DISPLAY_ROTATION_270`. So after `map_raw_to_screen()` yields displayed
landscape coords `(sx, sy)`, `touch_read()` converts them to the native frame —
the inverse of the `ROTATION_270` mapping:

```cpp
*x = (TFT_WIDTH - 1) - sy;   // native portrait x
*y = sx;                      // native portrait y
```

Without this, LVGL rotates our already-landscape point a second time and taps
land on the wrong widget. This matches how the original per-board code fed
native-portrait coordinates via `map(p.x, …, TFT_WIDTH)`. If the display
rotation in `main.cpp` changes, this transform must be updated to match.

## `touch_init()` decision flow

The shared `tft` instance is initialized on **every** board (not just the
Freenove) because the calibration screen draws through it. On the XPT2046 boards
that means we now bring up our `tft` instance for drawing *in addition* to the
dedicated touch SPI bus + `ts` object; LVGL still uses its own instance for the
GUI. `tft` is set to the landscape rotation (3) that matches the GUI's
`LV_DISPLAY_ROTATION_270`, and `touch_init()` runs before `lv_tft_espi_create()`
so the calibration screen renders on a known orientation.

```
tft.init(); tft.setRotation(3)                 // ALL boards (draw cal screen, defines map W/H)
#if XPT2046: SPI.begin(touch bus); ts.begin(); ts.setRotation(0)
if held_at_boot():                 // forces re-cal on demand
    run_calibration(); return
if load_cal() && spans_valid():
    cal_loaded = true; return      // normal path
run_calibration()                  // first boot / invalid / version bump
```

`held_at_boot()` reuses `read_raw()`: poll for ~300 ms and require the screen to
read pressed on a solid majority of samples (e.g. ≥ 25 of 30, ~10 ms apart). The
debounce prevents a single noisy sample from forcing recalibration; the short
window keeps normal boots fast.

## Error handling & edge cases

- **Divide-by-zero:** impossible to reach `touch_read()` — zero spans are caught
  at load and force recalibration.
- **`touch_read()` before init:** guarded by a `cal_loaded` flag; returns "not
  pressed" until calibration exists.
- **Freenove display ordering:** `touch_init()` runs after `tft.init()/
  setRotation()` but before `lv_tft_espi_create()`, so the calibration screen
  draws on a known orientation and LVGL takes over the display afterward.
- **Margin sanity:** `M = 16` is small and fixed, so the usable span
  `(W-1-2M)` stays large even on the 240-px-wide boards.

## `main.cpp` changes

- Remove the current `touch_init()`, `touch_get()`, the `touch_calibration[5]`
  array, and the conditional `XPT2046_Touchscreen` include/construction (these
  move into `touch.cpp`).
- Keep `TFT_eSPI tft = TFT_eSPI();` (referenced `extern` by `touch.cpp`).
- `#include "touch.h"`; call `touch_init()` in `setup()` (same spot as today);
  LVGL read callback calls `touch_read(&x, &y)`.

## Out of scope

- Serial-command recalibration (explicitly declined).
- Changing the display/LVGL bring-up beyond the existing ordering.
- Any change to the `Setup_*.h` touch pin definitions (already correct).

## Verification

1. Build all three environments.
2. Freenove: first boot shows calibration; taps land accurately after; reboot
   loads silently; holding a touch at boot re-runs calibration.
3. Both 2432S028R boards: same behavior; confirm the move from hardcoded map +
   IRQ to calibrated polling did not regress touch.
4. Confirm calibration persists across a reflash (NVS retained).
