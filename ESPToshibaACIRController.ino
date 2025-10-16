#include <Arduino.h>
#include <ArduinoJson.h>
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
  #error "Unsupported platform (ESP8266 or ESP32 required)."
#endif

#include <WiFiManager.h>

#include <IRremoteESP8266.h>
#include <IRrecv.h>
#include <IRsend.h>
#include <IRutils.h>
// ---- IRremoteESP8266 compatibility guards ----
// Some older/newer library versions change tick constant names.
// kRawTick is the canonical constant (microseconds per raw tick).
#ifndef kRawTick
  #define kRawTick 50U  // Fallback: 50us per raw tick
#endif
  // typeToString(), resultToHumanReadableBasic(), ...

#include "index_html.h"  // PROGMEM INDEX_HTML (WebUI)

// ================== HW / nastavení ==================
static constexpr uint16_t IR_RECV_PIN = 14;  // D5 / GPIO14
static constexpr uint16_t IR_SEND_PIN = 4;   // D2 / GPIO4

// Výchozí nosná frekvence pro raw send (kHz)
static constexpr uint16_t IR_DEFAULT_KHZ = 38;

// Soubory v LittleFS
static const char* CODES_JSON_PATH    = "/codes.json";
static const char* SETTINGS_JSON_PATH = "/settings.json";

// WiFiManager custom parametry
WiFiManager wm;
WiFiManagerParameter p_devName("devname", "Název zařízení", "ESP-IR-Bridge", 32);
WiFiManagerParameter p_learnTimeout("learntimeout", "Timeout učení (ms)", "60000", 10);

// Webserver
WebServerType server(80);

// IR
IRrecv irrecv(IR_RECV_PIN, 1024, 50, true);
IRsend irsend(IR_SEND_PIN);

// Stav učení
struct LearnState {
  bool active = false;
  String pendingName;
  String lastCaptured;
  unsigned long startedMs = 0;
  unsigned long timeoutMs = 60000;
} learn;

// Uložený kód
struct StoredCode {
  String name;
  String protocol;      // "NEC", "SAMSUNG", "UNKNOWN", ...
  uint16_t bits = 0;
  uint32_t value = 0;
  uint32_t address = 0;
  uint32_t command = 0;
  uint16_t khz = IR_DEFAULT_KHZ;
  std::vector<uint32_t> raw;       // durace v µs (mark/space)
  uint64_t updatedEpochMs = 0;     // pro zobrazení v UI
};

std::vector<StoredCode> g_codes;

// ===== Pomocné (FS) =====
static bool saveCodes() {
  DynamicJsonDocument doc(64 * 1024);
  JsonArray arr = doc.createNestedArray("items");
  for (const auto &c : g_codes) {
    JsonObject o = arr.createNestedObject();
    o["name"]    = c.name;
    o["protocol"]= c.protocol;
    o["bits"]    = c.bits;
    o["value"]   = c.value;
    o["address"] = c.address;
    o["command"] = c.command;
    o["khz"]     = c.khz;
    o["updated"] = c.updatedEpochMs;
    JsonArray raw = o.createNestedArray("raw");
    for (auto us : c.raw) raw.add(us);
  }
  File f = LITTLEFS.open(CODES_JSON_PATH, "w");
  if (!f) return false;
  bool ok = (serializeJson(doc, f) > 0);
  f.close();
  return ok;
}

static void loadCodes() {
  g_codes.clear();
  if (!LITTLEFS.exists(CODES_JSON_PATH)) return;
  File f = LITTLEFS.open(CODES_JSON_PATH, "r");
  if (!f) return;

  DynamicJsonDocument doc(64 * 1024);
  DeserializationError e = deserializeJson(doc, f);
  f.close();
  if (e) return;

  JsonArray arr = doc["items"].as<JsonArray>();
  if (arr.isNull()) return;

  for (JsonObject o : arr) {
    StoredCode c;
    c.name     = o["name"]     | "";
    c.protocol = o["protocol"] | "UNKNOWN";
    c.bits     = o["bits"]     | 0;
    c.value    = o["value"]    | 0;
    c.address  = o["address"]  | 0;
    c.command  = o["command"]  | 0;
    c.khz      = o["khz"]      | IR_DEFAULT_KHZ;
    c.updatedEpochMs = o["updated"] | 0ULL;
    if (o.containsKey("raw")) {
      for (JsonVariant v : o["raw"].as<JsonArray>()) c.raw.push_back((uint32_t)v.as<unsigned long>());
    }
    if (c.name.length()) g_codes.push_back(std::move(c));
  }
}

