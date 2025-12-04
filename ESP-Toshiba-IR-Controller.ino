#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <WiFiManager.h>

#include "AcState.h"
#include "IRControl.h"
#include "ModbusHandler.h"
#include "MqttHandler.h"
#include "WebUI.h"
#include "Logging.h"
#include "ConfigStorage.h"

// -------------------------------------------------
// Witty RGB LED – piny
// -------------------------------------------------
const uint8_t LED_G_PIN = 12;  // zelená
const uint8_t LED_B_PIN = 13;  // modrá
const uint8_t LED_R_PIN = 15;  // červená

// -------------------------------------------------
// Konfigurace zařízení – DEFAULT hodnoty
// Ty se uloží do EEPROM při prvním startu nebo po factory resetu.
// -------------------------------------------------

// WiFiManager – název AP pro konfigurační portál
const char *WIFI_AP_NAME = "ESP-IR-Bridge-Setup";

// Device info (WebUI + MQTT clientId)
const char *DEFAULT_DEVICE_NAME     = "ESP Toshiba IR Bridge Controller";
const char *DEFAULT_DEVICE_ID       = "esp-toshiba-01";
const char *DEFAULT_DEVICE_LOCATION = "Living room";

// MQTT default
const char *DEFAULT_MQTT_HOST       = "192.168.1.10";
const uint16_t DEFAULT_MQTT_PORT    = 1883;
const char *DEFAULT_MQTT_BASE_TOPIC = "home/espir";
const char *DEFAULT_MQTT_USER       = "";
const char *DEFAULT_MQTT_PASS       = "";
const bool  DEFAULT_MQTT_ENABLED    = false;

// Modbus TCP default
const uint8_t DEFAULT_MODBUS_UNIT_ID = 1;
const bool    DEFAULT_MODBUS_ENABLED = true;

// Log level default
const Logging::Level DEFAULT_LOG_LEVEL = Logging::LEVEL_INFO;

// -------------------------------------------------
// Globální proměnné (runtime konfigurace)
// -------------------------------------------------

WiFiClient gWifiClient;

WebUiConfig gWebCfg;
MqttConfig  gMqttCfg;
ModbusConfig gModbusCfg;
Logging::Level gLogLevel = DEFAULT_LOG_LEVEL;

void onAcStateChanged();

// -------------------------------------------------
// Pomocné funkce – RGB LED
// -------------------------------------------------

void ledAllOff() {
  digitalWrite(LED_R_PIN, LOW);
  digitalWrite(LED_G_PIN, LOW);
  digitalWrite(LED_B_PIN, LOW);
}

void ledGreen(bool on) {
  digitalWrite(LED_G_PIN, on ? HIGH : LOW);
}

void ledBlue(bool on) {
  digitalWrite(LED_B_PIN, on ? HIGH : LOW);
}

void ledRed(bool on) {
  digitalWrite(LED_R_PIN, on ? HIGH : LOW);
}

// -------------------------------------------------
// WiFi + WiFiManager + stavová LED
// -------------------------------------------------

static void setupWiFi() {
  Logging::logf(Logging::LEVEL_INFO, "WiFi", "Starting WiFi connect...");

  WiFi.mode(WIFI_STA);
  WiFi.begin(); // použije uložené credentials

  unsigned long start = millis();
  bool ledState = false;

  // Pomalé zelené blikání max cca 15s během pokusu o připojení
  while (WiFi.status() != WL_CONNECTED && (millis() - start) < 15000UL) {
    ledState = !ledState;
    ledAllOff();
    ledGreen(ledState);
    delay(500);
    yield();
  }

  if (WiFi.status() != WL_CONNECTED) {
    // Připojení selhalo – spustíme WiFiManager konfigurační portál.
    Logging::logf(Logging::LEVEL_WARN, "WiFi",
                  "WiFi connect failed, starting config portal '%s'", WIFI_AP_NAME);

    ledAllOff();
    ledBlue(true); // modrá = config portál

    WiFiManager wm;
    // startConfigPortal blokuje, dokud uživatel neuloží SSID/heslo nebo nevypne AP
    bool res = wm.startConfigPortal(WIFI_AP_NAME);

    if (!res || WiFi.status() != WL_CONNECTED) {
      Logging::logf(Logging::LEVEL_ERROR, "WiFi",
                    "Config portal finished but WiFi is not connected, restarting...");
      delay(2000);
      ESP.restart();
    }
  }

  // Tady už by měla být WiFi připojená
  ledAllOff();
  ledGreen(true);  // trvale svítící zelená = WiFi OK

  Logging::logf(Logging::LEVEL_INFO, "WiFi", "Connected, IP=%s",
                WiFi.localIP().toString().c_str());
}

