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
// Allow the specified command duration before querying readiness.
static const uint32_t SCD41_POLL_START_MS   = 5000;
static const uint32_t SCD41_POLL_TIMEOUT_MS = 5100;

// The library's measureSingleShot() includes delay(5000). Send its command
// directly so battery setup / WiFi can overlap the sensor's conversion.
static bool startSingleShot(uint32_t &triggerMs) {
    triggerMs = millis();
    Wire.beginTransmission(0x62);
    Wire.write(0x21);
    Wire.write(0x9d);
    return Wire.endTransmission() == 0;
}

// Wait for data ready starting from a given trigger timestamp.
// Returns true if data is ready, false on timeout or I2C error.
static bool waitForDataReady(uint32_t triggerMs) {
    uint32_t elapsed = millis() - triggerMs;
    if (elapsed < SCD41_POLL_START_MS)
        delay(SCD41_POLL_START_MS - elapsed);

    bool dataReady = false;
    uint16_t err = 0;
    // Always check once, even if WiFi took longer than the conversion.
    // An overdue conversion gets no new polling window.
    do {
        err = _scd41.getDataReadyStatus(dataReady);
        if (err != 0 || dataReady) break;
        uint32_t elapsedMs = millis() - triggerMs;
        if (elapsedMs >= SCD41_POLL_TIMEOUT_MS) break;
        delay(min((uint32_t)10, SCD41_POLL_TIMEOUT_MS - elapsedMs));
    } while (true);
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
    // should be discarded per datasheet. Battery setup overlaps this shot;
    // the second shot cannot start until the warmup has completed.
    if (!startSingleShot(_warmupTriggerMs)) {
        LOG("[SEN] SCD41 warmup shot command failed\n");
        ok = false;
    } else {
        LOG("[SEN] SCD41 warmup shot triggered\n");
    }

    return ok;
}

bool triggerMeasurement() {
    // Wait for the warmup shot and drain the result without using it.
    if (!waitForDataReady(_warmupTriggerMs)) {
        Serial.println("[SEN] SCD41 warmup shot did not complete — skipping real shot");
        return false;
    }
    uint16_t dummy = 0; float dummyT = 0.0f, dummyH = 0.0f;
    if (_scd41.readMeasurement(dummy, dummyT, dummyH) != 0) return false;

    // Trigger the real measurement. WiFi will connect during this window.
    if (!startSingleShot(_realShotTriggerMs)) {
        LOG("[SEN] SCD41 real shot command failed\n");
        return false;
    }
    LOG("[SEN] SCD41 real shot triggered\n");
    return true;
}

EnvironmentalReadings readSensors() {
    EnvironmentalReadings result{};
    result.valid = true;

    if (!waitForDataReady(_realShotTriggerMs)) {
        result.valid = false;
        return result;
    }
    LOG("[SEN] SCD41 ready after %lu ms\n", millis() - _realShotTriggerMs);

    // --- SHT40 read (on-demand, no timing constraint) -----------------------
    sensors_event_t hum_event, temp_event;
    if (!_sht40.getEvent(&hum_event, &temp_event)) {
        Serial.println("[SEN] SHT40 read failed");
        result.valid = false;
    } else {
        result.temperatureC = temp_event.temperature;
        result.humidityPercent    = hum_event.relative_humidity;
        LOG("[SEN] SHT40: T=%.2f°C  H=%.2f%%\n", result.temperatureC, result.humidityPercent);
    }

    // --- SCD41: read real measurement result --------------------------------
    uint16_t co2Raw = 0;
    float    scd41T = 0.0f;
    float    scd41H = 0.0f;
    uint16_t err = _scd41.readMeasurement(co2Raw, scd41T, scd41H);
    if (err != 0) {
        Serial.printf("[SEN] SCD41 readMeasurement error: %u\n", err);
        result.valid = false;
    } else {
        result.co2Ppm = (float)co2Raw;
        LOG("[SEN] SCD41: CO2=%u ppm  T=%.2f°C  H=%.2f%%\n", co2Raw, scd41T, scd41H);
    }
    return result;
}
