#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <WiFiManager.h>     // tzapu/WiFiManager
#include <IRremote.hpp>      // Armin Joachimsmeyer (IRremote.hpp)
#include <Preferences.h>     // NVS (nastavení)
#include <LittleFS.h>        // souborový systém pro "learned" databázi
#include <FS.h>

// ====== HW ======
static const uint8_t IR_RX_PIN = 10;   // ESP32-C3: funguje i 4/5/10

// ====== IR deduplikace ======
static const uint32_t DUP_FILTER_MS = 120;

// ====== Noise filtr (bez rawlen – kompatibilní s tvojí verzí) ======
static bool isNoise(const IRData &d) {
  if (d.flags & IRDATA_FLAGS_WAS_OVERFLOW) return true;
  if (d.protocol == UNKNOWN) {
    if (d.numberOfBits == 0) return true;
    if (d.decodedRawData == 0) return true;
  }
  if (d.decodedRawData > 0 && d.decodedRawData < 0x100) return true; // přitvrdit klidně na 0x200
  if (d.numberOfBits > 0 && d.numberOfBits < 8) return true;
  return false;
}

// ====== Web server ======
WebServer server(80);

// ====== NVS nastavení ======
Preferences prefs;                 // namespace: "irrecv"
static bool g_showOnlyUnknown = false;

// ====== Historie IR (ring buffer) ======
struct IREvent {
  uint32_t ms;
  decode_type_t proto;
  uint8_t bits;
  uint32_t address;
  uint16_t command;
  uint32_t value;
  uint32_t flags;
};
static const size_t HISTORY_LEN = 10;
static IREvent history[HISTORY_LEN];
static size_t histWrite = 0;
static size_t histCount = 0;

// Poslední validní UNKNOWN pro “Learn”
static bool   hasLastUnknown = false;
static IREvent lastUnknown = {0};

// Stav pro dup filtr
static uint32_t lastValue = 0;
static decode_type_t lastProto = UNKNOWN;
static uint8_t lastBits = 0;
static uint32_t lastMs = 0;

// ====== Pomocné ======
const __FlashStringHelper* protoName(decode_type_t p) {
  switch (p) {
    case NEC: return F("NEC"); case NEC2: return F("NEC2"); case SONY: return F("SONY");
    case RC5: return F("RC5"); case RC6: return F("RC6"); case PANASONIC: return F("PANASONIC");
    case KASEIKYO: return F("KASEIKYO"); case SAMSUNG: return F("SAMSUNG"); case JVC: return F("JVC");
    case LG: return F("LG"); case APPLE: return F("APPLE"); case ONKYO: return F("ONKYO");
    default: return F("UNKNOWN");
  }
}

void addToHistory(const IRData &d) {
  IREvent e;
  e.ms      = millis();
  e.proto   = d.protocol;
  e.bits    = d.numberOfBits;
  e.address = d.address;
  e.command = d.command;
  e.value   = d.decodedRawData;
  e.flags   = d.flags;

  history[histWrite] = e;
  histWrite = (histWrite + 1) % HISTORY_LEN;
  if (histCount < HISTORY_LEN) histCount++;
}

void printLine(const IRData &d, bool isRepeatSuppressed) {
  if (g_showOnlyUnknown && d.protocol != UNKNOWN) return; // nezahlcuj log, když filtr aktivní
  Serial.print(F("[IR] "));
  Serial.print(protoName(d.protocol));
  Serial.print(F(" bits=")); Serial.print(d.numberOfBits);
  Serial.print(F(" addr=0x")); Serial.print(d.address, HEX);
  Serial.print(F(" cmd=0x")); Serial.print(d.command, HEX);
  Serial.print(F(" value=0x")); Serial.print(d.decodedRawData, HEX);
  if (d.flags & IRDATA_FLAGS_IS_REPEAT) Serial.print(F(" (REPEAT)"));
  if (isRepeatSuppressed) Serial.print(F(" (suppressed)"));
  Serial.println();
}

