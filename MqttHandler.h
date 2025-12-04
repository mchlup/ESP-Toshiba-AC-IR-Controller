#ifndef MQTT_HANDLER_H
#define MQTT_HANDLER_H

#include <Arduino.h>
#include <Client.h>
#include "AcState.h"

typedef void (*AcStateChangedCallback)();

struct MqttConfig {
  String   host;
  uint16_t port;
  String   clientId;   // typicky = DEVICE_ID
  String   baseTopic;  // např. "home/espir"
  String   user;
  String   pass;
  bool     enabled;
};

void MqttHandler_init(Client &netClient, const MqttConfig &cfg, AcStateChangedCallback cb);
void MqttHandler_loop();
void MqttHandler_publishState();
MqttConfig MqttHandler_getConfig();
void MqttHandler_setConfig(const MqttConfig &cfg);

#endif // MQTT_HANDLER_H
