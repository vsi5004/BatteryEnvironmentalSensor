#include <Arduino.h>
#include <Wire.h>
#include "config.h"
#include "power.h"
#include "sensors.h"
#include "mqtt_ha.h"
#include "display.h"
#include "debug.h"


RTC_DATA_ATTR uint32_t lastDisplayUpdateS = 0;
RTC_DATA_ATTR uint32_t totalUptimeS       = 0;

#define HISTORY_LEN 120
RTC_DATA_ATTR uint16_t co2History[HISTORY_LEN];   // raw ppm
RTC_DATA_ATTR int16_t  tempHistory[HISTORY_LEN];  // °F × 10  (e.g. 724 = 72.4°F)
RTC_DATA_ATTR uint8_t  humHistory[HISTORY_LEN];   // 0–100 %RH
RTC_DATA_ATTR uint8_t  historyCount = 0;          // valid samples (saturates at HISTORY_LEN)
RTC_DATA_ATTR uint8_t  historyHead  = 0;          // next write index
RTC_DATA_ATTR bool discoveryPublished = false;

static void appendHistory(float co2, float temp, float hum) {
    co2History[historyHead]  = (uint16_t)co2;
    float tempF = temp * 9.0f / 5.0f + 32.0f;
    tempHistory[historyHead] = (int16_t)(tempF * 10.0f);
    humHistory[historyHead]  = (uint8_t)hum;
    historyHead = (historyHead + 1) % HISTORY_LEN;
    if (historyCount < HISTORY_LEN) historyCount++;
}

static void logWakeReason() {
    esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
    switch (cause) {
        case ESP_SLEEP_WAKEUP_TIMER:     LOG("[WAKE] Timer wakeup\n"); break;
        case ESP_SLEEP_WAKEUP_UNDEFINED: LOG("[WAKE] Power-on / hard reset\n"); break;
        default:                         LOG("[WAKE] Wakeup cause: %d\n", cause); break;
    }
}

void setup() {
    // Run at 80MHz — the awake window is dominated by delay() calls waiting on
    // the sensor and WiFi, so lower frequency costs nothing in wall-clock time
    // and roughly halves active CPU current draw.
    setCpuFrequencyMhz(80);

#if DEBUG
    Serial.begin(115200);
    delay(100);   // brief settle for USB serial
    Serial.println("\n[BOOT] envsensor wakeup");
#endif

    uint32_t wakeStartMs = millis();

    Wire.begin();   // must be called before any I2C device init
    initPower();

    // Start sensor init immediately — the SCD41 needs 1000ms after power-on
    // before it accepts commands, and the warmup shot must be discarded before
    // the real measurement. Firing this ASAP lets all subsequent init (battery,
    // display reset, etc.) overlap with the warmup window.
    bool sensorsOk = initSensors();

    logWakeReason();

    // On any non-sleep wakeup (power-on, reset, firmware flash) force a full
    // display refresh — RTC SRAM may hold a stale partial-refresh counter.
    if (esp_sleep_get_wakeup_cause() != ESP_SLEEP_WAKEUP_TIMER) {
        resetDisplayRefreshCount();
    }
    float battV = 0.0f, battPct = 0.0f;
    initBattery();
    readBattery(battV, battPct);

    LOG("[BAT] %.2fV  %.1f%%\n", battV, battPct);

    // Discard the warmup shot, trigger the real measurement, then connect WiFi
    // so the radio is only on during the real measurement window (~5s).
    bool shotOk = sensorsOk && triggerMeasurement();
    bool wifiOk = connectWifi();
    float co2 = 0.0f, temperature = 0.0f, humidity = 0.0f;
    bool readOk = false;

    if (shotOk) {
        readSensors(co2, temperature, humidity, readOk);
    }

    if (readOk)
        LOG("[SEN] CO2=%.0f ppm  T=%.1f°C  H=%.1f%%\n", co2, temperature, humidity);
    else
        LOG("[SEN] Sensor read failed.\n");

    if (readOk) {
        appendHistory(co2, temperature, humidity);
    }

    bool mqttOk = false;
    if (wifiOk) {
        if (!discoveryPublished) {
            publishDiscovery();
            discoveryPublished = true;
        }
        mqttOk = publishMQTT(co2, temperature, humidity, battV, battPct, readOk);
        disconnectWifi();
    }

    LOG("[NET] WiFi=%s  MQTT=%s\n", wifiOk ? "OK" : "FAIL", mqttOk ? "OK" : "FAIL");

    totalUptimeS += SLEEP_DURATION_S;

    bool doDisplayUpdate = (esp_sleep_get_wakeup_cause() != ESP_SLEEP_WAKEUP_TIMER) ||
                           (totalUptimeS - lastDisplayUpdateS) >= DISPLAY_UPDATE_INTERVAL;
    if (doDisplayUpdate) {
        initDisplay();
        updateDisplay(co2, temperature, humidity, battV, battPct,
                      readOk, totalUptimeS,
                      co2History, tempHistory, humHistory,
                      historyCount, historyHead);
        lastDisplayUpdateS = totalUptimeS;
    }

    LOG("[TIME] Awake for %lu ms\n", millis() - wakeStartMs);
    enterDeepSleep();
}

void loop() {}
