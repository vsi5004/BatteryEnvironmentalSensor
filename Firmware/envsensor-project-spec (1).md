# ESP32-S3 Environmental Sensor — Project Specification

## Overview

Battery-powered home environmental sensor built on an Adafruit ESP32-S3 Feather. Reads CO2, temperature, and humidity, displays data locally on an e-paper screen, and publishes readings to Home Assistant via MQTT. Designed for long battery life on a 3500mAh 18650 cell.

---

## Hardware

| Component | Part | Interface |
|---|---|---|
| MCU | Adafruit ESP32-S3 Feather | — |
| CO2 + T/H (secondary) | Sensirion SCD41 | I2C |
| Temperature / Humidity (primary) | Adafruit SHT40 breakout | I2C |
| Display | WeAct Studio 3.7" e-paper (GDEY037T03, UC8253, 240×416, BW) | SPI |
| Battery | 3500mAh 18650 Li-Ion | JST on Feather |

### Pin Assignments

Use Adafruit's ESP32-S3 Feather BSP pin definitions where available. Define the following explicitly:

```cpp
// E-paper display
#define EPD_CS    SS       // or explicit GPIO
#define EPD_DC    // to be confirmed from WeAct wiring
#define EPD_RES   // to be confirmed
#define EPD_BUSY  // to be confirmed

// Feather power management — use BSP-defined constants, do not hardcode GPIO numbers
// NEOPIXEL_POWER  — defined by Adafruit ESP32-S3 Feather BSP, pull LOW to cut NeoPixel
// I2C_POWER       — defined by BSP, pull LOW to cut power to STEMMA QT 3.3V rail
// PIN_NEOPIXEL    — defined by BSP, the NeoPixel data pin
```

I2C uses default SDA/SCL for the ESP32-S3 Feather. Both SCD41 and SHT40 share the same I2C bus — confirm no address conflict (SCD41: 0x62, SHT40: 0x44).

**Note on PSRAM**: The 5477 variant has 2MB PSRAM. This means the full GxEPD2 framebuffer (240×416 / 8 ≈ 12.5KB) fits comfortably in RAM — no need for paged drawing mode if the full-buffer approach is preferred.

---

## Libraries

Use the following — do not substitute without flagging:

| Library | Purpose |
|---|---|
| `Adafruit SHT4x` | SHT40 driver |
| `Sensirion I2C SCD4x` | SCD41 driver (Sensirion official) |
| `GxEPD2` | E-paper display (GDEY037T03 variant) |
| `Adafruit GFX` | Font and drawing primitives (pulled in by GxEPD2) |
| `Adafruit MAX1704X` | MAX17048 battery fuel gauge |
| `PubSubClient` | MQTT client |
| `ArduinoJson` | JSON payload construction |
| `WiFi` (built-in) | WiFi stack |

---

## Firmware Architecture

### Main Loop Concept

This is a **deep sleep duty-cycle** design, not a continuously running loop. The logical flow per wake cycle is:

```
Wake from deep sleep
  → Power on peripherals (NeoPixel pin, any load switches)
  → Connect to WiFi
  → Initialize sensors
  → Wait for SCD41 single-shot measurement (~5s)
  → Read SHT40
  → Read SCD41 result
  → Publish MQTT
  → Update e-paper display (if display update interval has elapsed)
  → Power off peripherals
  → Enter deep sleep for SLEEP_DURATION_S seconds
```

The `loop()` function should be essentially empty. All logic runs in `setup()` and ends with `esp_deep_sleep_start()`.

### Timing Constants (make configurable at top of file)

```cpp
#define SLEEP_DURATION_S        55      // Sleep between wake cycles (seconds)
#define DISPLAY_UPDATE_INTERVAL 300     // Refresh display every N seconds
#define WIFI_TIMEOUT_MS         10000   // Abort WiFi connect after this
#define MQTT_TIMEOUT_MS         5000
```

The effective sensor sample rate is approximately `1 / (SLEEP_DURATION_S + ~8s awake)` — roughly 1 sample per minute at default settings.

### Deep Sleep Implementation

Use ESP-IDF sleep API directly (callable from Arduino):

```cpp
esp_sleep_enable_timer_wakeup((uint64_t)SLEEP_DURATION_S * 1000000ULL);
esp_deep_sleep_start();
```

Before calling sleep:
- Pull `NEOPIXEL_POWER_PIN` LOW
- Disconnect WiFi cleanly (`WiFi.disconnect(true); WiFi.mode(WIFI_OFF);`)
- Call `display.hibernate()` on the GxEPD2 object

### RTC Memory for Display Timing and Graph History

Use `RTC_DATA_ATTR` to persist state across sleep cycles:

