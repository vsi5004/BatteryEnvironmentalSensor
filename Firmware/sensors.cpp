#include "sensors.h"
#include "config.h"
#include <Adafruit_SHT4x.h>
#include <SensirionI2cScd4x.h>
#include <Wire.h>
#include "debug.h"

static Adafruit_SHT4x    _sht40;
static SensirionI2cScd4x _scd41;

// Timestamps (ms) for each shot so we can compute elapsed time when needed.
static uint32_t _warmupTriggerMs  = 0;
static uint32_t _realShotTriggerMs = 0;

// Datasheet max command duration for measure_single_shot is 5000ms.
// We start polling for data ready at 3800ms and bail out at 5100ms.
static const uint32_t SCD41_POLL_START_MS   = 3800;
static const uint32_t SCD41_POLL_TIMEOUT_MS = 5100;

// Wait for data ready starting from a given trigger timestamp.
// Returns true if data is ready, false on timeout or I2C error.
static bool waitForDataReady(uint32_t triggerMs) {
    uint32_t elapsed = millis() - triggerMs;
    if (elapsed < SCD41_POLL_START_MS)
        delay(SCD41_POLL_START_MS - elapsed);

    bool dataReady = false;
    uint16_t err = 0;
    uint32_t pollStart = millis();
    while (millis() - pollStart < (SCD41_POLL_TIMEOUT_MS - SCD41_POLL_START_MS)) {
        err = _scd41.getDataReadyStatus(dataReady);
        if (err != 0 || dataReady) break;
        delay(10);
    }
    if (err != 0 || !dataReady) {
        Serial.printf("[SEN] SCD41 data not ready (err=%u, ready=%d)\n", err, dataReady);
        return false;
    }
    return true;
}

bool initSensors() {
    bool ok = true;

    // --- SHT40 --------------------------------------------------------------
    if (!_sht40.begin()) {
        Serial.println("[SEN] SHT40 not found!");
        ok = false;
    } else {
        _sht40.setPrecision(SHT4X_HIGH_PRECISION);
        _sht40.setHeater(SHT4X_NO_HEATER);
        LOG("[SEN] SHT40 OK\n");
    }

    // --- SCD41 --------------------------------------------------------------
    // I2C_POWER is cut every sleep cycle so the sensor always powers on fresh.
    // Datasheet specifies max 30ms power-up time after VDD ≥ 2.25V.
    delay(35);
    _scd41.begin(Wire, 0x62);

    if (_scd41.setTemperatureOffset(SCD41_TEMP_OFFSET) != 0) {
        Serial.println("[SEN] SCD41 setTemperatureOffset failed");
    }

    // Configure ASC periods for our ~155s cycle (default assumes 5-min intervals).
    // Read current values first — only write+persist if they differ, to protect
    // the EEPROM write cycle limit.
    {
        uint16_t curInitial = 0, curStandard = 0;
        bool needPersist = false;

        if (_scd41.getAutomaticSelfCalibrationInitialPeriod(curInitial) == 0 &&
            curInitial != SCD41_ASC_INITIAL_PERIOD_H) {
            _scd41.setAutomaticSelfCalibrationInitialPeriod(SCD41_ASC_INITIAL_PERIOD_H);
            needPersist = true;
        }
        if (_scd41.getAutomaticSelfCalibrationStandardPeriod(curStandard) == 0 &&
            curStandard != SCD41_ASC_STANDARD_PERIOD_H) {
            _scd41.setAutomaticSelfCalibrationStandardPeriod(SCD41_ASC_STANDARD_PERIOD_H);
            needPersist = true;
        }
        if (needPersist) {
            _scd41.persistSettings();
            Serial.printf("[SEN] SCD41 ASC periods updated (%uh / %uh) and persisted.\n",
                          SCD41_ASC_INITIAL_PERIOD_H, SCD41_ASC_STANDARD_PERIOD_H);
        } else {
            LOG("[SEN] SCD41 ASC periods already correct, skipping persist.\n");
        }
    }

    // Trigger a warmup shot immediately. The first single-shot after power-on
    // should be discarded per datasheet — firing it now lets it run while the
    // real shot triggers, so it doesn't add net awake time.
    uint16_t err = _scd41.measureSingleShot();
    if (err != 0) {
        Serial.printf("[SEN] SCD41 warmup shot error: %u\n", err);
        ok = false;
    } else {
        LOG("[SEN] SCD41 warmup shot triggered\n");
    }

    _warmupTriggerMs = millis();
    return ok;
}

bool triggerMeasurement() {
    // Wait for the warmup shot and drain the result without using it.
    if (!waitForDataReady(_warmupTriggerMs)) {
        Serial.println("[SEN] SCD41 warmup shot did not complete — skipping real shot");
        return false;
    }
    uint16_t dummy = 0; float dummyT = 0.0f, dummyH = 0.0f;
    _scd41.readMeasurement(dummy, dummyT, dummyH);  // discard

    // Trigger the real measurement. WiFi will connect during this window.
    uint16_t err = _scd41.measureSingleShot();
    if (err != 0) {
        Serial.printf("[SEN] SCD41 real shot error: %u\n", err);
        return false;
    }
    LOG("[SEN] SCD41 real shot triggered\n");
    _realShotTriggerMs = millis();
    return true;
}

void readSensors(float &co2, float &temperature, float &humidity, bool &ok) {
    co2 = 0.0f;
    temperature = 0.0f;
    humidity = 0.0f;
    ok = true;

    if (!waitForDataReady(_realShotTriggerMs)) {
        ok = false;
        return;
    }
    LOG("[SEN] SCD41 ready after %lu ms\n", millis() - _realShotTriggerMs);

    // --- SHT40 read (on-demand, no timing constraint) -----------------------
    sensors_event_t hum_event, temp_event;
    if (!_sht40.getEvent(&hum_event, &temp_event)) {
        Serial.println("[SEN] SHT40 read failed");
        ok = false;
    } else {
        temperature = temp_event.temperature;
        humidity    = hum_event.relative_humidity;
        LOG("[SEN] SHT40: T=%.2f°C  H=%.2f%%\n", temperature, humidity);
    }

    // --- SCD41: read real measurement result --------------------------------
    uint16_t co2Raw = 0;
    float    scd41T = 0.0f;
    float    scd41H = 0.0f;
    uint16_t err = _scd41.readMeasurement(co2Raw, scd41T, scd41H);
    if (err != 0) {
        Serial.printf("[SEN] SCD41 readMeasurement error: %u\n", err);
        ok = false;
    } else {
        co2 = (float)co2Raw;
        LOG("[SEN] SCD41: CO2=%u ppm  T=%.2f°C  H=%.2f%%\n", co2Raw, scd41T, scd41H);
    }
}
