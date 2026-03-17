# ESP32 Cheap Yellow Display Test

PlatformIO project for testing LVGL on an ESP32-based 2.8" 240x320 touch display (CYD-style board) using TFT_eSPI and XPT2046.

This code is originally from [Mastering ESP32-2432S028R with LVGL: The Ultimate Beginner’s Guide in Platform.io](https://kafkar.com/projects/smart-home/mastering-esp32-2432s028r-with-lvgl-the-ultimate-beginners-guide-in-platform-io/). I followed his tutorial with updates for a newer LVLG library and this is the result.

## Features

- LVGL v9 UI demo in `src/main.cpp`
- TFT driven through `TFT_eSPI`
- Touch input via `XPT2046_Touchscreen`
- Custom board/display setup headers in `src/`

## Project Layout

- `platformio.ini`: PlatformIO environment and build flags
- `src/main.cpp`: App entry point and LVGL UI logic
- `src/lv_conf.h`: LVGL compile-time configuration
- `src/Setup_ESP32_2432S028R_ST7789.h`: Version 3 CYD boards
- `src/Setup_ESP32_2432S028R_ILI9341.h`: Version 1 and 2 boards

## Requirements

- VS Code with PlatformIO extension, or PlatformIO Core installed
- ESP32 board connected over USB

## Build Configuration

The active environment in `platformio.ini` is:

- `env:nodemcu-32s`

Libraries are pulled automatically:

- `bodmer/TFT_eSPI`
- `PaulStoffregen/XPT2046_Touchscreen`
- `lvgl/lvgl`

Important build flags already set:

- `-D USER_SETUP_LOADED=1`
- `-D LV_CONF_INCLUDE_SIMPLE=1`
- `-include src/Setup_ESP32_2432S028R_ST7789.h`
- `-include src/lv_conf.h`

## Build

```powershell
pio run
```

## Upload

```powershell
pio run --target upload
```

## Serial Monitor

```powershell
pio device monitor --baud 115200
```

## Notes

- If your display controller differs, swap the `-include` setup header in `platformio.ini`.
- If touch coordinates are inverted, adjust calibration or rotation in `src/main.cpp` (`touchscreen.setRotation(...)` and `map(...)` ranges).
