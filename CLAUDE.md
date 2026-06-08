# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

PlatformIO firmware for "Cheap Yellow Display" (CYD) style ESP32 touch boards, running an LVGL v9 UI demo over TFT_eSPI with an XPT2046 resistive touch controller.

## Commands

`pio` is not on the bash PATH on this machine — use PowerShell, or the full path `~/.platformio/penv/Scripts/pio.exe`.

```powershell
# Build one environment (see the three env names below)
pio run -e freenove_esp32_lcd

# Build all targets (do this after touching shared code like src/main.cpp or src/touch.cpp)
pio run -e freenove_esp32_lcd -e ESP32_2432S028R_ILI9341 -e ESP32_2432S028R_ST7789

# Upload / serial monitor
pio run -e <env> -t upload
pio device monitor -b 115200

# Erase flash — also wipes stored touch calibration (NVS), forcing recalibration on next boot
pio run -e <env> -t erase
```

- **There is no automated test suite.** Verification = the build succeeding plus on-device behavior. "Done" claims for touch/display work require flashing real hardware.
- If `upload`/`erase` fails with `Access is denied` / port busy, a serial monitor is holding the COM port — close it first.

## Multi-board architecture

Three PlatformIO environments target different physical boards, selected entirely by a `-include src/Setup_*.h` build flag (one per env in `platformio.ini`):

| Env | Board | Display | Touch bus |
|-----|-------|---------|-----------|
| `ESP32_2432S028R_ILI9341` (default) | CYD v1/v2 | ILI9341 240×320 | XPT2046 on a **dedicated** SPI bus |
| `ESP32_2432S028R_ST7789` | CYD v3 | ST7789 240×320 | XPT2046 on a **dedicated** SPI bus |
| `freenove_esp32_lcd` | Freenove 4" | ST7796 320×480 | XPT2046 **shares the display SPI bus** |

Each `Setup_*.h` is a TFT_eSPI User_Setup (driver, pins, SPI frequency) **plus** a touch-driver selector macro that `src/touch.cpp` keys off:

- `TOUCH_XPT2046_LIB` + `XPT2046_{CLK,MISO,MOSI,CS,IRQ}` — touch is on its own SPI bus, driven by the standalone `XPT2046_Touchscreen` library.
- `TOUCH_TFT_ESPI` — touch shares the display bus, read through TFT_eSPI's built-in touch (`TOUCH_CS` defines the chip select).

This per-board split exists because the boards differ in **hardware topology**, not preference — there is no single touch driver that fits both bus layouts. Changing touch pins or the driver for a board means editing its `Setup_*.h`, not `main.cpp`.

## Touch module (`src/touch.{h,cpp}`)

All touch logic is isolated here so the rest of the sketch is identical across boards. `main.cpp` only calls `touch_init()` (in `setup()`, before `lv_tft_espi_create()`) and `touch_read()` (in the LVGL read callback).

- **Works in raw touch space.** The only per-board `#if` is `read_raw()` (XPT2046 polling vs `tft.getTouchRaw`); calibration, NVS, and mapping are shared.
- **3-corner calibration** (top-left, top-right, bottom-left) detects axis swap + inversion and builds a linear map. Stored in NVS via `Preferences` (namespace `cydtouch`, key `tcal_v1` — bump the key to invalidate old data). Runs automatically on first boot / invalid data, or when a touch is **held during boot** (`held_at_boot()`).
- **Calibration draws through `main.cpp`'s `tft` instance.** That instance is initialized in `touch_init()` on every board; LVGL keeps a *separate* `TFT_eSPI` instance (see below). Both share the HSPI bus.

### Critical gotcha: LVGL native-frame input transform

LVGL rotates every input point itself (`lv_indev.c` → `lv_display_rotate_point`) and expects coordinates in the display's **native (unrotated) frame**, not the displayed-landscape frame. `main.cpp` uses `LV_DISPLAY_ROTATION_270`, so `touch_read()` converts the calibrated landscape coords to the native frame:

```cpp
*x = (TFT_WIDTH - 1) - sy;   // native portrait x
*y = sx;                      // native portrait y
```

**If you change the display rotation in `main.cpp`, this transform must change to match the new `lv_display_rotate_point` mapping**, or touches will land on the wrong widgets (they were already being double-rotated before this fix).

## Display / LVGL wiring

`lv_tft_espi_create()` (LVGL's bundled TFT_eSPI driver) **constructs and `begin()`s its own private `TFT_eSPI` instance** for the display. The `TFT_eSPI tft` in `main.cpp` is a second instance used only by the touch module (calibration drawing + `getTouchRaw`). They coexist on the same HSPI bus; `touch_init()` runs first so LVGL's `begin()` ends up owning the final display state. LVGL config lives in `src/lv_conf.h` (`-include`d; RGB565, 64 KB LVGL heap).

## Reference docs

Design rationale and the full implementation history of the touch system are in `docs/superpowers/specs/2026-06-07-touch-calibration-design.md` and `docs/superpowers/plans/2026-06-07-touch-calibration.md`. Read these before reworking touch — they record why the bus topology, calibration, and rotation transform are the way they are.
