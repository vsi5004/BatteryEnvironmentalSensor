#pragma once
#include "readings.h"

// Home Assistant message construction. Connection policy belongs to network.cpp.
bool publishMQTT(const SensorReadings &readings);
// True only if all retained discovery messages were accepted for publication.
bool publishDiscovery();
