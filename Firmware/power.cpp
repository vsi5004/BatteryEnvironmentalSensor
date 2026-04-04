#include "power.h"
#include "config.h"
#include <Adafruit_MAX1704X.h>
#include <WiFi.h>
#include <esp_sleep.h>
#include "debug.h"

// Forward declaration — GxEPD2 display object is owned by display.cpp.
// Call display.hibernate() from there before enterDeepSleep().
extern void hibernateDisplay();

static Adafruit_MAX17048 _maxlipo;
static bool _batteryOk = false;

void initPower() {
    // Kill the NeoPixel immediately — it is never needed and wastes power.
    pinMode(NEOPIXEL_POWER, OUTPUT);
    digitalWrite(NEOPIXEL_POWER, LOW);

    // Ensure the STEMMA QT / I2C power rail is ON so sensors can be reached.
    pinMode(I2C_POWER, OUTPUT);
    enableSensors();

    LOG("[PWR] Power rails initialised. NeoPixel OFF, I2C_POWER ON.\n");
}

void enableSensors() {
    digitalWrite(I2C_POWER, HIGH);
}

void disableSensors() {
    digitalWrite(I2C_POWER, LOW);
    LOG("[PWR] I2C_POWER OFF.\n");
}

bool initBattery() {
    // MAX17048 is at 0x36, always powered — not gated by I2C_POWER.
    _batteryOk = _maxlipo.begin();
    if (!_batteryOk) {
        Serial.println("[PWR] MAX17048 not found!");
        return false;
    }
    // begin() issues a chip reset which clears the SOC register, so quickStart()
    // is required after every begin() call to re-initialise the state estimation.
    _maxlipo.quickStart();
    delay(125);
    LOG("[PWR] MAX17048 OK — raw: %.3fV  %.1f%%\n", _maxlipo.cellVoltage(), _maxlipo.cellPercent());
    return true;
}

bool readBattery(float &voltage, float &percent) {
    if (!_batteryOk) return false;
    voltage = _maxlipo.cellVoltage();
    percent = _maxlipo.cellPercent();
    return true;
}

void enterDeepSleep() {
    LOG("[PWR] Entering deep sleep...\n");
    LOG_FLUSH();

    // 1. Power down sensor rail.
    disableSensors();

    // 2. NeoPixel already LOW, but be explicit.
    digitalWrite(NEOPIXEL_POWER, LOW);

    // 3. Disconnect WiFi cleanly.
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);

    // 4. Hibernate the e-paper panel (implemented in display.cpp).
    hibernateDisplay();

    // 5. Configure wakeup timer and sleep.
    esp_sleep_enable_timer_wakeup((uint64_t)SLEEP_DURATION_S * 1000000ULL);
    esp_deep_sleep_start();
    // Never reaches here.
}
