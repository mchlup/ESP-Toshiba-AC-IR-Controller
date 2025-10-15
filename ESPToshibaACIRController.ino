#include <Arduino.h>
#include <ArduinoJson.h>
#include <IRrecv.h>
#include <IRremoteESP8266.h>
#include <IRsend.h>
#include <IRutils.h>
#include <vector>

#if defined(ESP8266)
#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <LittleFS.h>
using WebServerType = ESP8266WebServer;
#define LITTLEFS LittleFS
#elif defined(ESP32)
#include <WiFi.h>
#include <WebServer.h>
#include <FS.h>
#include <LittleFS.h>
using WebServerType = WebServer;
#define LITTLEFS LittleFS
#else
#error "Unsupported platform"
#endif

#include <WiFiManager.h>

#include "index_html.h"

// ====== Uživatelské nastavení ======
static constexpr uint16_t IR_RECV_PIN = 14;  // D5 na NodeMCU
static constexpr uint16_t IR_SEND_PIN = 4;   // D2 na NodeMCU (GPIO4)
static constexpr const char *kStorageFile = "/codes.json";
static constexpr const char *kConfigFile = "/config.json";
static constexpr uint32_t kDefaultCarrierFrequency = 38000;
static constexpr unsigned long kMinimumLearnTimeoutMs = 5000;

struct DeviceConfig {
  String deviceName = F("ESP IR Controller");
  unsigned long learnTimeoutMs = 60000;
};

struct StoredCode {
  String name;
  decode_type_t protocol = decode_type_t::UNKNOWN;
  uint64_t value = 0;
  uint16_t bits = 0;
  uint32_t frequency = kDefaultCarrierFrequency;
  std::vector<uint16_t> raw;
  unsigned long updated = 0;
};

IRrecv irReceiver(IR_RECV_PIN, 1024, 15, true);
IRsend irSender(IR_SEND_PIN);
WebServerType server(80);
WiFiManager wifiManager;

DeviceConfig deviceConfig;
std::vector<StoredCode> codes;

bool learningActive = false;
String learningName;
unsigned long learningStarted = 0;
String lastCapturedName;
String lastCapturedSummary;
unsigned long lastCaptureMillis = 0;

bool fsReady = false;
bool shouldPersistConfig = false;
bool wifiWasConnected = false;

char deviceNameBuffer[33] = "";
char learnTimeoutBuffer[12] = "";
WiFiManagerParameter deviceNameParam("device_name", "Název zařízení", deviceNameBuffer, sizeof(deviceNameBuffer));
WiFiManagerParameter learnTimeoutParam("learn_timeout", "Doba učení (ms)", learnTimeoutBuffer, sizeof(learnTimeoutBuffer));

constexpr uint16_t rawTickUsec() {
#if defined(USECPERTICK)
  return USECPERTICK;
#else
  return 50;
#endif
}

// ====== Utility funkce ======

StoredCode *findCode(const String &name) {
  for (auto &code : codes) {
    if (code.name.equalsIgnoreCase(name)) {
      return &code;
    }
  }
  return nullptr;
}

String protocolToString(decode_type_t proto) {
  if (proto == decode_type_t::UNKNOWN) {
    return F("UNKNOWN");
  }
  return String(typeToString(proto));
}

void saveDeviceConfig() {
  if (!fsReady) {
    return;
  }

  DynamicJsonDocument doc(512);
  doc["deviceName"] = deviceConfig.deviceName;
  doc["learnTimeoutMs"] = deviceConfig.learnTimeoutMs;

  File file = LITTLEFS.open(kConfigFile, "w");
  if (!file) {
    Serial.println(F("Nelze otevřít config pro zápis."));
    return;
  }
  if (serializeJson(doc, file) == 0) {
    Serial.println(F("Chyba při ukládání config.json"));
  }
  file.close();
}

