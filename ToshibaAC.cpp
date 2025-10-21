// ToshibaAC.cpp — pouze diagnostická weak funkce + vypnutí receiver části IRremote v této jednotce

// Důležité: tato TU NEMÁ kompilovat IR receiver (ten je v .ino s IR_GLOBAL),
// jinak dojde k duplicitě timerEnableReceiveInterrupt() na ESP32-C3 (IRremote 3.3.2).
#define DISABLE_CODE_FOR_RECEIVER

#include <Arduino.h>
#include <IRremote.hpp>

// Volitelná diagnostika – je-li definována v projektu, použije se.
// (weak symbol, aby kompilace prošla i bez implementace v .ino / WebUI)
__attribute__((weak))
void recordIrTxDiagnostics(bool /*ok*/, decode_type_t /*protoCode*/, size_t /*pulses*/,
                           uint8_t /*freqKhz*/,
                           const __FlashStringHelper* /*methodLabel*/) {
  // default: no-op
}