```cpp
// Timing
RTC_DATA_ATTR uint32_t lastDisplayUpdateS = 0;
RTC_DATA_ATTR uint32_t totalUptimeS       = 0;  // incremented by SLEEP_DURATION_S each cycle

// Circular history buffers for sparklines (~2h at 1 reading/min)
#define HISTORY_LEN 120

RTC_DATA_ATTR uint16_t co2History[HISTORY_LEN];   // raw ppm
RTC_DATA_ATTR int16_t  tempHistory[HISTORY_LEN];  // °C × 10 (e.g. 224 = 22.4°C)
RTC_DATA_ATTR uint8_t  humHistory[HISTORY_LEN];   // 0–100 integer
RTC_DATA_ATTR uint8_t  historyCount = 0;          // valid samples (saturates at HISTORY_LEN)
RTC_DATA_ATTR uint8_t  historyHead  = 0;          // next write index
```

Total RTC footprint: 120×(2+2+1) + overhead ≈ 620 bytes, well within the 8KB RTC SRAM limit.

Each wake cycle, after reading sensors, append to the circular buffer:
```cpp
co2History[historyHead]  = (uint16_t)co2;
tempHistory[historyHead] = (int16_t)(temperature * 10);
humHistory[historyHead]  = (uint8_t)humidity;
historyHead = (historyHead + 1) % HISTORY_LEN;
if (historyCount < HISTORY_LEN) historyCount++;
```

RTC SRAM is volatile — history is lost on power-off or hard reset, which is acceptable for a display trend graph. It survives deep sleep indefinitely.

---

## Sensor Details

### SHT40 (Primary Temperature + Humidity)

- Use **high precision mode** (`SHT4X_HIGH_PRECISION`)
- This is the **authoritative** T/H source for display and MQTT
- Reading takes ~10ms, blocking is fine

### SCD41 (CO2 + secondary T/H)

- Use **single-shot measurement mode** to avoid continuous operation
- Call `scd41.measureSingleShot()` then wait ~5 seconds before reading result
- Use SCD41 T/H only for internal SCD41 compensation — do NOT publish or display it
- Apply temperature offset to account for self-heating: start with `scd41.setTemperatureOffset(4.0)` and allow calibration via a `#define` constant

### Self-Heating Note

The SCD41 runs warm. Keep sensor PCB layout and enclosure design in mind — the SHT40 should be physically separated from both the SCD41 and the ESP32-S3 module.

---

## Display

### Panel Initialization

```cpp
GxEPD2_BW<GxEPD2_370_GDEY037T03, GxEPD2_370_GDEY037T03::HEIGHT> display(
  GxEPD2_370_GDEY037T03(EPD_CS, EPD_DC, EPD_RES, EPD_BUSY)
);
```

### Refresh Strategy

- Use **full refresh only** — no partial refresh. The panel is only updated every several minutes; ghosting from partial refresh is not worth the complexity.
- Always call `display.hibernate()` before deep sleep.
- Use `display.setFullWindow()` and `display.firstPage()` / `display.nextPage()` paged drawing pattern, OR allocate the full buffer if RAM permits (240×416 / 8 = ~12.5KB — feasible on S3).

### Display Layout (240×416 portrait)

Five zones from top to bottom:

```
┌────────────────────────┐
│  CO₂                   │  Zone 1 — full width, ~110px
│  1248 ppm  ⚠ HIGH      │  Large value, alert indicator if >threshold
├────────────────────────┤
│  Temp   │ ╭─────────╮  │  Zone 2 — ~100px
│  22.4°  │  trend 2h    │  Value left, sparkline right
│  Celsius │ ╰─────────╯ │
├────────────────────────┤
│ Humidity │ ╭─────────╮  │  Zone 3 — ~100px
│  47.2%  │  trend 2h    │  Same pattern as temp
│ relative │ ╰─────────╯ │
├────────────────────────┤
│ CO₂ 2h ─────────────── │  Zone 4 — ~100px, full width
│  · · ·╭─╮· · ·         │  Trend graph with dashed 1000ppm threshold line
│ 800  ──────────── 1500 │
├────────────────────────┤
│ Updated 14:32  84%·3.92V│  Zone 5 — ~42px status bar
│      envsensor_01       │
└────────────────────────┘
```

**Zone 1 — CO₂ (full width)**
- Label: `FreeSans9pt7b`, muted
- Value: largest available font (~28pt equivalent), centered
- Unit + alert: `FreeSans9pt7b` on same line; if CO₂ > `CO2_ALERT_PPM`, append `⚠ HIGH` and draw an inverted border rect around the zone