void loadDeviceConfig() {
  deviceConfig = DeviceConfig();

  if (!fsReady || !LITTLEFS.exists(kConfigFile)) {
    return;
  }

  File file = LITTLEFS.open(kConfigFile, "r");
  if (!file) {
    Serial.println(F("Nelze otevřít config pro čtení."));
    return;
  }

  DynamicJsonDocument doc(512);
  DeserializationError err = deserializeJson(doc, file);
  if (err) {
    Serial.print(F("Chyba při načítání config.json: "));
    Serial.println(err.c_str());
    file.close();
    return;
  }

  file.close();

  if (doc.containsKey("deviceName")) {
    deviceConfig.deviceName = doc["deviceName"].as<String>();
  }
  if (doc.containsKey("learnTimeoutMs")) {
    unsigned long value = doc["learnTimeoutMs"].as<unsigned long>();
    if (value < kMinimumLearnTimeoutMs) {
      value = kMinimumLearnTimeoutMs;
    }
    deviceConfig.learnTimeoutMs = value;
  }
  deviceConfig.deviceName.trim();
  if (!deviceConfig.deviceName.length()) {
    deviceConfig.deviceName = F("ESP IR Controller");
  }
}

void saveCodes() {
  DynamicJsonDocument doc(4096 + codes.size() * 512);
  JsonArray arr = doc.createNestedArray("items");
  for (const auto &code : codes) {
    JsonObject obj = arr.createNestedObject();
    obj["name"] = code.name;
    obj["protocol"] = static_cast<uint16_t>(code.protocol);
    obj["value"] = code.value;
    obj["bits"] = code.bits;
    obj["frequency"] = code.frequency;
    obj["updated"] = code.updated;
    JsonArray raw = obj.createNestedArray("raw");
    for (uint16_t v : code.raw) {
      raw.add(v);
    }
  }

  File file = LITTLEFS.open(kStorageFile, "w");
  if (!file) {
    Serial.println(F("Nelze otevřít soubor k zápisu."));
    return;
  }
  if (serializeJson(doc, file) == 0) {
    Serial.println(F("Chyba při ukládání JSON."));
  }
  file.close();
}

void loadCodes() {
  codes.clear();
  if (!LITTLEFS.exists(kStorageFile)) {
    return;
  }
  File file = LITTLEFS.open(kStorageFile, "r");
  if (!file) {
    Serial.println(F("Nelze otevřít soubor k čtení."));
    return;
  }

  DynamicJsonDocument doc(8192);
  DeserializationError err = deserializeJson(doc, file);
  if (err) {
    Serial.print(F("Chyba při načtení JSON: "));
    Serial.println(err.c_str());
    file.close();
    return;
  }
  file.close();

  JsonArray arr = doc["items"].as<JsonArray>();
  for (JsonObject obj : arr) {
    StoredCode code;
    code.name = obj["name"].as<String>();
    code.protocol = static_cast<decode_type_t>(obj["protocol"].as<uint16_t>());
    code.value = obj["value"].as<uint64_t>();
    code.bits = obj["bits"].as<uint16_t>();
    code.frequency = obj["frequency"].as<uint32_t>();
    code.updated = obj["updated"].as<unsigned long>();
    JsonArray raw = obj["raw"].as<JsonArray>();
    code.raw.reserve(raw.size());
    for (JsonVariant v : raw) {
      code.raw.push_back(v.as<uint16_t>());
    }
    codes.push_back(code);
  }
}

void startLearning(const String &name) {
  learningActive = true;
  learningName = name;
  learningStarted = millis();
  lastCapturedSummary = "";
  Serial.print(F("Učení zahájeno pro: "));
  Serial.println(name);
}

void cancelLearning() {
  learningActive = false;
  learningName = "";
  learningStarted = 0;
}

void handleCapturedCode(const decode_results &results) {
  StoredCode *existing = findCode(learningName);
  if (!existing) {
    codes.push_back(StoredCode());
    existing = &codes.back();
  }

  StoredCode &code = *existing;
  code.name = learningName;
  code.protocol = results.decode_type;
  code.value = results.value;
  code.bits = results.bits;
  code.frequency = kDefaultCarrierFrequency;
  code.raw.clear();
  if (results.rawlen > 1) {
    code.raw.reserve(results.rawlen - 1);
    for (uint16_t i = 1; i < results.rawlen; i++) {
      code.raw.push_back(results.rawbuf[i] * rawTickUsec());
    }
  }
  code.updated = millis();

  saveCodes();
  lastCapturedName = learningName;
  lastCaptureMillis = millis();
  String summary = protocolToString(results.decode_type);
  summary += F(" (bits: ");
  summary += results.bits;
  summary += F(")");
  lastCapturedSummary = summary;
  Serial.print(F("Kód uložen pro "));
  Serial.println(code.name);
  cancelLearning();
}

