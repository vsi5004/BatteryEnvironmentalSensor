#include "display.h"
#include "debug.h"
#include "config.h"
#include <GxEPD2_BW.h>
#include <gdey/GxEPD2_370_GDEY037T03.h>
#include <Fonts/FreeSans9pt7b.h>
#include <U8g2_for_Adafruit_GFX.h>

// =============================================================================
// Display object — pins from config.h, constructed before setup()
// =============================================================================
RTC_DATA_ATTR static uint16_t _partialCount = 0;
// Save render inputs (about 640 bytes), not the 12.5KB framebuffer. Rebuild
// the panel's previous image after reset/hibernate for differential refresh.
RTC_DATA_ATTR static DisplaySnapshot _previous;
RTC_DATA_ATTR static bool _previousValid = false;
static bool _displayInitialized = false;
static_assert(DISPLAY_FULL_REFRESH_EVERY > 0 && DISPLAY_FULL_REFRESH_EVERY <= 65535,
              "Invalid full-refresh interval");

static GxEPD2_BW<GxEPD2_370_GDEY037T03, GxEPD2_370_GDEY037T03::HEIGHT> _display(
    GxEPD2_370_GDEY037T03(EPD_CS, EPD_DC, EPD_RES, EPD_BUSY)
);
static U8G2_FOR_ADAFRUIT_GFX _u8g2;

// =============================================================================
// Layout constants
// =============================================================================
static const int16_t DISP_W  = 240;

// Zone top edges
static const int16_t Z1_TOP  = 0;    // temp + humidity   (~148px)
static const int16_t Z2_TOP  = 148;  // CO2               (~124px)
static const int16_t Z3_TOP  = 272;  // sparkline strip   (~108px)
static const int16_t Z4_TOP  = 380;  // status bar        (~36px)

// Zone 1: column split
static const int16_t COL_MID = 120;

// Zone 3: row centres and inter-row dividers (3 rows × 36px)
static const int16_t T_CY  = 290;   // temperature row  (Z3_TOP + 18)
static const int16_t H_CY  = 326;   // humidity row     (Z3_TOP + 54)
static const int16_t C_CY  = 362;   // CO2 row          (Z3_TOP + 90)
static const int16_t Z3_D1 = 308;   // divider between T and H rows
static const int16_t Z3_D2 = 344;   // divider between H and C rows

// Sparkline geometry: x span, and ±half-height from row centre
static const int16_t SPK_X0   = 20;
static const int16_t SPK_X1   = 234;
static const int16_t SPK_HALF = 14;  // ±px from row centre
static const int16_t SPK_PAD  = 3;   // inner margin within that rect

// =============================================================================
// Drawing utilities
// =============================================================================

// Map a value into a y pixel within [yTop, yBot].
static int16_t valueToY(float val, float mn, float mx,
                         int16_t yTop, int16_t yBot) {
    if (mx <= mn) return (yTop + yBot) / 2;
    float norm = constrain((val - mn) / (mx - mn), 0.0f, 1.0f);
    return yBot - (int16_t)(norm * (float)(yBot - yTop));
}

// Dashed horizontal line (dash=4, gap=4 per spec).
static void drawDashedHLine(int16_t x, int16_t y, int16_t w,
                             uint8_t dash, uint8_t gap) {
    for (int16_t i = 0; i < w; i += dash + gap) {
        int16_t len = min((int16_t)dash, (int16_t)(w - i));
        if (len > 0) _display.drawFastHLine(x + i, y, len, GxEPD_BLACK);
    }
}

// Draw str centered at cx, at baseline y.
// Tries FreeSansBold24pt7b; falls back to 18pt if wider than maxW.
static void drawValueCentered(const char *str, int16_t cx, int16_t y,
                               int16_t maxW) {
    _u8g2.setFont(u8g2_font_logisoso38_tn);
    uint16_t tw = _u8g2.getUTF8Width(str);
    if ((int16_t)tw > maxW) {
        _u8g2.setFont(u8g2_font_logisoso32_tn);
        tw = _u8g2.getUTF8Width(str);
    }
    _u8g2.setCursor(cx - (int16_t)(tw / 2), y);
    _u8g2.print(str);
}