void printJSON(const IRData &d) {
  if (g_showOnlyUnknown && d.protocol != UNKNOWN) return; // respektuj filtr i pro JSON log
  Serial.print(F("{\"proto\":\""));
  Serial.print(protoName(d.protocol));
  Serial.print(F("\",\"bits\":"));
  Serial.print(d.numberOfBits);
  Serial.print(F(",\"addr\":"));
  Serial.print(d.address);
  Serial.print(F(",\"cmd\":"));
  Serial.print(d.command);
  Serial.print(F(",\"value\":"));
  Serial.print((uint32_t)d.decodedRawData);
  Serial.print(F(",\"flags\":"));
  Serial.print((uint32_t)d.flags);
  Serial.print(F(",\"ms\":"));
  Serial.print(millis());
  Serial.println(F("}"));
}

// ====== Wi-Fi / WiFiManager ======
String makeApName() {
  uint8_t mac[6]; WiFi.macAddress(mac);
  char buf[32];
  snprintf(buf, sizeof(buf), "IR-Receiver-Setup-%02X%02X", mac[4], mac[5]);
  return String(buf);
}

// Na začátek souboru přidej util funkci:
String jsonEscape(const String& s) {
  String o; o.reserve(s.length() + 8);
  for (size_t i = 0; i < s.length(); i++) {
    char c = s[i];
    if (c == '\"' || c == '\\') { o += '\\'; o += c; }
    else if (c == '\n') { o += "\\n"; }
    else if (c == '\r') { o += "\\r"; }
    else if (c == '\t') { o += "\\t"; }
    else { o += c; }
  }
  return o;
}

void wifiSetupWithWiFiManager() {
  WiFi.mode(WIFI_STA);
  WiFiManager wm;
  wm.setTitle("IR Receiver – WiFi Setup");
  wm.setHostname("ir-receiver"); // pro mDNS by byl potřeba ještě MDNS.begin()

  String apName = makeApName();
  Serial.print(F("[NET] Připojuji Wi-Fi… "));
  if (!wm.autoConnect(apName.c_str())) {
    Serial.println(F("neúspěch, restartuji…"));
    delay(1000);
    ESP.restart();
  }
  Serial.println(F("OK"));
  Serial.print(F("[NET] IP: "));
  Serial.println(WiFi.localIP());
}

// ====== LittleFS – “databáze” learned kódů (JSON Lines) ======
// Každá položka je jeden JSON řádek s klíči:
//  ts, value, bits, addr, flags, vendor, proto_label, remote_label
static const char* LEARN_FILE = "/learned.jsonl";

// Uložení naučené položky – nyní s uložením názvu protokolu (proto) a "function" (název funkce tlačítka)
bool fsAppendLearned(uint32_t value,
                     uint8_t bits,
                     uint32_t addr,
                     uint32_t flags,
                     const String &protoStr,     // NOVÉ: název protokolu (např. "UNKNOWN", "NEC", ...)
                     const String &vendor,
                     const String &functionName, // dříve "protoLabel" -> sémanticky lepší "function"
                     const String &remoteLabel) {
  File f = LittleFS.open(LEARN_FILE, FILE_APPEND);
  if (!f) return false;

  String line;
  line.reserve(320);
  line += '{';
  line += "\"ts\":" + String((uint32_t)millis());
  line += ",\"proto\":\"";      line += protoStr;      line += '"';
  line += ",\"value\":" + String(value);
  line += ",\"bits\":" + String(bits);
  line += ",\"addr\":" + String(addr);
  line += ",\"flags\":" + String(flags);
  line += ",\"vendor\":\"";       line += jsonEscape(vendor);        line += '"';
  line += ",\"function\":\"";     line += jsonEscape(functionName);  line += '"';
  line += ",\"remote_label\":\""; line += jsonEscape(remoteLabel);   line += '"';
  line += "}\n";

  size_t w = f.print(line);
  f.close();
  return w == line.length();
}