bool wifiConnected() {
  return WiFi.status() == WL_CONNECTED;
}

String makeStatusMessage() {
  if (!wifiConnected()) {
    return F("Připojování k Wi-Fi...");
  }
  if (learningActive) {
    return F("Probíhá učení IR kódu...");
  }
  if (lastCapturedName.length()) {
    String message = F("Naposledy naučený: ");
    message += lastCapturedName;
    message += F(" ");
    message += lastCapturedSummary;
    return message;
  }
  return F("Připraveno");
}

void sendJsonResponse(const JsonDocument &doc) {
  String payload;
  serializeJson(doc, payload);
  server.send(200, F("application/json"), payload);
}

void handleRoot() {
  server.send_P(200, "text/html", INDEX_HTML);
}

void handleGetCodes() {
  DynamicJsonDocument doc(4096 + codes.size() * 256);
  JsonArray arr = doc.createNestedArray("items");
  for (const auto &code : codes) {
    JsonObject obj = arr.createNestedObject();
    obj["name"] = code.name;
    obj["protocol"] = protocolToString(code.protocol);
    obj["bits"] = code.bits;
    obj["updated"] = code.updated;
  }
  sendJsonResponse(doc);
}

void handleStatus() {
  DynamicJsonDocument doc(512);
  doc["learning"] = learningActive;
  doc["message"] = makeStatusMessage();
  doc["controllerName"] = deviceConfig.deviceName;
  doc["learnTimeoutMs"] = deviceConfig.learnTimeoutMs;
  doc["wifiConnected"] = wifiConnected();
  if (lastCapturedName.length()) {
    doc["lastCaptured"] = lastCapturedName;
    doc["lastCapturedSummary"] = lastCapturedSummary;
    doc["lastCapturedAt"] = lastCaptureMillis;
  }
  sendJsonResponse(doc);
}

bool parseJsonRequest(DynamicJsonDocument &doc) {
  if (!server.hasArg("plain")) {
    server.send(400, F("text/plain"), F("Missing body"));
    return false;
  }
  DeserializationError err = deserializeJson(doc, server.arg("plain"));
  if (err) {
    server.send(400, F("text/plain"), String(F("JSON error: ")) + err.c_str());
    return false;
  }
  return true;
}

void handleLearn() {
  DynamicJsonDocument doc(256);
  if (!parseJsonRequest(doc)) {
    return;
  }
  String name = doc["name"].as<String>();
  name.trim();
  if (!name.length()) {
    server.send(400, F("text/plain"), F("Name is required"));
    return;
  }
  startLearning(name);
  server.send(200, F("application/json"), F("{\"status\":\"learning\"}"));
}

void handleSend() {
  DynamicJsonDocument doc(256);
  if (!parseJsonRequest(doc)) {
    return;
  }
  String name = doc["name"].as<String>();
  StoredCode *code = findCode(name);
  if (!code) {
    server.send(404, F("text/plain"), F("Code not found"));
    return;
  }

  if (code->protocol != decode_type_t::UNKNOWN) {
    irSender.send(code->protocol, code->value, code->bits);
  } else if (!code->raw.empty()) {
    irSender.sendRaw(code->raw.data(), code->raw.size(), code->frequency);
  } else {
    server.send(500, F("text/plain"), F("Code has no data"));
    return;
  }
  server.send(200, F("application/json"), F("{\"status\":\"sent\"}"));
}

void handleDelete() {
  String name = server.arg("name");
  if (!name.length()) {
    server.send(400, F("text/plain"), F("Parameter 'name' je povinný"));
    return;
  }
  for (auto it = codes.begin(); it != codes.end(); ++it) {
    if (it->name.equalsIgnoreCase(name)) {
      codes.erase(it);
      saveCodes();
      server.send(200, F("application/json"), F("{\"status\":\"deleted\"}"));
      return;
    }
  }
  server.send(404, F("text/plain"), F("Code not found"));
}

