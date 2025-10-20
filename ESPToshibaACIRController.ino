#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <WiFiManager.h>
#include <IRremote.hpp>
#include <Preferences.h>
#include <LittleFS.h>
#include <FS.h>
#include <vector>
#include <unordered_map>

// === Přesuň DEFINICE typů nahoru (před první funkci) ===

// Z klíče pro indexaci naučených kódů
struct LearnedKey {
  uint32_t value;
  uint32_t addr;
  uint8_t  bits;
  bool operator==(const LearnedKey &o) const {
    return value == o.value && addr == o.addr && bits == o.bits;
  }
};
struct LearnedKeyHash {
  size_t operator()(const LearnedKey &k) const {
    size_t h = static_cast<size_t>(k.value);
    h ^= static_cast<size_t>(k.addr) + 0x9e3779b9 + (h << 6) + (h >> 2);
    h ^= static_cast<size_t>(k.bits) * 0x27d4eb2d;
    return h;
  }
};

// IR historie (potřebuje decode_type_t z IRremote.hpp)
struct IREvent {
  uint32_t ms;
  decode_type_t proto;
  uint8_t  bits;
  uint32_t address;
  uint16_t command;
  uint32_t value;
  uint32_t flags;
  int16_t  learnedIndex; // index do cache naučených kódů, -1 pokud neexistuje
};

// Záznam naučeného kódu (metadata + hodnoty)
struct LearnedCode {
  uint32_t value;
  uint8_t  bits;
  uint32_t addr;
  String   proto;
  String   vendor;
  String   function;
  String   remote;
};

// Číselná část řádku v /learned.jsonl (pro přepis)
struct LearnedLineDetails {
  uint32_t ts;
  uint32_t value;
  uint8_t  bits;
  uint32_t addr;
  uint32_t flags;
};

static inline LearnedKey makeLearnedKey(uint32_t value, uint8_t bits, uint32_t addr) {
  return LearnedKey{ value, addr, bits };
}

// FIX: server musí být deklarován dřív, než ho použijí handlery
WebServer server(80);

// Prototypy util funkcí, které jsou volané dřív než definované
static bool jsonExtractUint32(const String &line, const char *key, uint32_t &out);
static bool jsonExtractString(const String &line, const char *key, String &out);

// ==== HW ====
static const int8_t IR_TX_PIN_DEFAULT = 0;   // ESP32-C3: výchozí TX pin
static int8_t g_irTxPin = IR_TX_PIN_DEFAULT;
static const uint8_t IR_RX_PIN = 10;         // ESP32-C3: 4/5/10 fungují

// ====== IR deduplikace ======
static const uint32_t DUP_FILTER_MS = 120;

// ====== Forward deklarace util ======
String jsonEscape(const String& s);

// ====== Noise filtr (bez rawlen – kompatibilní s tvojí verzí) ======
static bool isNoise(const IRData &d) {
  if (d.flags & IRDATA_FLAGS_WAS_OVERFLOW) return true;
  if (d.protocol == UNKNOWN) {
    if (d.numberOfBits == 0) return true;
    if (d.decodedRawData == 0) return true;
  }
  if (d.decodedRawData > 0 && d.decodedRawData < 0x100) return true; // klidně přitvrď na 0x200
  if (d.numberOfBits > 0 && d.numberOfBits < 8) return true;
  return false;
}

// ====== NVS a FS ======
Preferences prefs;                 // namespace: "irrecv"
static const char* LEARN_FILE = "/learned.jsonl";
static bool g_showOnlyUnknown = false;

// ====== Historie IR (ring buffer) ======
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
static std::vector<LearnedCode> g_learnedCache;
static std::unordered_map<LearnedKey, int16_t, LearnedKeyHash> g_learnedIndex;
static bool g_learnedCacheValid = false;

void invalidateLearnedCache() {
  g_learnedCacheValid = false;
  g_learnedIndex.clear();
}
void ensureLearnedCacheLoaded();
const LearnedCode* getLearnedByIndex(int16_t idx);
int16_t findLearnedIndex(uint32_t value, uint8_t bits, uint32_t addr);
const LearnedCode* findLearnedMatch(const IRData &d, int16_t *outIndex);
void refreshLearnedAssociations();

// ====== IR Sender init ======
static void initIrSender(int8_t pin) {
  if (pin < 0) return;
  IrSender.begin(pin, DISABLE_LED_FEEDBACK, true);
  Serial.print(F("[IR-TX] Inicializován na pinu ")); Serial.println(pin);
}

