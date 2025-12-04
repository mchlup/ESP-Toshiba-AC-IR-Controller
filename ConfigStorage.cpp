#include "ConfigStorage.h"

#include <EEPROM.h>
#include <string.h>

// Potřebujeme kompletní definice struktur:
#include "WebUI.h"
#include "MqttHandler.h"
#include "ModbusHandler.h"

namespace {

// Struktura, která se fyzicky ukládá do EEPROM.
// Všechna pole jsou pevné délky, aby bylo možné ji bezpečně zapisovat/číst.
struct StoredConfig {
  uint32_t magic;       // Identifikace ("TSIR")
  uint16_t version;     // Verze struktury
  uint16_t reserved0;   // zarovnání

  char deviceName[32];
  char deviceId[32];
  char location[32];

  char mqttHost[64];
  uint16_t mqttPort;
  uint16_t reserved1;

  char mqttBaseTopic[64];
  char mqttUser[32];
  char mqttPass[32];
  uint8_t mqttEnabled;
  uint8_t modbusEnabled;
  uint8_t modbusUnitId;
  uint8_t logLevel;

  uint8_t reserved2[16];
};

static const uint32_t CFG_MAGIC   = 0x54534952; // "TSIR"
static const uint16_t CFG_VERSION = 1;
static const size_t   EEPROM_SIZE = 512;

template <size_t N>
static void copyStringToBuf(char (&dst)[N], const String &src) {
  strncpy(dst, src.c_str(), N - 1);
  dst[N - 1] = '\0';
}

template <size_t N>
static String bufToString(char (&src)[N]) {
  src[N - 1] = '\0';
  return String(src);
}

} // namespace

namespace ConfigStorage {

void begin() {
  EEPROM.begin(EEPROM_SIZE);
}

bool load(WebUiConfig &webCfg,
          MqttConfig &mqttCfg,
          ModbusConfig &modbusCfg,
          Logging::Level &logLevel) {
  StoredConfig sc{};
  EEPROM.get(0, sc);

  if (sc.magic != CFG_MAGIC || sc.version != CFG_VERSION) {
    Logging::logf(Logging::LEVEL_INFO, "CFG", "No valid config in EEPROM, using defaults");
    return false;
  }

  // Ujistíme se, že řetězce jsou ukončené.
  sc.deviceName[sizeof(sc.deviceName) - 1] = '\0';
  sc.deviceId[sizeof(sc.deviceId) - 1]     = '\0';
  sc.location[sizeof(sc.location) - 1]     = '\0';
  sc.mqttHost[sizeof(sc.mqttHost) - 1]     = '\0';
  sc.mqttBaseTopic[sizeof(sc.mqttBaseTopic) - 1] = '\0';
  sc.mqttUser[sizeof(sc.mqttUser) - 1]     = '\0';
  sc.mqttPass[sizeof(sc.mqttPass) - 1]     = '\0';

  webCfg.deviceName = String(sc.deviceName);
  webCfg.deviceId   = String(sc.deviceId);
  webCfg.location   = String(sc.location);

  mqttCfg.host      = String(sc.mqttHost);
  mqttCfg.port      = sc.mqttPort;
  mqttCfg.baseTopic = String(sc.mqttBaseTopic);
  mqttCfg.user      = String(sc.mqttUser);
  mqttCfg.pass      = String(sc.mqttPass);
  mqttCfg.enabled   = (sc.mqttEnabled != 0);

  modbusCfg.enabled = (sc.modbusEnabled != 0);
  modbusCfg.unitId  = sc.modbusUnitId;

  if (sc.logLevel <= Logging::LEVEL_VERBOSE) {
    logLevel = static_cast<Logging::Level>(sc.logLevel);
  } else {
    logLevel = Logging::LEVEL_INFO;
  }

  Logging::logf(Logging::LEVEL_INFO, "CFG",
                "Loaded config from EEPROM (deviceId=%s, mqttHost=%s, modbusUnit=%u)",
                webCfg.deviceId.c_str(),
                mqttCfg.host.c_str(),
                modbusCfg.unitId);

  return true;
}

void save(const WebUiConfig &webCfg,
          const MqttConfig &mqttCfg,
          const ModbusConfig &modbusCfg,
          Logging::Level logLevel) {
  StoredConfig sc{};
  sc.magic   = CFG_MAGIC;
  sc.version = CFG_VERSION;

  copyStringToBuf(sc.deviceName, webCfg.deviceName);
  copyStringToBuf(sc.deviceId,   webCfg.deviceId);
  copyStringToBuf(sc.location,   webCfg.location);

  copyStringToBuf(sc.mqttHost,      mqttCfg.host);
  sc.mqttPort = mqttCfg.port;
  copyStringToBuf(sc.mqttBaseTopic, mqttCfg.baseTopic);
  copyStringToBuf(sc.mqttUser,      mqttCfg.user);
  copyStringToBuf(sc.mqttPass,      mqttCfg.pass);
  sc.mqttEnabled  = mqttCfg.enabled ? 1 : 0;

  sc.modbusEnabled = modbusCfg.enabled ? 1 : 0;
  sc.modbusUnitId  = modbusCfg.unitId;

  sc.logLevel = static_cast<uint8_t>(logLevel);

  EEPROM.put(0, sc);
  EEPROM.commit();

  Logging::logf(Logging::LEVEL_INFO, "CFG", "Config saved to EEPROM");
}

void clear() {
  StoredConfig sc{};
  sc.magic   = 0;
  sc.version = 0;
  EEPROM.put(0, sc);
  EEPROM.commit();
  Logging::logf(Logging::LEVEL_WARN, "CFG", "Config cleared in EEPROM");
}

} // namespace ConfigStorage
