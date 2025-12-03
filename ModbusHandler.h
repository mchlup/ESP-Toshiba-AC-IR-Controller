#ifndef MODBUS_HANDLER_H
#define MODBUS_HANDLER_H

#include <Arduino.h>
#include <ModbusIP_ESP8266.h>
#include "AcState.h"

typedef void (*AcStateChangedCallback)();

struct ModbusConfig {
  uint8_t unitId;   // adresa zařízení (unit/slave id)
  bool enabled;
};

void ModbusHandler_init(const ModbusConfig &cfg, AcStateChangedCallback cb);

// Volat v setup() po WiFi: spustí server() atd.
void ModbusHandler_begin();

// Volat v loop()
void ModbusHandler_loop();

// Aktualizuje hodnoty registrů podle gAcState (zpětná vazba)
void ModbusHandler_updateRegsFromState();

const ModbusConfig &ModbusHandler_getConfig();
void ModbusHandler_setConfig(const ModbusConfig &cfg);

#endif // MODBUS_HANDLER_H