**Zones 2 & 3 — Temp and Humidity (split)**
- Left ~35% of width: label + value + unit, vertically centered
- Vertical divider line separating halves
- Right ~65% of width: sparkline from RTC history array + small "2h trend" label at top

**Zone 4 — CO₂ trend (full width)**
- Spans full 240px width for maximum resolution
- Auto-scaled Y axis from min/max of history array, with axis value labels at top-right and bottom-right
- Dashed horizontal line at `CO2_ALERT_PPM` (1000ppm by default) if it falls within the visible range
- Line drawn by mapping each RTC history sample to a pixel coordinate and calling `drawLine()` between consecutive points

**Zone 5 — Status bar**
- Left: last update time (cycle counter converted to HH:MM approximation)
- Right: battery percent + voltage from MAX17048
- Center: device ID

### Graph Rendering

No chart library is available — draw graphs by mapping data to pixel coordinates manually:

```cpp
// Map a value to a y-pixel within a graph rectangle
int16_t valueToY(float val, float minVal, float maxVal,
                 int16_t yTop, int16_t yBottom) {
    if (maxVal == minVal) return (yTop + yBottom) / 2;
    float norm = (val - minVal) / (maxVal - minVal);
    return yBottom - (int16_t)(norm * (yBottom - yTop));
}

// Draw sparkline from circular history buffer
void drawSparkline(int16_t xLeft, int16_t xRight,
                   int16_t yTop,  int16_t yBottom,
                   float* data, uint8_t count, uint8_t head,
                   float minVal, float maxVal) {
    if (count < 2) return;
    int16_t w = xRight - xLeft;
    for (uint8_t i = 1; i < count; i++) {
        uint8_t idxA = (head - count + i - 1 + HISTORY_LEN) % HISTORY_LEN;
        uint8_t idxB = (head - count + i     + HISTORY_LEN) % HISTORY_LEN;
        int16_t x0 = xLeft + (int16_t)((i - 1) * w / (count - 1));
        int16_t x1 = xLeft + (int16_t)(i       * w / (count - 1));
        int16_t y0 = valueToY(data[idxA], minVal, maxVal, yTop, yBottom);
        int16_t y1 = valueToY(data[idxB], minVal, maxVal, yTop, yBottom);
        display.drawLine(x0, y0, x1, y1, GxEPD_BLACK);
    }
}
```

Auto-scale by scanning the history array for min/max before each draw. Add a fixed margin (e.g. 5% of range) so the line never touches the top or bottom of the graph rect.

**Dashed threshold line** (no native primitive — implement manually):
```cpp
void drawDashedHLine(int16_t x, int16_t y, int16_t w,
                     uint8_t dashLen, uint8_t gapLen) {
    for (int16_t i = 0; i < w; i += dashLen + gapLen)
        display.drawFastHLine(x + i, y, min((int16_t)dashLen, (int16_t)(w - i)), GxEPD_BLACK);
}
```

---

## WiFi + MQTT

### WiFi

- Store SSID and password as `#define` constants in a separate `config.h` (gitignored)
- Use `WiFi.begin()` with a timeout — if WiFi fails to connect within `WIFI_TIMEOUT_MS`, skip MQTT and continue to display update + sleep. Do not hang.
- After MQTT publish, disconnect: `WiFi.disconnect(true); WiFi.mode(WIFI_OFF);`

### MQTT

- Broker address, port, and client ID in `config.h`
- Use PubSubClient
- Publish a single JSON payload to one topic per device:

```
Topic: homeassistant/sensor/envsensor_<DEVICE_ID>/state
```

Payload:
```json
{
  "co2": 842,
  "temperature": 21.4,
  "humidity": 47.2,
  "battery_v": 3.91,
  "battery_pct": 84.2
}
```

- On failed MQTT connection, log to Serial and skip — do not retry indefinitely

### MQTT Discovery (Home Assistant Auto-Discovery)

On first boot (or if a `FORCE_DISCOVERY` flag is set), publish discovery config messages so HA automatically creates entities. Publish to:

```
homeassistant/sensor/envsensor_<DEVICE_ID>/co2/config
homeassistant/sensor/envsensor_<DEVICE_ID>/temperature/config
homeassistant/sensor/envsensor_<DEVICE_ID>/humidity/config
homeassistant/sensor/envsensor_<DEVICE_ID>/battery_voltage/config
homeassistant/sensor/envsensor_<DEVICE_ID>/battery_percent/config
```

Each discovery payload should specify:
- `name`, `unique_id`, `device_class`, `unit_of_measurement`, `state_topic`, `value_template`
- Group all entities under a single `device` block with `identifiers`, `name`, `manufacturer`, `model`

Use `RTC_DATA_ATTR bool discoveryPublished = false;` to track whether discovery has been sent this power cycle. Re-send on first boot after hard reset.

