#ifndef MQTT_HANDLER_H
#define MQTT_HANDLER_H

#include <Arduino.h>
#include <PubSubClient.h>
#include "AcState.h"

// Konfigurace MQTT (včetně AUTH)
struct MqttConfig {
  String host;
  uint16_t port;
  String clientId;
  String baseTopic;   // např. "toshiba/ac"
  String user;
  String pass;
  bool enabled;
};

// Callback, který se zavolá, když z MQTT přijde změna stavu,
// kterou je potřeba promítnout do IR + feedback.
// Implementuješ ho v main jako např. onExternalAcStateChanged().
typedef void (*AcStateChangedCallback)();

void MqttHandler_init(Client &netClient, AcStateChangedCallback cb);

// Nastavení/změna konfigurace (přes webUI apod.)
void MqttHandler_setConfig(const MqttConfig &cfg);
const MqttConfig &MqttHandler_getConfig();

// Volat v loop()
void MqttHandler_loop();

// Publikace aktuálního stavu do MQTT (state topic), např. po odeslání IR
void MqttHandler_publishState();

#endif // MQTT_HANDLER_H
