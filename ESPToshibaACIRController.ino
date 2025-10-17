#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <WiFiManager.h>     // tzapu/WiFiManager
#include <IRremote.hpp>      // Armin Joachimsmeyer (IRremote.hpp)
#include <Preferences.h>     // NVS (nastavení)
#include <LittleFS.h>        // souborový systém pro "learned" databázi
#include <FS.h>
#include <vector>

struct LearnedCode;
struct IREvent;

String jsonEscape(const String& s);

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
// ====== LittleFS – “databáze” learned kódů (JSON Lines) ======
static const char* LEARN_FILE = "/learned.jsonl";
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
  int16_t learnedIndex; // index do cache naučených kódů, -1 pokud neexistuje
};
static const size_t HISTORY_LEN = 10;
static IREvent history[HISTORY_LEN];
static size_t histWrite = 0;
static size_t histCount = 0;

// Poslední validní UNKNOWN pro “Learn”
static bool   hasLastUnknown = false;
static IREvent lastUnknown = {0, UNKNOWN, 0, 0, 0, 0, 0, -1};

// Stav pro dup filtr
static uint32_t lastValue = 0;
static decode_type_t lastProto = UNKNOWN;
static uint8_t lastBits = 0;
static uint32_t lastMs = 0;

// ====== Learned cache ======
struct LearnedCode {
  uint32_t value;
  uint8_t bits;
  uint32_t addr;
  String proto;
  String vendor;
  String function;
  String remote;
};

static std::vector<LearnedCode> g_learnedCache;
static bool g_learnedCacheValid = false;

void invalidateLearnedCache() {
  g_learnedCacheValid = false;
}

struct LearnedLineDetails {
  uint32_t ts;
  uint32_t value;
  uint8_t bits;
  uint32_t addr;
  uint32_t flags;
};

static bool parseLearnedLineNumbers(const String &line, LearnedLineDetails &out) {
  uint32_t tmp = 0;
  if (!jsonExtractUint32(line, "ts", out.ts)) return false;
  if (!jsonExtractUint32(line, "value", out.value)) return false;
  if (!jsonExtractUint32(line, "bits", tmp)) return false;
  out.bits = static_cast<uint8_t>(tmp & 0xFF);
  if (!jsonExtractUint32(line, "addr", out.addr)) return false;
  if (!jsonExtractUint32(line, "flags", out.flags)) out.flags = 0;
  return true;
}

