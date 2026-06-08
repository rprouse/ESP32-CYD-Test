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
