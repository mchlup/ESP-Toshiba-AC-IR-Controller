#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <WiFiManager.h>
#include <IRremote.hpp>     // Armin Joachimsmeyer IRremote
#include <Preferences.h>
#include <LittleFS.h>
#include <FS.h>
#include <vector>
#include <unordered_map>

// ======================== Datové typy a pomocné struktury ========================

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
    h ^= (static_cast<size_t>(k.addr) << 1);
    h ^= (static_cast<size_t>(k.bits) << 3);
    return h;
  }
};

struct IREvent {
  uint32_t ms;
  decode_type_t proto;
  uint8_t  bits;
  uint32_t address;
  uint32_t command;
  uint32_t value;
  uint32_t flags;
  int16_t  learnedIndex; // -1 = žádná vazba
};

struct LearnedCode {
  uint32_t value;
  uint8_t  bits;
  uint32_t addr;
  String   proto;     // textový štítek protokolu (např. "NEC", "Toshiba-AC", ...)
  String   vendor;
  String   function;
  String   remote;
};

struct LearnedLineDetails {
  uint32_t ts;
  uint32_t value;
  uint8_t  bits;
  uint32_t addr;
  uint32_t flags;
};

// ======================== Globální proměnné ========================

WebServer server(80);

static const int8_t IR_TX_PIN_DEFAULT = 0;   // ESP32-C3: např. 0 (přizpůsob dle zapojení)
static int8_t g_irTxPin = IR_TX_PIN_DEFAULT;
static const uint8_t IR_RX_PIN = 10;         // ESP32-C3: ověřené 4/5/10

static const uint32_t DUP_FILTER_MS = 120;

Preferences prefs;                 // NVS namespace: "irrecv"
static const char* LEARN_FILE = "/learned.jsonl";
static bool g_showOnlyUnknown = false;

// Historie posledních zachycených rámců
static const size_t HISTORY_LEN = 10;
static IREvent history[HISTORY_LEN];
static size_t histWrite = 0;
static size_t histCount = 0;

