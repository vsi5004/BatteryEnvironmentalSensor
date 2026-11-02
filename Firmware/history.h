#pragma once
#include <stdint.h>
#include <type_traits>
#include "readings.h"

// Fixed storage: no pointers, allocation or constructor work across deep sleep.
struct HistoryBuffer {
    static constexpr uint8_t capacity = 120;
    uint16_t co2Ppm[capacity];
    int16_t temperatureF10[capacity];
    uint8_t humidityPercent[capacity];
    uint8_t count;
    uint8_t head;

    void append(const EnvironmentalReadings &sample) {
        if (!sample.valid) return;
        co2Ppm[head] = (uint16_t)sample.co2Ppm;
        temperatureF10[head] = (int16_t)((sample.temperatureC * 9.0f / 5.0f + 32.0f) * 10.0f);
        humidityPercent[head] = (uint8_t)sample.humidityPercent;
        head = (head + 1) % capacity;
        if (count < capacity) ++count;
    }
};
static_assert(std::is_trivially_copyable<HistoryBuffer>::value, "History must remain RTC-safe");
