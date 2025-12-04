#ifndef WEB_UI_H
#define WEB_UI_H

#include <Arduino.h>
#include "AcState.h"

typedef void (*AcStateChangedCallback)();

// Základní identifikace zařízení – ostatní config (MQTT/Modbus)
// si WebUI načítá z MqttHandler/ModbusHandler.
struct WebUiConfig {
  String deviceName;
  String deviceId;
  String location;
};

void WebUI_begin(const WebUiConfig &cfg, AcStateChangedCallback cb);
void WebUI_loop();

#endif // WEB_UI_H
