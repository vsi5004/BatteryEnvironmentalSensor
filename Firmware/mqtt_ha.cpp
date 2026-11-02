#include "mqtt_ha.h"
#include "network.h"
#include "config.h"
#include "debug.h"
#include <PubSubClient.h>
#include <ArduinoJson.h>

#define STATE_TOPIC "homeassistant/sensor/" DEVICE_ID "/state"

bool publishMQTT(const SensorReadings &readings) {
    if (!connectMqtt()) return false;
    const auto &environment = readings.environment;
    const auto &battery = readings.battery;

    // Allocate document large enough for the payload.
    JsonDocument doc;

    if (environment.valid) {
        doc["co2"]         = (int)environment.co2Ppm;
        float tempF = environment.temperatureC * 9.0f / 5.0f + 32.0f;
        doc["temperature"] = round(tempF * 10.0f) / 10.0f;
        doc["humidity"]    = round(environment.humidityPercent    * 10.0f) / 10.0f;
    }
    doc["battery_v"]   = round(battery.voltageV   * 100.0f) / 100.0f;
    doc["battery_pct"] = round(battery.chargePercent * 10.0f)  / 10.0f;

    char buf[256];
    size_t len = serializeJson(doc, buf, sizeof(buf));

    bool ok = mqttClient().publish(STATE_TOPIC, buf, /*retained=*/false);

    LOG("[MQTT] Publish %s: %s  [%s]\n", STATE_TOPIC, buf, ok ? "OK" : "FAIL");
    return ok;
}

// Publish a single discovery config message.
static bool publishEntityDiscovery(
        const char *entityId,
        const char *name,
        const char *deviceClass,
        const char *unit,
        const char *valueTemplate)
{
    char topic[128];
    snprintf(topic, sizeof(topic),
             "homeassistant/sensor/" DEVICE_ID "/%s/config", entityId);

    JsonDocument doc;
    doc["name"]                = name;
    doc["unique_id"]           = String(DEVICE_ID) + "_" + entityId;
    doc["device_class"]        = deviceClass;
    doc["unit_of_measurement"] = unit;
    doc["state_topic"]         = STATE_TOPIC;
    doc["value_template"]      = valueTemplate;

    JsonObject device = doc["device"].to<JsonObject>();
    device["identifiers"][0] = DEVICE_ID;
    device["name"]           = "EnvSensor " DEVICE_ID;
    device["manufacturer"]   = "DIY";
    device["model"]          = "ESP32-S3 Feather";

    char buf[512];
    serializeJson(doc, buf, sizeof(buf));

    bool ok = mqttClient().publish(topic, (uint8_t *)buf, strlen(buf), /*retained=*/true);

    LOG("[MQTT] Discovery %s: %s (%zu bytes)\n", ok ? "OK" : "FAIL", topic, strlen(buf));
    return ok;
}

bool publishDiscovery() {
    if (!connectMqtt()) return false;
    bool ok = true;

    ok &= publishEntityDiscovery("co2",             "CO2",             "carbon_dioxide",   "ppm",  "{{ value_json.co2 }}");
    ok &= publishEntityDiscovery("temperature",     "Temperature",     "temperature",      "\xc2\xb0""F", "{{ value_json.temperature }}");
    ok &= publishEntityDiscovery("humidity",        "Humidity",        "humidity",         "%",    "{{ value_json.humidity }}");
    ok &= publishEntityDiscovery("battery_voltage", "Battery Voltage", "voltage",          "V",    "{{ value_json.battery_v }}");
    ok &= publishEntityDiscovery("battery_percent", "Battery",         "battery",          "%",    "{{ value_json.battery_pct }}");
    delay(100);  // let TCP send buffer drain before next operation

    LOG("[MQTT] Discovery complete: %s\n", ok ? "OK" : "FAIL");
    return ok;
}
