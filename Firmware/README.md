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

### Wake cycle

`Firmware.ino` coordinates one wake cycle; `loop()` is empty. The order is intentional:

1. Initialize elapsed uptime, I2C and power rails.
2. Start the SCD41 warmup; initialize/read the battery while it converts.
3. Drain the warmup and start the real conversion.
4. Connect Wi-Fi while that conversion runs, then read the result.
5. Append valid readings to history and publish Home Assistant messages.
6. Disconnect Wi-Fi, refresh the display if due, then enter deep sleep.

`logReadings()`, `publishReadings()` and `refreshDisplayIfDue()` keep details out of the coordinator. Do not combine the sensor phases into a blocking read followed by Wi-Fi: that would lose the overlap and waste awake time.

### Data and ownership

- `EnvironmentalReadings`, `BatteryReadings` and `SensorReadings` are value objects with units in their field names. Sensor/power functions return them; publishing and display functions consume them.
- `HistoryBuffer` owns fixed arrays, count/head, validity filtering and unit conversion. It has no heap allocation, pointers or runtime initialization.
- `RetainedState` in the sketch groups history, the last-display timestamp and discovery status. Hardware objects remain separate and are recreated on each wake.
- `DisplaySnapshot` groups the readings, history and elapsed time for one frame. Both previous-image restoration and current-image drawing call the same `drawFrame()` renderer.
- `uptime.cpp` owns the retained clock origin. ON time includes awake work and deep sleep; non-deep-sleep resets restart it. Displayed time changes only on refresh. RTC oscillator drift remains. Adding SNTP or `settimeofday()` requires adjusting the origin too.

History and display snapshots are compile-time checked as trivially copyable so they remain suitable for RTC memory. No inheritance hierarchy is needed for these data types. The only adapter subclass is `BudgetWiFiClient`, required by PubSubClient's client API.

### File responsibilities

| File | Responsibility |
| --- | --- |
| `Firmware.ino` | Wake orchestration and retained application state |
| `readings.h` | Named reading values and units |
| `history.h` | Fixed circular history storage and append logic |
| `uptime.h/.cpp` | RTC-backed elapsed time |
| `sensors.h/.cpp` | SHT40/SCD41 initialization and measurement phases |
| `power.h/.cpp` | Power rails, battery gauge and shutdown |
| `network.h/.cpp` | Wi-Fi lifecycle and shared MQTT connection policy |
| `mqtt_transport.h` | Deadline-aware WiFiClient adapter |
| `mqtt_ha.h/.cpp` | Home Assistant discovery and state JSON |
| `display.h/.cpp` | Snapshots, rendering and refresh policy |
| `config.h` | Private credentials and tuning; unchanged by refactoring |
| `debug.h` | Logging helpers; includes its own configuration dependency |
| `tests/` | Host regressions generated from production code |

120 samples span roughly 5.5 hours at a 155-second sleep plus a 10-second awake cycle; network delays and failed samples change that span.

---

## Sensors

### SCD41 — Warmup Shot Pattern

The SCD41 datasheet recommends discarding the first single-shot measurement after a cold start. This is implemented with a two-shot pattern:

1. `initSensors()`: wait 35ms after power-on (datasheet max: 30ms), then fire a **warmup shot**. This shot is discarded per the Sensirion datasheet recommendation.
2. Other init (battery gauge etc.) runs during the 5s warmup window.
3. `triggerMeasurement()`: waits for the warmup shot to complete, discards it, fires the **real shot**.
4. WiFi connects during the ~5s real shot window.
5. `readSensors()`: polls for the real shot result (starts polling at 5s, timeout at 5.1s) then reads both sensors.

Battery setup overlaps the warmup; Wi-Fi connects during the real conversion. The two conversions require about 10 seconds total.

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
| CO₂ first reading unreliable | SCD41 datasheet recommends discarding first single-shot on cold start | Warmup shot pattern in `initSensors()` |
| Battery reads 0V | MAX17048 ADC not ready at first read | `quickStart()` + 125ms delay in `initBattery()` |
| MQTT discovery drops last entity | TCP send buffer not flushed before WiFi off | `_mqtt.disconnect()` + 100ms before `WiFi.disconnect()` |
| Mosquitto receives null retained messages | HA clears retained topics when a device is deleted | Don't delete HA device; power-cycle ESP32 to republish |
| Display blank after first flash | `DISPLAY_UPDATE_INTERVAL` (300s) > `SLEEP_DURATION_S` (155s); first cycle elapsed time fell short | Fixed — hard reset now forces an immediate update regardless of interval |


## Timing and power fixes (November 2026)

- SCD41 single-shot commands are sent directly over Wire (0x219d). The installed Sensirion library 1.1.0 blocks for 5000 ms in `measureSingleShot()`, so it must not be used as an asynchronous trigger. Battery initialization overlaps the warmup; Wi-Fi overlaps the real shot. Readiness polling begins at 5000 ms and ends at 5100 ms from command dispatch. Late callers check once without starting another wait window.
- Wi-Fi station mode starts before setting TX power. A failed connection turns the radio off immediately.
- MQTT has one shared attempt/budget per wake, including discovery and state publication. TCP connect uses the remaining budget; nonblocking socket writes and a deadline-aware availability check also bound stalled/fragmented replies. `MQTT_BROKER` must be a numeric IP address, avoiding an independent blocking DNS lookup. Discovery is marked complete only when all messages were accepted for publication (QoS 0 does not confirm broker delivery).
- Display initialization disables the library's implicit first-refresh behavior. Full refreshes explicitly seed both image planes. Partial refreshes reconstruct the previous image from retained render inputs before writing the new image, so controller RAM need not survive hibernation. The saved snapshot uses roughly 640 bytes of RTC RAM. Full-refresh cadence uses a bounded counter rather than overflowing at 256 updates. No display commands run on wakes that did not initialize the display.
- ON time uses elapsed RTC-backed system time, including awake work. No network time synchronization is enabled.

### Regression checks

`tests/generate_regression.py` extracts the production timing, network budget and display functions into a host test with fake hardware, using the real reading/history types. `tests/generate_wake_test.py` exercises the production sketch against fake hardware to verify phase ordering, display cadence, sensor/Wi-Fi failure paths and discovery retries. `tests/generate_uptime_test.py` checks RTC-backed uptime through sleep and reset. Run each generator from a scratch/build directory, compile its generated `regression.cpp`, `wake_test.cpp` or `uptime_test.cpp` with a C++11-or-newer compiler, then run the executable. The tests cover delayed sensor reads, failed commands, millisecond wraparound, blocked socket writes, history wraparound, previous-image reconstruction, 300 display updates, resets and skipped display shutdown.

A successful firmware build validates compilation/linking, not display ghosting or measured battery life. Verify partial refreshes and wake current on hardware before treating power savings as measured.

### Verified build and hardware

Built November 2, 2026 in local Docker with Arduino CLI 1.3.1, Arduino-ESP32 3.3.8 and the real configuration: 1,110,619 bytes flash (38%), 89,232 bytes RAM (27%). All three host regression suites passed. The Linux toolchain avoids the Windows compiler DLL loading error.

```sh
arduino-cli compile --fqbn esp32:esp32:adafruit_feather_esp32s3 --jobs 2 --build-path /build/compiled /build/Firmware
```

USB flashing passed hash verification. Sensor readings, MQTT publication and display updates were confirmed on the board. After flashing from download mode, tap RESET without holding BOOT if the firmware does not start. Battery lifetime and long-term display ghosting remain unmeasured.
