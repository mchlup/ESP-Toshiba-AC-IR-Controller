// ESP-IR-Transceiver.ino
// -----------------------------------------------------------------------------
// IR transceiver for learning (capturing) and sending IR codes.
// Built on the same base style as ESP-Toshiba-AC-Modbus: WiFiManager portal,
// Web UI + REST API, MQTT (optional), OTA, LittleFS storage, basic Modbus (optional).
//
// Hardware (defaults for ESP8266 D1 mini / ESP-12F):
//  - IR Receiver (TSOP38238 / VS1838B) -> GPIO5 (D1)
//  - IR LED (through NPN/MOSFET driver recommended) -> GPIO4 (D2)
//  - Status LED (built-in) -> GPIO2
//  - FLASH button -> GPIO0
//
// Notes:
//  - For "learning" arbitrary remotes reliably, use a demodulating IR receiver.
//  - Raw capture is stored as microsecond timings and carrier frequency (kHz).
//  - You can retransmit by ID over HTTP or MQTT. Files live in /codes/*.json
//  - Modbus TCP is optional and minimal here; disable to save flash/ram.
//
// Dependencies (Library Manager):
//   - tzapu/WiFiManager
//   - bblanchon/ArduinoJson
//   - me-no-dev/ESPAsyncTCP (NOT needed) -> We use synchronous ESP8266WebServer
//   - IRremoteESP8266 by David Conran et al. (IMPORTANT)
//   - emelianov/Modbus-ESP8266 (optional)
//
// Tested on: ESP8266 core 3.1.2. Should also compile for ESP32 (pins differ).
// -----------------------------------------------------------------------------

#if defined(ESP8266)
  #include <ESP8266WiFi.h>
  #include <ESP8266WebServer.h>
  using WebSrv = ESP8266WebServer;
  #include <LittleFS.h>
  #define FSYS LittleFS
#elif defined(ESP32)
  #include <WiFi.h>
  #include <WebServer.h>
  using WebSrv = WebServer;
  #include <FS.h>
  #include <LittleFS.h>
  #define FSYS LittleFS
#else
  #error "Unsupported platform (ESP8266 or ESP32 required)"
#endif

#include <WiFiManager.h>
#include <ArduinoOTA.h>
#include <ArduinoJson.h>
#include <PubSubClient.h>
#include <WiFiClient.h>
#include <WiFiClientSecure.h>

// IRremoteESP8266
#include <IRrecv.h>
#include <IRsend.h>
#include <IRutils.h>

// Optional Modbus TCP (minimal demo)
#define USE_MODBUS false
#if USE_MODBUS
  #if defined(ESP8266)
    #include <ModbusIP_ESP8266.h>
  #elif defined(ESP32)
    #include <ModbusIP_ESP32.h>
  #endif
#endif

#include <EEPROM.h>

// ================== Pins & HW config ==================
#if defined(ESP8266)
// Default pins for ESP8266 (D1 mini):
  #define IR_RECV_PIN   5   // D1
  #define IR_SEND_PIN   4   // D2
  #define STATUS_LED    2   // D4 (builtin, active LOW)
  #define FLASH_BTN     0   // D3 (LOW = pressed)
#elif defined(ESP32)
  #define IR_RECV_PIN   26
  #define IR_SEND_PIN   25
  #define STATUS_LED    2
  #define FLASH_BTN     0
#endif

// IR settings
static const uint16_t IR_KHZ_DEFAULT = 38;     // Default carrier for raw send if none captured
static const uint16_t CAPTURE_BUFFER  = 1024;  // Number of entries in capture buffer
static const uint16_t CAPTURE_TIMEOUT = 50;    // Milliseconds. Increase for long remotes

IRrecv irrecv(IR_RECV_PIN, CAPTURE_BUFFER, CAPTURE_TIMEOUT, true);
IRsend irsend(IR_SEND_PIN);

// Global decode buffer
decode_results g_results;

// ================== Networking & Services =============
WebSrv server(80);
WiFiClient netClient;
PubSubClient mqtt(netClient);

