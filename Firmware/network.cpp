#include "network.h"
#include "mqtt_transport.h"
#include <PubSubClient.h>
#include "debug.h"

static void expireMqttWait();

static BudgetWiFiClient _wifiClient(expireMqttWait);
static PubSubClient _mqtt(_wifiClient);
static bool _mqttAttempted = false;

static void expireMqttWait() {
    // PubSubClient otherwise restarts its socket timeout for each byte.
    _mqtt.setSocketTimeout(0);
}

bool connectWifi() {
    WiFi.mode(WIFI_STA);
    WiFi.setSleep(true);
    if (!WiFi.setTxPower(WIFI_TX_POWER)) LOG("[NET] TX power setting failed\n");

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
            WiFi.disconnect(true);
            WiFi.mode(WIFI_OFF);
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

bool connectMqtt() {
    if (_mqtt.connected()) return _wifiClient.remaining() > 0;
    if (_mqttAttempted) return false;
    _mqttAttempted = true;
    // Numeric address required: DNS resolution has a separate blocking timeout.
    IPAddress broker;
    if (!broker.fromString(MQTT_BROKER)) {
        LOG("[MQTT] MQTT_BROKER must be a numeric IP address\n");
        return false;
    }
    _mqtt.setServer(broker, MQTT_PORT);
    if (!_mqtt.setBufferSize(512)) return false;
    _wifiClient.startBudget();
    if (!_wifiClient.connect(broker, MQTT_PORT, (int32_t)_wifiClient.remaining())) return false;
    if (_wifiClient.remaining() == 0) { _wifiClient.stop(); return false; }
    _mqtt.setSocketTimeout((MQTT_TIMEOUT_MS + 999) / 1000);
    bool connected = strlen(MQTT_USER) > 0
        ? _mqtt.connect(DEVICE_ID, MQTT_USER, MQTT_PASS)
        : _mqtt.connect(DEVICE_ID);
    if (!connected || _wifiClient.remaining() == 0) {
        _wifiClient.stop();
        LOG("[MQTT] Connection failed or budget expired\n");
        return false;
    }
    LOG("[MQTT] Connected.\n");
    return true;
}

PubSubClient &mqttClient() { return _mqtt; }