// === Mapování textového názvu protokolu na decode_type_t ===
static decode_type_t parseProtoLabel(const String &s) {
  String u = s; u.trim(); u.toUpperCase();
  if (u == "NEC" || u == "NEC1" || u == "NEC2") return NEC;
  if (u == "SONY") return SONY;
  if (u == "RC5") return RC5;
  if (u == "RC6") return RC6;
  if (u == "SAMSUNG" || u == "SAMSUNG32") return SAMSUNG;
  if (u == "JVC") return JVC;
  if (u == "LG") return LG;
  if (u == "PANASONIC") return PANASONIC;
  if (u == "KASEIKYO" || u == "PANASONIC_KASEIKYO") return KASEIKYO;
  if (u == "APPLE") return APPLE;
  if (u == "ONKYO") return ONKYO;
  if (u == "TOSHIBA-AC" || u == "TOSHIBA") return NEC;
  // explicitně UNKNOWN/"" -> UNKNOWN
  return UNKNOWN;
}

static bool findProtoInHistory(uint32_t value, uint8_t bits, uint32_t addr, decode_type_t &outProto) {
  for (size_t i = 0; i < histCount; i++) {
    size_t idx = (histWrite + HISTORY_LEN - 1 - i) % HISTORY_LEN;
    const IREvent &e = history[idx];
    if (e.value == value && e.bits == bits && e.address == addr && e.proto != UNKNOWN) {
      outProto = e.proto;
      return true;
    }
  }
  return false;
}

// ========== Odeslání naučeného kódu ==========
static bool irSendLearned(const LearnedCode &e, uint8_t repeats) {
  decode_type_t p = parseProtoLabel(e.proto);

  // Fallback: learned je UNKNOWN → zkusíme dohledat protokol v historii
  if (p == UNKNOWN) {
    decode_type_t histP = UNKNOWN;
    if (findProtoInHistory(e.value, e.bits, e.addr, histP)) {
      p = histP;
      Serial.print(F("[IR-TX] Proto bylo UNKNOWN, beru z historie: "));
      Serial.println((int)p);
    }
  }

  // Diagnostika – uvidíš přesně co se snažíme posílat
  Serial.print(F("[IR-TX] send proto="));
  Serial.print((int)p);
  Serial.print(F(" label='"));
  Serial.print(e.proto);
  Serial.print(F("' bits="));
  Serial.print(e.bits);
  Serial.print(F(" addr=0x"));
  Serial.print(e.addr, HEX);
  Serial.print(F(" value=0x"));
  Serial.print(e.value, HEX);
  Serial.print(F(" reps="));
  Serial.println(repeats);

  // Počet skutečných vyslání (pro ty, co nemají repeats parametr)
  const uint8_t times = (uint8_t)min<int>(1 + repeats, 4);

  switch (p) {
    case NEC:
      IrSender.sendNEC((unsigned long)e.value, (int)e.bits, (int_fast8_t)repeats);
      return true;

    case SONY:
      IrSender.sendSony((unsigned long)e.value, (int)e.bits, (int_fast8_t)repeats);
      return true;

    case SAMSUNG:
      IrSender.sendSamsung((unsigned long)e.value, (int)e.bits, (int_fast8_t)repeats);
      return true;

    case JVC:
      IrSender.sendJVC((unsigned long)e.value, (int)e.bits, repeats > 0);
      for (uint8_t i = 1; i < times; i++) { delay(20); IrSender.sendJVC((unsigned long)e.value, (int)e.bits, true); }
      return true;

    case LG:
      for (uint8_t i = 0; i < times; i++) { IrSender.sendLG((unsigned long)e.value, (int)e.bits); if (i+1<times) delay(40); }
      return true;

    case RC5:
      for (uint8_t i = 0; i < times; i++) { IrSender.sendRC5((unsigned long)e.value, (int)e.bits); if (i+1<times) delay(100); }
      return true;

    case RC6:
      for (uint8_t i = 0; i < times; i++) { IrSender.sendRC6((unsigned long)e.value, (int)e.bits); if (i+1<times) delay(100); }
      return true;

    case PANASONIC:
    case KASEIKYO:
      for (uint8_t i = 0; i < times; i++) { IrSender.sendPanasonic((unsigned int)e.addr, (unsigned long)e.value, (int)e.bits); if (i+1<times) delay(100); }
      return true;

    case APPLE:
    case ONKYO:
      // často NEC-kompatibilní
      IrSender.sendNEC((unsigned long)e.value, (int)e.bits, (int_fast8_t)repeats);
      return true;

    default:
      Serial.println(F("[IR-TX] Nepodporovaný/neurčený protokol – nelze poslat."));
      return false;
  }
}

