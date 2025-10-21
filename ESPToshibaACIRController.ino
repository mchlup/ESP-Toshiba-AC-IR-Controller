#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <WiFiManager.h>
#include <Preferences.h>
#include <LittleFS.h>
#include <FS.h>
#define IR_GLOBAL
#include <IRremote.hpp>
#include <vector>
#include <unordered_map>
#include <ctype.h>
#include <algorithm>
#include "ToshibaAC.h"

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
static const int8_t IR_TX_PIN_DEFAULT = 3;   // ESP32-C3: např. 4 (přizpůsob dle zapojení)
static int8_t g_irTxPin = IR_TX_PIN_DEFAULT;
static const uint8_t IR_RX_PIN = 4;         // ESP32-C3: ověřené 4/5/10
ToshibaACIR toshiba;
static const uint8_t POWER_GND_PIN = -1 ;
static const uint8_t POWER_VCC_PIN = -1 ;
static const uint32_t DUP_FILTER_MS = 120;
Preferences prefs;                 // NVS namespace: "irrecv"
static const char* LEARN_FILE = "/learned.jsonl";
static bool g_showOnlyUnknown = false;
static const size_t HISTORY_LEN = 10;
static IREvent history[HISTORY_LEN];
static size_t histWrite = 0;
static size_t histCount = 0;
static const uint32_t RAW_EVENT_MATCH_WINDOW_MS = 250;
static std::vector<uint16_t> g_lastRaw;
static uint8_t  g_lastRawKhz = 38;   // default
static bool     g_lastRawValid = false;
static uint32_t g_lastRawCaptureMs = 0;
static String   g_lastRawSource = F("(none)");

static bool     g_lastSendValid = false;
static bool     g_lastSendOk = false;
static uint32_t g_lastSendMs = 0;
static String   g_lastSendMethod = F("none");
static decode_type_t g_lastSendProto = UNKNOWN;
static size_t   g_lastSendPulses = 0;
static uint8_t  g_lastSendFreq = 0;

static bool     g_lastDecodeValid = false;
static uint32_t g_lastDecodeMs = 0;
static decode_type_t g_lastDecodeProto = UNKNOWN;
static uint8_t  g_lastDecodeBits = 0;
static uint32_t g_lastDecodePulseCount = 0;
static String   g_lastDecodeSource = F("(none)");

// Soubor pro RAW: /learned/raw_<index>.bin  (binárně: [1B khz][2B len LE][2B*len pulzy])
static String rawPathForIndex(size_t index) {
  String p = F("/learned/raw_");
  p += String(index);
  p += F(".bin");
  return p;
}

static bool fsEnsureRawDir() {
  if (LittleFS.exists("/learned")) {
    return true;
  }
  return LittleFS.mkdir("/learned");
}

static bool fsSaveRawForIndex(size_t index, const uint16_t* buf, uint16_t len, uint8_t khz) {
  if (!fsEnsureRawDir()) {
    Serial.println(F("[FS] Nelze vytvořit adresář /learned pro RAW data."));
    return false;
  }

  File f = LittleFS.open(rawPathForIndex(index), "w");
  if (!f) return false;
  f.write(&khz, 1);
  f.write((const uint8_t*)&len, 2);
  f.write((const uint8_t*)buf, len * 2);
  f.close();
  return true;
}

static bool fsLoadRawForIndex(size_t index, std::vector<uint16_t> &out, uint8_t &khz) {
  File f = LittleFS.open(rawPathForIndex(index), "r");
  if (!f) return false;
  uint8_t kh; uint16_t len;
  if (f.read(&kh, 1) != 1) { f.close(); return false; }
  if (f.read((uint8_t*)&len, 2) != 2) { f.close(); return false; }
  out.resize(len);
  size_t need = len * 2;
  if (f.read((uint8_t*)out.data(), need) != need) { f.close(); return false; }
  f.close();
  khz = kh;
  return true;
}

static bool fsHasRaw(size_t index) {
  return LittleFS.exists(rawPathForIndex(index));
}

// Pokud tvá API vrstva nevrací index nově vložené položky, použij getLearnedCount()
extern size_t getLearnedCount(); // doplň, nebo přepiš dle tvé implementace