// Device config persisted to FS
struct DeviceConfig {
  char mqtt_host[64]    = "";
  uint16_t mqtt_port    = 1883;
  bool mqtt_tls         = false;
  char mqtt_user[32]    = "";
  char mqtt_pass[32]    = "";
  char mqtt_base[64]    = "ir";
  uint8_t log_level     = 1;  // 0=quiet,1=info,2=debug
  uint16_t ota_port     = 8266;
  uint16_t learn_timeout_ms = 10000; // auto-stop learn after 10s
  uint8_t web_auth      = 0;  // 0=no auth, 1=basic auth
  char web_user[16]     = "admin";
  char web_pass[16]     = "admin";
} cfg;

String g_chipId;
String g_hostname;

// ================== Modbus (optional) =================
#if USE_MODBUS
ModbusIP mb;
// Holding registers map (basic):
//  0: Status (bitfield) [bit0=WiFi, bit1=MQTT, bit2=IRrecv enabled, bit3: hasLastCapture]
//  1: Last capture length (timing count)
//  2: Command: send code index (write N => send code with index N in list)
//  3: Result code (0=ok, !=0 error)
//  10..: reserved
uint16_t mb_regs[32] = {0};
#endif

// ================== Helpers ===========================
void ledSet(bool on) {
#if defined(ESP8266)
  digitalWrite(STATUS_LED, on ? LOW : HIGH);
#else
  digitalWrite(STATUS_LED, on ? HIGH : LOW);
#endif
}

bool isButtonPressed() {
#if defined(ESP8266)
  pinMode(FLASH_BTN, INPUT_PULLUP);
#else
  pinMode(FLASH_BTN, INPUT_PULLUP);
#endif
  return digitalRead(FLASH_BTN) == LOW;
}

// Simple logging
void logi(const String& s) { if (cfg.log_level >= 1) Serial.println(s); }
void logd(const String& s) { if (cfg.log_level >= 2) Serial.println(s); }

// Forward declarations for globals used in helpers
extern volatile bool g_learn_active;

// --- Status helpers -------------------------------------------------
String macStr(const uint8_t* m) {
  char b[18];
  snprintf(b, sizeof(b), "%02X:%02X:%02X:%02X:%02X:%02X", m[0],m[1],m[2],m[3],m[4],m[5]);
  return String(b);
}

// ================== FS & Config ========================
static const char* CONFIG_PATH = "/config.json";
static const char* CODES_DIR   = "/codes";

bool saveConfig() {
  DynamicJsonDocument doc(1024);
  doc["mqtt_host"] = cfg.mqtt_host;
  doc["mqtt_port"] = cfg.mqtt_port;
  doc["mqtt_tls"]  = cfg.mqtt_tls;
  doc["mqtt_user"] = cfg.mqtt_user;
  doc["mqtt_pass"] = cfg.mqtt_pass;
  doc["mqtt_base"] = cfg.mqtt_base;
  doc["log_level"] = cfg.log_level;
  doc["ota_port"]  = cfg.ota_port;
  doc["learn_timeout_ms"] = cfg.learn_timeout_ms;
  doc["web_auth"]  = cfg.web_auth;
  doc["web_user"]  = cfg.web_user;
  doc["web_pass"]  = cfg.web_pass;

  File f = FSYS.open(CONFIG_PATH, "w");
  if (!f) return false;
  bool ok = (serializeJson(doc, f) > 0);
  f.close();
  return ok;
}

bool loadConfig() {
  if (!FSYS.exists(CONFIG_PATH)) return saveConfig();
  File f = FSYS.open(CONFIG_PATH, "r");
  if (!f) return false;
  DynamicJsonDocument doc(1024);
  DeserializationError e = deserializeJson(doc, f);
  f.close();
  if (e) return false;
  strlcpy(cfg.mqtt_host, doc["mqtt_host"] | "", sizeof(cfg.mqtt_host));
  cfg.mqtt_port = doc["mqtt_port"] | 1883;
  cfg.mqtt_tls  = doc["mqtt_tls"] | false;
  strlcpy(cfg.mqtt_user, doc["mqtt_user"] | "", sizeof(cfg.mqtt_user));
  strlcpy(cfg.mqtt_pass, doc["mqtt_pass"] | "", sizeof(cfg.mqtt_pass));
  strlcpy(cfg.mqtt_base, doc["mqtt_base"] | "ir", sizeof(cfg.mqtt_base));
  cfg.log_level = doc["log_level"] | 1;
  cfg.ota_port  = doc["ota_port"] | 8266;
  cfg.learn_timeout_ms = doc["learn_timeout_ms"] | 10000;
  cfg.web_auth  = doc["web_auth"] | 0;
  strlcpy(cfg.web_user, doc["web_user"] | "admin", sizeof(cfg.web_user));
  strlcpy(cfg.web_pass, doc["web_pass"] | "admin", sizeof(cfg.web_pass));
  return true;
}