static bool sendLearnedByIndex(int idx, uint8_t repeats) {
  const LearnedCode* e = getLearnedByIndex((int16_t)idx);
  if (!e) return false;
  return irSendLearned(*e, repeats);
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

static bool isEffectivelyUnknown(decode_type_t proto, const LearnedCode *learned) {
  return proto == UNKNOWN && !(learned && learned->proto.length());
}
static bool isEffectivelyUnknown(const IREvent &e) {
  const LearnedCode *learned = getLearnedByIndex(e.learnedIndex);
  return isEffectivelyUnknown(e.proto, learned);
}

void printLine(const IRData &d, bool isRepeatSuppressed, const LearnedCode *learned) {
  if (g_showOnlyUnknown && !isEffectivelyUnknown(d.protocol, learned)) return;
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
      Serial.print(F(" function='")); Serial.print(learned->function); Serial.print(F("'"));
    }
    if (learned->vendor.length()) {
      Serial.print(F(" vendor='")); Serial.print(learned->vendor); Serial.print(F("'"));
    }
    Serial.print(F("]"));
  }
  Serial.println();
}

void printJSON(const IRData &d, const LearnedCode *learned) {
  if (g_showOnlyUnknown && !isEffectivelyUnknown(d.protocol, learned)) return;
  Serial.print(F("{\"proto\":\""));
  if (learned && learned->proto.length()) Serial.print(jsonEscape(learned->proto));
  else Serial.print(protoName(d.protocol));
  Serial.print(F("\",\"bits\":"));  Serial.print(d.numberOfBits);
  Serial.print(F(",\"addr\":"));    Serial.print(d.address);
  Serial.print(F(",\"cmd\":"));     Serial.print(d.command);
  Serial.print(F(",\"value\":"));   Serial.print((uint32_t)d.decodedRawData);
  Serial.print(F(",\"flags\":"));   Serial.print((uint32_t)d.flags);
  Serial.print(F(",\"ms\":"));      Serial.print(millis());
  Serial.print(F(",\"learned\":")); Serial.print(learned ? "true" : "false");
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

void wifiSetupWithWiFiManager() {
  WiFi.mode(WIFI_STA);
  WiFiManager wm;
  wm.setTitle("IR Receiver – WiFi Setup");
  wm.setHostname("ir-receiver");

  // Rozumné timeouty
  wm.setConnectTimeout(20);   // s na připojení k uložené WiFi
  wm.setConfigPortalTimeout(180); // s pro konfigurační AP

  String apName = makeApName();
  Serial.print(F("[NET] Připojuji Wi-Fi / otevírám portál… "));
  bool ok = wm.autoConnect(apName.c_str());  // blokuje max 180 s
  if (!ok) {
    Serial.println(F("nepodařilo se. Zařízení běží bez Wi-Fi (AP portál vypršel)."));
  } else {
    Serial.println(F("OK"));
    Serial.print(F("[NET] IP: "));
    Serial.println(WiFi.localIP());
  }
}

// ====== FS „databáze“ learned kódů (JSON Lines) ======

static bool isWhitespace(char c) {
  return c==' '||c=='\t'||c=='\n'||c=='\r';
}

static bool jsonExtractUint32(const String &line, const char *key, uint32_t &out) {
  String pattern = String('\"') + key + String("\":");
  int idx = line.indexOf(pattern);
  if (idx < 0) return false;
  idx += pattern.length();
  while (idx < (int)line.length() && isWhitespace(line[idx])) idx++;
  if (idx >= (int)line.length()) return false;
  bool hasDigit = false;
  uint32_t value = 0;
  while (idx < (int)line.length()) {
    char c = line[idx];
    if (c >= '0' && c <= '9') { hasDigit = true; value = value*10 + (c-'0'); idx++; }
    else break;
  }
  if (!hasDigit) return false;
  out = value;
  return true;
}

static bool jsonExtractString(const String &line, const char *key, String &out) {
  String pattern = String('\"') + key + String("\":\"");
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
        case '\"': result += '\"'; break;
        case '\\': result += '\\'; break;
        case '/': result += '/'; break;
        case 'b': result += '\b'; break;
        case 'f': result += '\f'; break;
        case 'n': result += '\n'; break;
        case 'r': result += '\r'; break;
        case 't': result += '\t'; break;
        default:  result += esc; break;
      }
    } else if (c == '\"') { out = result; return true; }
    else { result += c; }
  }
  return false;
}

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

