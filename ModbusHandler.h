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

// Jednoduché status kódy pro zpětnou vazbu do Modbusu.
// 0 = OK, ostatní můžeš používat dle libosti (IR error, invalid value, ...).
enum ModbusStatus : uint16_t {
  MODBUS_STATUS_OK           = 0,
  MODBUS_STATUS_IR_ERROR     = 1,
  MODBUS_STATUS_INVALID_VALUE= 2,
  MODBUS_STATUS_BUSY         = 3
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

// Ruční nastavení status registru (např. z IR/MQTT vrstvy).
void ModbusHandler_setStatus(uint16_t status);
uint16_t ModbusHandler_getStatus();

#endif // MODBUS_HANDLER_H
