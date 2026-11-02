#pragma once
#include "history.h"

struct DisplaySnapshot {
    SensorReadings readings;
    uint32_t uptimeS;
    HistoryBuffer history;
};
static_assert(std::is_trivially_copyable<DisplaySnapshot>::value, "Snapshot must remain RTC-safe");

void initDisplay();
void updateDisplay(const DisplaySnapshot &snapshot);
void hibernateDisplay();
void resetDisplayRefreshCount();