// Vrátí JSON array všech naučených kódů
String fsReadLearnedAsArrayJSON() {
  File f = LittleFS.open(LEARN_FILE, FILE_READ);
  if (!f) return F("[]");
  String out; out.reserve(2048);
  out += '[';
  bool first = true;
  while (f.available()) {
    String line = f.readStringUntil('\n');
    line.trim();
    if (line.length() == 0) continue;
    if (!first) out += ',';
    out += line;
    first = false;
  }
  out += ']';
  f.close();
  return out;
}

// ====== Web UI ======

void handleRoot() {
  String html;
  html.reserve(9000);
  html += F(
    "<!doctype html><html lang='cs'><head><meta charset='utf-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>IR Receiver – ESP32-C3</title>"
    "<style>"
    "body{font-family:system-ui,-apple-system,Segoe UI,Roboto,Arial,sans-serif;margin:16px}"
    "h1{font-size:20px;margin:0 0 12px}"
    ".muted{color:#666}"
    "table{border-collapse:collapse;width:100%;max-width:980px}"
    "th,td{border:1px solid #ddd;padding:6px 8px;font-size:14px;text-align:left}"
    "th{background:#f5f5f5}"
    "code{font-family:ui-monospace,SFMono-Regular,Consolas,monospace}"
    "a.btn,button.btn{display:inline-block;padding:6px 10px;border-radius:8px;border:1px solid #bbb;background:#fafafa;text-decoration:none;color:#222}"
    ".row{margin:8px 0}"
    "</style></head><body>"
  );
  html += F("<h1>IR Receiver – ESP32-C3</h1>");

  html += F("<div class='muted'>IP: ");
  html += WiFi.localIP().toString();
  html += F(" &nbsp; | &nbsp; RSSI: ");
  html += String(WiFi.RSSI());
  html += F(" dBm</div>");

  // Ovládací lišta
  html += F("<div class='row'>");
  html += F("<form method='POST' action='/settings' style='display:inline'>");
  html += F("<label><input type='checkbox' name='only_unk' value='1'");
  if (g_showOnlyUnknown) html += F(" checked");
  html += F("> Zobrazovat jen <b>UNKNOWN</b></label> ");
  html += F("<button class='btn' type='submit'>Uložit</button>");
  html += F("</form> &nbsp; ");
  html += F("<a class='btn' href='/learn'>Učit kód</a> ");
  html += F("<a class='btn' href='/learned'>Naučené kódy</a> ");
  html += F("<a class='btn' href='/api/history'>API /history</a> ");
  html += F("<a class='btn' href='/api/learned'>API /learned</a>");
  html += F("</div>");

  html += F("<h2 style='font-size:16px;margin:16px 0 8px'>Posledních 10 kódů</h2>");
    html += F("<table><thead><tr>"
            "<th>#</th><th>čas [ms]</th><th>protokol</th><th>bits</th>"
            "<th>addr</th><th>cmd</th><th>value</th><th>flags</th><th>Akce</th></tr></thead><tbody>");

  size_t shown = 0;
    for (size_t i = 0; i < histCount; i++) {
    size_t idx = (histWrite + HISTORY_LEN - 1 - i) % HISTORY_LEN;
    const IREvent &e = history[idx];
    if (g_showOnlyUnknown && e.proto != UNKNOWN) continue;

    html += F("<tr><td>");
    html += String(++shown);
    html += F("</td><td>");
    html += String(e.ms);
    html += F("</td><td>");
    html += String(String() + (const __FlashStringHelper*)protoName(e.proto));
    html += F("</td><td>");
    html += String(e.bits);
    html += F("</td><td><code>0x");
    html += String(e.address, HEX);
    html += F("</code></td><td><code>0x");
    html += String(e.command, HEX);
    html += F("</code></td><td><code>0x");
    html += String(e.value, HEX);
    html += F("</code></td><td>");
    html += String(e.flags);
    html += F("</td><td>");   // === Akce ===

    if (e.proto == UNKNOWN) {
      html += F("<button class='btn' onclick=\"openLearn(");
      html += String((uint32_t)e.value);   html += F(",");
      html += String((uint32_t)e.bits);    html += F(",");
      html += String((uint32_t)e.address); html += F(",");
      html += String((uint32_t)e.flags);   html += F(",");
      html += F("'UNKNOWN'");
      html += F(")\">Učit</button>");
    } else {
      html += F("<span class='muted'>—</span>");
    }

    html += F("</td></tr>");
  }

  if (shown == 0) {
    html += F("<tr><td colspan='9' class='muted'>Žádné položky k zobrazení…</td></tr>");
  }
  html += F("</tbody></table>");

  html += F("<p class='muted' style='margin-top:12px'>Tip: S volbou „jen UNKNOWN“ snadno odfiltruješ známé protokoly a zaměříš se na učení.</p>");
  // --- Modal a JS (inline) ---
  html += F(
    "<div id='learnModal' style='position:fixed;inset:0;display:none;align-items:center;justify-content:center;background:rgba(0,0,0,.35)'>"
      "<div style='background:#fff;padding:16px 16px 12px;border-radius:10px;min-width:300px;max-width:90vw'>"
        "<h3 style='margin:0 0 8px;font-size:16px'>Učit kód</h3>"
        "<form id='learnForm'>"
          "<input type='hidden' name='value'>"
          "<input type='hidden' name='bits'>"
          "<input type='hidden' name='addr'>"
          "<input type='hidden' name='flags'>"
          "<input type='hidden' name='proto'>"
          "<label>Výrobce:</label><input type='text' name='vendor' placeholder='např. Toshiba' required>"
          "<label>Funkce:</label><input type='text' name='function' placeholder='např. Power, TempUp' required>"
          "<label>Ovladač (volitelné):</label><input type='text' name='remote_label' placeholder='např. Klima Obývák'>"
          "<div style='margin-top:10px;display:flex;gap:8px;justify-content:flex-end'>"
            "<button type='button' class='btn' id='cancelBtn'>Zrušit</button>"
            "<button type='submit' class='btn'>Uložit</button>"
          "</div>"
        "</form>"
      "</div>"
    "</div>"
    "<script>"
    "const modal=document.getElementById('learnModal');"
    "const form=document.getElementById('learnForm');"
    "const cancelBtn=document.getElementById('cancelBtn');"
    "function openLearn(value,bits,addr,flags,proto){"
      "form.value.value=value; form.bits.value=bits; form.addr.value=addr; form.flags.value=flags; form.proto.value=proto;"
      "modal.style.display='flex';"
    "}"
    "cancelBtn.onclick=()=>{modal.style.display='none'};"
    "form.onsubmit=async (e)=>{"
      "e.preventDefault();"
      "const fd=new FormData(form);"
      "const params=new URLSearchParams(fd);"
      "try{"
        "const r=await fetch('/api/learn_save',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:params});"
        "const j=await r.json();"
        "if(j.ok){ alert('Uloženo.'); modal.style.display='none'; location.reload(); }"
        "else{ alert('Uložení selhalo.'); }"
      "}catch(err){ alert('Chyba připojení.'); }"
    "}"
    "</script>"
  );
  html += F("</body></html>");
  server.send(200, "text/html; charset=utf-8", html);
}

