#include <Arduino.h>

// ===== Wi-Fi + WebServer (ESP8266) =====
#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <DNSServer.h>
#include <WiFiManager.h>

// ===== IR knihovna =====
#include <IRremoteESP8266.h>
#include <IRsend.h>
#include <ir_Toshiba.h>   // IRToshibaAC

// ===== Toshiba HVAC + IR generátor =====
#include "ToshibaCarrierHvac.h"
#include "ToshibaIrGenerator.h"

// ===== HW konfigurace =====
// ESP8266 Witty: IR LED výstup – doporučený GPIO4 (D2)
const uint16_t IR_SEND_PIN = 4;

// Globální „chytrý“ Toshiba sender z IRremoteESP8266
IRToshibaAC irToshiba(IR_SEND_PIN);

// Jednoduchý HTTP server
ESP8266WebServer server(80);

// ===== Stav „virtuálního ovladače“ (web UI) =====

String  uiState        = "off";      // "on" / "off"
uint8_t uiTemp         = 24;         // 17–30
String  uiMode         = "auto";     // "auto","cool","dry","heat","fan_only"
String  uiFan          = "auto";     // "quiet","lvl_1".."lvl_5","auto"
String  uiSwing        = "fix";      // "fix","v_swing","h_swing","hv_swing","fix_pos_X"
String  uiPure         = "off";      // "on","off"
String  uiPowerSelect  = "100%";     // "50%","75%","100%"
String  uiOperation    = "normal";   // "normal","high_power",...
String  uiWifiLed      = "off";      // placeholder, aby to sedělo na hvacSettings

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

  html += F(
      "<!DOCTYPE html><html><head><meta charset='utf-8'>"
      "<meta name='viewport' content='width=device-width, initial-scale=1'>"
      "<title>Toshiba IR Remote</title>"
      "<style>"
      "body{font-family:Arial,Helvetica,sans-serif;background:#111;color:#eee;margin:0;padding:0;}"
      ".wrap{max-width:480px;margin:0 auto;padding:16px;}"
      "h1{font-size:1.4rem;margin:0 0 12px;}"
      "fieldset{border:1px solid #444;border-radius:8px;margin:8px 0 16px;padding:8px 12px;}"
      "legend{padding:0 6px;font-weight:bold;}"
      "label{display:inline-block;margin:4px 6px 4px 0;}"
      "input[type=number],select{width:100%;padding:6px 8px;margin:4px 0 8px;"
      "border-radius:4px;border:1px solid #555;background:#222;color:#eee;box-sizing:border-box;}"
      ".btn{display:inline-block;background:#2c7be5;color:#fff;border:none;border-radius:4px;"
      "padding:8px 14px;font-size:1rem;cursor:pointer;margin-right:8px;}"
      ".btn-secondary{background:#555;}"
      ".row{display:flex;gap:8px;}"
      ".row>div{flex:1;}"
      ".status{font-size:0.9rem;color:#aaa;margin-bottom:8px;}"
      "a{color:#8ab4ff;}"
      "</style></head><body><div class='wrap'>"
      "<h1>Toshiba IR Remote (ESP8266)</h1>"
      "<div class='status'>Wi-Fi: ");

  html += WiFi.SSID();
  html += F(" &middot; IP: ");
  html += WiFi.localIP().toString();
  html += F("</div>"
            "<form action='/set' method='GET'>");

  // Power
  html += F("<fieldset><legend>Napájení</legend>");
  html += F("<label><input type='radio' name='power' value='on'");
  html += htmlChecked("on", uiState);
  html += F("> Zapnuto</label>");
  html += F("<label><input type='radio' name='power' value='off'");
  html += htmlChecked("off", uiState);
  html += F("> Vypnuto</label>");
  html += F("</fieldset>");

  // Mode + teplota
  html += F("<fieldset><legend>Režim &amp; teplota</legend>");
  html += F("<label>Režim:</label><select name='mode'>");

  struct Option { const char* value; const char* label; };
  const Option modes[] = {
      {"auto",      "Auto"},
      {"cool",      "Chlazení"},
      {"dry",       "Odvlhčování"},
      {"heat",      "Topení"},
      {"fan_only",  "Pouze ventilátor"},
  };
  for (const auto &m : modes) {
    html += "<option value='";
    html += m.value;
    html += "'";
    html += htmlSelected(m.value, uiMode);
    html += ">";
    html += m.label;
    html += "</option>";
  }
  html += F("</select>");

  html += F("<label>Nastavená teplota [\xC2\xB0""C]:</label>");
  html += "<input type='number' name='temp' min='17' max='30' value='";
  html += String(uiTemp);
  html += F("'>");

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
    html += htmlSelected(f.value, uiFan);
    html += ">";
    html += f.label;
    html += "</option>";
  }
  html += F("</select>");

  html += F("<label>Pohyb lamel (swing):</label><select name='swing'>");
  const Option swings[] = {
      {"fix",      "Fixní pozice"},
      {"v_swing",  "Vertikální swing"},
      {"h_swing",  "Horizontální swing"},
      {"hv_swing", "Oba směry"},
  };
  for (const auto &sw : swings) {
    html += "<option value='";
    html += sw.value;
    html += "'";
    html += htmlSelected(sw.value, uiSwing);
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
    html += htmlSelected(p.value, uiPure);
    html += ">";
    html += p.label;
    html += "</option>";
  }
  html += F("</select></div><div>");

  html += F("<label>Power select:</label><select name='pselect'>");
  const Option psel[] = {
      {"50%",  "50 %"},
      {"75%",  "75 %"},
      {"100%", "100 %"},
  };
  for (const auto &ps : psel) {
    html += "<option value='";
    html += ps.value;
    html += "'";
    html += htmlSelected(ps.value, uiPowerSelect);
    html += ">";
    html += ps.label;
    html += "</option>";
  }
  html += F("</select></div></div>");

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
            "</div></body></html>");

  server.send(200, "text/html", html);
}

