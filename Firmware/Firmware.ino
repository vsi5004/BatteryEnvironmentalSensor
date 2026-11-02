#include <Arduino.h>
#include <Wire.h>
#include <esp_system.h>
#include "config.h"
#include "power.h"
#include "sensors.h"
#include "network.h"
#include "mqtt_ha.h"
#include "display.h"
#include "uptime.h"
#include "debug.h"

struct RetainedState {
    HistoryBuffer history;
    uint32_t lastDisplayUpdateS;
    bool discoveryPublished;
};
RTC_DATA_ATTR static RetainedState state{};

static void logWakeReason() {
    esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
    switch (cause) {
        case ESP_SLEEP_WAKEUP_TIMER:     LOG("[WAKE] Timer wakeup\n"); break;
        case ESP_SLEEP_WAKEUP_UNDEFINED: LOG("[WAKE] Power-on / hard reset\n"); break;
        default:                         LOG("[WAKE] Wakeup cause: %d\n", cause); break;
    }
}

static void logReadings(const SensorReadings &readings) {
    const auto &environment = readings.environment;
    if (environment.valid)
        LOG("[SEN] CO2=%.0f ppm  T=%.1f°C  H=%.1f%%\n",
            environment.co2Ppm, environment.temperatureC, environment.humidityPercent);
    else
        LOG("[SEN] Sensor read failed.\n");
}

static bool publishReadings(const SensorReadings &readings) {
    if (!state.discoveryPublished)
        state.discoveryPublished = publishDiscovery();
    bool ok = publishMQTT(readings);
    disconnectWifi();
    return ok;
}

static void refreshDisplayIfDue(const SensorReadings &readings) {
    bool firstWake = esp_sleep_get_wakeup_cause() != ESP_SLEEP_WAKEUP_TIMER;
    if (!firstWake && uptimeSeconds() - state.lastDisplayUpdateS < DISPLAY_UPDATE_INTERVAL)
        return;
    initDisplay();
    DisplaySnapshot snapshot{readings, uptimeSeconds(), state.history};
    updateDisplay(snapshot);
    state.lastDisplayUpdateS = snapshot.uptimeS;
}

void setup() {
    initializeUptime();
    if (esp_reset_reason() != ESP_RST_DEEPSLEEP) state.lastDisplayUpdateS = 0;
    setCpuFrequencyMhz(80); // Most awake time waits on sensors / Wi-Fi.
#if DEBUG
    Serial.begin(115200);
    delay(100);
    Serial.println("\n[BOOT] envsensor wakeup");
#endif
    uint32_t wakeStartMs = millis();
    Wire.begin();
    initPower();

    // Warmup conversion overlaps battery setup.
    bool sensorsOk = initSensors();
    logWakeReason();
    if (esp_sleep_get_wakeup_cause() != ESP_SLEEP_WAKEUP_TIMER)
        resetDisplayRefreshCount();
    SensorReadings readings{};
    initBattery();
    readings.battery = readBattery();
    LOG("[BAT] %.2fV  %.1f%%\n", readings.battery.voltageV, readings.battery.chargePercent);

    // Real conversion overlaps Wi-Fi. Keep this ordering visible.
    bool shotOk = sensorsOk && triggerMeasurement();
    bool wifiOk = connectWifi();
    if (shotOk) readings.environment = readSensors();
    logReadings(readings);
    state.history.append(readings.environment);

    bool mqttOk = wifiOk && publishReadings(readings);
    LOG("[NET] WiFi=%s  MQTT=%s\n", wifiOk ? "OK" : "FAIL", mqttOk ? "OK" : "FAIL");
    refreshDisplayIfDue(readings);
    LOG("[TIME] Awake for %lu ms\n", millis() - wakeStartMs);
    enterDeepSleep();
}

void loop() {}