void handleSettingsPost() {
  bool only = (server.hasArg("only_unk") && server.arg("only_unk") == "1");
  g_showOnlyUnknown = only;
  prefs.putBool("only_unk", g_showOnlyUnknown);
  server.sendHeader("Location", "/");
  server.send(302);
}

void handleJsonHistory() {
  // Vrací historii s ohledem na filtr g_showOnlyUnknown
  String out; out.reserve(2048);
  out += F("{\"ip\":\"");
  out += WiFi.localIP().toString();
  out += F("\",\"rssi\":");
  out += String(WiFi.RSSI());
  out += F(",\"only_unknown\":");
  out += g_showOnlyUnknown ? "true" : "false";
  out += F(",\"history\":[");
  bool first = true;
  for (size_t i = 0; i < histCount; i++) {
    size_t idx = (histWrite + HISTORY_LEN - 1 - i) % HISTORY_LEN;
    const IREvent &e = history[idx];
    if (g_showOnlyUnknown && e.proto != UNKNOWN) continue;
    if (!first) out += ',';
    out += F("{\"ms\":"); out += String(e.ms);
    out += F(",\"proto\":\"");
    out += String(String() + (const __FlashStringHelper*)protoName(e.proto));
    out += F("\",\"bits\":"); out += String(e.bits);
    out += F(",\"addr\":"); out += String(e.address);
    out += F(",\"cmd\":");  out += String(e.command);
    out += F(",\"value\":");out += String(e.value);
    out += F(",\"flags\":");out += String(e.flags);
    out += F("}");
    first = false;
  }
  out += F("]}");
  server.send(200, "application/json", out);
}

