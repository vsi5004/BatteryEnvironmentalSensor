from pathlib import Path
p=Path(__file__).resolve().parents[1]
source='\n'.join(line for line in (p/'Firmware.ino').read_text(encoding='utf-8').splitlines() if not line.startswith('#include'))
prefix=r'''
#include <stdint.h>
#include <assert.h>
#include <stdio.h>
#include <vector>
#include <string>
#include <algorithm>
#include "DISPLAY_HEADER"
#define RTC_DATA_ATTR
#define DEBUG 0
#define LOG(...) ((void)0)
#define DISPLAY_UPDATE_INTERVAL 300
using esp_sleep_wakeup_cause_t=int;
constexpr int ESP_RST_DEEPSLEEP=5, ESP_SLEEP_WAKEUP_TIMER=1, ESP_SLEEP_WAKEUP_UNDEFINED=0;
static int resetReason=1, wakeReason=0;
static uint32_t nowS=10;
static bool sensorOk=true, shotOkMock=true, wifiOkMock=true, discoveryOk=true;
static std::vector<std::string> events;
static DisplaySnapshot lastFrame;
void event(const char* value) { events.emplace_back(value); }
uint32_t millis() { return nowS*1000; }
void delay(int) {}
int esp_reset_reason() { return resetReason; }
int esp_sleep_get_wakeup_cause() { return wakeReason; }
void initializeUptime() {} uint32_t uptimeSeconds() { return nowS; }
void setCpuFrequencyMhz(int) {}
struct WireMock { void begin() {} } Wire;
void initPower() { event("power"); }
bool initSensors() { event("warmup"); return sensorOk; }
void resetDisplayRefreshCount() { event("reset-display"); }
bool initBattery() { event("battery-init"); return true; }
BatteryReadings readBattery() { event("battery-read"); return {4,90}; }
bool triggerMeasurement() { event("real-shot"); return shotOkMock; }
bool connectWifi() { event("wifi"); return wifiOkMock; }
EnvironmentalReadings readSensors() { event("read"); return {500,20,50,true}; }
bool publishDiscovery() { event("discovery"); return discoveryOk; }
bool publishMQTT(const SensorReadings& r) { event(r.environment.valid?"publish":"battery-only"); return true; }
void disconnectWifi() { event("disconnect"); }
void initDisplay() { event("display-init"); }
void updateDisplay(const DisplaySnapshot& s) { event("display"); lastFrame=s; }
void enterDeepSleep() { event("sleep"); }
'''
tests=r'''
bool has(const char* value) { return std::find(events.begin(),events.end(),value)!=events.end(); }
size_t at(const char* value) { return std::find(events.begin(),events.end(),value)-events.begin(); }
int main() {
    setup();
    assert(at("warmup")<at("battery-read"));
    assert(at("battery-read")<at("real-shot"));
    assert(at("real-shot")<at("wifi") && at("wifi")<at("read"));
    assert(at("publish")<at("disconnect") && at("disconnect")<at("display"));
    assert(events.back()=="sleep" && state.history.count==1);
    assert(lastFrame.history.count==1 && lastFrame.readings.environment.co2Ppm==500);
    events.clear(); resetReason=ESP_RST_DEEPSLEEP; wakeReason=ESP_SLEEP_WAKEUP_TIMER; nowS=175;
    setup(); assert(!has("display") && !has("discovery") && state.history.count==2);
    events.clear(); nowS=340; setup(); assert(has("display") && state.history.count==3);
    events.clear(); sensorOk=false; nowS=505; setup();
    assert(!has("real-shot") && !has("read") && has("battery-only") && state.history.count==3);
    events.clear(); wifiOkMock=false; nowS=670; setup();
    assert(!has("publish") && !has("battery-only") && !has("discovery"));
    assert(has("display") && events.back()=="sleep");
    events.clear(); wifiOkMock=true; sensorOk=true; state.discoveryPublished=false; discoveryOk=false;
    nowS=835; setup(); assert(has("discovery") && !state.discoveryPublished);
    events.clear(); discoveryOk=true; nowS=1000; setup(); assert(has("discovery") && state.discoveryPublished);
    events.clear(); resetReason=1; wakeReason=ESP_SLEEP_WAKEUP_UNDEFINED; nowS=3; setup();
    assert(has("display") && state.lastDisplayUpdateS==3);
    puts("PASS: production wake ordering, sensor/Wi-Fi overlap, display cadence/reset, failed sensors/Wi-Fi, discovery retry");
}
'''
Path('wake_test.cpp').write_text(prefix.replace('DISPLAY_HEADER',(p/'display.h').as_posix())+'\n'+source+'\n'+tests,encoding='utf-8')
