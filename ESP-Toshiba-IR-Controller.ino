#include <Arduino.h>

// ===== Wi-Fi + WebServer (ESP8266) =====
#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <DNSServer.h>
#include <WiFiManager.h>

// ===== IR knihovna =====
#include <IRremoteESP8266.h>
#include <ir_Toshiba.h>   // IRToshibaAC
#include "AcState.h"
#include "IRControl.h"
#include "MqttHandler.h"
#include "ModbusHandler.h"

// ===== Toshiba HVAC + IR generátor =====
#include "ToshibaCarrierHvac.h"
#include "ToshibaIrGenerator.h"

// ===== HW konfigurace =====
// ESP8266 Witty: IR LED výstup – doporučený GPIO4 (D2)
const uint16_t IR_SEND_PIN = 4;

// Jednoduchý HTTP server
ESP8266WebServer server(80);

// ===== Stav klimatizace je v AcState (gAcState) =====

// ===== MQTT config (můžeš později nastavovat přes WebUI) =====
WiFiClient gMqttNetClient;

MqttConfig gMqttCfg = {
  "192.168.1.10",   // host
  1883,             // port
  "esp-toshiba-ir", // clientId
  "toshiba/ac",     // baseTopic
  "",               // user
  "",               // pass
  false             // enabled (zapni až po otestování)
};

// ===== Identifikátor IR zařízení =====
String gDeviceId = "IR-1";

// ===== Modbus TCP config =====
ModbusConfig gModbusCfg = {
  1,     // unitId (adresa zařízení)
  true   // enabled
};

// ---------------------------------------------------------------------------
// Pomocné funkce pro HTML „checked/selected“ stav
// ---------------------------------------------------------------------------
String htmlChecked(const String &value, const String &current) {
  return (value == current) ? " checked" : "";
}

String htmlSelected(const String &value, const String &current) {
  return (value == current) ? " selected" : "";
}

