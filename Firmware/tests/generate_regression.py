from pathlib import Path
p=Path(__file__).resolve().parents[1]
sensors=(p/'sensors.cpp').read_text(encoding='utf-8')
network=(p/'mqtt_transport.h').read_text(encoding='utf-8')
display=(p/'display.cpp').read_text(encoding='utf-8')
sensor_code=sensors[sensors.index('static const uint32_t SCD41_POLL_START_MS'):sensors.index('bool initSensors()')]
network_code=network[network.index('class BudgetWiFiClient'):]
display_struct=display[display.index('RTC_DATA_ATTR static DisplaySnapshot'):display.index('static GxEPD2_BW')]
display_api=display[display.index('static void drawFrame('):]
pre=r'''
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>
#include <assert.h>
#include <errno.h>
#include <algorithm>
#include <type_traits>
#include "PROJECT_DISPLAY_HEADER"
using std::min;
#define RTC_DATA_ATTR
#define LOG(...) ((void)0)
#define MQTT_TIMEOUT_MS 5000
#define DISPLAY_FULL_REFRESH_EVERY 10
#define MSG_DONTWAIT 0
#ifndef EWOULDBLOCK
#define EWOULDBLOCK EAGAIN
#endif
static uint32_t nowMs;
uint32_t millis() { return nowMs; }
void delay(uint32_t n) { nowMs += n; }
struct SerialMock { template<class... T> void printf(const char*,T...) {} } Serial;
struct WireMock {
    int bytes[2], count=0, address=0, result=0;
    void beginTransmission(int a) { address=a; count=0; }
    void write(int b) { bytes[count++]=b; }
    int endTransmission() { return result; }
} Wire;
struct SensorMock {
    bool ready=false; int error=0, calls=0;
    uint16_t getDataReadyStatus(bool& out) { ++calls; ++nowMs; out=ready; return error; }
} _scd41;
static bool expired=false, blocked=false;
static int sentBytes=0;
void expireMqttWait() { expired=true; }
class WiFiClient {
public:
    bool stopped=false;
    virtual int available() { return 0; }
    virtual size_t write(uint8_t) { return 0; }
    virtual size_t write(const uint8_t*,size_t) { return 0; }
    void stop() { stopped=true; }
    int fd() { return 3; }
};
int send(int,const uint8_t*,size_t n,int) {
    if (blocked) { errno=EAGAIN; return -1; }
    int count=(int)min(n,(size_t)2); sentBytes+=count; return count;
}
#define GxEPD_BLACK 0
#define GxEPD_WHITE 1
struct GxEPD2_370_GDEY037T03 { static const int WIDTH=240, HEIGHT=416; };
static uint16_t _partialCount=0;
static int currentFrame,previousPlane,newPlane,refreshes,hibernations;
static bool wasPartial;
struct DisplayMock {
    bool partial=false;
    void init(int,bool initial,int,bool) { assert(!initial); }
    void setRotation(int) {} void setTextWrap(bool) {}
    void setFullWindow() { partial=false; }
    void setPartialWindow(int,int,int,int) { partial=true; }
    void firstPage() {}
    void fillScreen(int) { currentFrame=0; }
    bool nextPageToPrevious() { previousPlane=currentFrame; return false; }
    bool nextPage() { newPlane=currentFrame; wasPartial=partial; ++refreshes; return false; }
    void hibernate() { ++hibernations; }
} _display;
struct U8Mock { void begin(DisplayMock&) {} void setForegroundColor(int) {} void setBackgroundColor(int) {} } _u8g2;
void drawZone1(float t,float h,bool ok) { currentFrame+=(int)(t+h)+ok; }
void drawZone2(float co2,bool ok) { currentFrame+=(int)co2+ok; }
void drawZone3(const HistoryBuffer &history) {
    currentFrame+=history.count+history.head;
    for (int i=0;i<120;++i) currentFrame+=history.co2Ppm[i]+history.temperatureF10[i]+history.humidityPercent[i];
}
void drawZone4(float v,float p,bool ok,uint32_t u) { currentFrame+=(int)(v+p)+ok+u; }
'''
tests=r'''
int main() {
    HistoryBuffer history{};
    history.append({400,20,50,false}); assert(history.count==0 && history.head==0);
    for (int i=0;i<300;++i) history.append({400.0f+i,20,50,true});
    assert(history.count==120 && history.head==60);
    assert(history.co2Ppm[59]==699 && history.temperatureF10[59]==680 && history.humidityPercent[59]==50);
    nowMs=123; uint32_t trigger=0;
    assert(startSingleShot(trigger)); assert(trigger==123 && nowMs==123);
    assert(Wire.address==0x62 && Wire.count==2 && Wire.bytes[0]==0x21 && Wire.bytes[1]==0x9d);
    Wire.result=2; assert(!startSingleShot(trigger)); Wire.result=0;
    _scd41.ready=true; _scd41.calls=0; nowMs=123;
    assert(waitForDataReady(trigger)); assert(nowMs==5124 && _scd41.calls==1);
    nowMs=trigger+10000; _scd41.ready=false; _scd41.calls=0;
    assert(!waitForDataReady(trigger)); assert(nowMs==trigger+10001 && _scd41.calls==1);
    _scd41.ready=true; assert(waitForDataReady(trigger)); // Late but ready must succeed.
    _scd41.ready=false; nowMs=trigger;
    assert(!waitForDataReady(trigger)); assert(nowMs-trigger<=5101);
    trigger=0xfffffff0U; nowMs=trigger; _scd41.ready=true;
    assert(waitForDataReady(trigger)); assert((uint32_t)(nowMs-trigger)==5001);
    BudgetWiFiClient client(expireMqttWait); nowMs=0xfffffff0U; client.startBudget();
    uint8_t bytes[7]={}; blocked=false;
    assert(client.write(bytes,7)==7 && sentBytes==7);
    blocked=true; assert(client.write(bytes,7)==0);
    assert(client.remaining()==0 && client.stopped);
    assert(client.available()==0 && expired);
    hibernateDisplay(); assert(hibernations==0); // No SPI call on skipped wake.
    uint16_t co2[120]={}; int16_t temp[120]={}; uint8_t hum[120]={};
    resetDisplayRefreshCount(); initDisplay();
    for (int i=0;i<300;++i) {
        int oldFrame=newPlane;
        co2[i%120]=(uint16_t)(400+i); temp[i%120]=700+i; hum[i%120]=50;
        initDisplay();
        DisplaySnapshot snapshot{{{400.0f+i,20,50,true},{4,90}},(uint32_t)i*330,{}};
        memcpy(snapshot.history.co2Ppm,co2,sizeof(co2));
        memcpy(snapshot.history.temperatureF10,temp,sizeof(temp));
        memcpy(snapshot.history.humidityPercent,hum,sizeof(hum));
        snapshot.history.count=120; snapshot.history.head=i%120;
        updateDisplay(snapshot);
        assert(wasPartial==(i%10!=0));
        if (wasPartial) assert(previousPlane==oldFrame);
    }
    assert(refreshes==300); hibernateDisplay(); assert(hibernations==1);
    resetDisplayRefreshCount();
    DisplaySnapshot snapshot{{{400,20,50,true},{4,90}},0,{}};
    updateDisplay(snapshot); assert(!wasPartial);
    puts("PASS: nonblocking command, sensor deadlines/late readiness/wrap, blocked network writes/deadline/wrap, previous-frame restoration, 300 refreshes, reset, skipped display shutdown");
}
'''
pre=pre.replace('PROJECT_DISPLAY_HEADER', (p/'display.h').as_posix())
Path('regression.cpp').write_text(pre+'\n'+sensor_code+'\n'+network_code+'\n'+display_struct+'\n'+display_api+'\n'+tests)