bool fsAppendLearned(uint32_t value, uint8_t bits, uint32_t addr, uint32_t flags,
                     const String &protoStr, const String &vendor,
                     const String &functionName, const String &remoteLabel) {
  File f = LittleFS.open(LEARN_FILE, FILE_APPEND);
  if (!f) return false;

  String line;
  line.reserve(320);
  line += '{';
  line += F("\"ts\":");         line += static_cast<uint32_t>(millis());
  line += F(",\"proto\":\"");   line += protoStr;                   line += '\"';
  line += F(",\"value\":");     line += value;
  line += F(",\"bits\":");      line += static_cast<uint32_t>(bits);
  line += F(",\"addr\":");      line += addr;
  line += F(",\"flags\":");     line += flags;
  line += F(",\"vendor\":\"");  line += jsonEscape(vendor);         line += '\"';
  line += F(",\"function\":\"");line += jsonEscape(functionName);   line += '\"';
  line += F(",\"remote_label\":\""); line += jsonEscape(remoteLabel); line += F("\"}\n");

  size_t w = f.print(line);
  f.close();
  bool ok = (w == line.length());
  if (ok) { invalidateLearnedCache(); refreshLearnedAssociations(); }
  return ok;
}

bool fsUpdateLearned(size_t index, const String &protoStr,
                     const String &vendor, const String &functionName, const String &remoteLabel) {
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
  line += F(",\"proto\":\"");   line += proto;                    line += '\"';
  line += F(",\"value\":");     line += details.value;
  line += F(",\"bits\":");      line += static_cast<uint32_t>(details.bits);
  line += F(",\"addr\":");      line += details.addr;
  line += F(",\"flags\":");     line += details.flags;
  line += F(",\"vendor\":\"");  line += jsonEscape(vendor);       line += '\"';
  line += F(",\"function\":\"");line += jsonEscape(functionName); line += '\"';
  line += F(",\"remote_label\":\""); line += jsonEscape(remoteLabel); line += F("\"}");

  lines[index] = line;

  File out = LittleFS.open(LEARN_FILE, FILE_WRITE);
  if (!out) return false;

  bool ok = true;
  for (size_t i = 0; i < lines.size(); i++) {
    size_t w = out.print(lines[i]);
    if (w != lines[i].length()) { ok = false; break; }
    if (out.print('\n') != 1)   { ok = false; break; }
  }
  out.close();

  if (ok) { invalidateLearnedCache(); refreshLearnedAssociations(); }
  return ok;
}

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

void ensureLearnedCacheLoaded() {
  if (g_learnedCacheValid) return;
  g_learnedCache.clear();
  g_learnedIndex.clear();

  File f = LittleFS.open(LEARN_FILE, FILE_READ);
  if (!f) { g_learnedCacheValid = true; return; }

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
        const int16_t idx = static_cast<int16_t>(g_learnedCache.size() - 1);
        LearnedKey key{ entry.value, entry.addr, entry.bits };
        g_learnedIndex.emplace(key, idx);
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
  LearnedKey key{ value, addr, bits };
  auto it = g_learnedIndex.find(key);
  if (it == g_learnedIndex.end()) return -1;
  return it->second;
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
    if (idx >= 0) { hasLastUnknown = false; lastUnknown.learnedIndex = idx; }
    else          { lastUnknown.learnedIndex = -1; }
  }
}

#include "WebUI.h"

// ====== SETUP / LOOP ======
void setup() {
  Serial.begin(115200);
  delay(200);

  Serial.println();
  Serial.println(F("=== IR Receiver (IRremote) – ESP32-C3 ==="));
  Serial.print(F("IR RX pin: ")); Serial.println(IR_RX_PIN);

  if (!LittleFS.begin(true)) {
    Serial.println(F("[FS] LittleFS mount selhal (format=true), pokračuji bez learned databáze."));
  }

  prefs.begin("irrecv", false);
  g_showOnlyUnknown = prefs.getBool("only_unk", false);

  g_irTxPin = prefs.getInt("tx_pin", IR_TX_PIN_DEFAULT);
  if (g_irTxPin >= 0) initIrSender(g_irTxPin);

  wifiSetupWithWiFiManager();
  startWebServer();

  // IR přijímač
  IrReceiver.begin(IR_RX_PIN, DISABLE_LED_FEEDBACK);
  Serial.print(F("Protokoly povoleny: "));
  IrReceiver.printActiveIRProtocols(&Serial);
  Serial.println();
}

void loop() {
  if (!IrReceiver.decode()) { serviceClient(); return; }

  IRData &d = IrReceiver.decodedIRData;

  if (isNoise(d)) { IrReceiver.resume(); serviceClient(); return; }

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

  printLine(d, suppress, learned);
  if (!suppress) {
    printJSON(d, learned);
    addToHistory(d, learnedIndex);
  }

  lastMs   = now;
  lastProto= d.protocol;
  lastBits = d.numberOfBits;
  lastValue= d.decodedRawData;

  IrReceiver.resume();
  serviceClient();
}