// Unique name: IR-<chip>-GW
String makeHostname() {
#if defined(ESP8266)
  uint32_t chip = ESP.getChipId();
#elif defined(ESP32)
  uint32_t chip = (uint32_t)(ESP.getEfuseMac() & 0xFFFFFF);
#endif
  char buf[16];
  snprintf(buf, sizeof(buf), "%06X", chip);
  return String("IR-") + buf;
}

// =============== MQTT ===============================
void mqttCallback(char* topic, byte* payload, unsigned int len);
void mqttConnect() {
  if (strlen(cfg.mqtt_host) == 0) return;
  mqtt.setServer(cfg.mqtt_host, cfg.mqtt_port);
  mqtt.setCallback(mqttCallback);
  String cid = g_hostname + "-mqtt";
  if (strlen(cfg.mqtt_user) > 0) {
    mqtt.connect(cid.c_str(), cfg.mqtt_user, cfg.mqtt_pass);
  } else {
    mqtt.connect(cid.c_str());
  }
  if (mqtt.connected()) {
    String tcmd = String(cfg.mqtt_base) + "/" + g_chipId + "/cmd/#";
    mqtt.subscribe(tcmd.c_str());
    logi("[MQTT] Connected, subscribed: " + tcmd);
  }
}

void mqttPublish(const String& subtopic, const String& payload, bool retain=false) {
  if (!mqtt.connected()) return;
  String t = String(cfg.mqtt_base) + "/" + g_chipId + "/" + subtopic;
  mqtt.publish(t.c_str(), payload.c_str(), retain);
}

void mqttCallback(char* topic, byte* payload, unsigned int len) {
  String t = topic;
  String msg;
  msg.reserve(len+1);
  for (unsigned int i=0;i<len;i++) msg += (char)payload[i];
  logd("[MQTT] " + t + " = " + msg);

  // Topics:
  // <base>/<chip>/cmd/send_id -> {"id":"<file>"}
  // <base>/<chip>/cmd/send_raw -> {"khz":38,"raw":[...us...]}
  // <base>/<chip>/cmd/learn -> {"timeout":10000}
  if (t.endsWith("/cmd/send_id")) {
    DynamicJsonDocument doc(2048);
    if (!deserializeJson(doc, msg)) {
      String id = doc["id"] | "";
      if (id.length()) {
        bool ok = sendCodeById(id);
        mqttPublish("result", ok ? "{\"ok\":true}" : "{\"ok\":false}", false);
      }
    }
  } else if (t.endsWith("/cmd/send_raw")) {
    DynamicJsonDocument doc(4096);
    if (!deserializeJson(doc, msg)) {
      uint16_t khz = doc["khz"] | IR_KHZ_DEFAULT;
      JsonArray arr = doc["raw"].as<JsonArray>();
      size_t n = arr.size();
      if (n > 0 && n < 2000) {
        std::unique_ptr<uint16_t[]> timings(new uint16_t[n]);
        size_t i=0;
        for (JsonVariant v : arr) timings[i++] = (uint16_t)(v.as<int>());
        irsend.sendRaw(timings.get(), n, khz);
        mqttPublish("result", "{\"ok\":true}", false);
      }
    }
  } else if (t.endsWith("/cmd/learn")) {
    startLearn(cfg.learn_timeout_ms);
  }
}

// =============== IR Capture/Send =====================
// --- Status helpers -------------------------------------------------
uint32_t countCodesFiles() {
  uint32_t cnt = 0;
  File dir = FSYS.open("/codes", "r");   // <- nepoužívá CODES_DIR
  if (dir && dir.isDirectory()) {
    File f;
    while ((f = dir.openNextFile())) {
      if (!f.isDirectory() && String(f.name()).endsWith(".json")) cnt++;
      f.close();
    }
  }
  return cnt;
}

