#pragma once

// Plain data only: safe to copy into an RTC-retained display snapshot.
struct EnvironmentalReadings {
    float co2Ppm;
    float temperatureC;
    float humidityPercent;
    bool valid;
};

struct BatteryReadings {
    float voltageV;
    float chargePercent;
};

struct SensorReadings {
    EnvironmentalReadings environment;
    BatteryReadings battery;
};