static bool isWhitespace(char c) {
  return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

static bool jsonExtractUint32(const String &line, const char *key, uint32_t &out) {
  String pattern = String('"') + key + String("\":");
  int idx = line.indexOf(pattern);
  if (idx < 0) return false;
  idx += pattern.length();
  while (idx < (int)line.length() && isWhitespace(line[idx])) idx++;
  if (idx >= (int)line.length()) return false;
  bool hasDigit = false;
  uint32_t value = 0;
  while (idx < (int)line.length()) {
    char c = line[idx];
    if (c >= '0' && c <= '9') {
      hasDigit = true;
      value = value * 10 + (c - '0');
      idx++;
    } else {
      break;
    }
  }
  if (!hasDigit) return false;
  out = value;
  return true;
}

static bool jsonExtractString(const String &line, const char *key, String &out) {
  String pattern = String('"') + key + String("\":\"");
  int idx = line.indexOf(pattern);
  if (idx < 0) return false;
  idx += pattern.length();
  String result;
  while (idx < (int)line.length()) {
    char c = line[idx++];
    if (c == '\\') {
      if (idx >= (int)line.length()) break;
      char esc = line[idx++];
      switch (esc) {
        case '"': result += '"'; break;
        case '\\': result += '\\'; break;
        case '/': result += '/'; break;
        case 'b': result += '\b'; break;
        case 'f': result += '\f'; break;
        case 'n': result += '\n'; break;
        case 'r': result += '\r'; break;
        case 't': result += '\t'; break;
        default:  result += esc; break;
      }
    } else if (c == '"') {
      out = result;
      return true;
    } else {
      result += c;
    }
  }
  return false;
}

void ensureLearnedCacheLoaded() {
  if (g_learnedCacheValid) return;

  g_learnedCache.clear();

  File f = LittleFS.open(LEARN_FILE, FILE_READ);
  if (!f) {
    g_learnedCacheValid = true;
    return;
  }

  while (f.available()) {
    String line = f.readStringUntil('\n');
    line.trim();
    if (!line.length()) continue;

    LearnedCode entry = {};
    uint32_t tmp = 0;
    if (jsonExtractUint32(line, "value", entry.value)) {
      if (jsonExtractUint32(line, "bits", tmp)) entry.bits = static_cast<uint8_t>(tmp & 0xFF);
      if (jsonExtractUint32(line, "addr", entry.addr)) {
        jsonExtractString(line, "proto", entry.proto);
        jsonExtractString(line, "vendor", entry.vendor);
        jsonExtractString(line, "function", entry.function);
        jsonExtractString(line, "remote_label", entry.remote);
        g_learnedCache.push_back(entry);
      }
    }
  }
  f.close();
  g_learnedCacheValid = true;
}

const LearnedCode* getLearnedByIndex(int16_t idx) {
  ensureLearnedCacheLoaded();
  if (idx < 0) return nullptr;
  if (idx >= static_cast<int16_t>(g_learnedCache.size())) return nullptr;
  return &g_learnedCache[idx];
}

int16_t findLearnedIndex(uint32_t value, uint8_t bits, uint32_t addr) {
  ensureLearnedCacheLoaded();
  for (size_t i = 0; i < g_learnedCache.size(); i++) {
    const LearnedCode &entry = g_learnedCache[i];
    if (entry.value == value && entry.bits == bits && entry.addr == addr) {
      return static_cast<int16_t>(i);
    }
  }
  return -1;
}

const LearnedCode* findLearnedMatch(const IRData &d, int16_t *outIndex) {
  int16_t idx = findLearnedIndex(d.decodedRawData, d.numberOfBits, d.address);
  if (outIndex) *outIndex = idx;
  return idx >= 0 ? &g_learnedCache[idx] : nullptr;
}

void refreshLearnedAssociations() {
  ensureLearnedCacheLoaded();
  for (size_t i = 0; i < histCount; i++) {
    size_t idx = (histWrite + HISTORY_LEN - 1 - i) % HISTORY_LEN;
    IREvent &e = history[idx];
    e.learnedIndex = findLearnedIndex(e.value, e.bits, e.address);
  }
  if (hasLastUnknown) {
    int16_t idx = findLearnedIndex(lastUnknown.value, lastUnknown.bits, lastUnknown.address);
    if (idx >= 0) {
      hasLastUnknown = false;
      lastUnknown.learnedIndex = idx;
    } else {
      lastUnknown.learnedIndex = -1;
    }
  }
}

static bool isEffectivelyUnknown(decode_type_t proto, const LearnedCode *learned) {
  return proto == UNKNOWN && !(learned && learned->proto.length());
}

static bool isEffectivelyUnknown(const IREvent &e) {
  const LearnedCode *learned = getLearnedByIndex(e.learnedIndex);
  return isEffectivelyUnknown(e.proto, learned);
}

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

void addToHistory(const IRData &d, int16_t learnedIndex) {
  IREvent e;
  e.ms      = millis();
  e.proto   = d.protocol;
  e.bits    = d.numberOfBits;
  e.address = d.address;
  e.command = d.command;
  e.value   = d.decodedRawData;
  e.flags   = d.flags;
  e.learnedIndex = learnedIndex;

  history[histWrite] = e;
  histWrite = (histWrite + 1) % HISTORY_LEN;
  if (histCount < HISTORY_LEN) histCount++;
}

void printLine(const IRData &d, bool isRepeatSuppressed, const LearnedCode *learned) {
  if (g_showOnlyUnknown && !isEffectivelyUnknown(d.protocol, learned)) return; // nezahlcuj log, když filtr aktivní
  Serial.print(F("[IR] "));
  if (learned && learned->proto.length()) Serial.print(learned->proto);
  else Serial.print(protoName(d.protocol));
  Serial.print(F(" bits=")); Serial.print(d.numberOfBits);
  Serial.print(F(" addr=0x")); Serial.print(d.address, HEX);
  Serial.print(F(" cmd=0x")); Serial.print(d.command, HEX);
  Serial.print(F(" value=0x")); Serial.print(d.decodedRawData, HEX);
  if (d.flags & IRDATA_FLAGS_IS_REPEAT) Serial.print(F(" (REPEAT)"));
  if (isRepeatSuppressed) Serial.print(F(" (suppressed)"));
  if (learned) {
    Serial.print(F(" [learned"));
    if (learned->function.length()) {
      Serial.print(F(" function='"));
      Serial.print(learned->function);
      Serial.print(F("'"));
    }
    if (learned->vendor.length()) {
      Serial.print(F(" vendor='"));
      Serial.print(learned->vendor);
      Serial.print(F("'"));
    }
    Serial.print(F("]"));
  }
  Serial.println();
}

void printJSON(const IRData &d, const LearnedCode *learned) {
  if (g_showOnlyUnknown && !isEffectivelyUnknown(d.protocol, learned)) return; // respektuj filtr i pro JSON log
  Serial.print(F("{\"proto\":\""));
  if (learned && learned->proto.length()) Serial.print(jsonEscape(learned->proto));
  else Serial.print(protoName(d.protocol));
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
  Serial.print(F(",\"learned\":"));
  Serial.print(learned ? "true" : "false");
  Serial.print(F(",\"learned_proto\":\""));
  if (learned && learned->proto.length()) Serial.print(jsonEscape(learned->proto));
  Serial.print(F("\",\"learned_vendor\":\""));
  if (learned && learned->vendor.length()) Serial.print(jsonEscape(learned->vendor));
  Serial.print(F("\",\"learned_function\":\""));
  if (learned && learned->function.length()) Serial.print(jsonEscape(learned->function));
  Serial.print(F("\",\"learned_remote\":\""));
  if (learned && learned->remote.length()) Serial.print(jsonEscape(learned->remote));
  Serial.print('"');
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
  line += F("\"ts\":");         line += static_cast<uint32_t>(millis());
  line += F(",\"proto\":\""); line += protoStr;                   line += '"';
  line += F(",\"value\":");     line += value;
  line += F(",\"bits\":");      line += static_cast<uint32_t>(bits);
  line += F(",\"addr\":");      line += addr;
  line += F(",\"flags\":");     line += flags;
  line += F(",\"vendor\":\""); line += jsonEscape(vendor);        line += '"';
  line += F(",\"function\":\""); line += jsonEscape(functionName);  line += '"';
  line += F(",\"remote_label\":\"");
  line += jsonEscape(remoteLabel);
  line += F("\"}\n");

  size_t w = f.print(line);
  f.close();
  bool ok = (w == line.length());
  if (ok) {
    invalidateLearnedCache();
    refreshLearnedAssociations();
  }
  return ok;
}

bool fsUpdateLearned(size_t index,
                     const String &protoStr,
                     const String &vendor,
                     const String &functionName,
                     const String &remoteLabel) {
  File f = LittleFS.open(LEARN_FILE, FILE_READ);
  if (!f) return false;

  std::vector<String> lines;
  while (f.available()) {
    String line = f.readStringUntil('\n');
    line.trim();
    if (!line.length()) continue;
    lines.push_back(line);
  }
  f.close();

  if (index >= lines.size()) return false;

  LearnedLineDetails details;
  if (!parseLearnedLineNumbers(lines[index], details)) return false;

  String proto = protoStr.length() ? protoStr : String("UNKNOWN");

  String line;
  line.reserve(320);
  line += '{';
  line += F("\"ts\":");         line += details.ts;
  line += F(",\"proto\":\""); line += proto;                    line += '"';
  line += F(",\"value\":");     line += details.value;
  line += F(",\"bits\":");      line += static_cast<uint32_t>(details.bits);
  line += F(",\"addr\":");      line += details.addr;
  line += F(",\"flags\":");     line += details.flags;
  line += F(",\"vendor\":\""); line += jsonEscape(vendor);        line += '"';
  line += F(",\"function\":\""); line += jsonEscape(functionName); line += '"';
  line += F(",\"remote_label\":\"");
  line += jsonEscape(remoteLabel);
  line += F("\"}");

  lines[index] = line;

  File out = LittleFS.open(LEARN_FILE, FILE_WRITE);
  if (!out) return false;

  bool ok = true;
  for (size_t i = 0; i < lines.size(); i++) {
    size_t w = out.print(lines[i]);
    if (w != lines[i].length()) { ok = false; break; }
    if (out.print('\n') != 1) { ok = false; break; }
  }
  out.close();

  if (ok) {
    invalidateLearnedCache();
    refreshLearnedAssociations();
  }
  return ok;
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
  html += WiFi.RSSI();
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
    const LearnedCode *learned = getLearnedByIndex(e.learnedIndex);
    if (g_showOnlyUnknown && !isEffectivelyUnknown(e)) continue;

    html += F("<tr><td>");
    html += ++shown;
    html += F("</td><td>");
    html += e.ms;
    html += F("</td><td>");
    if (learned && learned->proto.length()) html += learned->proto;
    else html += String(protoName(e.proto));
    html += F("</td><td>");
    html += static_cast<uint32_t>(e.bits);
    html += F("</td><td><code>0x");
    html += String(e.address, HEX);
    html += F("</code></td><td><code>0x");
    html += String(e.command, HEX);
    html += F("</code></td><td><code>0x");
    html += String(e.value, HEX);
    html += F("</code></td><td>");
    html += e.flags;
    html += F("</td><td>");   // === Akce ===

    if (isEffectivelyUnknown(e)) {
      html += F("<button class='btn' onclick=\"openLearn(");
      html += static_cast<uint32_t>(e.value);   html += F(",");
      html += static_cast<uint32_t>(e.bits);    html += F(",");
      html += static_cast<uint32_t>(e.address); html += F(",");
      html += static_cast<uint32_t>(e.flags);   html += F(",");
      html += F("'UNKNOWN'");
      html += F(")\">Učit</button>");
    } else if (learned) {
      html += F("<span class='muted'>");
      if (learned->function.length()) html += learned->function;
      else html += F("Naučený kód");
      if (learned->vendor.length()) {
        html += F(" (");
        html += learned->vendor;
        html += F(")");
      }
      html += F("</span>");
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
  out += WiFi.RSSI();
  out += F(",\"only_unknown\":");
  out += g_showOnlyUnknown ? "true" : "false";
  out += F(",\"history\":[");
  bool first = true;
  for (size_t i = 0; i < histCount; i++) {
    size_t idx = (histWrite + HISTORY_LEN - 1 - i) % HISTORY_LEN;
    const IREvent &e = history[idx];
    const LearnedCode *learned = getLearnedByIndex(e.learnedIndex);
    if (g_showOnlyUnknown && !isEffectivelyUnknown(e)) continue;
    if (!first) out += ',';
    out += F("{\"ms\":"); out += e.ms;
    out += F(",\"proto\":\"");
    String protoStr = learned && learned->proto.length() ? learned->proto : String(protoName(e.proto));
    out += jsonEscape(protoStr);
    out += F("\",\"bits\":"); out += static_cast<uint32_t>(e.bits);
    out += F(",\"addr\":"); out += e.address;
    out += F(",\"cmd\":");  out += e.command;
    out += F(",\"value\":");out += e.value;
    out += F(",\"flags\":");out += e.flags;
    out += F(",\"learned\":"); out += learned ? "true" : "false";
    out += F(",\"learned_proto\":\"");
    if (learned && learned->proto.length()) out += jsonEscape(learned->proto);
    out += F("\",\"learned_vendor\":\"");
    if (learned && learned->vendor.length()) out += jsonEscape(learned->vendor);
    out += F("\",\"learned_function\":\"");
    if (learned && learned->function.length()) out += jsonEscape(learned->function);
      out += F(",\"learned_remote\":\"");
    if (learned && learned->remote.length()) out += jsonEscape(learned->remote);
    out += '"';
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
  html += F("<li>bits: ");   html += static_cast<uint32_t>(lastUnknown.bits); html += F("</li>");
  html += F("<li>addr: <code>0x"); html += String(lastUnknown.address, HEX); html += F("</code></li>");
  html += F("<li>cmd:  <code>0x"); html += String(lastUnknown.command, HEX); html += F("</code></li>");
  html += F("<li>value:<code>0x"); html += String(lastUnknown.value, HEX);   html += F("</code></li>");
  html += F("<li>flags: ");  html += lastUnknown.flags;                  html += F("</li></ul>");

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
  String html; html.reserve(7000);
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
    "a.btn,button.btn{display:inline-block;padding:6px 10px;border-radius:8px;border:1px solid #bbb;background:#fafafa;text-decoration:none;color:#222}"
    "#editModal{position:fixed;inset:0;display:none;align-items:center;justify-content:center;background:rgba(0,0,0,.35)}"
    "#editModal .card{background:#fff;padding:16px 16px 12px;border-radius:10px;min-width:320px;max-width:90vw}"
    "#editModal label{display:block;margin:6px 0 2px;font-size:14px}"
    "#editModal input[type=text]{width:100%;padding:6px 8px;font-size:14px}"
    "</style></head><body>"
    "<h1>Naučené kódy</h1>"
    "<table><thead><tr>"
    "<th>#</th><th>vendor</th><th>proto</th><th>function</th><th>remote_label</th>"
    "<th>bits</th><th>addr</th><th>value</th><th>flags</th><th>Akce</th>"
    "</tr></thead><tbody id='tb'></tbody></table>"
    "<p><a href='/'>← Domů</a></p>"
    "<div id='editModal'><div class='card'>"
      "<h3 style='margin:0 0 8px;font-size:16px'>Upravit kód</h3>"
      "<form id='editForm'>"
        "<input type='hidden' name='index'>"
        "<label>Protokol:</label><input type='text' name='proto' placeholder='např. NEC' required>"
        "<label>Výrobce:</label><input type='text' name='vendor' placeholder='např. Toshiba' required>"
        "<label>Funkce:</label><input type='text' name='function' placeholder='např. Power, TempUp' required>"
        "<label>Ovladač (volitelné):</label><input type='text' name='remote_label' placeholder='např. Klima Obývák'>"
        "<div style='margin-top:10px;display:flex;gap:8px;justify-content:flex-end'>"
          "<button type='button' class='btn' id='editCancel'>Zrušit</button>"
          "<button type='submit' class='btn'>Uložit</button>"
        "</div>"
      "</form>"
    "</div></div>"
    "<script>const data="
  );
  html += data;
  html += F(";"
    "const tb=document.getElementById('tb');"
    "const modal=document.getElementById('editModal');"
    "const form=document.getElementById('editForm');"
    "const cancelBtn=document.getElementById('editCancel');"
    "const idxInput=form.querySelector('input[name=index]');"
    "function toHex(num){return '0x'+((num>>>0).toString(16).toUpperCase());}"
    "function openEdit(idx,obj){"
      "idxInput.value=idx;"
      "form.proto.value=obj.proto||'UNKNOWN';"
      "form.vendor.value=obj.vendor||'';"
      "form.function.value=obj.function||'';"
      "form.remote_label.value=obj.remote_label||'';"
      "modal.style.display='flex';"
    "}"
    "cancelBtn.onclick=()=>{modal.style.display='none';};"
    "modal.addEventListener('click',e=>{if(e.target===modal){modal.style.display='none';}});"
    "form.onsubmit=async(e)=>{"
      "e.preventDefault();"
      "const fd=new FormData(form);"
      "const params=new URLSearchParams(fd);"
      "try{"
        "const r=await fetch('/api/learn_update',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:params});"
        "const j=await r.json();"
        "if(j.ok){alert('Uloženo.');modal.style.display='none';location.reload();}"
        "else{alert('Uložení selhalo.');}"
      "}catch(err){alert('Chyba připojení.');}"
    "};"
    "data.forEach((o,i)=>{"
      "const tr=document.createElement('tr');"
      "function addCell(text){const td=document.createElement('td');td.textContent=text;tr.appendChild(td);}"
      "addCell(i+1);"
      "addCell(o.vendor||'');"
      "addCell(o.proto||'UNKNOWN');"
      "addCell(o.function||'');"
      "addCell(o.remote_label||'');"
      "addCell(o.bits||0);"
      "addCell(toHex(o.addr||0));"
      "addCell(toHex(o.value||0));"
      "addCell(o.flags||0);"
      "const actionTd=document.createElement('td');"
      "const btn=document.createElement('button');"
      "btn.type='button';"
      "btn.textContent='Upravit';"
      "btn.className='btn';"
      "btn.onclick=()=>openEdit(i,o);"
      "actionTd.appendChild(btn);"
      "tr.appendChild(actionTd);"
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

void handleApiLearnUpdate() {
  auto need = [&](const char* k){ return server.hasArg(k) && server.arg(k).length() > 0; };

  if (!need("index") || !need("proto") || !need("vendor") || !need("function")) {
    server.send(400, "application/json", "{\"ok\":false,\"err\":\"missing params\"}");
    return;
  }

  size_t index = static_cast<size_t>(strtoul(server.arg("index").c_str(), nullptr, 10));
  String proto  = server.arg("proto");        proto.trim();
  String vendor = server.arg("vendor");       vendor.trim();
  String func   = server.arg("function");     func.trim();
  String remote = server.hasArg("remote_label") ? server.arg("remote_label") : "";
  remote.trim();

  bool ok = fsUpdateLearned(index, proto, vendor, func, remote);
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
  server.on("/api/learn_update", HTTP_POST, handleApiLearnUpdate);

  server.begin();
  Serial.println(F("[NET] WebServer běží na portu 80"));
}

inline void serviceClient() {
  server.handleClient();
  delay(1);
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
    serviceClient();
    return;
  }

  IRData &d = IrReceiver.decodedIRData;

  // Šum pryč
  if (isNoise(d)) {
    IrReceiver.resume();
    serviceClient();
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

  int16_t learnedIndex = -1;
  const LearnedCode *learned = findLearnedMatch(d, &learnedIndex);
  const bool effectiveUnknown = isEffectivelyUnknown(d.protocol, learned);

  // Pokud chceme učit UNKNOWN, ulož poslední zachycený UNKNOWN (ne-suppressnutý)
  if (effectiveUnknown && !suppress) {
    hasLastUnknown = true;
    lastUnknown.ms      = now;
    lastUnknown.proto   = UNKNOWN;
    lastUnknown.bits    = d.numberOfBits;
    lastUnknown.address = d.address;
    lastUnknown.command = d.command;
    lastUnknown.value   = d.decodedRawData;
    lastUnknown.flags   = d.flags;
    lastUnknown.learnedIndex = -1;
  }

  // Log + historie (respektuj filtr v print funkcích; do historie ukládáme vždy “nesuppressnuté”)
  printLine(d, suppress, learned);
  if (!suppress) {
    printJSON(d, learned);
    addToHistory(d, learnedIndex);
  }

  // update dup stav
  lastMs   = now;
  lastProto= d.protocol;
  lastBits = d.numberOfBits;
  lastValue= d.decodedRawData;

  IrReceiver.resume();
  serviceClient();
}
