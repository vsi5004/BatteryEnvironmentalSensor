#pragma once
#include <stdint.h>

// Call once per wake. Deep sleep preserves the origin; real resets restart it.
void initializeUptime();
uint32_t uptimeSeconds();
