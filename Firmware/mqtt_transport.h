#pragma once
#include <WiFi.h>
#include <lwip/sockets.h>
#include <errno.h>
#include "config.h"

// Adapter required by PubSubClient: one budget across connect, writes and reads.
class BudgetWiFiClient : public WiFiClient {
public:
    explicit BudgetWiFiClient(void (*onDeadline)()) : _onDeadline(onDeadline) {}
    void startBudget() { _start = millis(); _armed = true; }
    uint32_t remaining() const {
        uint32_t elapsed = millis() - _start;
        return elapsed >= MQTT_TIMEOUT_MS ? 0 : MQTT_TIMEOUT_MS - elapsed;
    }
    int available() override {
        if (_armed && remaining() == 0) {
            _onDeadline();
            stop();
            return 0;
        }
        return WiFiClient::available();
    }
    size_t write(uint8_t value) override { return write(&value, 1); }
    size_t write(const uint8_t *buffer, size_t size) override {
        size_t sent = 0;
        while (sent < size && remaining() > 0) {
            int n = ::send(fd(), buffer + sent, size - sent, MSG_DONTWAIT);
            if (n > 0) sent += (size_t)n;
            else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) delay(1);
            else break;
        }
        if (sent != size) stop();
        return sent;
    }
private:
    void (*_onDeadline)();
    uint32_t _start = 0;
    bool _armed = false;
};