void handleLearnPage() {
  String html; html.reserve(4000);
  html += F(
    "<!doctype html><html lang='cs'><head><meta charset='utf-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>Učení kódu (UNKNOWN)</title>"
    "<style>"
    "body{font-family:system-ui,-apple-system,Segoe UI,Roboto,Arial,sans-serif;margin:16px}"
    "label{display:block;margin:6px 0 2px}"
    "input[type=text]{width:100%;max-width:420px;padding:6px 8px}"
    "code{font-family:ui-monospace,SFMono-Regular,Consolas,monospace}"
    "button{padding:6px 10px;border-radius:8px;border:1px solid #bbb;background:#fafafa}"
    ".muted{color:#666}"
    "</style></head><body>"
    "<h1>Učení kódu (UNKNOWN)</h1>"
  );
  if (!hasLastUnknown) {
    html += F("<p class='muted'>Zatím nebyl zachycen žádný validní kód typu <b>UNKNOWN</b>. "
              "Vrať se na <a href='/'>hlavní stránku</a> a zkuste odeslat IR z ovladače.</p></body></html>");
    server.send(200, "text/html; charset=utf-8", html);
    return;
  }

  html += F("<p>Poslední UNKNOWN zachycený kód:</p><ul>");
  html += F("<li>bits: ");   html += String(lastUnknown.bits);   html += F("</li>");
  html += F("<li>addr: <code>0x"); html += String(lastUnknown.address, HEX); html += F("</code></li>");
  html += F("<li>cmd:  <code>0x"); html += String(lastUnknown.command, HEX); html += F("</code></li>");
  html += F("<li>value:<code>0x"); html += String(lastUnknown.value, HEX);   html += F("</code></li>");
  html += F("<li>flags: ");  html += String(lastUnknown.flags);  html += F("</li></ul>");

  html += F(
    "<form method='POST' action='/learn_save'>"
    "<label>Výrobce zařízení (vendor):</label>"
    "<input type='text' name='vendor' placeholder='např. Toshiba' required>"
    "<label>Označení protokolu (label):</label>"
    "<input type='text' name='proto_label' placeholder='např. Toshiba-IR-RAW' required>"
    "<label>Označení ovladače / zařízení:</label>"
    "<input type='text' name='remote_label' placeholder='např. Klima Obývák' required>"
    "<div style='margin-top:10px'><button type='submit'>Uložit do naučených</button></div>"
    "</form>"
    "<p class='muted' style='margin-top:10px'>Pozn.: ukládá se aktuálně poslední zachycený UNKNOWN kód.</p>"
    "<p><a href='/'>← Zpět</a> &nbsp; <a href='/learned'>Naučené kódy</a></p>"
    "</body></html>"
  );
  server.send(200, "text/html; charset=utf-8", html);
}