// ---------------------------------------------------------------------------
// Zpracování /set?… – aktualizace stavu z formuláře + odeslání IR
// ---------------------------------------------------------------------------
void handleSet() {
  // Načíst parametry z GET
  if (server.hasArg("power")) {
    String p = server.arg("power");
    if (p == "on" || p == "off") uiState = p;
  }

  if (server.hasArg("mode")) {
    uiMode = server.arg("mode");
  }

  if (server.hasArg("temp")) {
    int t = server.arg("temp").toInt();
    if (t < 17) t = 17;
    if (t > 30) t = 30;
    uiTemp = static_cast<uint8_t>(t);
  }

  if (server.hasArg("fan")) {
    uiFan = server.arg("fan");
  }

  if (server.hasArg("swing")) {
    uiSwing = server.arg("swing");
  }

  if (server.hasArg("pure")) {
    uiPure = server.arg("pure");
  }

  if (server.hasArg("pselect")) {
    uiPowerSelect = server.arg("pselect");
  }

  // zatím jen defaulty pro další pole
  uiOperation = "normal";
  uiWifiLed   = "off";

  bool justRefresh = server.hasArg("refresh");

  if (!justRefresh) {
    Serial.println(F("[IR] Sending IR frame based on current UI settings"));

    // Sestavení hvacSettings s ukazateli na String::c_str().
    hvacSettings s;
    s.state        = uiState.c_str();
    s.setpoint     = uiTemp;
    s.mode         = uiMode.c_str();
    s.swing        = uiSwing.c_str();
    s.fanMode      = uiFan.c_str();
    s.pure         = uiPure.c_str();
    s.powerSelect  = uiPowerSelect.c_str();
    s.operation    = uiOperation.c_str();
    s.wifiLed      = uiWifiLed.c_str();

    // 1) vygenerovat 9B Toshiba 72bit frame
    ToshibaIR::Frame72 frame = ToshibaIR::buildFromHvacSettings(s);

    // Debug výpis 9 bytů rámce
    Serial.print(F("[IR] Frame72:"));
    for (uint8_t i = 0; i < 9; i++) {
      Serial.print(' ');
      if (frame.data[i] < 0x10) Serial.print('0');
      Serial.print(frame.data[i], HEX);
    }
    Serial.println();

    // 2) předat rámec knihovně IRToshibaAC a skutečně vyslat IR
    //
    // IRremoteESP8266 má pro Toshiba vlastní třídu, která zná správné
    // časování, počet opakování atd. My jí jen podstrčíme náš raw stav.
    //
    // Délku předáme 9 (72 bitů) – odpovídá „short state“ zprávě:
    // F2 0D 03 FC 01 .. .. .. CHK
    irToshiba.setRaw(frame.data, 9);

    // volitelně: můžeš si nechat vypsat toString:
    String dbg = irToshiba.toString();
    Serial.print(F("[IR] IRToshibaAC state: "));
    Serial.println(dbg);

    // Reálné vyslání IR – knihovna už obstará 38 kHz, leader, mezery atd.
    irToshiba.send();  // default repeat = kToshibaACMinRepeat

    Serial.println(F("[IR] Sent."));
  }

  // Redirect zpět na /
  server.sendHeader("Location", "/", true);
  server.send(302, "text/plain", "");
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

  // Inicializace Toshiba IR senderu
  irToshiba.begin();

  // WiFiManager – konfigurační portál
  WiFiManager wm;
  wm.setConfigPortalBlocking(true);
  wm.setDebugOutput(true);

  if (!wm.autoConnect("Toshiba-IR-Setup")) {
    Serial.println(F("WiFiManager: nepodařilo se připojit, restartuji..."));
    delay(3000);
    ESP.restart();
  }

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
  server.handleClient();
}