void printBootBanner() {
  Serial.println();
  Serial.println(F("============================================================"));
  Serial.println(F("=                ESP IR Transceiver — BOOT                 ="));
  Serial.println(F("============================================================"));

#if defined(ESP8266)
  Serial.printf_P(PSTR("[Chip] ESP8266 chipId=0x%06X core=%s\n"),
                  ESP.getChipId(), ESP.getCoreVersion().c_str());
  Serial.printf_P(PSTR("[CPU ] freq=%u MHz, freeHeap=%u B\n"),
                  ESP.getCpuFreqMHz(), ESP.getFreeHeap());
  // FS info (ESP8266)
  FSInfo info; LittleFS.info(info);
  Serial.printf("[FS  ] LittleFS mounted: %s\n", "yes"); // v setup už bylo FSYS.begin()
  Serial.printf("[FS  ] total=%u B, used=%u B\n", info.totalBytes, info.usedBytes);
#elif defined(ESP32)
  uint64_t mac = ESP.getEfuseMac();
  Serial.printf("[Chip] ESP32 efuseMAC=%04X%08X\n", (uint16_t)(mac>>32), (uint32_t)mac);
  Serial.printf("[CPU ] freq=%u MHz, freeHeap=%u B\n", ESP.getCpuFreqMHz(), ESP.getFreeHeap());
  // FS info (ESP32)
  size_t total = FSYS.totalBytes();
  size_t used  = FSYS.usedBytes();
  Serial.printf("[FS  ] LittleFS mounted: %s\n", "yes");
  Serial.printf("[FS  ] total=%u B, used=%u B\n", (unsigned)total, (unsigned)used);
#endif
}

void printNetworkStatus(const char* prefix = "[NET ]") {
  wl_status_t st = WiFi.status();
  Serial.printf("%s mode=%s, status=%d\n", prefix,
    (WiFi.getMode()==WIFI_STA?"STA":(WiFi.getMode()==WIFI_AP?"AP":(WiFi.getMode()==WIFI_AP_STA?"AP+STA":"OFF"))),
    (int)st);

  if (st == WL_CONNECTED) {
    Serial.printf("%s SSID=%s, IP=%s, GW=%s, DNS=%s\n", prefix,
      WiFi.SSID().c_str(),
      WiFi.localIP().toString().c_str(),
      WiFi.gatewayIP().toString().c_str(),
      WiFi.dnsIP().toString().c_str());
    Serial.printf("%s RSSI=%d dBm, BSSID=%s, Channel=%d\n", prefix,
      WiFi.RSSI(),
      WiFi.BSSIDstr().c_str(),
      WiFi.channel());
  }
}

void printServiceStatus() {
  Serial.printf("[HTTP] Web UI: http://%s/  or  http://%s/\n",
                WiFi.localIP().toString().c_str(), g_hostname.c_str());
  Serial.printf("[IR  ] learn_active=%s, codes=%lu\n",
                g_learn_active ? "true" : "false", (unsigned long)countCodesFiles());
  Serial.printf("[MQTT] host=%s, port=%u, connected=%s, base=%s/%s\n",
                cfg.mqtt_host, cfg.mqtt_port,
                mqtt.connected() ? "yes" : "no",
                cfg.mqtt_base, g_chipId.c_str());
}

void printOneLineHeartbeat() {
  Serial.printf("[HB ] IP=%s RSSI=%d MQTT=%s IR=%s codes=%lu Uptime=%lus\n",
    WiFi.localIP().toString().c_str(),
    WiFi.isConnected() ? WiFi.RSSI() : 0,
    mqtt.connected() ? "on" : "off",
    g_learn_active ? "learn" : "idle",
    (unsigned long)countCodesFiles(),
    (unsigned long)(millis()/1000UL));
}

volatile bool g_learn_active = false;
uint32_t g_learn_deadline_ms = 0;
String   g_last_id = "";
uint16_t g_last_len = 0;

String nowMsStr() { return String(millis()); }