void handleLearnSave() {
  if (!hasLastUnknown) { server.sendHeader("Location", "/learn"); server.send(302); return; }

  String vendor      = server.hasArg("vendor") ? server.arg("vendor") : "";
  String protoLabel  = server.hasArg("proto_label") ? server.arg("proto_label") : "";
  String remoteLabel = server.hasArg("remote_label") ? server.arg("remote_label") : "";

  vendor.trim(); protoLabel.trim(); remoteLabel.trim();

  bool ok = (vendor.length() && protoLabel.length() && remoteLabel.length()) &&
          fsAppendLearned(lastUnknown.value,
                          lastUnknown.bits,
                          lastUnknown.address,
                          lastUnknown.flags,
                          String("UNKNOWN"),   // proto pro stránku Učení
                          vendor,
                          protoLabel,          // použijeme jako "function"
                          remoteLabel);

  String html; html.reserve(1200);
  html += F("<!doctype html><html><meta charset='utf-8'><title>Uloženo</title><body>");
  if (ok) {
    html += F("<p>✅ Kód uložen.</p>");
  } else {
    html += F("<p>❌ Uložení selhalo.</p>");
  }
  html += F("<p><a href='/learn'>← Zpět na učení</a> &nbsp; <a href='/learned'>Naučené kódy</a> &nbsp; <a href='/'>Domů</a></p>");
  html += F("</body></html>");
  server.send(200, "text/html; charset=utf-8", html);
}

void handleLearnedList() {
  // jednoduchý výpis z JSONL jako tabulka
  String data = fsReadLearnedAsArrayJSON();
  // Sestav HTML s minimálním parserem na straně klienta (JS) – jednodušší
  String html; html.reserve(4000);
  html += F(
    "<!doctype html><html lang='cs'><head><meta charset='utf-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>Naučené kódy</title>"
    "<style>"
    "body{font-family:system-ui,-apple-system,Segoe UI,Roboto,Arial,sans-serif;margin:16px}"
    "table{border-collapse:collapse;width:100%;max-width:980px}"
    "th,td{border:1px solid #ddd;padding:6px 8px;font-size:14px;text-align:left}"
    "th{background:#f5f5f5}"
    "code{font-family:ui-monospace,SFMono-Regular,Consolas,monospace}"
    "</style></head><body>"
    "<h1>Naučené kódy</h1>"
    "<table><thead><tr>"
    "<th>#</th><th>vendor</th><th>proto</th><th>function</th><th>remote_label</th>"
    "<th>bits</th><th>addr</th><th>value</th><th>flags</th>"
    "</tr></thead><tbody id='tb'></tbody></table>"
    "<p><a href='/'>← Domů</a></p>"
    "<script>const data="
  );
  html += data;
  html += F(";"
    "const tb=document.getElementById('tb');"
    "data.forEach((o,i)=>{"
      "const tr=document.createElement('tr');"
      "function td(t){const e=document.createElement('td');e.innerHTML=t;tr.appendChild(e);} "
      "td(i+1); td(o.vendor||''); td(o.proto||'UNKNOWN'); td(o.function||''); td(o.remote_label||''); "
      "td(o.bits||0); td('0x'+(o.addr>>>0).toString(16).toUpperCase()); "
      "td('0x'+(o.value>>>0).toString(16).toUpperCase()); td(o.flags||0); "
      "tb.appendChild(tr);"
    "});"
    "</script></body></html>"
  );
  server.send(200, "text/html; charset=utf-8", html);
}

void handleApiLearned() {
  server.send(200, "application/json", fsReadLearnedAsArrayJSON());
}

// Přímé uložení z hlavní stránky (POST application/x-www-form-urlencoded)
// očekává parametry: value,bits,addr,flags,proto,vendor,function,remote_label
void handleApiLearnSave() {
  auto need = [&](const char* k){ return server.hasArg(k) && server.arg(k).length() > 0; };

  if (!(need("value") && need("bits") && need("addr") && need("flags") && need("proto") && need("vendor") && need("function"))) {
    server.send(400, "application/json", "{\"ok\":false,\"err\":\"missing params\"}");
    return;
  }

  uint32_t value = (uint32_t) strtoul(server.arg("value").c_str(),  nullptr, 10);
  uint8_t  bits  = (uint8_t)  strtoul(server.arg("bits").c_str(),   nullptr, 10);
  uint32_t addr  = (uint32_t) strtoul(server.arg("addr").c_str(),   nullptr, 10);
  uint32_t flags = (uint32_t) strtoul(server.arg("flags").c_str(),  nullptr, 10);

  String proto   = server.arg("proto");        proto.trim();
  String vendor  = server.arg("vendor");       vendor.trim();
  String func    = server.arg("function");     func.trim();
  String remote  = server.hasArg("remote_label") ? server.arg("remote_label") : "";
  remote.trim();

  bool ok = fsAppendLearned(value, bits, addr, flags, proto, vendor, func, remote);
  server.send(200, "application/json", ok ? "{\"ok\":true}" : "{\"ok\":false}");
}