static bool saveSettings() {
  DynamicJsonDocument doc(1024);
  doc["devname"]         = p_devName.getValue();
  doc["learn_timeout_ms"]= strtoul(p_learnTimeout.getValue(), nullptr, 10);

  File f = LITTLEFS.open(SETTINGS_JSON_PATH, "w");
  if (!f) return false;
  bool ok = (serializeJson(doc, f) > 0);
  f.close();
  return ok;
}

static void loadSettings() {
  if (!LITTLEFS.exists(SETTINGS_JSON_PATH)) return;
  File f = LITTLEFS.open(SETTINGS_JSON_PATH, "r");
  if (!f) return;
  DynamicJsonDocument doc(1024);
  if (deserializeJson(doc, f)) { f.close(); return; }
  f.close();

  const char* dn = doc["devname"] | nullptr;
  if (dn && strlen(dn)) p_devName.setValue(dn, strlen(dn));

  unsigned long lt = doc["learn_timeout_ms"] | 60000UL;
  char buf[16]; snprintf(buf, sizeof(buf), "%lu", lt);
  p_learnTimeout.setValue(buf, strlen(buf));
  learn.timeoutMs = lt;
}

// Hledání kódu podle názvu
static int findCodeIndex(const String& name) {
  for (size_t i = 0; i < g_codes.size(); ++i)
    if (g_codes[i].name == name) return (int)i;
  return -1;
}

// Převod decode_results -> StoredCode (RAW + metadata)
static StoredCode resultsToStored(const decode_results& results, const String& name) {
  StoredCode c;
  c.name     = name;
  c.bits     = results.bits;
  c.value    = results.value;
  c.address  = results.address;
  c.command  = results.command;
  c.protocol = typeToString(results.decode_type);
  c.khz      = IR_DEFAULT_KHZ; // results frekvenci neobsahují -> použij default

  // rawbuf[] je v tick jednotkách; 1 tick = kRawTick µs
  for (uint16_t i = 0; i < results.rawlen; i++) {
    uint32_t usec = (uint32_t)results.rawbuf[i] * kRawTick;
    c.raw.push_back(usec);
  }
  c.updatedEpochMs = millis();
  return c;
}

// ===== HTTP Handlery =====
static void handleRoot() {
  server.send_P(200, "text/html", INDEX_HTML);
}

static void handleStatus() {
  DynamicJsonDocument doc(512);
  String msg;
  if (learn.active) msg = "Učení probíhá...";
  else if (learn.lastCaptured.length()) msg = String("Připraveno. Naposledy: ") + learn.lastCaptured;
  else msg = "Připraveno.";

  doc["message"]        = msg;
  doc["learning"]       = learn.active;
  doc["lastCaptured"]   = learn.lastCaptured;
  doc["controllerName"] = String(p_devName.getValue());
  doc["wifiConnected"]  = (WiFi.status() == WL_CONNECTED);

  String out; out.reserve(256);
  serializeJson(doc, out);
  server.send(200, "application/json", out);
}

static void handleCodesList() {
  DynamicJsonDocument doc(64 * 1024);
  JsonArray arr = doc.createNestedArray("items");
  for (const auto& c : g_codes) {
    JsonObject o = arr.createNestedObject();
    o["name"]    = c.name;
    o["protocol"]= c.protocol;
    o["bits"]    = c.bits;
    o["updated"] = c.updatedEpochMs;
  }
  String out; serializeJson(doc, out);
  server.send(200, "application/json", out);
}

static void handleCodesDelete() {
  String name = server.arg("name");
  if (!name.length()) { server.send(400, "application/json", "{\"error\":\"Missing name\"}"); return; }
  int idx = findCodeIndex(name);
  if (idx < 0) { server.send(404, "application/json", "{\"error\":\"Not found\"}"); return; }
  g_codes.erase(g_codes.begin() + idx);
  saveCodes();
  server.send(200, "application/json", "{\"ok\":true}");
}

static void handleLearnPost() {
  if (server.arg("plain").isEmpty()) {
    server.send(400, "application/json", "{\"error\":\"Missing JSON body\"}");
    return;
  }
  DynamicJsonDocument in(512);
  DeserializationError e = deserializeJson(in, server.arg("plain"));
  if (e) { server.send(400, "application/json", "{\"error\":\"Invalid JSON\"}"); return; }

  String name = in["name"] | "";
  if (!name.length()) { server.send(400, "application/json", "{\"error\":\"Missing name\"}"); return; }

  learn.active      = true;
  learn.pendingName = name;
  learn.startedMs   = millis();
  learn.timeoutMs   = strtoul(p_learnTimeout.getValue(), nullptr, 10);
  irrecv.enableIRIn();   // jistota, že je RX zapnutý

  DynamicJsonDocument out(256);
  out["ok"] = true; out["learning"] = true;
  String s; serializeJson(out, s);
  server.send(200, "application/json", s);
}

