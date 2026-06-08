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