void startWebServer() {
  server.on("/", handleRoot);
  server.on("/settings", HTTP_POST, handleSettingsPost);

  server.on("/learn", handleLearnPage);
  server.on("/learn_save", HTTP_POST, handleLearnSave);
  server.on("/learned", handleLearnedList);

  server.on("/api/history", handleJsonHistory);
  server.on("/api/learned", handleApiLearned);
  server.on("/api/learn_save", HTTP_POST, handleApiLearnSave); // NOVÉ

  server.begin();
  Serial.println(F("[NET] WebServer běží na portu 80"));
}

// ====== SETUP / LOOP ======
void setup() {
  Serial.begin(115200);
  delay(200);

  Serial.println();
  Serial.println(F("=== IR Receiver (IRremote) – ESP32-C3 ==="));
  Serial.print(F("IR pin: ")); Serial.println(IR_RX_PIN);

  // FS (pro naučené kódy)
  if (!LittleFS.begin(true)) {
    Serial.println(F("[FS] LittleFS mount selhal (format=true), pokračuji bez uložení learned."));
  }

  // NVS (pro nastavení)
  prefs.begin("irrecv", false);
  g_showOnlyUnknown = prefs.getBool("only_unk", false);

  // Wi-Fi přes WiFiManager (otevřený AP bez hesla při prvním nastavení)
  wifiSetupWithWiFiManager();

  startWebServer();

  // IR přijímač
  IrReceiver.begin(IR_RX_PIN, DISABLE_LED_FEEDBACK);

  Serial.print(F("Protokoly povoleny: "));
  IrReceiver.printActiveIRProtocols(&Serial);
  Serial.println();
}

void loop() {
  if (!IrReceiver.decode()) {
    server.handleClient();
    delay(1);
    return;
  }

  IRData &d = IrReceiver.decodedIRData;

  // Šum pryč
  if (isNoise(d)) {
    IrReceiver.resume();
    server.handleClient();
    delay(1);
    return;
  }

  // Deduplikace (repeat)
  const uint32_t now = millis();
  bool suppress = false;
  if (d.flags & IRDATA_FLAGS_IS_REPEAT) {
    if (now - lastMs < DUP_FILTER_MS &&
        d.protocol == lastProto &&
        d.numberOfBits == lastBits &&
        d.decodedRawData == lastValue) {
      suppress = true;
    }
  }

  // Pokud chceme učit UNKNOWN, ulož poslední zachycený UNKNOWN (ne-suppressnutý)
  if (d.protocol == UNKNOWN && !suppress) {
    hasLastUnknown = true;
    lastUnknown.ms      = now;
    lastUnknown.proto   = UNKNOWN;
    lastUnknown.bits    = d.numberOfBits;
    lastUnknown.address = d.address;
    lastUnknown.command = d.command;
    lastUnknown.value   = d.decodedRawData;
    lastUnknown.flags   = d.flags;
  }

  // Log + historie (respektuj filtr v print funkcích; do historie ukládáme vždy “nesuppressnuté”)
  printLine(d, suppress);
  if (!suppress) {
    printJSON(d);
    addToHistory(d);
  }

  // update dup stav
  lastMs   = now;
  lastProto= d.protocol;
  lastBits = d.numberOfBits;
  lastValue= d.decodedRawData;

  IrReceiver.resume();
  server.handleClient();
  delay(1);
}
