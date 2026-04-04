#include "mqtt_ha.h"
#include "config.h"
#include <WiFi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include "debug.h"

#define STATE_TOPIC  "homeassistant/sensor/" DEVICE_ID "/state"

static WiFiClient   _wifiClient;
static PubSubClient _mqtt(_wifiClient);

bool connectWifi() {
    WiFi.setSleep(true);
    WiFi.setTxPower(WIFI_TX_POWER);

#if WIFI_STATIC_IP
    IPAddress ip, gateway, subnet, dns;
    ip.fromString(WIFI_IP);
    gateway.fromString(WIFI_GATEWAY);
    subnet.fromString(WIFI_SUBNET);
    dns.fromString(WIFI_DNS);
    WiFi.config(ip, gateway, subnet, dns);
#endif

    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

    LOG("[NET] Connecting to %s", WIFI_SSID);

    uint32_t start = millis();
    while (WiFi.status() != WL_CONNECTED) {
        if (millis() - start > WIFI_TIMEOUT_MS) {
            LOG("\n[NET] WiFi timeout.\n");
            return false;
        }
        delay(100);
        LOG(".");
    }

    LOG("\n[NET] Connected. IP: %s\n", WiFi.localIP().toString().c_str());
    return true;
}

void disconnectWifi() {
    _mqtt.disconnect();   // graceful MQTT DISCONNECT + TCP FIN, drains send buffer
    delay(100);
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
}

static bool mqttConnect() {
    _mqtt.setServer(MQTT_BROKER, MQTT_PORT);
    _mqtt.setBufferSize(512);

    uint32_t start = millis();
    while (!_mqtt.connected()) {
        if (millis() - start > MQTT_TIMEOUT_MS) {
            Serial.println("[MQTT] Connection timeout.");
            return false;
        }
        bool connected;
        if (strlen(MQTT_USER) > 0) {
            connected = _mqtt.connect(DEVICE_ID, MQTT_USER, MQTT_PASS);
        } else {
            connected = _mqtt.connect(DEVICE_ID);
        }
        if (!connected) {
            delay(200);
        }
    }
    LOG("[MQTT] Connected.\n");
    return true;
}

bool publishMQTT(float co2, float temperature, float humidity,
                 float battV, float battPct, bool readOk) {
    if (!mqttConnect()) return false;

    // Allocate document large enough for the payload.
    JsonDocument doc;

    if (readOk) {
        doc["co2"]         = (int)co2;
        float tempF = temperature * 9.0f / 5.0f + 32.0f;
        doc["temperature"] = round(tempF * 10.0f) / 10.0f;
        doc["humidity"]    = round(humidity    * 10.0f) / 10.0f;
    }
    doc["battery_v"]   = round(battV   * 100.0f) / 100.0f;
    doc["battery_pct"] = round(battPct * 10.0f)  / 10.0f;

    char buf[256];
    size_t len = serializeJson(doc, buf, sizeof(buf));

    bool ok = _mqtt.publish(STATE_TOPIC, buf, /*retained=*/false);

    LOG("[MQTT] Publish %s: %s  [%s]\n", STATE_TOPIC, buf, ok ? "OK" : "FAIL");
    return ok;
}

// Publish a single discovery config message.
static void publishEntityDiscovery(
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

    bool ok = _mqtt.publish(topic, (uint8_t *)buf, strlen(buf), /*retained=*/true);

    LOG("[MQTT] Discovery %s: %s (%zu bytes)\n", ok ? "OK" : "FAIL", topic, strlen(buf));
}

void publishDiscovery() {
    if (!mqttConnect()) return;

    publishEntityDiscovery("co2",             "CO2",             "carbon_dioxide",   "ppm",  "{{ value_json.co2 }}");
    publishEntityDiscovery("temperature",     "Temperature",     "temperature",      "\xc2\xb0""F", "{{ value_json.temperature }}");
    publishEntityDiscovery("humidity",        "Humidity",        "humidity",         "%",    "{{ value_json.humidity }}");
    publishEntityDiscovery("battery_voltage", "Battery Voltage", "voltage",          "V",    "{{ value_json.battery_v }}");
    publishEntityDiscovery("battery_percent", "Battery",         "battery",          "%",    "{{ value_json.battery_pct }}");
    delay(100);  // let TCP send buffer drain before next operation

    LOG("[MQTT] Discovery payloads published.\n");
}
