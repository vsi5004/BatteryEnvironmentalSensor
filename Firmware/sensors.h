#pragma once

#include "readings.h"

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

// Finish the real measurement after Wi-Fi has had a chance to connect.
// Returns a value object; valid is false if either sensor failed.
EnvironmentalReadings readSensors();