// Zavolej hned po IrReceiver.decode() úspěchu (tj. když máš vyplněné decodedIRData).
// captureLastRawFromReceiver() se postará o bezpečné převzetí posledních pulsů.

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
static std::vector<LearnedCode> g_learnedCache;
static std::unordered_map<LearnedKey, int16_t, LearnedKeyHash> g_learnedIndex;
static bool g_learnedCacheValid = false;
// ===== RAW sniffer (nezávislý na knihovně) =====
// Vstup je výstup IR demodulátoru (obvykle invertovaný: idle=HIGH, MARK=LOW)
static const uint16_t RAW_MAX_PULSES = 512;       // stačí pro AC rámce
static const uint32_t RAW_FRAME_GAP_US = 15000;   // 15 ms = konec rámce

volatile uint16_t g_isrPulses[RAW_MAX_PULSES];
volatile uint16_t g_isrCount = 0;
volatile uint32_t g_isrLastEdgeUs = 0;
volatile int      g_isrLastLevel = -1;  // -1 = neumíme
volatile bool     g_isrFrameReady = false;
static uint16_t   g_rawScratch[RAW_MAX_PULSES];

// Pomocné: bezpečné čtení micros v ISR/loop
static inline uint32_t micros_safe() { return micros(); }

static void finalizeRawCapture(const uint16_t *src, uint16_t count,
                               const __FlashStringHelper *label) {
  g_lastRaw.clear();
  if (!src || count == 0) {
    g_lastRawValid = false;
    g_lastRawKhz = 38;
    g_lastRawSource = F("(missing)");
    g_lastRawCaptureMs = millis();
    return;
  }

  g_lastRaw.reserve(count);
  for (uint16_t i = 0; i < count; ++i) {
    g_lastRaw.push_back(src[i]);
  }

  if (g_lastRaw.size() > 2 && g_lastRaw[0] < 150) {
    g_lastRaw[1] = (uint16_t)std::min<uint32_t>(0xFFFF, (uint32_t)g_lastRaw[0] + g_lastRaw[1]);
    g_lastRaw.erase(g_lastRaw.begin());
  }

  g_lastRawKhz = 38;
  g_lastRawValid = !g_lastRaw.empty();
  g_lastRawCaptureMs = millis();
  if (label) {
    g_lastRawSource = String(label);
  } else {
    g_lastRawSource = F("sniffer");
  }
}

// ISR: ukládá délky pulsů v µs mezi hranami
void IRAM_ATTR irEdgeISR() {
  const uint32_t now = micros_safe();
  int lvl = digitalRead(IR_RX_PIN);    // na ESP32-C3 je to rychlé

  if (g_isrLastLevel < 0) {
    // první zachycení – inicializace
    g_isrLastLevel = lvl;
    g_isrLastEdgeUs = now;
    g_isrCount = 0;
    return;
  }

  uint32_t dur = now - g_isrLastEdgeUs;
  g_isrLastEdgeUs = now;

  if (g_isrCount < RAW_MAX_PULSES) {
    // saturace na 16-bit
    if (dur > 0xFFFF) dur = 0xFFFF;
    g_isrPulses[g_isrCount++] = (uint16_t)dur;
  } else {
    // přetečeno – rámec už je moc dlouhý, příště se uzavře gapem
  }

  g_isrLastLevel = lvl;
}

// Služba pro loop(): uzavře rámec po mezeře a převede do g_lastRaw
static void rawSnifferService() {
  // Pokud proběhly hrany a dlouho žádná nebyla => máme hotový rámec
  if (g_isrCount > 0 && !g_isrFrameReady) {
    uint32_t gap = micros_safe() - g_isrLastEdgeUs;
    if (gap > RAW_FRAME_GAP_US) {
      noInterrupts();
      uint16_t n = g_isrCount;
      for (uint16_t i = 0; i < n; i++) g_rawScratch[i] = g_isrPulses[i];
      g_isrCount = 0;
      g_isrLastLevel = -1;
      interrupts();

      finalizeRawCapture(g_rawScratch, n, F("sniffer"));
      g_isrFrameReady = true;  // jen pro debug; další hrana to zruší
    }
  }
}

static void recordSendDiagnostics(bool ok, const String &method,
                                  decode_type_t proto, size_t pulses,
                                  uint8_t freqKhz) {
  g_lastSendValid = true;
  g_lastSendOk = ok;
  g_lastSendMs = millis();
  g_lastSendMethod = method;
  g_lastSendProto = proto;
  g_lastSendPulses = pulses;
  g_lastSendFreq = freqKhz;
}

void recordIrTxDiagnostics(bool ok, decode_type_t proto, size_t pulses,
                           uint8_t freqKhz,
                           const __FlashStringHelper* methodLabel) {
  String label;
  if (methodLabel) {
    label = String(methodLabel);
  } else {
    label = F("external");
  }
  recordSendDiagnostics(ok, label, proto, pulses, freqKhz);
}

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
static bool parseRawDurationsArg(const String &arg, std::vector<uint16_t> &out);

