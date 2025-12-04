#include "WebUI.h"

#include <ESP8266WebServer.h>
#include <ESP8266WiFi.h>
#include <WiFiManager.h>      // kvůli factory resetu WiFi
#include "Logging.h"
#include "MqttHandler.h"
#include "ModbusHandler.h"
#include "ConfigStorage.h"

static WebUiConfig             gCfg;
static AcStateChangedCallback  gStateChangedCb = nullptr;
static ESP8266WebServer        server(80);

// HTML stránka v PROGMEM s identifikací a konfigurací zařízení
static const char INDEX_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width,initial-scale=1">
  <title>{{DEVICE_NAME}}</title>
  <style>
    body{font-family:sans-serif;margin:0;padding:0;background:#111;color:#eee;}
    header{padding:1rem;background:#222;}
    main{padding:1rem;}
    h1{margin:0;font-size:1.4rem;}
    h2{margin-top:0;margin-bottom:0.5rem;font-size:1.1rem;}
    h3{margin-top:0.8rem;margin-bottom:0.2rem;font-size:0.95rem;color:#ccc;}
    small{color:#aaa;}
    .card{background:#1e1e1e;border-radius:8px;padding:1rem;margin-bottom:1rem;}
    label{display:block;margin-top:0.5rem;font-size:0.85rem;}
    input,select{width:100%;padding:0.3rem;margin-top:0.2rem;border-radius:4px;border:1px solid #444;background:#111;color:#eee;}
    button{margin-top:0.8rem;padding:0.5rem 1rem;border:none;border-radius:4px;background:#3a8dde;color:#fff;cursor:pointer;}
    button:hover{background:#2f7ac3;}
    .btn-danger{background:#c0392b;}
    .btn-danger:hover{background:#a93226;}
    pre{background:#000;padding:0.5rem;border-radius:4px;max-height:220px;overflow:auto;font-size:0.8rem;}
    .grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(220px,1fr));gap:1rem;}
    .row{display:flex;flex-wrap:wrap;gap:0.5rem;font-size:0.85rem;}
    .row div{min-width:140px;}
  </style>
</head>
<body>
  <header>
    <h1>{{DEVICE_NAME}}</h1>
    <small>ID: {{DEVICE_ID}} &nbsp; | &nbsp; Location: {{DEVICE_LOCATION}}</small>
  </header>
  <main>
    <div class="card">
      <h2>Control</h2>
      <form id="stateForm">
        <label>Power
          <select id="power" name="power">
            <option value="off">Off</option>
            <option value="on">On</option>
          </select>
        </label>
        <label>Temperature [°C]
          <input id="temp" name="temp" type="number" min="17" max="30">
        </label>
        <label>Mode
          <select id="mode" name="mode">
            <option value="auto">Auto</option>
            <option value="cool">Cool</option>
            <option value="dry">Dry</option>
            <option value="heat">Heat</option>
            <option value="fan_only">Fan only</option>
          </select>
        </label>
        <label>Fan
          <select id="fan" name="fan">
            <option value="auto">Auto</option>
            <option value="quiet">Quiet</option>
            <option value="lvl_1">Level 1</option>
            <option value="lvl_2">Level 2</option>
            <option value="lvl_3">Level 3</option>
            <option value="lvl_4">Level 4</option>
            <option value="lvl_5">Level 5</option>
          </select>
        </label>
        <label>Swing
          <select id="swing" name="swing">
            <option value="fix">Fixed</option>
            <option value="v_swing">Vertical swing</option>
            <option value="h_swing">Horizontal swing</option>
            <option value="hv_swing">Vert+Horiz swing</option>
          </select>
        </label>
        <label>Pure / Ion
          <select id="pure" name="pure">
            <option value="off">Off</option>
            <option value="on">On</option>
          </select>
        </label>
        <label>Power select
          <select id="powerSelect" name="powerSelect">
            <option value="50%">50%</option>
            <option value="75%">75%</option>
            <option value="100%">100%</option>
          </select>
        </label>
        <button type="submit">Apply &amp; send IR</button>
      </form>
    </div>

    <div class="grid">
      <div class="card">
        <h2>Status</h2>
        <div class="row">
          <div>Power: <span id="s_power">-</span></div>
          <div>Temp: <span id="s_temp">-</span> °C</div>
          <div>Mode: <span id="s_mode">-</span></div>
          <div>Fan: <span id="s_fan">-</span></div>
          <div>Swing: <span id="s_swing">-</span></div>
          <div>Pure: <span id="s_pure">-</span></div>
          <div>Power sel.: <span id="s_psel">-</span></div>
        </div>
      </div>

      <div class="card">
        <h2>Device</h2>
        <div class="row">
          <div>Device ID: <span id="d_id">{{DEVICE_ID}}</span></div>
          <div>Name: <span id="d_name">{{DEVICE_NAME}}</span></div>
          <div>Location: <span id="d_loc">{{DEVICE_LOCATION}}</span></div>
          <div>IP: <span id="d_ip">-</span></div>
          <div>WiFi: <span id="d_ssid">-</span></div>
          <div>RSSI: <span id="d_rssi">-</span> dBm</div>
          <div>Log level: <span id="d_loglevel">-</span></div>
        </div>
        <label>Change log level
          <select id="logLevelSel">
            <option value="ERROR">ERROR</option>
            <option value="WARN">WARN</option>
            <option value="INFO">INFO</option>
            <option value="DEBUG">DEBUG</option>
            <option value="VERBOSE">VERBOSE</option>
          </select>
        </label>
        <button type="button" onclick="changeLogLevel()">Set log level</button>
      </div>
    </div>

    <div class="card">
      <h2>Configuration</h2>
      <form id="configForm">
        <h3>Device</h3>
        <label>Device name
          <input id="c_deviceName" name="deviceName" type="text">
        </label>
        <label>Device ID
          <input id="c_deviceId" name="deviceId" type="text">
        </label>
        <label>Location
          <input id="c_deviceLocation" name="deviceLocation" type="text">
        </label>

        <h3>MQTT</h3>
        <label>Enabled
          <select id="c_mqttEnabled" name="mqttEnabled">
            <option value="1">Enabled</option>
            <option value="0">Disabled</option>
          </select>
        </label>
        <label>Host
          <input id="c_mqttHost" name="mqttHost" type="text">
        </label>
        <label>Port
          <input id="c_mqttPort" name="mqttPort" type="number" min="1" max="65535">
        </label>
        <label>Base topic
          <input id="c_mqttBaseTopic" name="mqttBaseTopic" type="text">
        </label>
        <label>User
          <input id="c_mqttUser" name="mqttUser" type="text">
        </label>
        <label>Password
          <input id="c_mqttPass" name="mqttPass" type="password">
        </label>

        <h3>Modbus TCP</h3>
        <label>Enabled
          <select id="c_modbusEnabled" name="modbusEnabled">
            <option value="1">Enabled</option>
            <option value="0">Disabled</option>
          </select>
        </label>
        <label>Unit ID
          <input id="c_modbusUnitId" name="modbusUnitId" type="number" min="1" max="247">
        </label>

        <button type="submit">Save configuration</button>
        <button type="button" class="btn-danger" onclick="factoryReset()">Factory reset</button>
      </form>
    </div>

    <div class="card">
      <h2>Logs</h2>
      <pre id="logBox">(loading...)</pre>
    </div>
  </main>

<script>
function updateFromState(data) {
  var s = data.state;
  document.getElementById('power').value = s.power;
  document.getElementById('temp').value = s.temp;
  document.getElementById('mode').value = s.mode;
  document.getElementById('fan').value = s.fan;
  document.getElementById('swing').value = s.swing;
  document.getElementById('pure').value = s.pure;
  document.getElementById('powerSelect').value = s.powerSelect;

  document.getElementById('s_power').textContent = s.power;
  document.getElementById('s_temp').textContent = s.temp;
  document.getElementById('s_mode').textContent = s.mode;
  document.getElementById('s_fan').textContent = s.fan;
  document.getElementById('s_swing').textContent = s.swing;
  document.getElementById('s_pure').textContent = s.pure;
  document.getElementById('s_psel').textContent = s.powerSelect;

  document.getElementById('d_ip').textContent = data.network.ip;
  document.getElementById('d_ssid').textContent = data.network.ssid;
  document.getElementById('d_rssi').textContent = data.network.rssi;
  document.getElementById('d_loglevel').textContent = data.logLevel;
  document.getElementById('logLevelSel').value = data.logLevel;
}

function updateConfig(data) {
  var d = data.device;
  document.getElementById('c_deviceName').value     = d.name;
  document.getElementById('c_deviceId').value       = d.id;
  document.getElementById('c_deviceLocation').value = d.location;

  var mq = data.mqtt;
  document.getElementById('c_mqttEnabled').value    = mq.enabled ? "1" : "0";
  document.getElementById('c_mqttHost').value       = mq.host;
  document.getElementById('c_mqttPort').value       = mq.port;
  document.getElementById('c_mqttBaseTopic').value  = mq.baseTopic;
  document.getElementById('c_mqttUser').value       = mq.user;
  document.getElementById('c_mqttPass').value       = mq.pass;

  var mb = data.modbus;
  document.getElementById('c_modbusEnabled').value  = mb.enabled ? "1" : "0";
  document.getElementById('c_modbusUnitId').value   = mb.unitId;
}

function loadState() {
  fetch('/api/state')
    .then(function(r){ return r.json(); })
    .then(function(data){ updateFromState(data); })
    .catch(function(e){ console.log(e); });
}

function loadConfig() {
  fetch('/api/config')
    .then(function(r){ return r.json(); })
    .then(function(data){ updateConfig(data); })
    .catch(function(e){ console.log(e); });
}

function submitState(ev) {
  ev.preventDefault();
  var form = document.getElementById('stateForm');
  var fd = new FormData(form);
  var params = new URLSearchParams(fd);
  fetch('/api/state', {
    method: 'POST',
    headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
    body: params.toString()
  }).then(function(){ loadState(); });
}

function submitConfig(ev) {
  ev.preventDefault();
  var form = document.getElementById('configForm');
  var fd = new FormData(form);
  var params = new URLSearchParams(fd);
  fetch('/api/config', {
    method: 'POST',
    headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
    body: params.toString()
  }).then(function(){
    alert('Configuration saved. Some changes may require reconnection (MQTT).');
    loadConfig();
  });
}

function loadLog() {
  fetch('/api/log')
    .then(function(r){ return r.text(); })
    .then(function(text){ document.getElementById('logBox').textContent = text; })
    .catch(function(e){ console.log(e); });
}

function changeLogLevel() {
  var lvl = document.getElementById('logLevelSel').value;
  fetch('/api/loglevel', {
    method: 'POST',
    headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
    body: 'level=' + encodeURIComponent(lvl)
  }).then(function(){ loadState(); });
}

function factoryReset() {
  if (!confirm('Opravdu resetovat do továrního nastavení? WiFi a konfigurace budou ztraceny a zařízení se restartuje.')) return;
  fetch('/api/reset', {
    method: 'POST'
  }).then(function(){
    alert('Factory reset spuštěn, zařízení se restartuje...');
  });
}

document.addEventListener('DOMContentLoaded', function() {
  document.getElementById('stateForm').addEventListener('submit', submitState);
  document.getElementById('configForm').addEventListener('submit', submitConfig);
  loadState();
  loadConfig();
  loadLog();
  setInterval(loadState, 5000);
  setInterval(loadLog, 5000);
});
</script>
</body>
</html>
)rawliteral";

static void handleRoot() {
  String page = FPSTR(INDEX_HTML);
  page.replace("{{DEVICE_NAME}}", gCfg.deviceName);
  page.replace("{{DEVICE_ID}}",   gCfg.deviceId);
  page.replace("{{DEVICE_LOCATION}}", gCfg.location);
  server.send(200, "text/html", page);
}

static void handleGetState() {
  String json;
  json.reserve(512);

  json += F("{\"device\":{");
  json += F("\"name\":\"");     json += gCfg.deviceName; json += F("\",");
  json += F("\"id\":\"");       json += gCfg.deviceId;   json += F("\",");
  json += F("\"location\":\""); json += gCfg.location;   json += F("\"},");

  json += F("\"state\":{");
  json += F("\"power\":\"");        json += gAcState.power;       json += F("\",");
  json += F("\"temp\":");           json += gAcState.temp;        json += F(",");
  json += F("\"mode\":\"");         json += gAcState.mode;        json += F("\",");
  json += F("\"fan\":\"");          json += gAcState.fan;         json += F("\",");
  json += F("\"swing\":\"");        json += gAcState.swing;       json += F("\",");
  json += F("\"pure\":\"");         json += gAcState.pure;        json += F("\",");
  json += F("\"powerSelect\":\"");  json += gAcState.powerSelect; json += F("\"},");

  json += F("\"network\":{");
  json += F("\"ip\":\"");   json += WiFi.localIP().toString(); json += F("\",");
  json += F("\"ssid\":\""); json += WiFi.SSID();               json += F("\",");
  json += F("\"rssi\":");   json += WiFi.RSSI();               json += F("},");
  json += F("\"logLevel\":\""); json += Logging::levelToString(Logging::getLevel()); json += F("\"");

  json += F("}");

  server.send(200, "application/json", json);
}

static void handlePostState() {
  bool changed = false;

  if (server.hasArg("power")) {
    gAcState.power = server.arg("power");
    changed = true;
  }
  if (server.hasArg("temp")) {
    gAcState.temp = (uint8_t) server.arg("temp").toInt();
    changed = true;
  }
  if (server.hasArg("mode")) {
    gAcState.mode = server.arg("mode");
    changed = true;
  }
  if (server.hasArg("fan")) {
    gAcState.fan = server.arg("fan");
    changed = true;
  }
  if (server.hasArg("swing")) {
    gAcState.swing = server.arg("swing");
    changed = true;
  }
  if (server.hasArg("pure")) {
    gAcState.pure = server.arg("pure");
    changed = true;
  }
  if (server.hasArg("powerSelect")) {
    gAcState.powerSelect = server.arg("powerSelect");
    changed = true;
  }

  if (changed) {
    Logging::logf(Logging::LEVEL_INFO, "WEB", "Web UI changed AC state");
    AcState_normalize();
    if (gStateChangedCb) gStateChangedCb();
  }

  server.send(200, "text/plain", "OK");
}

static void handleGetLog() {
  String body;
  body.reserve(2048);
  uint8_t count = Logging::getLogCount();
  for (uint8_t i = 0; i < count; i++) {
    Logging::Level lvl;
    char line[120];
    if (!Logging::getLogLine(i, lvl, line, sizeof(line))) continue;
    body += line;
    body += '\n';
  }
  server.send(200, "text/plain", body);
}

static void handleSetLogLevel() {
  if (!server.hasArg("level")) {
    server.send(400, "text/plain", "missing 'level'");
    return;
  }
  String lvl = server.arg("level");
  lvl.toUpperCase();

  Logging::Level target = Logging::getLevel();
  if      (lvl == "ERROR")   target = Logging::LEVEL_ERROR;
  else if (lvl == "WARN")    target = Logging::LEVEL_WARN;
  else if (lvl == "INFO")    target = Logging::LEVEL_INFO;
  else if (lvl == "DEBUG")   target = Logging::LEVEL_DEBUG;
  else if (lvl == "VERBOSE") target = Logging::LEVEL_VERBOSE;

  Logging::setLevel(target);

  // Perzistence log levelu + ostatní configu
  MqttConfig mq = MqttHandler_getConfig();
  ModbusConfig mb = ModbusHandler_getConfig();
  ConfigStorage::save(gCfg, mq, mb, target);

  server.send(200, "text/plain", "OK");
}

// -------- Konfigurace Device / MQTT / Modbus -------------------------

static void handleGetConfig() {
  MqttConfig mq = MqttHandler_getConfig();
  ModbusConfig mb = ModbusHandler_getConfig();

  String json;
  json.reserve(512);

  json += F("{\"device\":{");
  json += F("\"name\":\"");     json += gCfg.deviceName; json += F("\",");
  json += F("\"id\":\"");       json += gCfg.deviceId;   json += F("\",");
  json += F("\"location\":\""); json += gCfg.location;   json += F("\"},");

  json += F("\"mqtt\":{");
  json += F("\"enabled\":");    json += (mq.enabled ? F("true") : F("false")); json += F(",");
  json += F("\"host\":\"");     json += mq.host;        json += F("\",");
  json += F("\"port\":");       json += mq.port;        json += F(",");
  json += F("\"baseTopic\":\"");json += mq.baseTopic;   json += F("\",");
  json += F("\"user\":\"");     json += mq.user;        json += F("\",");
  json += F("\"pass\":\"");     json += mq.pass;        json += F("\"},");

  json += F("\"modbus\":{");
  json += F("\"enabled\":");    json += (mb.enabled ? F("true") : F("false")); json += F(",");
  json += F("\"unitId\":");     json += mb.unitId;      json += F("}");

  json += F("}");

  server.send(200, "application/json", json);
}

static void handlePostConfig() {
  bool changedDevice = false;
  bool changedMqtt   = false;
  bool changedModbus = false;

  // Device identita (zatím v RAM)
  if (server.hasArg("deviceName")) {
    gCfg.deviceName = server.arg("deviceName");
    changedDevice = true;
  }
  if (server.hasArg("deviceId")) {
    gCfg.deviceId = server.arg("deviceId");
    changedDevice = true;
  }
  if (server.hasArg("deviceLocation")) {
    gCfg.location = server.arg("deviceLocation");
    changedDevice = true;
  }

  // MQTT
  MqttConfig mq = MqttHandler_getConfig();
  if (server.hasArg("mqttEnabled")) {
    mq.enabled = (server.arg("mqttEnabled") == "1");
    changedMqtt = true;
  }
  if (server.hasArg("mqttHost")) {
    mq.host = server.arg("mqttHost");
    changedMqtt = true;
  }
  if (server.hasArg("mqttPort")) {
    uint16_t port = (uint16_t) server.arg("mqttPort").toInt();
    if (port == 0) port = 1883;
    mq.port = port;
    changedMqtt = true;
  }
  if (server.hasArg("mqttBaseTopic")) {
    mq.baseTopic = server.arg("mqttBaseTopic");
    changedMqtt = true;
  }
  if (server.hasArg("mqttUser")) {
    mq.user = server.arg("mqttUser");
    changedMqtt = true;
  }
  if (server.hasArg("mqttPass")) {
    mq.pass = server.arg("mqttPass");
    changedMqtt = true;
  }
  if (changedMqtt) {
    MqttHandler_setConfig(mq);
  }

  // Modbus
  ModbusConfig mb = ModbusHandler_getConfig();
  if (server.hasArg("modbusEnabled")) {
    mb.enabled = (server.arg("modbusEnabled") == "1");
    changedModbus = true;
  }
  if (server.hasArg("modbusUnitId")) {
    uint8_t uid = (uint8_t) server.arg("modbusUnitId").toInt();
    if (uid == 0) uid = 1;
    mb.unitId = uid;
    changedModbus = true;
  }
  if (changedModbus) {
    ModbusHandler_setConfig(mb);
  }

  Logging::logf(Logging::LEVEL_INFO, "WEB",
                "Config updated via WebUI (device=%d mqtt=%d modbus=%d)",
                (int)changedDevice, (int)changedMqtt, (int)changedModbus);

  // Uložit aktuální konfiguraci do EEPROM
  ConfigStorage::save(gCfg, mq, mb, Logging::getLevel());

  server.send(200, "text/plain", "OK");
}

// -------- Factory reset ----------------------------------------------

static void handleReset() {
  Logging::logf(Logging::LEVEL_WARN, "WEB", "Factory reset requested from WebUI");

  // Reset AC stavu na default
  AcState_initDefaults();

  // Reset uložené konfigurace (MQTT/Modbus/Device/Log level)
  ConfigStorage::clear();

  // Reset WiFi konfigurace (uložené kredence) přes WiFiManager
  WiFiManager wm;
  wm.resetSettings();

  server.send(200, "text/plain", "OK");

  // malá pauza a restart
  delay(500);
  ESP.restart();
}

static void handleNotFound() {
  server.send(404, "text/plain", "Not found");
}

void WebUI_begin(const WebUiConfig &cfg, AcStateChangedCallback cb) {
  gCfg            = cfg;
  gStateChangedCb = cb;

  server.on("/",             HTTP_GET,  handleRoot);
  server.on("/api/state",    HTTP_GET,  handleGetState);
  server.on("/api/state",    HTTP_POST, handlePostState);
  server.on("/api/log",      HTTP_GET,  handleGetLog);
  server.on("/api/loglevel", HTTP_POST, handleSetLogLevel);

  server.on("/api/config",   HTTP_GET,  handleGetConfig);
  server.on("/api/config",   HTTP_POST, handlePostConfig);
  server.on("/api/reset",    HTTP_POST, handleReset);

  server.onNotFound(handleNotFound);

  server.begin();
  Logging::logf(Logging::LEVEL_INFO, "WEB", "HTTP server started on port 80");
}

void WebUI_loop() {
  server.handleClient();
}