// Draw a single-character row label vertically centred on rowCY.
static void drawRowLabel(const char *lbl, int16_t rowCY) {
    int16_t tx, ty; uint16_t tw, th;
    _display.setFont(&FreeSans9pt7b);
    _display.getTextBounds(lbl, 0, 0, &tx, &ty, &tw, &th);
    // cursor y so that bounding box centre aligns with rowCY
    _display.setCursor(6, rowCY - ty - (int16_t)(th / 2));
    _display.print(lbl);
}

// Draw a sparkline for one row from a typed circular history buffer.
// outMn / outMx receive the data min/max (needed for CO2 threshold line).
template<typename T>
static void drawSparkRow(const T *hist, uint8_t count, uint8_t head, uint8_t histLen,
                          float scale, int16_t rowCY,
                          float &outMn, float &outMx) {
    outMn = 0.0f;
    outMx = 0.0f;
    if (count < 2) return;

    int16_t yTop = rowCY - SPK_HALF + SPK_PAD;
    int16_t yBot = rowCY + SPK_HALF - SPK_PAD;

    uint8_t start = (uint8_t)((head - count + histLen) % histLen);
    float mn = 1e9f, mx = -1e9f;
    for (uint8_t i = 0; i < count; i++) {
        float v = hist[(start + i) % histLen] * scale;
        if (v < mn) mn = v;
        if (v > mx) mx = v;
    }
    if (mx <= mn) mx = mn + 1.0f;
    outMn = mn;
    outMx = mx;

    int16_t w = SPK_X1 - SPK_X0;
    for (uint8_t i = 1; i < count; i++) {
        float v0 = hist[(start + i - 1) % histLen] * scale;
        float v1 = hist[(start + i)     % histLen] * scale;
        int16_t x0 = SPK_X0 + (int16_t)((uint32_t)(i - 1) * w / (count - 1));
        int16_t x1 = SPK_X0 + (int16_t)((uint32_t)i       * w / (count - 1));
        int16_t y0 = valueToY(v0, mn, mx, yTop, yBot);
        int16_t y1 = valueToY(v1, mn, mx, yTop, yBot);
        _display.drawLine(x0, y0, x1, y1, GxEPD_BLACK);
    }
}

// =============================================================================
// Zone 1 — Temperature (left column) | Humidity (right column)
// y = Z1_TOP .. Z2_TOP  (~112px)
// =============================================================================
static void drawZone1(float tempC, float humidity, bool readOk) {
    _display.setTextColor(GxEPD_BLACK);

    const int16_t lcx = COL_MID / 2;                    // left column centre x
    const int16_t rcx = COL_MID + (DISP_W - COL_MID) / 2; // right column centre x
    const int16_t lblY = Z1_TOP + 22;
    const int16_t valY = Z1_TOP + 110;

    // Unit labels
    _display.setFont(&FreeSans9pt7b);
    {
        int16_t tx, ty; uint16_t tw, th;
        _display.getTextBounds("\xb0""F", 0, 0, &tx, &ty, &tw, &th);
        _display.setCursor(lcx - tw / 2, lblY);
        _display.print("\xb0""F");
    }
    {
        int16_t tx, ty; uint16_t tw, th;
        _display.getTextBounds("RH", 0, 0, &tx, &ty, &tw, &th);
        _display.setCursor(rcx - tw / 2, lblY);
        _display.print("RH");
    }

    // Values
    char buf[8];
    if (readOk) {
        float tempF = tempC * 9.0f / 5.0f + 32.0f;
        snprintf(buf, sizeof(buf), "%.1f", tempF);
    } else {
        snprintf(buf, sizeof(buf), "--.-");
    }
    drawValueCentered(buf, lcx, valY, COL_MID - 8);

    if (readOk) {
        snprintf(buf, sizeof(buf), "%.1f", humidity);
    } else {
        snprintf(buf, sizeof(buf), "--.-");
    }
    drawValueCentered(buf, rcx, valY, DISP_W - COL_MID - 8);

    // Vertical divider
    _display.drawFastVLine(COL_MID, Z1_TOP + 10, 128, GxEPD_BLACK);

    // Bottom divider
    _display.drawFastHLine(0, Z2_TOP, DISP_W, GxEPD_BLACK);
}