// ---------------------------------------------------------------------------
// Vygenerování jednoduchého web UI
// ---------------------------------------------------------------------------
void handleRoot() {
  String html;
  html.reserve(4000);
  
  struct Option { const char* value; const char* label; };

  html += F(
      "<!DOCTYPE html><html><head><meta charset='utf-8'>"
      "<meta name='viewport' content='width=device-width, initial-scale=1'>"
      "<title>Toshiba IR Remote</title>"
      "<style>"
      "body{font-family:Arial,Helvetica,sans-serif;background:#0b0b0f;color:#f3f3f3;margin:0;padding:0;}"
      ".wrap{max-width:640px;margin:0 auto;padding:16px;}"
      "h1{font-size:1.5rem;margin:0 0 12px;}"
      "fieldset{border:1px solid #2a2a33;border-radius:10px;margin:8px 0 16px;padding:10px 14px;background:#15151c;}"
      "legend{padding:0 6px;font-weight:bold;}"
      "label{display:inline-block;margin:4px 6px 4px 0;}"
      "input[type=number],input[type=text],input[type=password],select{width:100%;padding:6px 8px;margin:4px 0 8px;"
      "border-radius:6px;border:1px solid #444;background:#111722;color:#f0f0f0;box-sizing:border-box;}"
      "input[type=checkbox]{margin-right:4px;}"
      ".btn{display:inline-block;background:#2c7be5;color:#fff;border:none;border-radius:999px;"
      "padding:8px 16px;font-size:0.95rem;cursor:pointer;margin-right:8px;}"
      ".btn-secondary{background:#444;}"
      ".row{display:flex;gap:8px;flex-wrap:wrap;}"
      ".row>div{flex:1;min-width:120px;}"
      ".status{font-size:0.9rem;color:#aaa;margin-bottom:12px;}"
      "a{color:#8ab4ff;}"
      ".chip-group{display:flex;flex-wrap:wrap;gap:6px;margin:4px 0 8px;}"
      ".chip{cursor:pointer;display:inline-flex;align-items:center;}"
      ".chip input{display:none;}"
      ".chip span{display:inline-flex;align-items:center;gap:4px;padding:6px 12px;border-radius:999px;"
      "border:1px solid #444;background:#111722;color:#eee;font-size:0.85rem;}"
      ".chip input:checked~span{background:#2c7be5;border-color:#2c7be5;color:#fff;}"
      ".power-group{display:flex;gap:8px;flex-wrap:wrap;margin-top:4px;}"
      ".power-radio{cursor:pointer;display:inline-flex;align-items:center;}"
      ".power-radio input{display:none;}"
      ".power-radio span{display:inline-flex;align-items:center;justify-content:center;"
      "padding:6px 12px;border-radius:999px;border:1px solid #444;background:#111722;"
      "color:#eee;font-size:0.9rem;margin:2px 4px;}"
      ".power-radio input:checked~span{background:#2c7be5;border-color:#2c7be5;color:#fff;}"
      ".temp-row{margin-top:4px;}"
      ".temp-slider{display:flex;align-items:center;gap:8px;}"
      ".temp-slider input[type=range]{flex:1;}"
      ".temp-value{min-width:3ch;text-align:right;font-variant-numeric:tabular-nums;}"
      "</style></head><body><div class='wrap'>"
      "<h1>Toshiba IR Remote Bridge (ESP8266)</h1>"
      "<div class='status'>Wi-Fi: ");

  html += WiFi.SSID();
  html += F(" &middot; IP: ");
  html += WiFi.localIP().toString();
  html += F(" &middot; ID: ");
  html += gDeviceId;
  html += F("</div>"
            "<form action='/set' method='GET'>");

  // Power
  html += F("<fieldset><legend>Napájení</legend>");
  html += F("<label><input type='radio' name='power' value='on'");
  html += htmlChecked("on", gAcState.power);
  html += F("> Zapnuto</label>");
  html += F("<label><input type='radio' name='power' value='off'");
  html += htmlChecked("off", gAcState.power);
  html += F("> Vypnuto</label>");
  html += F("</fieldset>");

  // Mode + teplota
  html += F("<fieldset><legend>Režim &amp; teplota</legend>");
  html += F("<label>Režim:</label>");
  html += F("<div class='chip-group'>");

  // Auto
  html += F("<label class='chip'><input type='radio' name='mode' value='auto'");
  if (gAcState.mode == "auto") html += F(" checked");
  html += F("><span>&#9881; Auto</span></label>");

  // Chlazení
  html += F("<label class='chip'><input type='radio' name='mode' value='cool'");
  if (gAcState.mode == "cool") html += F(" checked");
  html += F("><span>&#10052; Chlazení</span></label>");

  // Dry (odvlhčování)
  html += F("<label class='chip'><input type='radio' name='mode' value='dry'");
  if (gAcState.mode == "dry") html += F(" checked");
  html += F("><span>&#128166; Dry</span></label>");

  // Topení
  html += F("<label class='chip'><input type='radio' name='mode' value='heat'");
  if (gAcState.mode == "heat") html += F(" checked");
  html += F("><span>&#128293; Topení</span></label>");

  // Pouze ventilátor
  html += F("<label class='chip'><input type='radio' name='mode' value='fan_only'");
  if (gAcState.mode == "fan_only") html += F(" checked");
  html += F("><span>&#127788; Ventilátor</span></label>");

  html += F("</div>");

  html += F("<div class='temp-row'><label>Nastavená teplota [&deg;C]:</label>");
  html += F("<div class='temp-slider'>");
  html += F("<input id='tempRange' type='range' name='temp' min='17' max='30' value='");
  html += String(gAcState.temp);
  html += F("'>");
  html += F("<span class='temp-value' id='tempVal'>");
  html += String(gAcState.temp);
  html += F(" &deg; C</span>");
  html += F("</div></div>");

  html += F("</fieldset>");

  // Fan + swing
  html += F("<fieldset><legend>Ventilátor &amp; lamely</legend>");

  html += F("<label>Rychlost ventilátoru:</label><select name='fan'>");
  const Option fans[] = {
      {"auto",  "Auto"},
      {"quiet", "Tichý"},
      {"lvl_1", "1"},
      {"lvl_2", "2"},
      {"lvl_3", "3"},
      {"lvl_4", "4"},
      {"lvl_5", "5"},
  };
  for (const auto &f : fans) {
    html += "<option value='";
    html += f.value;
    html += "'";
    html += htmlSelected(f.value, gAcState.fan);
    html += ">";
    html += f.label;
    html += "</option>";
  }
  html += F("</select>");

  html += F("<label>Pohyb lamel (swing):</label><select name='swing'>");
  const Option swings[] = {
      {"auto",     "Auto"},
      {"fix",      "Fixní pozice"},
      {"v_swing",  "Vertikální swing"},
      {"h_swing",  "Horizontální swing"},
      {"hv_swing", "Oba směry"},
  };
  for (const auto &sw : swings) {
    html += "<option value='";
    html += sw.value;
    html += "'";
    html += htmlSelected(sw.value, gAcState.swing);
    html += ">";
    html += sw.label;
    html += "</option>";
  }
  html += F("</select>");

  html += F("</fieldset>");

  // Filtr + výkon
  html += F("<fieldset><legend>Další funkce</legend>");

  html += F("<div class='row'><div>");
  html += F("<label>Čistič / ion (PURE):</label><select name='pure'>");
  const Option pures[] = {
      {"off", "Vypnuto"},
      {"on",  "Zapnuto"},
  };
  for (const auto &p : pures) {
    html += "<option value='";
    html += p.value;
    html += "'";
    html += htmlSelected(p.value, gAcState.pure);
    html += ">";
    html += p.label;
    html += "</option>";
  }
  html += F("</select></div><div>");

  html += F("<label>Výkon:</label>");
  html += F("<div class='power-group'>");

  // ECO  -> powerSelect = 50 %
  html += F("<label class='power-radio'><input type='radio' name='pselect' value='50%'");
  if (gAcState.powerSelect == "50%") html += F(" checked");
  html += F("><span>ECO</span></label>");

  // NORMAL -> powerSelect = 75 %
  html += F("<label class='power-radio'><input type='radio' name='pselect' value='75%'");
  if (gAcState.powerSelect == "75%") html += F(" checked");
  html += F("><span>NORMAL</span></label>");

  // HI-POWER -> powerSelect = 100 %
  html += F("<label class='power-radio'><input type='radio' name='pselect' value='100%'");
  if (gAcState.powerSelect == "100%") html += F(" checked");
  html += F("><span>HI-POWER</span></label>");

  html += F("</div></div>");

  html += F("</fieldset>");

  // Identifikace zařízení
  html += F("<fieldset><legend>Identifikace zařízení</legend>");
  html += F("<label>ID zařízení:</label>");
  html += F("<input type='text' name='device_id' maxlength='32' value='");
  html += gDeviceId;
  html += F("'></fieldset>");

  // MQTT konfigurace
  html += F("<fieldset><legend>MQTT</legend>");
  html += F("<label><input type='checkbox' name='mqtt_enabled' value='1'");
  if (gMqttCfg.enabled) html += F(" checked");
  html += F("> Povolit MQTT</label>");

  html += F("<label>MQTT host:</label><input type='text' name='mqtt_host' value='");
  html += gMqttCfg.host;
  html += F("'>");

  html += F("<div class='row'><div>");
  html += F("<label>Port:</label><input type='number' name='mqtt_port' min='1' max='65535' value='");
  html += String(gMqttCfg.port);
  html += F("'></div><div>");
  html += F("<label>Client ID:</label><input type='text' name='mqtt_clientId' value='");
  html += gMqttCfg.clientId;
  html += F("'></div></div>");

  html += F("<label>Base topic:</label><input type='text' name='mqtt_baseTopic' value='");
  html += gMqttCfg.baseTopic;
  html += F("'>");

  html += F("<div class='row'><div>");
  html += F("<label>Uživatel:</label><input type='text' name='mqtt_user' value='");
  html += gMqttCfg.user;
  html += F("'></div><div>");
  html += F("<label>Heslo:</label><input type='password' name='mqtt_pass' value='");
  html += gMqttCfg.pass;
  html += F("'></div></div>");

  html += F("</fieldset>");

  // Modbus TCP konfigurace
  html += F("<fieldset><legend>Modbus TCP</legend>");
  html += F("<label><input type='checkbox' name='modbus_enabled' value='1'");
  if (gModbusCfg.enabled) html += F(" checked");
  html += F("> Povolit Modbus TCP server</label>");

  html += F("<label>Unit ID (adresa zařízení):</label>");
  html += F("<input type='number' name='modbus_unitId' min='1' max='247' value='");
  html += String(gModbusCfg.unitId);
  html += F("'>");

  html += F("</fieldset>");

  // Tlačítka
  html += F("<div style='margin-top:8px;'>"
            "<button class='btn' type='submit'>Odeslat IR</button>"
            "<button class='btn btn-secondary' type='submit' name='refresh' value='1'>Jen uložit / obnovit</button>"
            "</div>");

  html += F("</form>"
            "<p style='margin-top:16px;font-size:0.8rem;color:#777;'>"
            "Tento ESP8266 Witty nahrazuje IR ovladač Toshiba (např. WH-L11SE). "
            "IR rámec se generuje podle aktuálního stavu UI a odesílá se přes IR LED."
            "</p>"
            "<script>"
            "!(function(){var s=document.getElementById('tempRange');"
            "var v=document.getElementById('tempVal');"
            "if(s&&v){v.textContent=s.value+'\\u00B0C';"
            "s.addEventListener('input',function(){v.textContent=this.value+'\\u00B0C';});}})();"
            "</script>"
            "</div></body></html>");

  server.send(200, "text/html", html);
}

