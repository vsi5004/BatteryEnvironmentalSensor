#pragma once

#include <Arduino.h>

// Initialise power rails: pull NeoPixel power LOW immediately, enable I2C rail.
void initPower();

// Enable the STEMMA QT 3.3V rail (powers SHT40 + SCD41).
void enableSensors();

// Disable the STEMMA QT 3.3V rail (call before deep sleep).
void disableSensors();

// Initialise the MAX17048 fuel gauge. Call after I2C is available.
// Returns true on success.
bool initBattery();

// Read battery voltage (V) and state-of-charge (%).
// Returns false if the fuel gauge is unavailable.
bool readBattery(float &voltage, float &percent);

// Full shutdown sequence, then enter deep sleep for SLEEP_DURATION_S seconds.
// Never returns.
void enterDeepSleep();