// =============================================================================
// Zone 2 — CO2
// y = Z2_TOP .. Z3_TOP  (~94px)
// =============================================================================
static void drawZone2(float co2, bool readOk) {
    _display.setTextColor(GxEPD_BLACK);

    // CO2 value — top of zone
    char buf[8];
    if (readOk) {
        snprintf(buf, sizeof(buf), "%d", (int)co2);
    } else {
        snprintf(buf, sizeof(buf), "----");
    }

    // 32pt value; fall back to 24pt if too wide (e.g. "----" or 5-digit CO2)
    _u8g2.setFont(u8g2_font_logisoso38_tn);
    uint16_t tw = _u8g2.getUTF8Width(buf);
    if ((int16_t)tw > DISP_W - 16) {
        _u8g2.setFont(u8g2_font_logisoso32_tn);
        tw = _u8g2.getUTF8Width(buf);
    }
    const int16_t valueBaselineY = Z2_TOP + 68;
    const int16_t valueX = DISP_W / 2 - (int16_t)(tw / 2);
    _u8g2.setCursor(valueX, valueBaselineY);
    _u8g2.print(buf);

    // Alert: solid 2px underline spanning the value width
    if (readOk && co2 >= CO2_ALERT_PPM) {
        int16_t ulY = valueBaselineY + 6;
        _display.drawFastHLine(valueX, ulY,     tw, GxEPD_BLACK);
        _display.drawFastHLine(valueX, ulY + 1, tw, GxEPD_BLACK);
    }

    // "ppm CO2" label — bottom of zone
    _display.setFont(&FreeSans9pt7b);
    {
        int16_t lx, ly; uint16_t lw, lh;
        _display.getTextBounds("ppm CO2", 0, 0, &lx, &ly, &lw, &lh);
        _display.setCursor(DISP_W / 2 - lw / 2, Z2_TOP + 112);
        _display.print("ppm CO2");
    }

    // Bottom divider
    _display.drawFastHLine(0, Z3_TOP, DISP_W, GxEPD_BLACK);
}

// =============================================================================
// Zone 3 — Sparkline strip  (T / H / C rows)
// y = Z3_TOP .. Z4_TOP  (~158px)
// =============================================================================
static void drawZone3(const HistoryBuffer &history) {
    const uint8_t count = history.count, head = history.head;
    const uint8_t histLen = HistoryBuffer::capacity;
    // Row labels
    drawRowLabel("T", T_CY);
    drawRowLabel("H", H_CY);
    drawRowLabel("C", C_CY);

    // Sparklines — tempHistory is °F×10, scale 0.1 gives °F directly
    float tMn, tMx, hMn, hMx, cMn, cMx;
    drawSparkRow(history.temperatureF10, count, head, histLen, 0.1f, T_CY, tMn, tMx);
    drawSparkRow(history.humidityPercent,  count, head, histLen, 1.0f, H_CY, hMn, hMx);
    drawSparkRow(history.co2Ppm,  count, head, histLen, 1.0f, C_CY, cMn, cMx);

    // CO2 threshold dashed line (on C row only, if in visible range)
    if (count >= 2 && (float)CO2_ALERT_PPM >= cMn && (float)CO2_ALERT_PPM <= cMx) {
        int16_t yTop = C_CY - SPK_HALF + SPK_PAD;
        int16_t yBot = C_CY + SPK_HALF - SPK_PAD;
        int16_t threshY = valueToY((float)CO2_ALERT_PPM, cMn, cMx, yTop, yBot);
        drawDashedHLine(SPK_X0, threshY, SPK_X1 - SPK_X0, 4, 4);
    }

    // Inter-row dividers — dashed to appear lighter on BW panel
    drawDashedHLine(0, Z3_D1, DISP_W, 3, 3);
    drawDashedHLine(0, Z3_D2, DISP_W, 3, 3);

    // Bottom divider
    _display.drawFastHLine(0, Z4_TOP, DISP_W, GxEPD_BLACK);
}

// Draw a battery icon with up to 5 filled bars.
// x,y = top-left of the body rect.
// Body: 28×14px outline. Tip: 4×8px solid on right, centred.
// Inner bar area: 24×10px (2px border+pad each side).
// 5 bars × 4px + 4 gaps × 1px = 24px exactly.
static void drawBatteryIcon(int16_t x, int16_t y, float pct) {
    const int16_t BW = 28, BH = 14, TW = 4, TH = 8;

    _display.drawRect(x, y, BW, BH, GxEPD_BLACK);
    _display.fillRect(x + BW, y + (BH - TH) / 2, TW, TH, GxEPD_BLACK);

    int16_t ix = x + 2, iy = y + 2;
    int16_t ih = BH - 4;   // inner height = 10px

    int bars = constrain((int)((pct + 10.0f) / 20.0f), 0, 5);
    for (int i = 0; i < bars; i++) {
        _display.fillRect(ix + i * 5, iy, 4, ih, GxEPD_BLACK);
    }
}

