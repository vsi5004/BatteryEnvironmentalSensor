#pragma once
class PubSubClient;

bool connectWifi();
void disconnectWifi();
// One MQTT attempt per wake, shared by all publishers.
bool connectMqtt();
PubSubClient &mqttClient();