bool ensureCodesDir() {
  if (!FSYS.exists(CODES_DIR)) return FSYS.mkdir(CODES_DIR);
  return true;
}

// Generate file id: code-<millis>.json
String makeCodeId() {
  char buf[32];
  snprintf(buf, sizeof(buf), "code-%lu.json", (unsigned long)millis());
  return String(buf);
}

// Save captured result as JSON; returns file id
// Save captured result as JSON; returns file id
String saveCaptured(const decode_results& res) {
  if (!ensureCodesDir()) return "";
  String id = makeCodeId();
  String path = String(CODES_DIR) + "/" + id;

  DynamicJsonDocument doc(8192);
  // Store meta & raw timing
  doc["ts"] = millis();
  doc["proto"] = typeToString(res.decode_type);
  doc["bits"] = res.bits;
  doc["address"] = res.address;
  doc["command"] = res.command;
  doc["repeat"] = res.repeat;

  // POZOR: některé verze IRremoteESP8266 neposkytují res.frequency
  // Uložíme default, odesílač při absenci použije IR_KHZ_DEFAULT.
  doc["frequency"] = IR_KHZ_DEFAULT;

  doc["rawlen"] = res.rawlen;

  // Raw timing (microseconds) as array
  JsonArray raw = doc.createNestedArray("raw");
  for (uint16_t i = 1; i < res.rawlen; i++) { // skip first gap
    raw.add(res.rawbuf[i] * kRawTick);        // kRawTick = 50us
  }

  File f = FSYS.open(path, "w");
  if (!f) return "";
  bool ok = (serializeJson(doc, f) > 0);
  f.close();
  if (!ok) return "";
  return id;
}


// Load code JSON and send
bool sendCodeById(const String& id) {
  String path = String(CODES_DIR) + "/" + id;
  if (!FSYS.exists(path)) return false;
  File f = FSYS.open(path, "r");
  if (!f) return false;
  DynamicJsonDocument doc(8192);
  DeserializationError e = deserializeJson(doc, f);
  f.close();
  if (e) return false;
  uint16_t khz = IR_KHZ_DEFAULT;
if (doc.containsKey("frequency")) {
  khz = (uint16_t)(doc["frequency"].as<int>());
  if (khz == 0) khz = IR_KHZ_DEFAULT;
}

  JsonArray arr = doc["raw"].as<JsonArray>();
  size_t n = arr.size();
  if (n == 0 || n > 2000) return false;
  std::unique_ptr<uint16_t[]> timings(new uint16_t[n]);
  size_t i=0;
  for (JsonVariant v : arr) timings[i++] = (uint16_t)(v.as<int>());
  ledSet(true);
  irsend.sendRaw(timings.get(), n, khz);
  ledSet(false);
  return true;
}

void startLearn(uint32_t timeout_ms) {
  g_learn_active = true;
  g_learn_deadline_ms = millis() + timeout_ms;
  irrecv.enableIRIn();
  logi("[IR] Learn started for " + String(timeout_ms) + " ms");
}

void stopLearn() {
  g_learn_active = false;
  irrecv.disableIRIn();
  logi("[IR] Learn stopped");
}

// =============== Web UI/REST =========================
bool checkAuth() {
  if (cfg.web_auth == 0) return true;
  if (!server.authenticate(cfg.web_user, cfg.web_pass)) {
    server.requestAuthentication();
    return false;
  }
  return true;
}

