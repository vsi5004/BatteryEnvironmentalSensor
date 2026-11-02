#include "uptime.h"
#include <Arduino.h>
#include <sys/time.h>
#include <esp_system.h>
#include <esp_timer.h>

RTC_DATA_ATTR static int64_t uptimeOriginUs = 0;

// gettimeofday() uses the RTC during deep sleep and the high-resolution
// timer while awake. No network clock synchronisation is used by this sketch.
static int64_t systemTimeUs() {
    struct timeval now;
    gettimeofday(&now, nullptr);
    return (int64_t)now.tv_sec * 1000000LL + now.tv_usec;
}

uint32_t uptimeSeconds() {
    return (uint32_t)((systemTimeUs() - uptimeOriginUs) / 1000000LL);
}

void initializeUptime() {
    if (esp_reset_reason() != ESP_RST_DEEPSLEEP)
        uptimeOriginUs = systemTimeUs() - esp_timer_get_time();
}