// -------------------------------------------------
// SETUP & LOOP
// -------------------------------------------------

void setup() {
  // LED piny
  pinMode(LED_R_PIN, OUTPUT);
  pinMode(LED_G_PIN, OUTPUT);
  pinMode(LED_B_PIN, OUTPUT);
  ledAllOff();

  // Logging
  Logging::begin(115200, DEFAULT_LOG_LEVEL);

  Logging::logf(Logging::LEVEL_INFO, "MAIN", "Booting ESP-Toshiba-IR-Controller...");

  // Stav AC
  AcState_initDefaults();

  // EEPROM config
  ConfigStorage::begin();

  // Naplníme runtime config defaulty
  gWebCfg.deviceName = DEFAULT_DEVICE_NAME;
  gWebCfg.deviceId   = DEFAULT_DEVICE_ID;
  gWebCfg.location   = DEFAULT_DEVICE_LOCATION;

  gMqttCfg.host      = DEFAULT_MQTT_HOST;
  gMqttCfg.port      = DEFAULT_MQTT_PORT;
  gMqttCfg.clientId  = DEFAULT_DEVICE_ID;
  gMqttCfg.baseTopic = DEFAULT_MQTT_BASE_TOPIC;
  gMqttCfg.user      = DEFAULT_MQTT_USER;
  gMqttCfg.pass      = DEFAULT_MQTT_PASS;
  gMqttCfg.enabled   = DEFAULT_MQTT_ENABLED;

  gModbusCfg.unitId  = DEFAULT_MODBUS_UNIT_ID;
  gModbusCfg.enabled = DEFAULT_MODBUS_ENABLED;

  gLogLevel = DEFAULT_LOG_LEVEL;

  // Zkusíme načíst uloženou konfiguraci z EEPROM
  if (ConfigStorage::load(gWebCfg, gMqttCfg, gModbusCfg, gLogLevel)) {
    // Pro jistotu – clientId sladíme s deviceId
    if (gMqttCfg.clientId.length() == 0) {
      gMqttCfg.clientId = gWebCfg.deviceId;
    }
  } else {
    // Poprvé – uložíme defaulty
    ConfigStorage::save(gWebCfg, gMqttCfg, gModbusCfg, gLogLevel);
  }

  // Nastavíme log level podle konfigurace
  Logging::setLevel(gLogLevel);

  // WiFi (LED: blikání => trvale zelená)
  setupWiFi();

  // IR
  IRControl_begin(4); // D2 / GPIO4 na Witty – IR LED, uprav podle HW
  Logging::logf(Logging::LEVEL_INFO, "IR", "IR transmitter initialised on pin %u", 4);

  // Modbus
  ModbusHandler_init(gModbusCfg, onAcStateChanged);
  ModbusHandler_begin();
  Logging::logf(Logging::LEVEL_INFO, "MODBUS", "Modbus TCP enabled=%d unitId=%u",
                (int)gModbusCfg.enabled, gModbusCfg.unitId);

  // MQTT
  // ClientId sladíme s deviceId (z configu)
  gMqttCfg.clientId = gWebCfg.deviceId;
  MqttHandler_init(gWifiClient, gMqttCfg, onAcStateChanged);
  Logging::logf(Logging::LEVEL_INFO, "MQTT", "MQTT enabled=%d host=%s",
                (int)gMqttCfg.enabled, gMqttCfg.host.c_str());

  // WebUI
  WebUI_begin(gWebCfg, onAcStateChanged);
  Logging::logf(Logging::LEVEL_INFO, "WEB", "WebUI ready: http://%s/",
                WiFi.localIP().toString().c_str());

  // První synchronizace (IR + Modbus + MQTT)
  onAcStateChanged();
}

void loop() {
  ModbusHandler_loop();
  MqttHandler_loop();
  WebUI_loop();
}

// -------------------------------------------------
// Centrální handler po změně stavu gAcState
// -------------------------------------------------

void onAcStateChanged() {
  AcState_normalize();
  Logging::logf(Logging::LEVEL_INFO, "AC",
                "State changed: power=%s temp=%u mode=%s fan=%s swing=%s",
                gAcState.power.c_str(),
                gAcState.temp,
                gAcState.mode.c_str(),
                gAcState.fan.c_str(),
                gAcState.swing.c_str());

  // 1) IR příkaz do klimatizace
  IRControl_sendFromState();

  // 2) Zpětná vazba do Modbus registrů (Loxone)
  ModbusHandler_updateRegsFromState();

  // 3) MQTT publish aktuálního stavu
  MqttHandler_publishState();
}