static const char APP_CSS[] PROGMEM = R"CSS(
:root{--bg:#0b0f12;--fg:#e6e6e6;--muted:#8899a6;--card:#121820;--accent:#4da3ff}
*{box-sizing:border-box}body{margin:0;font:14px/1.4 system-ui,Segoe UI,Roboto,Arial;background:var(--bg);color:var(--fg)}
.container{max-width:960px;margin:0 auto;padding:16px;display:grid;grid-template-columns:repeat(12,1fr);gap:12px}
.card{background:var(--card);border-radius:16px;padding:16px;box-shadow:0 6px 20px rgba(0,0,0,.3);grid-column:span 12}
h1,h2,h3{margin:.2em 0}h1{font-size:22px}.row{display:flex;gap:8px;flex-wrap:wrap}
.btn{background:#1e2630;color:#fff;border:1px solid #2a3442;border-radius:10px;padding:8px 12px;cursor:pointer}
.btn.primary{background:var(--accent);border-color:transparent;color:#001428}
.input{background:#0d1319;border:1px solid #2a3442;color:#fff;border-radius:10px;padding:8px 10px}
pre{white-space:pre-wrap;background:#0d1319;border:1px solid #2a3442;border-radius:10px;padding:8px}
table{width:100%;border-collapse:collapse}th,td{padding:8px;border-bottom:1px solid #223}
.badge{display:inline-block;padding:2px 6px;border-radius:999px;background:#1f2833;color:#a7c7ff;border:1px solid #2a3442}
.small{color:var(--muted);font-size:12px}
)CSS";

static const char APP_HTML[] PROGMEM = R"HTML(
<!doctype html><html lang="cs"><head>
<meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>ESP IR Transceiver</title><link rel="stylesheet" href="/app.css">
</head><body><div class="container">
  <div class="card"><h1>ESP IR Transceiver</h1>
    <div class="row">
      <button class="btn primary" onclick="learn()">Učit IR (10s)</button>
      <button class="btn" onclick="refresh()">Obnovit seznam</button>
    </div>
    <div id="status" class="small"></div>
  </div>
  <div class="card">
    <h3>Uložené IR kódy</h3>
    <table id="codes"><thead><tr><th>ID</th><th>Protokol</th><th>Bits</th><th>Délka</th><th>Akce</th></tr></thead><tbody></tbody></table>
  </div>
  <div class="card">
    <h3>Detail</h3>
    <pre id="detail">(vyberte kód)</pre>
  </div>
</div>
<script>
async function learn(){
  setStatus('Čekám na IR…'); 
  const r = await fetch('/api/learn?timeout=10000'); 
  const j = await r.json(); 
  setStatus(JSON.stringify(j));
  refresh();
}
function setStatus(s){document.getElementById('status').textContent=s}
async function refresh(){
  const r = await fetch('/api/codes'); const j = await r.json();
  const tb = document.querySelector('#codes tbody'); tb.innerHTML='';
  j.forEach(row=>{
    const tr = document.createElement('tr');
    tr.innerHTML = `<td><span class="badge">${row.id}</span></td>
      <td>${row.proto||'-'}</td><td>${row.bits||'-'}</td><td>${row.rawlen||'-'}</td>
      <td class="row">
        <button class="btn" onclick="send('${row.id}')">Odeslat</button>
        <button class="btn" onclick="show('${row.id}')">Detail</button>
        <button class="btn" onclick="del('${row.id}')">Smazat</button>
      </td>`;
    tb.appendChild(tr);
  });
}
async function send(id){
  const r = await fetch('/api/send?id='+encodeURIComponent(id)); const j = await r.json();
  setStatus(JSON.stringify(j));
}
async function show(id){
  const r = await fetch('/api/code?id='+encodeURIComponent(id)); const j = await r.json();
  document.getElementById('detail').textContent = JSON.stringify(j,null,2);
}
async function del(id){
  if(!confirm('Opravdu smazat '+id+'?')) return;
  const r = await fetch('/api/delete?id='+encodeURIComponent(id)); const j = await r.json();
  setStatus(JSON.stringify(j)); refresh();
}
refresh();
</script>
</body></html>
)HTML";

void handleRoot() {
  if (!checkAuth()) return;
  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.send(200, "text/html; charset=utf-8", "");
  WiFiClient client = server.client();
  client.write_P(APP_HTML, strlen_P(APP_HTML));
}

void handleCss() {
  if (!checkAuth()) return;
  server.send_P(200, "text/css", APP_CSS);
}

void replyJSON(int code, const String& body) {
  server.send(code, "application/json", body);
}

void handleLearn() {
  if (!checkAuth()) return;
  uint32_t timeout = server.hasArg("timeout") ? server.arg("timeout").toInt() : cfg.learn_timeout_ms;
  startLearn(timeout);
  replyJSON(200, "{\"ok\":true,\"timeout\":"+String(timeout)+"}");
}

void handleCodesList() {
  if (!checkAuth()) return;
  DynamicJsonDocument doc(4096);
  JsonArray arr = doc.to<JsonArray>();
  File dir = FSYS.open(CODES_DIR, "r");
  if (dir && dir.isDirectory()) {
    File f;
    while ((f = dir.openNextFile())) {
      if (!f.isDirectory()) {
        String name = f.name();
        if (name.endsWith(".json")) {
          // Summaries
          DynamicJsonDocument cdoc(1024);
          DeserializationError e = deserializeJson(cdoc, f);
          f.close();
          if (!e) {
            JsonObject o = arr.createNestedObject();
            String id = String(name).substring(String(CODES_DIR).length()+1);
            o["id"] = id;
            o["proto"] = cdoc["proto"];
            o["bits"] = cdoc["bits"];
            o["rawlen"] = cdoc["rawlen"];
          }
        } else {
          f.close();
        }
      } else {
        f.close();
      }
    }
  }
  String out; serializeJson(arr, out);
  replyJSON(200, out);
}

void handleCodeDetail() {
  if (!checkAuth()) return;
  String id = server.arg("id");
  String path = String(CODES_DIR) + "/" + id;
  if (!FSYS.exists(path)) return replyJSON(404, "{\"ok\":false,\"err\":\"notfound\"}");
  File f = FSYS.open(path, "r");
  if (!f) return replyJSON(500, "{\"ok\":false}");
  server.streamFile(f, "application/json");
  f.close();
}

void handleDelete() {
  if (!checkAuth()) return;
  String id = server.arg("id");
  String path = String(CODES_DIR) + "/" + id;
  if (!FSYS.exists(path)) return replyJSON(404, "{\"ok\":false}");
  bool ok = FSYS.remove(path);
  replyJSON(ok ? 200 : 500, ok ? "{\"ok\":true}" : "{\"ok\":false}");
}

void handleSendId() {
  if (!checkAuth()) return;
  String id = server.arg("id");
  bool ok = sendCodeById(id);
  replyJSON(ok ? 200 : 404, ok ? "{\"ok\":true}" : "{\"ok\":false}");
}

// =============== Setup & Loop =========================
void setupWiFi() {
  WiFi.mode(WIFI_STA);
  WiFiManager wm;
  wm.setClass("invert");
  String apName = g_hostname + "-Setup";
  wm.setConfigPortalBlocking(true);
  wm.setTimeout(120);
  bool res = wm.autoConnect(apName.c_str());
  if (!res) {
    // Start AP
    wm.startConfigPortal(apName.c_str());
  }
  logi("[WiFi] IP: " + WiFi.localIP().toString());
  printNetworkStatus();
  Serial.printf("[NET ] Hostname=%s\n", g_hostname.c_str());
  Serial.printf("[NET ] Web UI: http://%s/  or  http://%s/\n",
                WiFi.localIP().toString().c_str(), g_hostname.c_str());

}

void setupOTA() {
  ArduinoOTA.setHostname(g_hostname.c_str());
  ArduinoOTA.setPort(cfg.ota_port);
  ArduinoOTA.onStart([](){
    logi("[OTA] Start");
  });
  ArduinoOTA.onEnd([](){
    logi("[OTA] End");
  });
  ArduinoOTA.onProgress([](unsigned int p, unsigned int t){
    (void)p; (void)t;
  });
  ArduinoOTA.onError([](ota_error_t e){ Serial.printf("[OTA] Error %u\n", e); });
  ArduinoOTA.begin();
}

void setupWeb() {
  server.on("/", HTTP_GET, handleRoot);
  server.on("/app.css", HTTP_GET, handleCss);
  server.on("/api/learn", HTTP_GET, handleLearn);
  server.on("/api/codes", HTTP_GET, handleCodesList);
  server.on("/api/code", HTTP_GET, handleCodeDetail);
  server.on("/api/send", HTTP_GET, handleSendId);
  server.on("/api/delete", HTTP_GET, handleDelete);
  server.onNotFound([](){ server.send(404, "text/plain", "Not found"); });
  server.begin();
  logi("[HTTP] WebServer started");
}

void setupIR() {
  irsend.begin();
  irrecv.setUnknownThreshold(12); // Noise filter
  irrecv.enableIRIn();            // For immediate readiness; will disable when not learning
}

void setupModbus() {
#if USE_MODBUS
  mb.server();
  mb.addHreg(0, 0, 32); // allocate 32 registers
#endif
}

// Capture loop
void processLearn() {
  if (!g_learn_active) return;
  if (millis() > g_learn_deadline_ms) {
    stopLearn();
    mqttPublish("learn", "{\"ok\":false,\"reason\":\"timeout\"}", false);
    return;
  }
  if (irrecv.decode(&g_results)) {
    ledSet(true);
    String id = saveCaptured(g_results);
    ledSet(false);
    if (id.length()) {
      g_last_id = id;
      g_last_len = g_results.rawlen;
      // publish summary
      DynamicJsonDocument doc(1024);
      doc["ok"] = true;
      doc["id"] = id;
      doc["proto"] = typeToString(g_results.decode_type);
      doc["bits"] = g_results.bits;
      String out; serializeJson(doc, out);
      mqttPublish("learn", out, false);
      logi("[IR] Saved " + id);
#if USE_MODBUS
      mb_regs[1] = g_last_len;
      mb_regs[0] |= (1 << 3); // hasLastCapture
#endif
    } else {
      mqttPublish("learn", "{\"ok\":false,\"reason\":\"save_failed\"}", false);
    }
    stopLearn();
    irrecv.resume();
  }
}

// =============== Setup =================================
void setup() {
  pinMode(STATUS_LED, OUTPUT);
  ledSet(false);
  Serial.begin(115200);
  delay(10);
#if defined(ESP8266)
  FSYS.begin();
#elif defined(ESP32)
  FSYS.begin(true);
#endif

  loadConfig();
  g_hostname = makeHostname();
  printBootBanner();
#if defined(ESP8266)
  g_chipId = String(ESP.getChipId(), HEX);
#else
  g_chipId = String((uint32_t)(ESP.getEfuseMac() & 0xFFFFFF), HEX);
#endif

  setupWiFi();
  setupOTA();
  setupWeb();
  setupIR();
#if USE_MODBUS
  setupModbus();
#endif

  mqttConnect();
  ledSet(false);
    // Finální sumarizace po startu služeb
  printNetworkStatus();
  printServiceStatus();
}

// =============== Loop ==================================
uint32_t lastMqttMs = 0;
uint32_t lastStateMs = 0;

void loop() {
  ArduinoOTA.handle();
  server.handleClient();
  processLearn();

  if (!mqtt.connected()) {
    static uint32_t nextTry = 0;
    if (millis() > nextTry) {
      mqttConnect();
      nextTry = millis() + 5000;
    }
  } else {
    mqtt.loop();
  }

    static uint32_t lastHb = 0;
  if (millis() - lastHb > 30000UL) {  // 30 s
    printOneLineHeartbeat();
    lastHb = millis();
  }

  #if USE_MODBUS
  // Update status bits
  uint16_t st = 0;
  if (WiFi.isConnected()) st |= 1;
  if (mqtt.connected()) st |= (1 << 1);
  if (irrecv.isIdle() == false) st |= (1 << 2); // learning
  mb_regs[0] = st;
  // Process Modbus
  mb.task();
  // Command: send code by index (write to Hreg2)
  static uint16_t lastCmd = 0;
  uint16_t cmd = mb.Hreg(2);
  if (cmd != lastCmd) {
    lastCmd = cmd;
    // map index to file listing order
    // For simplicity, send by enumerating directory and selecting Nth.
    uint16_t i = 0;
    String selected;
    File dir = FSYS.open(CODES_DIR, "r");
    if (dir && dir.isDirectory()) {
      File f;
      while ((f = dir.openNextFile())) {
        if (!f.isDirectory()) {
          String name = f.name();
          f.close();
          if (name.endsWith(".json")) {
            if (i == cmd) { selected = String(name).substring(String(CODES_DIR).length()+1); break; }
            i++;
          }
        } else f.close();
      }
    }
    if (selected.length()) {
      bool ok = sendCodeById(selected);
      mb_regs[3] = ok ? 0 : 2;
    } else {
      mb_regs[3] = 1; // not found
    }
  }
#endif
}
