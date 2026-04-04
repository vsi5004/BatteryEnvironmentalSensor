# envsensor — ESP32-S3 Environmental Sensor Firmware

Battery-powered home environmental sensor. Reads CO₂, temperature, and humidity on a deep-sleep duty cycle, displays data on a 3.7" e-paper screen, and publishes to Home Assistant via MQTT auto-discovery.

---

## Hardware

| Component | Part | Interface |
|-----------|------|-----------|
| MCU | Adafruit ESP32-S3 Feather (#5477, 2MB PSRAM) | — |
| CO₂ (+ secondary T/H) | Sensirion SCD41 | I2C 0x62 |
| Temperature / Humidity (primary) | Adafruit SHT40 | I2C 0x44 |
| Display | WeAct Studio 3.7" e-paper (GDEY037T03, 240×416, BW) | SPI |
| Battery gauge | Onboard MAX17048 | I2C 0x36 |
| Battery | 3500mAh 18650 Li-Ion | JST on Feather |

### Wiring — E-paper Display

The WeAct GDEY037T03 panel is write-only SPI (SDA = MOSI, SCL = SCK, no MISO).

**Data lines** use the Feather's main SPI bus on the left-side header — not the FSPI pins (D11/D12), which are shared with flash/PSRAM:

| Signal | Feather silkscreen | GPIO |
|--------|--------------------|------|
| SCK    | SCK (left header)  | 36   |
| MOSI   | MO (left header)   | 35   |

**Control lines** use the right-side header:

| Signal | Feather pin | `config.h` define |
|--------|-------------|-------------------|
| CS/CSB | D10         | `EPD_CS  = 10`    |
| DC     | D9          | `EPD_DC  = 9`     |
| RES    | D6          | `EPD_RES = 6`     |
| BUSY   | D5          | `EPD_BUSY = 5`    |

Connect panel VCC to 3.3V and GND to GND.

### I2C Bus

SHT40 and SCD41 connect via the STEMMA QT connector (or SDA=GPIO3, SCL=GPIO4 directly). The MAX17048 is on the same I2C bus but powered independently of the STEMMA QT rail.

---

## Libraries

Install all of the following via the Arduino Library Manager:

| Library | Purpose |
|---------|---------|
| `GxEPD2` | E-paper driver (includes GDEY037T03) |
| `Adafruit GFX Library` | Font + drawing primitives |
| `U8g2_for_Adafruit_GFX` | U8g2 font bridge for Adafruit GFX (large display fonts) |
| `U8g2` | U8g2 font data (pulled in by the bridge above) |
| `Adafruit SHT4x Library` | SHT40 driver |
| `Sensirion I2C SCD4x` | SCD41 driver (Sensirion official, v2 API) |
| `Adafruit MAX1704X` | MAX17048 fuel gauge |
| `PubSubClient` | MQTT client |
| `ArduinoJson` | JSON payload construction |

Board package: **Adafruit ESP32-S3 Feather** via the Adafruit board index.

---

## Configuration

Copy `config.example.h` to `config.h` (gitignored) and fill in your values:

```cpp
// WiFi
#define WIFI_SSID       "your_ssid"
#define WIFI_PASSWORD   "your_password"

// MQTT / Home Assistant
#define MQTT_BROKER     "192.168.x.x"
#define MQTT_PORT       1883
#define MQTT_USER       ""          // leave empty if no auth
#define MQTT_PASS       ""
#define DEVICE_ID       "envsensor_01"

// Sensor tuning
#define SCD41_TEMP_OFFSET   4.0f    // °C — adjust after calibration

// CO₂ alert threshold
#define CO2_ALERT_PPM   2000

// Timing
#define SLEEP_DURATION_S        155     // seconds between wake cycles
#define DISPLAY_UPDATE_INTERVAL     300     // seconds between display refreshes
#define DISPLAY_FULL_REFRESH_EVERY   10     // full refresh every N updates (~50 min); clears ghosting
#define WIFI_TIMEOUT_MS         10000
#define MQTT_TIMEOUT_MS         5000

// E-paper pins
#define EPD_CS    10
#define EPD_DC     9
#define EPD_RES    6
#define EPD_BUSY   5

// Serial debug output
#define DEBUG 1
```

---

## Firmware Architecture

### Wake Cycle

All logic runs inside `setup()`. `loop()` is empty. Each wake cycle:

```
Power-on / wake from deep sleep
  → Wire.begin()
  → initPower()           — NeoPixel OFF, STEMMA QT rail ON
  → initSensors()         — SHT40 init; 35ms SCD41 power-up delay; warmup shot triggered
  → initBattery()         — MAX17048 quickStart + 125ms settle  ← overlaps warmup window
  → readBattery()
  → triggerMeasurement()  — waits for warmup shot, discards it, fires real shot
  → connectWifi()         — real shot runs during WiFi connect (~3–5s overlap)
  → readSensors()         — polls for real shot result, reads both sensors
  → appendHistory()       — push to RTC circular buffers
  → publishDiscovery()    — first boot only (RTC flag)
  → publishMQTT()
  → disconnectWifi()      — graceful MQTT disconnect before radio off
  → totalUptimeS += SLEEP_DURATION_S
  → initDisplay() + updateDisplay()  — only if interval elapsed
  → enterDeepSleep()
```

### RTC Memory

State that persists across deep sleep (lost on power-off):

```cpp
RTC_DATA_ATTR uint32_t lastDisplayUpdateS;
RTC_DATA_ATTR uint32_t totalUptimeS;

RTC_DATA_ATTR uint16_t co2History[120];   // raw ppm
RTC_DATA_ATTR int16_t  tempHistory[120];  // °F × 10  (e.g. 724 = 72.4°F)
RTC_DATA_ATTR uint8_t  humHistory[120];   // 0–100 %RH
RTC_DATA_ATTR uint8_t  historyCount;
RTC_DATA_ATTR uint8_t  historyHead;
RTC_DATA_ATTR bool     discoveryPublished;
// (display.cpp) RTC_DATA_ATTR static uint8_t _partialCount; — partial refresh counter
```

120 samples at one per cycle (~155s) covers approximately 5 hours of history.

### File Structure

```
envsensor/
├── envsensor.ino        — main sketch, RTC vars, wake cycle orchestration
├── config.example.h     — configuration template (copy to config.h and fill in)
├── config.h             — credentials and tuning (gitignored)
├── debug.h              — LOG() / LOG_FLUSH() inline template helpers
├── power.h / .cpp       — power rail control, MAX17048, deep sleep entry
├── sensors.h / .cpp     — SHT40 + SCD41 init and read
├── mqtt_ha.h / .cpp     — WiFi, MQTT publish, HA auto-discovery
├── display.h / .cpp     — GxEPD2 layout, sparklines, battery icon
└── .gitignore
```

---

## Sensors

### SCD41 — Warmup Shot Pattern

The SCD41 always returns CO₂ = 0 on its **first** single-shot measurement after a power cycle. This is avoided with a two-shot pattern:

1. `initSensors()`: wait 35ms after power-on (datasheet max: 30ms), then fire a **warmup shot**. This shot is discarded per Sensirion's recommendation.
2. Other init (battery gauge etc.) runs during the ~3.8s warmup window.
3. `triggerMeasurement()`: waits for the warmup shot to complete, discards it, fires the **real shot**.
4. WiFi connects during the ~5s real shot window.
5. `readSensors()`: polls for the real shot result (starts polling at 3.8s, timeout at 5.1s) then reads both sensors.

Total extra cost: near zero — the warmup delay is hidden behind WiFi connect time.

### SHT40 — Primary T/H Source

SHT40 is the authoritative source for temperature and humidity. The SCD41 also reports T/H but those values are only used internally by the SCD41 for compensation — they are not displayed or published.

All temperatures are stored and displayed in **Fahrenheit**.

### MAX17048 — Battery Gauge

The MAX17048 requires a `quickStart()` call and ~125ms settle time before its first reading is valid. Without this, `cellVoltage()` may return 0 even with a battery connected. The chip is not gated by the STEMMA QT power rail and remains powered through deep sleep.

---

## Display Layout

The panel is 240×416 pixels, portrait orientation. Four zones top to bottom:

```
┌────────────────────────┐  y=0
│   °F        │   RH     │  Zone 1 — 148px
│   72.1      │  31.5    │  Large values, vertical divider
├────────────────────────┤  y=148
│           789          │  Zone 2 — 124px
│          ppm CO₂       │  Large value top, label bottom; 2px underline alert if ≥ CO2_ALERT_PPM
├────────────────────────┤  y=272
│ T ╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌ │  Zone 3 — 108px (3 rows × 36px)
│ H ╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌ │  Sparklines for T / H / CO₂
│ C ╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌ │  Dashed CO₂ threshold line if in range
├────────────────────────┤  y=380
│ 2d14:22     3.62V [███░░] │  Zone 4 — 36px status bar
└────────────────────────┘  y=416
```

**Zone 1** — Temperature (left) and Humidity (right) in large font (`FreeSansBold24pt7b`, 18pt fallback if value too wide). Labels `°F` / `RH` above.

**Zone 2** — CO₂ value in large font (same 24pt/18pt fallback) at the top of the zone, `ppm CO₂` label at the bottom. A solid 2px underline appears under the value if CO₂ ≥ `CO2_ALERT_PPM`.

**Zone 3** — Three stacked sparklines (T / H / CO₂), each labelled with a single character. Dashed inter-row dividers. A dashed horizontal threshold line is drawn on the CO₂ row if `CO2_ALERT_PPM` falls within the visible y-range.

**Zone 4** — Status bar. Left: elapsed time as `Dd HH:MM` (e.g. `2d14:22`). Right: voltage as text + 5-bar battery icon. Icon fills one bar per 20% of charge.

Display is only refreshed when `(totalUptimeS − lastDisplayUpdateS) ≥ DISPLAY_UPDATE_INTERVAL`. Routine updates use the partial refresh waveform (~350ms, minimal flicker); a full refresh (~1500ms) runs every `DISPLAY_FULL_REFRESH_EVERY` updates to clear ghosting. On power-on or reset, the counter is always reset to force a clean full refresh first.

---

## MQTT / Home Assistant

### State Topic

Published each cycle to:
```
homeassistant/sensor/<DEVICE_ID>/state
```

Payload (fields omitted if sensor read failed):
```json
{
  "co2": 789,
  "temperature": 69.8,
  "humidity": 28.2,
  "battery_v": 3.62,
  "battery_pct": 74.3
}
```

Temperature is published in **°F**.

### Auto-Discovery

Five retained discovery configs are published once per power cycle (guarded by `discoveryPublished` in RTC SRAM):

```
homeassistant/sensor/<DEVICE_ID>/co2/config
homeassistant/sensor/<DEVICE_ID>/temperature/config
homeassistant/sensor/<DEVICE_ID>/humidity/config
homeassistant/sensor/<DEVICE_ID>/battery_voltage/config
homeassistant/sensor/<DEVICE_ID>/battery_percent/config
```

All entities are grouped under a single HA device block. To force re-discovery, power-cycle the board (hard reset clears RTC SRAM).

### MQTT Notes

- PubSubClient buffer is set to 512 bytes — discovery payloads are ~330–345 bytes each.
- `_mqtt.disconnect()` is called before `WiFi.disconnect()` to ensure the TCP send buffer is flushed. Skipping this causes the last published message to be silently dropped.
- The `°F` unit in the temperature discovery payload must be sent as UTF-8 bytes (`\xc2\xb0F`). A bare `°` (Latin-1 0xB0) causes Mosquitto to drop the message and close the connection, losing all subsequent discovery payloads.

---

## Known Quirks

| Issue | Cause | Fix |
|-------|-------|-----|
| CO₂ always 0 | SCD41 first shot after power-on always returns 0 | Warmup shot pattern in `initSensors()` |
| Battery reads 0V | MAX17048 ADC not ready at first read | `quickStart()` + 125ms delay in `initBattery()` |
| MQTT discovery drops last entity | TCP send buffer not flushed before WiFi off | `_mqtt.disconnect()` + 100ms before `WiFi.disconnect()` |
| Mosquitto receives null retained messages | HA clears retained topics when a device is deleted | Don't delete HA device; power-cycle ESP32 to republish |
| Display blank after first flash | `DISPLAY_UPDATE_INTERVAL` (300s) > `SLEEP_DURATION_S` (155s); first cycle elapsed time fell short | Fixed — hard reset now forces an immediate update regardless of interval |
