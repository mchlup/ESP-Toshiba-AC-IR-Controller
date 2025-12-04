#ifndef CONFIG_STORAGE_H
#define CONFIG_STORAGE_H

#include <Arduino.h>
#include "Logging.h"

// Forward deklarace – definice jsou v jiných hlavičkách.
struct WebUiConfig;
struct MqttConfig;
struct ModbusConfig;

namespace ConfigStorage {

  // Inicializace EEPROM (zavolat v setupu před load/save)
  void begin();

  // Načte konfiguraci z EEPROM.
  // Vrátí true, pokud je validní uložená konfigurace; jinak false.
  bool load(WebUiConfig &webCfg,
            MqttConfig &mqttCfg,
            ModbusConfig &modbusCfg,
            Logging::Level &logLevel);

  // Uloží aktuální konfiguraci do EEPROM.
  void save(const WebUiConfig &webCfg,
            const MqttConfig &mqttCfg,
            const ModbusConfig &modbusCfg,
            Logging::Level logLevel);

  // Vymaže uloženou konfiguraci (příští boot => použije defaulty).
  void clear();

} // namespace ConfigStorage

#endif // CONFIG_STORAGE_H
