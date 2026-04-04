#pragma once

#include <Arduino.h>
#include "config.h"

// Initialise GxEPD2 display. Pin assignments come from config.h.
// Must be called before updateDisplay().
void initDisplay();

// Render the full display layout with current readings and history.
//   co2, temperature, humidity — current sensor readings
//   battV, battPct             — battery voltage and percent
//   readOk                     — if false, sensor values are shown as "—"
//   totalUptimeS               — used to derive HH:MM timestamp for status bar
//   co2Hist, tempHist, humHist — RTC circular history arrays
//   histCount, histHead        — circular buffer state
void updateDisplay(float co2, float temperature, float humidity,
                   float battV, float battPct, bool readOk,
                   uint32_t totalUptimeS,
                   uint16_t *co2Hist,
                   int16_t  *tempHist,
                   uint8_t  *humHist,
                   uint8_t   histCount,
                   uint8_t   histHead);

// Put the display into deep-sleep (low-power hibernation mode).
// Called by power.cpp before esp_deep_sleep_start().
void hibernateDisplay();

// Force the next updateDisplay() call to use a full refresh.
// Call this on any non-sleep wakeup (power-on, reset, firmware flash) to
// ensure a clean frame after RTC SRAM may contain a stale partial count.
void resetDisplayRefreshCount();