String fsReadLearnedAsArrayJSON();
bool fsAppendLearned(uint32_t value, uint8_t bits, uint32_t addr, uint32_t flags,
                     const String &protoStr, const String &vendor,
                     const String &functionName, const String &remoteLabel,
                     const std::vector<uint16_t> *rawOpt = nullptr,
                     uint8_t rawKhz = 38);
bool fsUpdateLearned(size_t index, const String &protoStr,
                     const String &vendor, const String &functionName, const String &remoteLabel);

// RAW z FS
static bool fsReadLearnedRawByValue(uint32_t value, uint8_t bits, uint32_t addr,
                                    std::vector<uint16_t> &outRaw, uint16_t &outFreqKHz);

// Odesílání
static bool irSendLearned(const LearnedCode &e, uint8_t repeats);
static bool irSendLastRaw(uint8_t repeats);
static void recordSendDiagnostics(bool ok, const String &method,
                                  decode_type_t proto, size_t pulses,
                                  uint8_t freqKhz);
String buildDiagnosticsJson();
String buildRawDumpJson();

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

// === Mapování label -> IRremote dekodér ===
static decode_type_t parseProtoLabelRelaxed(const String &sIn) {
  String s = sIn; s.trim(); s.toUpperCase();
  if (s == F("NEC")) return NEC;
  if (s == F("SONY")) return SONY;
  if (s == F("RC5")) return RC5;
  if (s == F("RC6")) return RC6;
  if (s == F("SAMSUNG")) return SAMSUNG;
  if (s == F("PANASONIC")) return PANASONIC;
  if (s == F("JVC")) return JVC;
  if (s == F("SHARP")) return SHARP;
  if (s == F("LG")) return LG;
  if (s == F("B&O") || s == F("BANGOLUFSEN")) return BANG_OLUFSEN;
  // Přidej další podle potřeby…
  return UNKNOWN;
}