// =============================================================================
// Zone 4 — Status bar
// y = Z4_TOP .. 416  (~36px)
// =============================================================================
static void drawZone4(float battV, float battPct, bool battOk,
                       uint32_t totalUptimeS) {
    _display.setFont(&FreeSans9pt7b);
    _display.setTextColor(GxEPD_BLACK);

    const int16_t baseline = Z4_TOP + 24;

    // Time (HH:MM derived from uptime)
    uint32_t totalMin = totalUptimeS / 60;
    char timeBuf[12];
    snprintf(timeBuf, sizeof(timeBuf), "%ud%02u:%02u",
             (uint32_t)(totalMin / 1440),
             (uint32_t)((totalMin % 1440) / 60),
             (uint32_t)(totalMin % 60));
    _display.setCursor(8, baseline);
    _display.print(timeBuf);

    // Battery icon + voltage — right-aligned; omit if not available
    if (battOk) {
        // Icon: body left at x=206, tip ends at x=238 (2px from edge)
        const int16_t iconX = DISP_W - 4 - 28;          // = 208... body left
        const int16_t iconY = Z4_TOP + (36 - 14) / 2;   // vertically centred
        drawBatteryIcon(iconX, iconY, battPct);

        // Voltage text just left of the icon
        char voltBuf[8];
        snprintf(voltBuf, sizeof(voltBuf), "%.2fV", battV);
        int16_t tx, ty; uint16_t tw, th;
        _display.getTextBounds(voltBuf, 0, 0, &tx, &ty, &tw, &th);
        _display.setCursor(iconX - 4 - (int16_t)tw, baseline);
        _display.print(voltBuf);
    }
}

// =============================================================================
// Public API
// =============================================================================

// Both the old image plane and the new visible frame use exactly this renderer.
static void drawFrame(const DisplaySnapshot &snapshot) {
    const auto &environment = snapshot.readings.environment;
    const auto &battery = snapshot.readings.battery;
    _display.fillScreen(GxEPD_WHITE);
    drawZone1(environment.temperatureC, environment.humidityPercent, environment.valid);
    drawZone2(environment.co2Ppm, environment.valid);
    drawZone3(snapshot.history);
    drawZone4(battery.voltageV, battery.chargePercent, battery.voltageV > 0, snapshot.uptimeS);
}

void initDisplay() {
    _display.init(0, false, 10, false); // We explicitly seed both image planes.
    _displayInitialized = true;
    _display.setRotation(0);            // portrait
    _display.setTextWrap(false);
    _u8g2.begin(_display);
    _u8g2.setForegroundColor(GxEPD_BLACK);
    _u8g2.setBackgroundColor(GxEPD_WHITE);
}

void resetDisplayRefreshCount() {
    _partialCount = 0;
    _previousValid = false;
}

void updateDisplay(const DisplaySnapshot &snapshot) {
    bool doFull = !_previousValid || _partialCount == 0;
    if (!doFull) {
        _display.setFullWindow();
        _display.firstPage();
        do {
            drawFrame(_previous);
        } while (_display.nextPageToPrevious());
    }
    LOG("[DSP] %s refresh (count=%u)\n", doFull ? "Full" : "Partial", _partialCount);
    if (doFull) {
        _display.setFullWindow();
    } else {
        _display.setPartialWindow(0, 0, GxEPD2_370_GDEY037T03::WIDTH,
                                        GxEPD2_370_GDEY037T03::HEIGHT);
    }
    _partialCount = (_partialCount + 1) % DISPLAY_FULL_REFRESH_EVERY;

    _display.firstPage();
    do {
        drawFrame(snapshot);
    } while (_display.nextPage());

    _previous = snapshot;
    _previousValid = true;
    LOG("[DSP] Display updated.\n");
}

void hibernateDisplay() {
    if (_displayInitialized) _display.hibernate();
}
