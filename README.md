# ESP32 Cheap Yellow Display Test

PlatformIO project for testing LVGL on ESP32-based "Cheap Yellow Display" (CYD) style touch boards, using `TFT_eSPI` for the display and an XPT2046 resistive touch controller.

The original 2.8" demo is from [Mastering ESP32-2432S028R with LVGL: The Ultimate Beginner's Guide in Platform.io](https://kafkar.com/projects/smart-home/mastering-esp32-2432s028r-with-lvgl-the-ultimate-beginners-guide-in-platform-io/). I followed the tutorial, updated it for a newer LVGL release, and extended it to support multiple boards with a unified, persistent touch-calibration system.

## Features

- LVGL v9 UI demo in `src/main.cpp`
- Display driven through `TFT_eSPI`
- Touch handled by a dedicated module (`src/touch.{h,cpp}`) with **on-screen 3-corner calibration saved to flash (NVS)**
- Support for three boards selected at build time via per-board setup headers

## Supported boards

| PlatformIO environment | Board | Display | Touch |
|------------------------|-------|---------|-------|
| `ESP32_2432S028R_ILI9341` *(default)* | CYD v1 / v2 | ILI9341 240×320 | XPT2046 on a dedicated SPI bus |
| `ESP32_2432S028R_ST7789` | CYD v3 | ST7789 240×320 | XPT2046 on a dedicated SPI bus |
| `freenove_esp32_lcd` | Freenove 4" | ST7796 320×480 | XPT2046 sharing the display SPI bus |

The board is chosen by an `-include src/Setup_*.h` build flag in `platformio.ini`. Each setup header configures the `TFT_eSPI` driver/pins **and** selects the touch driver for that board's wiring.

## Project layout

- `platformio.ini` — environments, build flags, and library dependencies
- `src/main.cpp` — app entry point and LVGL UI
- `src/touch.{h,cpp}` — unified touch driver: per-board raw read, calibration, NVS persistence
- `src/lv_conf.h` — LVGL compile-time configuration
- `src/Setup_ESP32_2432S028R_ILI9341.h` — CYD v1/v2
- `src/Setup_ESP32_2432S028R_ST7789.h` — CYD v3
- `src/Setup_ESP32_Freenove_ST7796.h` — Freenove 4"
- `docs/superpowers/` — design spec and implementation plan for the touch system

## Requirements

- VS Code with the PlatformIO extension, or PlatformIO Core installed
- An ESP32 CYD board connected over USB

Libraries are pulled automatically: `bodmer/TFT_eSPI`, `paulstoffregen/XPT2046_Touchscreen`, `lvgl/lvgl`.

## Build, upload, monitor

Pick the environment that matches your board (`-e <env>`).

```powershell
# Build
pio run -e ESP32_2432S028R_ILI9341

# Upload
pio run -e ESP32_2432S028R_ILI9341 -t upload

# Serial monitor
pio device monitor -b 115200
```

## Touch calibration

On first boot (or after erasing flash) the firmware shows a calibration screen and asks you to tap three crosshair targets. The result is saved to NVS and loaded automatically on every subsequent boot.

- **Recalibrate:** hold a finger on the screen while the board boots (e.g. press reset while touching) to force the calibration screen again.
- **Reset stored calibration:** `pio run -e <env> -t erase` wipes NVS, so the next boot recalibrates.

## Notes

- To add or change a board, edit the appropriate `src/Setup_*.h` (display driver, pins, and touch-driver selector) and reference it from a `platformio.ini` environment — `src/main.cpp` stays unchanged.
- The display uses `LV_DISPLAY_ROTATION_270`. If you change the rotation, the touch coordinate transform in `touch_read()` (`src/touch.cpp`) must be updated to match. See `CLAUDE.md` for details.
