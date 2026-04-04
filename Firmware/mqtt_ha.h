#pragma once

#include <Arduino.h>

// Connect to WiFi using credentials from config.h.
// Returns true if connected within WIFI_TIMEOUT_MS.
bool connectWifi();

// Disconnect WiFi cleanly (call before deep sleep).
void disconnectWifi();

// Publish sensor readings as a JSON payload to the HA state topic.
// Pass readOk=false to omit sensor fields (publishes only battery data).
// Returns true on successful publish.
bool publishMQTT(float co2, float temperature, float humidity,
                 float battV, float battPct, bool readOk);

// Publish Home Assistant MQTT auto-discovery config messages (retained).
// Call once per power cycle when discoveryPublished == false.
void publishDiscovery();
