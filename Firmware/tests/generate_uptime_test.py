from pathlib import Path
p=Path(__file__).resolve().parents[1]
prefix=r'''
#include <stdint.h>
#include <assert.h>
#include <stdio.h>
struct timeval { int64_t tv_sec; int64_t tv_usec; };
static int64_t clockUs, bootUs;
static uint32_t lastDisplayUpdateS;
static int resetReason;
const int ESP_RST_DEEPSLEEP = 5;
int esp_reset_reason() { return resetReason; }
int64_t esp_timer_get_time() { return bootUs; }
int gettimeofday(timeval* t, void*) {
    t->tv_sec = clockUs / 1000000LL;
    t->tv_usec = clockUs % 1000000LL;
    return 0;
}

#define RTC_DATA_ATTR
'''
tests=r'''
int main() {
    clockUs = 900000000000LL + 250000; bootUs = 250000;
    resetReason = 1; initializeUptime();
    int64_t origin = uptimeOriginUs;
    assert(uptimeSeconds() == 0);
    clockUs = origin + 10900000;
    assert(uptimeSeconds() == 10); // No future sleep credited on first display.
    lastDisplayUpdateS = uptimeSeconds();
    clockUs += 1300000 + 156300000 + 12000000;
    resetReason = ESP_RST_DEEPSLEEP; bootUs = 12000000; initializeUptime();
    assert(uptimeOriginUs == origin);
    assert(uptimeSeconds() == 180); // Includes display/shutdown, actual sleep and next wake.
    assert(uptimeSeconds() - lastDisplayUpdateS < 300);
    clockUs += 169600000;
    assert(uptimeSeconds() - lastDisplayUpdateS >= 300);
    for (int i=0; i<1000; ++i) {
        clockUs += 166123456;
        initializeUptime();
        assert(uptimeSeconds() == (uint32_t)((clockUs-origin)/1000000LL));
    }
    clockUs = origin + 60LL * 24 * 3600 * 1000000;
    assert(uptimeSeconds() == 60UL * 24 * 3600); // Beyond millis() wrap.
    resetReason = 1; bootUs = 350000; initializeUptime();
    assert(uptimeSeconds() == 0);

    puts("PASS: first boot, variable awake/sleep, refresh scheduling, fractional retention, 60-day uptime, reset");
}
'''
source='\n'.join(line for line in (p/'uptime.cpp').read_text(encoding='utf-8').splitlines() if not line.startswith('#include'))
Path('uptime_test.cpp').write_text(prefix+'\n'+source+'\n'+tests,encoding='utf-8')