// ---------------------------------------------------------------------------
// Zpracování /set?… – aktualizace stavu z formuláře + odeslání IR
// ---------------------------------------------------------------------------
void handleSet() {
  // ----- AC stav z webového formuláře -----
  if (server.hasArg("power")) {
    String p = server.arg("power");
    if (p == "on" || p == "off") gAcState.power = p;
  }

  if (server.hasArg("mode")) {
    gAcState.mode = server.arg("mode");
  }

  if (server.hasArg("temp")) {
    int t = server.arg("temp").toInt();
    if (t < 17) t = 17;
    if (t > 30) t = 30;
    gAcState.temp = (uint8_t)t;
  }

  if (server.hasArg("fan")) {
    gAcState.fan = server.arg("fan");
  }

  if (server.hasArg("swing")) {
    gAcState.swing = server.arg("swing");
  }

  if (server.hasArg("pure")) {
    gAcState.pure = server.arg("pure");
  }

  if (server.hasArg("pselect")) {
    gAcState.powerSelect = server.arg("pselect");
  }

  // ----- ID zařízení z WebUI -----
  if (server.hasArg("device_id")) {
    gDeviceId = server.arg("device_id");
  }

  // ----- MQTT konfigurace z WebUI -----
  bool mqttCfgChanged = false;
  MqttConfig newMqttCfg = gMqttCfg;

  bool mqttEnabled = server.hasArg("mqtt_enabled");
  if (newMqttCfg.enabled != mqttEnabled) {
    newMqttCfg.enabled = mqttEnabled;
    mqttCfgChanged = true;
  }

  if (server.hasArg("mqtt_host")) {
    String v = server.arg("mqtt_host");
    if (v != newMqttCfg.host) {
      newMqttCfg.host = v;
      mqttCfgChanged = true;
    }
  }

  if (server.hasArg("mqtt_port")) {
    uint16_t p = server.arg("mqtt_port").toInt();
    if (p == 0) p = 1883;
    if (p != newMqttCfg.port) {
      newMqttCfg.port = p;
      mqttCfgChanged = true;
    }
  }

  if (server.hasArg("mqtt_clientId")) {
    String v = server.arg("mqtt_clientId");
    if (v.length() && v != newMqttCfg.clientId) {
      newMqttCfg.clientId = v;
      mqttCfgChanged = true;
    }
  }

  if (server.hasArg("mqtt_baseTopic")) {
    String v = server.arg("mqtt_baseTopic");
    if (v.length() && v != newMqttCfg.baseTopic) {
      newMqttCfg.baseTopic = v;
      mqttCfgChanged = true;
    }
  }

  if (server.hasArg("mqtt_user")) {
    String v = server.arg("mqtt_user");
    if (v != newMqttCfg.user) {
      newMqttCfg.user = v;
      mqttCfgChanged = true;
    }
  }

  if (server.hasArg("mqtt_pass")) {
    String v = server.arg("mqtt_pass");
    if (v != newMqttCfg.pass) {
      newMqttCfg.pass = v;
      mqttCfgChanged = true;
    }
  }

  if (mqttCfgChanged) {
    gMqttCfg = newMqttCfg;
    MqttHandler_setConfig(gMqttCfg);
  }

  // ----- Modbus TCP konfigurace z WebUI -----
  bool modbusCfgChanged = false;
  ModbusConfig newModbusCfg = gModbusCfg;

  bool modbusEnabled = server.hasArg("modbus_enabled");
  if (newModbusCfg.enabled != modbusEnabled) {
    newModbusCfg.enabled = modbusEnabled;
    modbusCfgChanged = true;
  }

  if (server.hasArg("modbus_unitId")) {
    int uid = server.arg("modbus_unitId").toInt();
    if (uid < 1) uid = 1;
    if (uid > 247) uid = 247;
    if (newModbusCfg.unitId != (uint8_t)uid) {
      newModbusCfg.unitId = (uint8_t)uid;
      modbusCfgChanged = true;
    }
  }

  if (modbusCfgChanged) {
    gModbusCfg = newModbusCfg;
    ModbusHandler_setConfig(gModbusCfg);
    // (Re)start Modbus server podle nové konfigurace
    ModbusHandler_begin();
  }

  // ----- Akce po submitu -----
  bool justRefresh = server.hasArg("refresh");

  if (!justRefresh) {
    Serial.println(F("[AC] Web UI changed state, propagating..."));
    onExternalAcStateChanged();
  }

  // Redirect zpět na /
  server.sendHeader("Location", "/", true);
  server.send(302, "text/plain", "");
}