static void handleSendPost() {
  if (server.arg("plain").isEmpty()) {
    server.send(400, "application/json", "{\"error\":\"Missing JSON body\"}");
    return;
  }
  DynamicJsonDocument in(512);
  if (deserializeJson(in, server.arg("plain"))) {
    server.send(400, "application/json", "{\"error\":\"Invalid JSON\"}");
    return;
  }
  String name = in["name"] | "";
  if (!name.length()) { server.send(400, "application/json", "{\"error\":\"Missing name\"}"); return; }

  int idx = findCodeIndex(name);
  if (idx < 0) { server.send(404, "application/json", "{\"error\":\"Code not found\"}"); return; }
  const StoredCode& c = g_codes[idx];

  // Preferuj RAW – univerzální
  if (!c.raw.empty()) {
    std::vector<uint16_t> raw16; raw16.reserve(c.raw.size());
    for (auto us : c.raw) raw16.push_back((uint16_t)std::min<uint32_t>(us, 0xFFFF));

    irsend.begin();
    delay(10);
    irsend.sendRaw(raw16.data(), raw16.size(), c.khz);
  } else if (c.protocol == "NEC") {
    irsend.begin(); delay(10);
    irsend.sendNEC(c.value, c.bits);
  } else {
    server.send(500, "application/json", "{\"error\":\"Code has no RAW and unsupported protocol\"}");
    return;
  }

  server.send(200, "application/json", "{\"ok\":true}");
}

// ===== Uložení přijatého kódu =====
static void handleCapturedCode(const decode_results& results) {
  StoredCode code = resultsToStored(results, learn.pendingName);

  int idx = findCodeIndex(code.name);
  if (idx >= 0) g_codes[idx] = std::move(code);
  else g_codes.push_back(std::move(code));

  saveCodes();

  learn.lastCaptured = learn.pendingName;
  learn.pendingName  = "";
  learn.active       = false;
}

// ===== Setup & Loop =====
static void setupFS() {
#if defined(ESP8266)
  LITTLEFS.begin();
#else
  LITTLEFS.begin(true);  // ESP32: auto-format on fail
#endif
}

static void setupWiFi() {
  wm.setAPCallback([](WiFiManager* manager) {
    Serial.println(F("-- WiFiManager config portal --"));
    Serial.print(F("  SSID: "));
    Serial.println(manager->getConfigPortalSSID());
    Serial.print(F("  IP: "));
    Serial.println(WiFi.softAPIP());
    Serial.println(F("  AP security: open (no password)"));
  });
  wm.setClass("invert");
  wm.addParameter(&p_devName);
  wm.addParameter(&p_learnTimeout);
  wm.setConfigPortalBlocking(true);
  wm.setConnectTimeout(30);
  wm.setConfigPortalTimeout(180);

  Serial.println(F("Connecting via WiFiManager..."));
  if (!wm.autoConnect("ESP-IR-Bridge")) {
    Serial.println(F("WiFiManager failed to connect, rebooting."));
    ESP.restart();
  }
  Serial.println(F("WiFiManager connected."));
  saveSettings();
}

static void setupHTTP() {
  server.on("/",             HTTP_GET,    handleRoot);
  server.on("/api/status",   HTTP_GET,    handleStatus);
  server.on("/api/codes",    HTTP_GET,    handleCodesList);
  server.on("/api/codes",    HTTP_DELETE, handleCodesDelete);
  server.on("/api/learn",    HTTP_POST,   handleLearnPost);
  server.on("/api/send",     HTTP_POST,   handleSendPost);
  server.begin();
}

void setup() {
  Serial.begin(115200);
  delay(50);
  Serial.println();
  Serial.println(F("== ESP IR Bridge start =="));

  setupFS();
  loadSettings();
  loadCodes();

  setupWiFi();

  irrecv.enableIRIn();
  irsend.begin();

  setupHTTP();

  Serial.print(F("IP: ")); Serial.println(WiFi.localIP());
}

void loop() {
  server.handleClient();

  if (learn.active) {
    if (millis() - learn.startedMs > learn.timeoutMs) {
      // Timeout učení
      learn.active = false;
      learn.pendingName = "";
      Serial.println(F("Learn timeout."));
    } else {
      decode_results results;
      if (irrecv.decode(&results)) {
        Serial.println(resultToHumanReadableBasic(&results));
        handleCapturedCode(results);
        irrecv.resume();  // připravit na další přijem
      }
    }
  } else {
    // Nepovinně: pasivně čistíme RX buffer (bez zpracování)
    decode_results dummy;
    if (irrecv.decode(&dummy)) {
      irrecv.resume();
    }
  }
}