Discovery payloads must use `retain: true`.

---

## Power Management

### NeoPixel and STEMMA QT Power

The board has two GPIO-controlled power rails managed via BSP constants:

- `NEOPIXEL_POWER` — gates power to the onboard NeoPixel. Pull LOW to disable. The Arduino BSP enables it by default at boot; pull it LOW immediately in `setup()` since the NeoPixel is never needed.
- `I2C_POWER` — gates the 3.3V supply to the STEMMA QT connector. **Both the SHT40 and SCD41 are powered through this rail if connected via STEMMA QT.** Do NOT pull this LOW until after sensors have been read and you are ready to sleep. Re-enable it at wakeup before sensor init.

Before sleep, the sequence must be:
1. Read all sensors
2. Publish MQTT
3. `digitalWrite(I2C_POWER, LOW)` — power down sensors
4. `digitalWrite(NEOPIXEL_POWER, LOW)` — already LOW but confirm
5. `WiFi.disconnect(true); WiFi.mode(WIFI_OFF);`
6. `display.hibernate()`
7. `esp_deep_sleep_start()`

### Battery Monitoring — MAX17048

The 5477 board has an onboard **MAX17048** I2C fuel gauge (address 0x36). There is no ADC pin for battery voltage on this board — the fuel gauge is the only path to this data.

```cpp
#include <Adafruit_MAX1704X.h>

Adafruit_MAX17048 maxlipo;

void initBatteryMonitor() {
    maxlipo.begin();
}

void readBattery(float &voltage, float &percent) {
    voltage = maxlipo.cellVoltage();
    percent = maxlipo.cellPercent();
}
```

The MAX17048 remains powered as long as the board has power — it is not gated by `I2C_POWER`. Include both `battery_v` and `battery_pct` in the MQTT payload and display.

### WiFi Power

Use `WiFi.setSleep(true)` to enable modem sleep during connection. After publish, `WiFi.disconnect(true)` and `WiFi.mode(WIFI_OFF)` before sleep.

### Expected Power Budget (targets, not guaranteed)

| Phase | Current | Duration |
|---|---|---|
| Deep sleep | ~20–50µA | ~55s |
| WiFi connect + MQTT | ~80–150mA | ~3–5s |
| SCD41 measurement | ~15mA | ~5s |
| SHT40 read | ~1mA | <100ms |
| E-paper refresh (when triggered) | ~20–30mA | ~4s |

Target average current: <2mA, targeting >70 days on 3500mAh under realistic WiFi conditions.

---

## Configuration File (`config.h`)

Create `config.h` (add to `.gitignore`) with:

```cpp
#pragma once

// WiFi
#define WIFI_SSID       "your_ssid"
#define WIFI_PASSWORD   "your_password"

// MQTT
#define MQTT_BROKER     "192.168.x.x"
#define MQTT_PORT       1883
#define MQTT_USER       ""       // leave empty if no auth
#define MQTT_PASS       ""
#define DEVICE_ID       "envsensor_01"

// Sensor tuning
#define SCD41_TEMP_OFFSET   4.0f    // degrees C, adjust after calibration

// Thresholds
#define CO2_ALERT_PPM   1000

// Timing
#define SLEEP_DURATION_S        55
#define DISPLAY_UPDATE_INTERVAL 300
```

---

## File Structure

```
envsensor/
├── envsensor.ino       # Main sketch
├── config.h            # Credentials and tuning (gitignored)
├── display.h / .cpp    # Display layout and drawing logic
├── sensors.h / .cpp    # SHT40 + SCD41 init and read
├── mqtt_ha.h / .cpp    # MQTT connect, publish, discovery payloads
├── power.h / .cpp      # Sleep entry, power rail control, MAX17048 battery reads
└── .gitignore
```

---

## Error Handling Expectations

- WiFi timeout → skip MQTT, continue to display/sleep. Log to Serial.
- MQTT connect failure → skip publish, continue to sleep. Log to Serial.
- Sensor read failure → display "—" for failed value, publish null or omit field.
- Do not use `while(1)` hangs anywhere. The device must always reach `esp_deep_sleep_start()`.

---

## Serial Logging

Enable verbose Serial logging at 115200 baud. Each wake cycle should log:
- Wake reason
- Sensor readings
- WiFi status and MQTT publish result
- Battery voltage
- Time spent awake before sleep

Use `#define DEBUG 1` guard so logging can be disabled for production.

---

## Out of Scope

- OTA firmware updates
- Web UI or captive portal
- Particulate matter (PM2.5) sensing — deferred
- Multi-room or multi-device coordination
- Any cloud connectivity beyond local HA instance