// Centrální callback – volají ho WebUI, MQTT i Modbus,
// když změní gAcState. Tady se postaráme o:
// 1) IR odeslání
// 2) MQTT stavový publish
// 3) Modbus update registrů (zpětná vazba)
void onExternalAcStateChanged() {
  IRControl_sendFromState();
  MqttHandler_publishState();
  ModbusHandler_updateRegsFromState();
}

// ---------------------------------------------------------------------------
// Setup
// ---------------------------------------------------------------------------
void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println();
  Serial.println(F("==== Toshiba IR Remote (ESP8266 Witty) ===="));

  Serial.print(F("IR pin: GPIO"));
  Serial.println(IR_SEND_PIN);

   // Stav klimatizace
  AcState_initDefaults();

  // Inicializace IR vysílače
  IRControl_begin(IR_SEND_PIN);

  // WiFiManager – konfigurační portál
  WiFiManager wm;
  wm.setConfigPortalBlocking(true);
  wm.setDebugOutput(true);

  if (!wm.autoConnect("Toshiba-IR-Setup")) {
    Serial.println(F("WiFiManager: nepodařilo se připojit, restartuji..."));
    delay(3000);
    ESP.restart();
  }

    // MQTT
  MqttHandler_init(gMqttNetClient, onExternalAcStateChanged);
  MqttHandler_setConfig(gMqttCfg);

  // Modbus TCP
  ModbusHandler_init(gModbusCfg, onExternalAcStateChanged);
  ModbusHandler_begin();

  Serial.print(F("Wi-Fi připojeno, IP: "));
  Serial.println(WiFi.localIP());

  // HTTP routy
  server.on("/", handleRoot);
  server.on("/set", HTTP_GET, handleSet);
  server.onNotFound([]() {
    server.sendHeader("Location", "/", true);
    server.send(302, "text/plain", "");
  });

  server.begin();
  Serial.println(F("HTTP server běží na portu 80."));
}

// ---------------------------------------------------------------------------
// Loop
// ---------------------------------------------------------------------------
void loop() {
  MqttHandler_loop();
  ModbusHandler_loop();
  server.handleClient();
}