// === Core sender – zkus nativní protokol, jinak RAW ===
static bool irSendLearnedCore(const LearnedCode &e, uint8_t repeats,
                              const std::vector<uint16_t>* rawOpt = nullptr,
                              uint8_t rawKhz = 38) {
  auto doRepeats = [&](auto &&fnOnce){
    for (uint8_t r=0; r<=repeats; r++){ fnOnce(); delay(40); }
    return true;
  };

  const bool hasRaw = rawOpt && !rawOpt->empty();
  const uint8_t rawFreq = rawKhz ? rawKhz : 38;

  if (hasRaw) {
    IrSender.sendRaw(rawOpt->data(), static_cast<uint16_t>(rawOpt->size()), rawFreq);
    for (uint8_t r = 0; r < repeats; r++) {
      delay(60);
      IrSender.sendRaw(rawOpt->data(), static_cast<uint16_t>(rawOpt->size()), rawFreq);
    }
    if (rawOpt == &g_lastRaw) {
      recordSendDiagnostics(true, F("raw-capture"), UNKNOWN, rawOpt->size(), rawFreq);
    } else {
      recordSendDiagnostics(true, F("raw-storage"), UNKNOWN, rawOpt->size(), rawFreq);
    }
    return true;
  }

  const decode_type_t t = parseProtoLabelRelaxed(e.proto.length() ? e.proto : String(F("UNKNOWN")));

  // 1) nativní protokoly (když je známý label)
  switch (t) {
    case NEC:
      doRepeats([&]{ IrSender.sendNEC((unsigned long)e.value, (int)e.bits); });
      recordSendDiagnostics(true, F("proto-NEC"), t, 0, 0);
      return true;
    case SONY:
      doRepeats([&]{ IrSender.sendSony((unsigned long)e.value, (int)e.bits); });
      recordSendDiagnostics(true, F("proto-SONY"), t, 0, 0);
      return true;
    case RC5:
      doRepeats([&]{ IrSender.sendRC5((unsigned long)e.value, (int)e.bits); });
      recordSendDiagnostics(true, F("proto-RC5"), t, 0, 0);
      return true;
    case RC6:
      doRepeats([&]{ IrSender.sendRC6((unsigned long)e.value, (int)e.bits); });
      recordSendDiagnostics(true, F("proto-RC6"), t, 0, 0);
      return true;
    case JVC:
      doRepeats([&]{ IrSender.sendJVC((unsigned long)e.value, (int)e.bits, false); });
      recordSendDiagnostics(true, F("proto-JVC"), t, 0, 0);
      return true;
    case LG:
      doRepeats([&]{ IrSender.sendLG((unsigned long)e.value, (int)e.bits); });
      recordSendDiagnostics(true, F("proto-LG"), t, 0, 0);
      return true;
    case SAMSUNG: {
      uint16_t a=(e.addr)?(uint16_t)e.addr:(uint16_t)(e.value>>16);
      uint16_t c=(uint16_t)(e.value & 0xFFFF);
      doRepeats([&]{ IrSender.sendSamsung(a,c,0); });
      recordSendDiagnostics(true, F("proto-SAMSUNG"), t, 0, 0);
      return true;
    }
    case PANASONIC:
      doRepeats([&]{ IrSender.sendPanasonic((uint16_t)e.addr, (uint32_t)e.value, 0); });
      recordSendDiagnostics(true, F("proto-PANASONIC"), t, 0, 0);
      return true;
    case SHARP:
      doRepeats([&]{ IrSender.sendSharp((uint16_t)e.addr, (uint16_t)(e.value&0xFFFF), 0); });
      recordSendDiagnostics(true, F("proto-SHARP"), t, 0, 0);
      return true;
    default: break;
  }

  // 2) HEURISTICKÝ FALLBACK pro UNKNOWN bez RAW
switch (e.bits) {
  case 32:
    // 32 b je nejčastěji NEC – zkus jen NEC (ať se přijímač zbytečně nechytačí na ONKYO/Kaseikyo)
    if (doRepeats([&]{ IrSender.sendNEC((unsigned long)e.value, 32); })) {
      recordSendDiagnostics(true, F("fallback-NEC"), NEC, 0, 0);
      return true;
    }

    // volitelně Samsung jen pokud máme aspoň něco v addr/cmd (jinak to často „přepřekládá“ na ONKYO)
    if (e.addr || (e.value & 0xFFFF)) {
      uint16_t a = e.addr ? (uint16_t)e.addr : (uint16_t)(e.value >> 16);
      uint16_t c = (uint16_t)(e.value & 0xFFFF);
      if (doRepeats([&]{ IrSender.sendSamsung(a, c, 0); })) {
        recordSendDiagnostics(true, F("fallback-SAMSUNG"), SAMSUNG, 0, 0);
        return true;
      }
    }
    break;

  case 12: case 15:
    if (doRepeats([&]{ IrSender.sendSony((unsigned long)e.value, (int)e.bits); })) {
      recordSendDiagnostics(true, F("fallback-SONY"), SONY, 0, 0);
      return true;
    }
    break;

  case 16:
    if (doRepeats([&]{ IrSender.sendJVC((unsigned long)e.value, 16, false); })) {
      recordSendDiagnostics(true, F("fallback-JVC"), JVC, 0, 0);
      return true;
    }
    break;

  case 20:
    if (doRepeats([&]{ IrSender.sendRC5((unsigned long)e.value, 20); })) {
      recordSendDiagnostics(true, F("fallback-RC5"), RC5, 0, 0);
      return true;
    }
    break;
}
recordSendDiagnostics(false, F("fallback-failed"), t, 0, rawFreq);
return false;
}

