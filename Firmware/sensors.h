#pragma once

#include <Arduino.h>

// Initialise SHT40 and SCD41, apply temperature offset, and fire the SCD41
// warmup single-shot (to be discarded per datasheet). Call this as early as
// possible in setup(). Returns true if both sensors were found and initialised.
bool initSensors();

// Wait for the warmup shot to complete, discard the result, then immediately
// trigger the real measurement. Call this after initSensors() and BEFORE
// connectWifi() so that WiFi connection overlaps with the ~5s measurement
// window, minimising total awake time with the radio on.
// Returns true if the real shot was successfully triggered.
bool triggerMeasurement();

// Wait for the real measurement result and read both sensors.
//   co2         — CO2 in ppm (from SCD41)
//   temperature — temperature in °C (from SHT40, authoritative)
//   humidity    — relative humidity in % (from SHT40, authoritative)
//   ok          — set to false if any sensor failed; individual fields will be 0
//
// Must be called after triggerMeasurement(). Blocks until the SCD41 result
// is ready (typically within a few ms if WiFi connection took ~5s).
void readSensors(float &co2, float &temperature, float &humidity, bool &ok);