static void addToHistory(const IRData &d, int16_t learnedIndex) {
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

static bool hasLastUnknown = false;
static IREvent lastUnknown = {0, UNKNOWN, 0, 0, 0, 0, 0, -1};

static uint32_t lastValue = 0;
static decode_type_t lastProto = UNKNOWN;
static uint8_t lastBits = 0;
static uint32_t lastMs = 0;

// RAW buffer (pokud je dodán ze souboru)
static std::vector<uint16_t> g_lastRawMicros; // pouze informační; nevyužíváme pro učení teď
static uint16_t g_lastFreqKHz = 38;
static bool g_lastRawValid = false;

// Cache naučených kódů
static std::vector<LearnedCode> g_learnedCache;
static std::unordered_map<LearnedKey, int16_t, LearnedKeyHash> g_learnedIndex;
static bool g_learnedCacheValid = false;

// ======================== Deklarace funkcí ========================

static bool isNoise(const IRData &d);
String jsonEscape(const String& s);
const __FlashStringHelper* protoName(decode_type_t p);
static decode_type_t parseProtoLabel(const String &s);
static bool findProtoInHistory(uint32_t value, uint8_t bits, uint32_t addr, decode_type_t &outProto);

void invalidateLearnedCache();
void ensureLearnedCacheLoaded();
const LearnedCode* getLearnedByIndex(int16_t idx);
int16_t findLearnedIndex(uint32_t value, uint8_t bits, uint32_t addr);
const LearnedCode* findLearnedMatch(const IRData &d, int16_t *outIndex);
void refreshLearnedAssociations();

static bool jsonExtractUint32(const String &line, const char *key, uint32_t &out);
static bool jsonExtractString(const String &line, const char *key, String &out);

String fsReadLearnedAsArrayJSON();
bool fsAppendLearned(uint32_t value, uint8_t bits, uint32_t addr, uint32_t flags,
                     const String &protoStr, const String &vendor,
                     const String &functionName, const String &remoteLabel);
bool fsUpdateLearned(size_t index, const String &protoStr,
                     const String &vendor, const String &functionName, const String &remoteLabel);

// RAW z FS
static bool fsReadLearnedRawByValue(uint32_t value, uint8_t bits, uint32_t addr,
                                    std::vector<uint16_t> &outRaw, uint16_t &outFreqKHz);

// Odesílání
static bool irSendLearned(const LearnedCode &e, uint8_t repeats);

// Web
void startWebServer();
void serviceClient();

// ======================== Implementace ========================

static bool isNoise(const IRData &d) {
  if (d.flags & IRDATA_FLAGS_WAS_OVERFLOW) return true;
  if (d.protocol == UNKNOWN) {
    if (d.numberOfBits == 0) return true;
    if (d.decodedRawData == 0) return true;
  }
  if (d.decodedRawData > 0 && d.decodedRawData < 0x100) return true;
  if (d.numberOfBits > 0 && d.numberOfBits < 8) return true;
  return false;
}

String jsonEscape(const String& s) {
  String r; r.reserve(s.length() + 8);
  for (size_t i = 0; i < s.length(); i++) {
    char c = s[i];
    if (c == '"' || c == '\\') { r += '\\'; r += c; }
    else if ((uint8_t)c < 0x20) { r += '?'; }
    else r += c;
  }
  return r;
}

const __FlashStringHelper* protoName(decode_type_t p) {
  switch (p) {
    case NEC:       return F("NEC");
    case NEC2:      return F("NEC2");
    case SONY:      return F("SONY");
    case RC5:       return F("RC5");
    case RC6:       return F("RC6");
    case PANASONIC: return F("PANASONIC");
    case KASEIKYO:  return F("KASEIKYO");
    case SAMSUNG:   return F("SAMSUNG");
    case JVC:       return F("JVC");
    case LG:        return F("LG");
    case APPLE:     return F("APPLE");
    case ONKYO:     return F("ONKYO");
    default:        return F("UNKNOWN");
  }
}

static decode_type_t parseProtoLabel(const String &s) {
  String u = s; u.toUpperCase();
  if (u == "NEC") return NEC;
  if (u == "NEC2") return NEC2;
  if (u == "SONY") return SONY;
  if (u == "RC5") return RC5;
  if (u == "RC6") return RC6;
  if (u == "PANASONIC") return PANASONIC;
  if (u == "SAMSUNG") return SAMSUNG;
  if (u == "JVC") return JVC;
  if (u == "LG") return LG;
  if (u == "KASEIKYO" || u == "PANASONIC_KASEIKYO") return KASEIKYO;
  if (u == "APPLE") return APPLE;
  if (u == "ONKYO") return ONKYO;
  if (u == "TOSHIBA-AC" || u == "TOSHIBA") return NEC; // fallback
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

void invalidateLearnedCache() {
  g_learnedCacheValid = false;
  g_learnedIndex.clear();
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
      if (jsonExtractUint32(line, "bits", tmp)) {
        entry.bits = static_cast<uint8_t>(tmp & 0xFF);
      }
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
  if (idx < 0 || (size_t)idx >= g_learnedCache.size()) return nullptr;
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
  // Zatím není nutné nic dalšího
}

static bool jsonExtractUint32(const String &line, const char *key, uint32_t &out) {
  String k = String("\"") + key + String("\":");
  int p = line.indexOf(k);
  if (p < 0) return false;
  p += k.length();
  int e = p;
  while (e < (int)line.length() && isdigit(line[e])) e++;
  if (e == p) return false;
  out = (uint32_t) strtoul(line.substring(p, e).c_str(), nullptr, 10);
  return true;
}

static bool jsonExtractString(const String &line, const char *key, String &out) {
  String k = String("\"") + key + String("\":\"");
  int p = line.indexOf(k);
  if (p < 0) { out = ""; return false; }
  p += k.length();
  int e = line.indexOf('"', p);
  if (e < 0) { out = ""; return false; }
  out = line.substring(p, e);
  return true;
}

String fsReadLearnedAsArrayJSON() {
  File f = LittleFS.open(LEARN_FILE, FILE_READ);
  if (!f) return "[]";
  String out = "[";
  bool first = true;
  while (f.available()) {
    String line = f.readStringUntil('\n');
    line.trim();
    if (!line.length()) continue;
    if (!first) out += ',';
    first = false;
    out += line;
  }
  f.close();
  out += "]";
  return out;
}

// Pozn.: Pokud pošleš sem položku, jejíž value/bits/addr odpovídají poslednímu zachycenému kódu,
// můžeš teoreticky doplnit RAW i bez přímého přístupu do interního bufferu knihovny.
// Aktuálně je capture stub (bezpečné na tvé verzi), RAW očekáváme v souboru.
bool fsAppendLearned(uint32_t value, uint8_t bits, uint32_t addr, uint32_t flags,
                     const String &protoStr, const String &vendor,
                     const String &functionName, const String &remoteLabel) {
  File f = LittleFS.open(LEARN_FILE, FILE_APPEND);
  if (!f) return false;

  String line;
  line.reserve(1024);
  line += '{';
  line += F("\"ts\":");         line += static_cast<uint32_t>(millis());
  line += F(",\"proto\":\"");   line += protoStr;                   line += '\"';
  line += F(",\"value\":");     line += value;
  line += F(",\"bits\":");      line += static_cast<uint32_t>(bits);
  line += F(",\"addr\":");      line += addr;
  line += F(",\"flags\":");     line += flags;
  line += F(",\"vendor\":\"");  line += jsonEscape(vendor);         line += '\"';
  line += F(",\"function\":\"");line += jsonEscape(functionName);   line += '\"';
  line += F(",\"remote_label\":\""); line += jsonEscape(remoteLabel); line += '\"';

  // Pokud bys měl čerstvě připravené RAW (externě), můžeš jej tady připojit
  // (g_lastRawValid zatím nevyužíváme – není spolehlivě naplněno na tvé verzi knihovny)
  if (g_lastRawValid && lastUnknown.value == value && lastUnknown.bits == bits && lastUnknown.address == addr) {
    line += F(",\"raw\":[");
    for (size_t i = 0; i < g_lastRawMicros.size(); i++) {
      if (i) line += ',';
      line += (uint32_t)g_lastRawMicros[i];
    }
    line += ']';
    line += F(",\"freq\":"); line += (uint32_t)g_lastFreqKHz;
  }

  line += F("}\n");

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
    lines.push_back(f.readStringUntil('\n'));
  }
  f.close();
  if (index >= lines.size()) return false;

  String line = lines[index]; line.trim();
  if (!line.length()) return false;

  auto replaceString = [&](const char* key, const String &val) {
    String k = String("\"") + key + String("\":\"");
    int p = line.indexOf(k);
    if (p < 0) return;
    int s = p + k.length();
    int e = line.indexOf('"', s);
    if (e < 0) return;
    line = line.substring(0, s) + jsonEscape(val) + line.substring(e);
  };

  replaceString("proto", protoStr);
  replaceString("vendor", vendor);
  replaceString("function", functionName);
  replaceString("remote_label", remoteLabel);

  lines[index] = line + "\n";

  f = LittleFS.open(LEARN_FILE, FILE_WRITE);
  if (!f) return false;
  for (auto &ln : lines) f.print(ln);
  f.close();

  invalidateLearnedCache();
  refreshLearnedAssociations();
  return true;
}

// ======================== RAW práce ze souboru ========================

static bool fsReadLearnedRawByValue(uint32_t value, uint8_t bits, uint32_t addr,
                                    std::vector<uint16_t> &outRaw, uint16_t &outFreqKHz) {
  File f = LittleFS.open(LEARN_FILE, FILE_READ);
  if (!f) return false;
  bool ok = false;
  while (f.available()) {
    String line = f.readStringUntil('\n');
    if (!line.length()) continue;
    uint32_t v=0,a=0; uint32_t b=0;
    if (!jsonExtractUint32(line, "value", v)) continue;
    if (!jsonExtractUint32(line, "addr", a)) continue;
    if (!jsonExtractUint32(line, "bits", b)) continue;
    if (v==value && a==addr && b==(uint32_t)bits) {
      // parse raw array
      int rpos = line.indexOf(F("\"raw\""));
      if (rpos >= 0) {
        int sb = line.indexOf('[', rpos);
        int se = line.indexOf(']', sb);
        if (sb >= 0 && se > sb) {
          outRaw.clear();
          String arr = line.substring(sb+1, se);
          int i = 0;
          while (i < arr.length()) {
            while (i < arr.length() && (arr[i] == ' ' || arr[i] == '\t' || arr[i] == ',')) i++;
            int j = i;
            while (j < arr.length() && isDigit(arr[j])) j++;
            if (j > i) {
              uint32_t val = strtoul(arr.substring(i, j).c_str(), nullptr, 10);
              if (val > 0xFFFF) val = 0xFFFF;
              outRaw.push_back((uint16_t)val);
            }
            i = j + 1;
          }
          uint32_t fq=0;
          if (jsonExtractUint32(line, "freq", fq) && fq > 0) outFreqKHz = (uint16_t)fq;
          else outFreqKHz = 38;
          ok = !outRaw.empty();
        }
      }
      break;
    }
  }
  f.close();
  return ok;
}

// ======================== Odesílání naučeného (RAW-first) ========================

static bool irSendLearned(const LearnedCode &e, uint8_t repeats) {
  // 1) RAW z FS – pokud existuje, pošli přesně původní průběh
  std::vector<uint16_t> raw;
  uint16_t freq = 38;
  if (fsReadLearnedRawByValue(e.value, e.bits, e.addr, raw, freq)) {
    Serial.print(F("[IR-TX] RAW len=")); Serial.print(raw.size());
    Serial.print(F(" freq=")); Serial.print(freq); Serial.println(F("kHz"));
    for (uint8_t r=0; r<=repeats; r++) {
      IrSender.sendRaw(raw.data(), (uint16_t)raw.size(), freq);
      delay(40);
    }
    return true;
  }

  // 2) Fallback podle protokolu
  decode_type_t p = parseProtoLabel(e.proto);
  if (p == UNKNOWN) {
    decode_type_t histP = UNKNOWN;
    if (findProtoInHistory(e.value, e.bits, e.addr, histP)) p = histP;
  }

  Serial.print(F("[IR-TX] proto="));
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

  // jednotný cyklus opakování
  auto sendLoop = [&](auto &&fnSendOnce) {
    for (uint8_t r = 0; r <= repeats; r++) {
      fnSendOnce();
      delay(40);
    }
    return true;
  };

  switch (p) {
    case NEC:
      return sendLoop([&](){ IrSender.sendNEC((unsigned long)e.value, (int)e.bits); });

    case SONY:
      return sendLoop([&](){ IrSender.sendSony((unsigned long)e.value, (int)e.bits); });

    case SAMSUNG: {
      // IRremote: sendSamsung(uint16_t address, uint16_t command, int_fast8_t repeats)
      uint16_t addr16 = (e.addr != 0) ? (uint16_t)e.addr : (uint16_t)(e.value >> 16);
      uint16_t cmd16  = (uint16_t)(e.value & 0xFFFF);
      return sendLoop([&](){ IrSender.sendSamsung(addr16, cmd16, 0); });
    }

    case JVC:
      // použijeme overload (data, nbits, bool repeat=false) a opakujeme ručně
      return sendLoop([&](){ IrSender.sendJVC((unsigned long)e.value, (int)e.bits, false); });

    case RC5:
      return sendLoop([&](){ IrSender.sendRC5((unsigned long)e.value, (int)e.bits); });

    case RC6:
      return sendLoop([&](){ IrSender.sendRC6((unsigned long)e.value, (int)e.bits); });

    case PANASONIC:
    case KASEIKYO:
      // tyto protokoly typicky vyžadují oddělené parametry; bez RAW je nejisté
      return false;

    default:
      return false;
  }
}


// ======================== „Efektivně neznámý“ helper ========================

static bool isEffectivelyUnknown(decode_type_t proto, const LearnedCode *learned) {
  if (proto != UNKNOWN) return false;
  if (!learned) return true;
  if (learned->proto.length() == 0) return true;
  decode_type_t lp = parseProtoLabel(learned->proto);
  return (lp == UNKNOWN);
}

static inline bool isEffectivelyUnknown(const IREvent &ev) {
  int16_t idx = findLearnedIndex(ev.value, ev.bits, ev.address);
  const LearnedCode* lc = (idx >= 0) ? getLearnedByIndex(idx) : nullptr;
  return isEffectivelyUnknown(ev.proto, lc);
}

// Pohodlná obálka pro použití s IREvent (kvůli WebUI.h)
static bool isEffectivelyUnknownEvent(const IREvent &ev) {
  // zkusit dohledat learned položku
  const LearnedCode* lc = nullptr;
  int16_t idx = findLearnedIndex(ev.value, ev.bits, ev.address);
  if (idx >= 0) lc = getLearnedByIndex(idx);
  return isEffectivelyUnknown(ev.proto, lc);
}

// ======================== WiFi + Web ========================

String makeApName() {
  uint8_t mac[6]; WiFi.macAddress(mac);
  char buf[32];
  snprintf(buf, sizeof(buf), "IR-Receiver-Setup-%02X%02X", mac[4], mac[5]);
  return String(buf);
}

void wifiSetupWithWiFiManager() {
  WiFi.mode(WIFI_STA);
  WiFiManager wm;
  wm.setConfigPortalTimeout(180);
  String apName = makeApName();
  if (!wm.autoConnect(apName.c_str())) {
    Serial.println(F("[NET] WiFi připojení selhalo, pokračuju bez sítě."));
  } else {
    Serial.print(F("[NET] Připojeno: ")); Serial.println(WiFi.localIP());
  }
}

// ======================== Tisk / JSON pro debug ========================

static void printLine(const IRData &d, bool suppress, const LearnedCode *learned) {
  Serial.print(d.decodedRawData, HEX);
  Serial.print(F("  "));
  Serial.print(protoName(d.protocol));
  Serial.print(F("  "));
  Serial.print(d.numberOfBits);
  Serial.print(F("b  addr:0x"));
  Serial.print(d.address, HEX);
  Serial.print(F("  cmd:0x"));
  Serial.print(d.command, HEX);
  Serial.print(F("  flags:0x"));
  Serial.print(d.flags, HEX);
  if (learned) {
    Serial.print(F("  [learned: "));
    Serial.print(learned->vendor);
    Serial.print(F(" / "));
    Serial.print(learned->function);
    Serial.print(F("]"));
  }
  if (suppress) Serial.print(F("  (dup)"));
  Serial.println();
}

static void printJSON(const IRData &d, const LearnedCode *learned) {
  Serial.print(F("{\"ms\":"));
  Serial.print((uint32_t)millis());
  Serial.print(F(",\"proto\":\""));
  Serial.print(protoName(d.protocol));
  Serial.print(F("\",\"value\":"));
  Serial.print(d.decodedRawData);
  Serial.print(F(",\"bits\":"));
  Serial.print(d.numberOfBits);
  Serial.print(F(",\"addr\":"));
  Serial.print(d.address);
  Serial.print(F(",\"cmd\":"));
  Serial.print(d.command);
  Serial.print(F(",\"flags\":"));
  Serial.print(d.flags);
  if (learned) {
    Serial.print(F(",\"learned\":{"));
    Serial.print(F("\"vendor\":\"")); Serial.print(jsonEscape(learned->vendor)); Serial.print(F("\","));
    Serial.print(F("\"function\":\"")); Serial.print(jsonEscape(learned->function)); Serial.print(F("\","));
    Serial.print(F("\"remote\":\"")); Serial.print(jsonEscape(learned->remote)); Serial.print('"');
    Serial.print(F("}"));
  }
  Serial.println(F("}"));
}

// ======================== IR Sender init ========================

static void initIrSender(int8_t pin) {
  if (pin < 0) return;
  IrSender.begin(pin, DISABLE_LED_FEEDBACK, true);
  Serial.print(F("[IR-TX] Inicializován na pinu ")); Serial.println(pin);
}

// ======================== Sběr RAW – STUB (bezpečný) ========================
// Tady nic neděláme, protože tvá verze IRremote neposkytuje rawDataPtr.
// RAW se očekává v /learned.jsonl u položky (klíče "raw" a "freq").
static void captureLastRawFromReceiver() {
  g_lastRawValid = false;
  g_lastRawMicros.clear();
  g_lastFreqKHz = 38;
}

// ======================== WebUI.h bude používat tyto symboly ========================

extern bool g_showOnlyUnknown;
extern IREvent history[];
extern size_t histCount;
extern size_t histWrite;
extern String fsReadLearnedAsArrayJSON();
extern const LearnedCode* getLearnedByIndex(int16_t idx);
extern int16_t findLearnedIndex(uint32_t value, uint8_t bits, uint32_t addr);
extern bool fsUpdateLearned(size_t index, const String &protoStr,
                            const String &vendor, const String &functionName, const String &remoteLabel);
extern bool fsAppendLearned(uint32_t value, uint8_t bits, uint32_t addr, uint32_t flags,
                            const String &protoStr, const String &vendor,
                            const String &functionName, const String &remoteLabel);
extern bool isEffectivelyUnknownEvent(const IREvent &ev);
extern bool irSendByIndex(int16_t idx, uint8_t repeats);

// Wrapper pro WebUI: odeslání podle indexu
bool irSendByIndex(int16_t idx, uint8_t repeats) {
  const LearnedCode* e = getLearnedByIndex(idx);
  if (!e) return false;
  return irSendLearned(*e, repeats);
}

#include "WebUI.h"  // používá výše deklarované symboly

// ======================== SETUP / LOOP ========================

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
  serviceClient();

  if (!IrReceiver.decode()) {
    delay(1);
    return;
  }

  IRData d = IrReceiver.decodedIRData;

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
    captureLastRawFromReceiver();  // STUB – neplní RAW, jen udržuje kompatibilitu
  }

  lastMs   = now;
  lastProto= d.protocol;
  lastBits = d.numberOfBits;
  lastValue= d.decodedRawData;

  IrReceiver.resume();
  serviceClient();
}