// === Odeslání „podle indexu“ – načte případný RAW a zavolá Core ===
static bool irSendLearnedByIndex(int index, uint8_t repeats) {
  const LearnedCode* e = getLearnedByIndex(index);
  if (!e) {
    recordSendDiagnostics(false, F("index-invalid"), UNKNOWN, 0, 0);
    return false;
  }

  std::vector<uint16_t> raw;
  uint8_t khz = 38;
  if (fsLoadRawForIndex(index, raw, khz)) {
    return irSendLearnedCore(*e, repeats, &raw, khz);
  } else {
    // Bez RAW, zkus aspoň nativní dle labelu
    return irSendLearnedCore(*e, repeats, nullptr, 38);
  }
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

static bool parseRawDurationsArg(const String &arg, std::vector<uint16_t> &out) {
  out.clear();
  const char *ptr = arg.c_str();
  while (ptr && *ptr) {
    while (*ptr && (isspace((unsigned char)*ptr) || *ptr == ',' || *ptr == ';' || *ptr == '[' || *ptr == ']')) {
      ++ptr;
    }
    if (!*ptr) break;

    char *endPtr = nullptr;
    unsigned long val = strtoul(ptr, &endPtr, 0);
    if (endPtr == ptr) {
      return false;
    }
    if (out.size() >= RAW_MAX_PULSES) {
      return false;
    }
    if (val > 0xFFFFUL) {
      val = 0xFFFFUL;
    }
    out.push_back(static_cast<uint16_t>(val));
    ptr = endPtr;
  }
  return !out.empty();
}

String fsReadLearnedAsArrayJSON() {
  File f = LittleFS.open(LEARN_FILE, FILE_READ);
  if (!f) return "[]";

  String out;
  out.reserve(256);
  out += '[';
  bool first = true;

  while (f.available()) {
    String line = f.readStringUntil('\n');
    line.trim();
    if (!line.length()) continue;

    int start = 0;
    while (start < line.length()) {
      int glued = line.indexOf(F("}{"), start);
      String chunk;
      if (glued >= 0) {
        chunk = line.substring(start, glued + 1);
        start = glued + 1;
      } else {
        chunk = line.substring(start);
        start = line.length();
      }

      chunk.trim();
      if (!chunk.length()) continue;

      if (!first) {
        out += ',';
      }
      out += chunk;
      first = false;
    }
  }

  f.close();
  out += ']';
  return out;
}


// Pozn.: RAW je možné přidat dvěma způsoby – buď automaticky (přes g_lastRaw po zachycení rámce),
// nebo explicitně předáním v parametru rawOpt (např. z API). Funkce se postará o serializaci do
// JSON i o uložení binární kopie do LittleFS.
bool fsAppendLearned(uint32_t value, uint8_t bits, uint32_t addr, uint32_t flags,
                     const String &protoStr, const String &vendor,
                     const String &functionName, const String &remoteLabel,
                     const std::vector<uint16_t> *rawOpt,
                     uint8_t rawKhz) {
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

  const std::vector<uint16_t> *rawSource = nullptr;
  uint8_t freqKhz = rawKhz ? rawKhz : 38;

  if (rawOpt && !rawOpt->empty()) {
    rawSource = rawOpt;
  } else if (g_lastRawValid && !g_lastRaw.empty()) {
    uint32_t age = millis() - g_lastRawCaptureMs;
    if (age < 5000UL) {
      rawSource = &g_lastRaw;
      freqKhz = g_lastRawKhz;
    }
  }

  if (rawSource && !rawSource->empty()) {
    line += F(",\"raw\":[");
    for (size_t i = 0; i < rawSource->size(); i++) {
      if (i) line += ',';
      line += (uint32_t)(*rawSource)[i];
    }
    line += ']';
    line += F(",\"freq\":");
    line += (uint32_t)freqKhz;
  }

  line += F("}\n");

  size_t w = f.print(line);
  f.close();
  bool ok = (w == line.length());

  if (!ok) {
    return false;
  }

  invalidateLearnedCache();
  refreshLearnedAssociations();

  if (rawSource && !rawSource->empty()) {
    size_t count = getLearnedCount();
    if (count > 0) {
      if (!fsSaveRawForIndex(count - 1, rawSource->data(), (uint16_t)rawSource->size(), freqKhz)) {
        Serial.println(F("[FS] Varování: RAW data se nepodařilo uložit do binárního souboru."));
      }
    }
    if (rawSource == &g_lastRaw) {
      g_lastRawValid = false;
      g_lastRawSource = F("(uloženo)");
      g_lastRaw.clear();
      g_lastDecodeSource = g_lastRawSource;
      g_lastDecodePulseCount = 0;
    }
  }

  return true;
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

  for (auto &ln : lines) {
    ln.trim();
  }

  String line = lines[index];
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

  lines[index] = line;

  f = LittleFS.open(LEARN_FILE, FILE_WRITE);
  if (!f) return false;
  for (auto &ln : lines) {
    if (!ln.length()) continue;
    f.print(ln);
    f.print('\n');
  }
  f.close();

  invalidateLearnedCache();
  refreshLearnedAssociations();
  return true;
}

bool fsDeleteLearned(size_t index) {
  File f = LittleFS.open(LEARN_FILE, FILE_READ);
  if (!f) return false;

  std::vector<String> lines;
  while (f.available()) {
    lines.push_back(f.readStringUntil('\n'));
  }
  f.close();

  if (index >= lines.size()) return false;

  for (auto &ln : lines) {
    ln.trim();
  }

  const size_t oldCount = lines.size();
  lines.erase(lines.begin() + index);

  f = LittleFS.open(LEARN_FILE, FILE_WRITE);
  if (!f) return false;
  for (auto &ln : lines) {
    if (!ln.length()) continue;
    f.print(ln);
    f.print('\n');
  }
  f.close();

  // Remove RAW capture for the deleted entry and shift subsequent files down.
  String target = rawPathForIndex(index);
  if (LittleFS.exists(target)) {
    LittleFS.remove(target);
  }
  for (size_t i = index + 1; i < oldCount; ++i) {
    String from = rawPathForIndex(i);
    if (!LittleFS.exists(from)) continue;
    String to = rawPathForIndex(i - 1);
    if (LittleFS.exists(to)) {
      LittleFS.remove(to);
    }
    LittleFS.rename(from.c_str(), to.c_str());
  }

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
  std::vector<uint16_t> raw;
  uint8_t rawFreq = 38;
  const std::vector<uint16_t>* rawPtr = nullptr;

  int16_t idx = findLearnedIndex(e.value, e.bits, e.addr);
  if (idx >= 0) {
    if (fsLoadRawForIndex(static_cast<size_t>(idx), raw, rawFreq) && !raw.empty()) {
      rawPtr = &raw;
    }
  }

  if (!rawPtr) {
    uint16_t freqFromJson = rawFreq;
    if (fsReadLearnedRawByValue(e.value, e.bits, e.addr, raw, freqFromJson) && !raw.empty()) {
      rawFreq = (freqFromJson > 0)
                  ? static_cast<uint8_t>(std::min<uint16_t>(freqFromJson, 255))
                  : static_cast<uint8_t>(38);
      rawPtr = &raw;
    }
  }

  return irSendLearnedCore(e, repeats, rawPtr, rawFreq);
}

static bool irSendLastRaw(uint8_t repeats) {
  if (!g_lastRawValid || g_lastRaw.empty()) {
    recordSendDiagnostics(false, F("raw-capture-missing"), UNKNOWN, 0, g_lastRawKhz);
    return false;
  }

  Serial.print(F("[IR-TX] Posílám poslední zachycený RAW ("));
  Serial.print(g_lastRaw.size());
  Serial.print(F(" pulsů, "));
  Serial.print(g_lastRawKhz);
  Serial.println(F("kHz)"));

  for (uint8_t r = 0; r <= repeats; ++r) {
    IrSender.sendRaw(g_lastRaw.data(), static_cast<uint16_t>(g_lastRaw.size()), g_lastRawKhz);
    if (r < repeats) delay(60);
  }

  recordSendDiagnostics(true, F("raw-capture"), UNKNOWN, g_lastRaw.size(), g_lastRawKhz);
  return true;
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
  g_irTxPin = pin;
  toshiba.setSendPin(g_irTxPin);
#if !defined(IR_SEND_PIN)
  if (g_irTxPin < 0) {
    Serial.println(F("[IR-TX] TX pin není nastaven, odesílání zakázáno."));
    return;
  }
#endif
  // Inicializuj i globální instanci IrSender používanou pro přehrávání naučených kódů.
  // Bez explicitního begin() zůstane neaktivní a sendRaw()/sendNEC atd. nebudou nic vysílat.
  IrSender.begin(g_irTxPin);
  toshiba.begin();
  Serial.print(F("[IR-TX] Inicializován na pinu ")); Serial.println(g_irTxPin);
}

static void configureAuxPowerPins() {
  // Nejprve přepneme piny do známých stavů, abychom neovlivnili boot strapping.
  pinMode(POWER_GND_PIN, INPUT);
  pinMode(POWER_VCC_PIN, INPUT);

  digitalWrite(POWER_GND_PIN, LOW);
  pinMode(POWER_GND_PIN, OUTPUT);

  digitalWrite(POWER_VCC_PIN, HIGH);
  pinMode(POWER_VCC_PIN, OUTPUT);

  Serial.print(F("[POWER] GND pin ")); Serial.print(POWER_GND_PIN);
  Serial.print(F(", VCC pin ")); Serial.print(POWER_VCC_PIN);
  Serial.println(F(" inicializovány."));
}

// ======================== Sběr RAW – STUB (bezpečný) ========================
// Bezpečný STUB – tvoje verze IRremote neumí přímo surový buffer.
// Tímto jen resetneme případný předchozí RAW.
static void captureLastRawFromReceiver() {
  if (g_lastRawValid) {
    g_lastRawCaptureMs = millis();
    g_isrFrameReady = false;
    return;
  }

  noInterrupts();
  uint16_t count = g_isrCount;
  bool truncated = (count >= RAW_MAX_PULSES);
  if (count > RAW_MAX_PULSES) {
    count = RAW_MAX_PULSES;
  }
  for (uint16_t i = 0; i < count; ++i) {
    g_rawScratch[i] = g_isrPulses[i];
  }
  g_isrCount = 0;
  g_isrLastLevel = -1;
  interrupts();

  if (count > 0) {
    finalizeRawCapture(g_rawScratch, count, F("decoder"));
    if (truncated) {
      Serial.println(F("[RAW] Varování: zachycený rámec byl zkrácen na 512 pulsů."));
    }
  } else {
    g_lastRawValid = false;
    g_lastRaw.clear();
    g_lastRawKhz = 38;
    g_lastRawCaptureMs = millis();
    g_lastRawSource = F("(missing)");
    Serial.println(F("[RAW] Upozornění: pro poslední rámec není dostupný RAW záznam."));
  }

  g_isrFrameReady = false;
}

// Jednotné mapování labelu na IRremote enum.
// Použijeme už tvou "relaxed" verzi pro tolerantní match.
static decode_type_t parseProtoLabel(const String &s) {
  return parseProtoLabelRelaxed(s);
}

// Počet naučených položek pro WebUI (/learn_save připojuje RAW k poslední).
size_t getLearnedCount() {
  ensureLearnedCacheLoaded();
  return g_learnedCache.size();
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
                            const String &functionName, const String &remoteLabel,
                            const std::vector<uint16_t> *rawOpt,
                            uint8_t rawKhz);
extern bool fsDeleteLearned(size_t index);
extern bool isEffectivelyUnknownEvent(const IREvent &ev);
extern bool irSendByIndex(int16_t idx, uint8_t repeats);
extern bool irSendEvent(const IREvent &ev, uint8_t repeats);

// Wrapper pro WebUI: odeslání podle indexu
bool irSendByIndex(int16_t idx, uint8_t repeats) {
  const LearnedCode* e = getLearnedByIndex(idx);
  if (!e) return false;
  return irSendLearned(*e, repeats);
}

bool irSendEvent(const IREvent &ev, uint8_t repeats) {
  if (ev.learnedIndex >= 0) {
    return irSendLearnedByIndex(ev.learnedIndex, repeats);
  }

  LearnedCode tmp{};
  tmp.value = ev.value;
  tmp.bits  = ev.bits;
  tmp.addr  = ev.address;
  tmp.proto = String(protoName(ev.proto));

  std::vector<uint16_t> rawFromStorage;
  uint8_t rawFreq = 38;
  const std::vector<uint16_t>* rawPtr = nullptr;

  int16_t storedIdx = findLearnedIndex(ev.value, ev.bits, ev.address);
  if (storedIdx >= 0) {
    if (fsLoadRawForIndex(static_cast<size_t>(storedIdx), rawFromStorage, rawFreq) && !rawFromStorage.empty()) {
      rawPtr = &rawFromStorage;
    }
  }

  if (!rawPtr && g_lastRawValid) {
    uint32_t diff = (ev.ms > g_lastRawCaptureMs)
                      ? (ev.ms - g_lastRawCaptureMs)
                      : (g_lastRawCaptureMs - ev.ms);
    if (diff <= RAW_EVENT_MATCH_WINDOW_MS) {
      rawPtr = &g_lastRaw;
      rawFreq = g_lastRawKhz;
    }
  }

  return irSendLearnedCore(tmp, repeats, rawPtr, rawFreq);
}

String buildDiagnosticsJson() {
  String out; out.reserve(512);
  out += F("{\"raw\":{");
  out += F("\"valid\":"); out += g_lastRawValid ? "true" : "false";
  out += F(",\"source\":\""); out += jsonEscape(g_lastRawSource); out += F("\"");
  out += F(",\"age_ms\":");
  if (g_lastRawValid) {
    out += static_cast<uint32_t>(millis() - g_lastRawCaptureMs);
  } else {
    out += 0;
  }
  out += F(",\"len\":"); out += static_cast<uint32_t>(g_lastRaw.size());
  out += F(",\"freq\":"); out += static_cast<uint32_t>(g_lastRawKhz);

  out += F(",\"preview\":[");
  if (g_lastRawValid && !g_lastRaw.empty()) {
    size_t limit = std::min<size_t>(g_lastRaw.size(), 16);
    for (size_t i = 0; i < limit; ++i) {
      if (i) out += ',';
      out += static_cast<uint32_t>(g_lastRaw[i]);
    }
    out += F("]");
    out += F(",\"preview_truncated\":");
    out += (g_lastRaw.size() > limit) ? "true" : "false";
  } else {
    out += F("]");
    out += F(",\"preview_truncated\":false");
  }
  out += F(",\"decode_valid\":"); out += g_lastDecodeValid ? "true" : "false";
  out += F(",\"decode_age_ms\":");
  if (g_lastDecodeValid) {
    out += static_cast<uint32_t>(millis() - g_lastDecodeMs);
  } else {
    out += 0;
  }
  out += F(",\"decode_proto\":\""); out += jsonEscape(String(protoName(g_lastDecodeProto))); out += F("\"");
  out += F(",\"decode_bits\":"); out += static_cast<uint32_t>(g_lastDecodeBits);
  out += F(",\"decode_len\":"); out += g_lastDecodePulseCount;
  out += F(",\"decode_source\":\""); out += jsonEscape(g_lastDecodeSource); out += F("\"");
  out += F("},\"send\":{");
  out += F("\"valid\":"); out += g_lastSendValid ? "true" : "false";
  out += F(",\"ok\":"); out += g_lastSendOk ? "true" : "false";
  out += F(",\"age_ms\":");
  if (g_lastSendValid) {
    out += static_cast<uint32_t>(millis() - g_lastSendMs);
  } else {
    out += 0;
  }
  out += F(",\"method\":\""); out += jsonEscape(g_lastSendMethod); out += F("\"");
  out += F(",\"proto\":\""); out += jsonEscape(String(protoName(g_lastSendProto))); out += F("\"");
  out += F(",\"freq\":"); out += static_cast<uint32_t>(g_lastSendFreq);
  out += F(",\"pulses\":"); out += static_cast<uint32_t>(g_lastSendPulses);
  out += F("}}");
  return out;
}

String buildRawDumpJson() {
  if (!g_lastRawValid || g_lastRaw.empty()) {
    return F("{\"ok\":false,\"err\":\"no_raw\"}");
  }

  String out; out.reserve(256 + g_lastRaw.size() * 6);
  out += F("{\"ok\":true,\"freq\":"); out += static_cast<uint32_t>(g_lastRawKhz);
  out += F(",\"source\":\""); out += jsonEscape(g_lastRawSource); out += F("\",\"data\":[");
  for (size_t i = 0; i < g_lastRaw.size(); ++i) {
    if (i) out += ',';
    out += static_cast<uint32_t>(g_lastRaw[i]);
  }
  out += F("]}");
  return out;
}

#include "WebUI.h"  // používá výše deklarované symboly

// ======================== SETUP / LOOP ========================

void setup() {
  Serial.begin(115200);
  delay(200);

  Serial.println();
  Serial.println(F("=== IR Receiver (IRremote) – ESP32-C3 ==="));
  configureAuxPowerPins();
  Serial.print(F("IR RX pin: ")); Serial.println(IR_RX_PIN);

  if (!LittleFS.begin(true)) {
    Serial.println(F("[FS] LittleFS mount selhal (format=true), pokračuji bez learned databáze."));
  }

  prefs.begin("irrecv", false);
  g_showOnlyUnknown = prefs.getBool("only_unk", false);

  g_irTxPin = prefs.getInt("tx_pin", IR_TX_PIN_DEFAULT);
  initIrSender(g_irTxPin);

  wifiSetupWithWiFiManager();
  startWebServer();

  // IR přijímač
  IrReceiver.begin(IR_RX_PIN, DISABLE_LED_FEEDBACK);
  pinMode(IR_RX_PIN, INPUT_PULLUP);              // demodulátor většinou tahá do HIGH
  attachInterrupt(digitalPinToInterrupt(IR_RX_PIN), irEdgeISR, CHANGE);
  Serial.println(F("[RAW] Sniffer aktivní (GPIO CHANGE ISR)."));

  Serial.print(F("Protokoly povoleny: "));
  IrReceiver.printActiveIRProtocols(&Serial);
  Serial.println();
}

void loop() {
  serviceClient();
  rawSnifferService();

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
    captureLastRawFromReceiver();

    g_lastDecodeValid = true;
    g_lastDecodeMs = now;
    g_lastDecodeProto = d.protocol;
    g_lastDecodeBits = d.numberOfBits;
    if (g_lastRawValid && !g_lastRaw.empty()) {
      g_lastDecodePulseCount = g_lastRaw.size();
      g_lastDecodeSource = g_lastRawSource;
    } else {
      g_lastDecodePulseCount = 0;
      g_lastDecodeSource = F("decoder");
    }
  }

  lastMs   = now;
  lastProto= d.protocol;
  lastBits = d.numberOfBits;
  lastValue= d.decodedRawData;

  IrReceiver.resume();
  serviceClient();
}