void syncConfigFromParams() {
  deviceConfig.deviceName = String(deviceNameParam.getValue());
  unsigned long timeout = strtoul(learnTimeoutParam.getValue(), nullptr, 10);
  if (timeout < kMinimumLearnTimeoutMs) {
    timeout = kMinimumLearnTimeoutMs;
  }
  deviceConfig.learnTimeoutMs = timeout;
  deviceConfig.deviceName.trim();
  if (!deviceConfig.deviceName.length()) {
    deviceConfig.deviceName = F("ESP IR Controller");
  }
  snprintf(deviceNameBuffer, sizeof(deviceNameBuffer), "%s", deviceConfig.deviceName.c_str());
  snprintf(learnTimeoutBuffer, sizeof(learnTimeoutBuffer), "%lu", deviceConfig.learnTimeoutMs);
}

void saveConfigIfNeeded() {
  if (!shouldPersistConfig) {
    return;
  }
  shouldPersistConfig = false;
  syncConfigFromParams();
  saveDeviceConfig();
  Serial.println(F("Konfigurace uložena."));
}

void handleWiFi() {
  wifiManager.process();

  if (wifiConnected()) {
    if (!wifiWasConnected) {
      wifiWasConnected = true;
      Serial.print(F("Wi-Fi připojeno, IP adresa: "));
      Serial.println(WiFi.localIP());
      syncConfigFromParams();
      saveDeviceConfig();
    }
  } else {
    if (wifiWasConnected) {
      Serial.println(F("Wi-Fi odpojeno"));
    }
    wifiWasConnected = false;
  }
}

void setup() {
  Serial.begin(115200);
  delay(100);
  Serial.println();
  Serial.println(F("Startuji ESP IR kontrolér"));

  WiFi.mode(WIFI_STA);

#if defined(ESP8266)
  fsReady = LITTLEFS.begin();
#elif defined(ESP32)
  fsReady = LITTLEFS.begin(true);
#endif
  if (!fsReady) {
    Serial.println(F("Nelze připojit souborový systém LittleFS"));
  }
  loadDeviceConfig();
  loadCodes();

  wifiManager.setConfigPortalBlocking(false);
  wifiManager.setSaveParamsCallback([]() {
    shouldPersistConfig = true;
  });

  snprintf(deviceNameBuffer, sizeof(deviceNameBuffer), "%s", deviceConfig.deviceName.c_str());
  snprintf(learnTimeoutBuffer, sizeof(learnTimeoutBuffer), "%lu", deviceConfig.learnTimeoutMs);
  wifiManager.addParameter(&deviceNameParam);
  wifiManager.addParameter(&learnTimeoutParam);

  if (!wifiManager.autoConnect("ESP-IR-Setup")) {
    Serial.println(F("Nelze se připojit k Wi-Fi, čekám na konfiguraci."));
  } else {
    wifiWasConnected = true;
    Serial.print(F("Wi-Fi připojeno, IP adresa: "));
    Serial.println(WiFi.localIP());
    syncConfigFromParams();
    saveDeviceConfig();
  }

  WiFi.setAutoReconnect(true);

  irReceiver.enableIRIn();
  irSender.begin();

  server.on(F("/"), HTTP_GET, handleRoot);
  server.on(F("/api/codes"), HTTP_GET, handleGetCodes);
  server.on(F("/api/status"), HTTP_GET, handleStatus);
  server.on(F("/api/learn"), HTTP_POST, handleLearn);
  server.on(F("/api/send"), HTTP_POST, handleSend);
  server.on(F("/api/codes"), HTTP_DELETE, handleDelete);
  server.begin();
  Serial.println(F("HTTP server spuštěn"));
}

void loop() {
  handleWiFi();
  saveConfigIfNeeded();
  server.handleClient();

  if (learningActive && millis() - learningStarted > deviceConfig.learnTimeoutMs) {
    Serial.println(F("Učení vypršelo."));
    cancelLearning();
  }

  decode_results results;
  if (irReceiver.decode(&results)) {
    if (learningActive) {
      handleCapturedCode(results);
    }
    irReceiver.resume();
  }
}
