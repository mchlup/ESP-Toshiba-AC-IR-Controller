// ToshibaAC.cpp — pouze diagnostická weak funkce + vypnutí receiver části IRremote v této jednotce

// Důležité: tato TU NEMÁ kompilovat IR receiver (ten je v .ino s IR_GLOBAL),
// jinak dojde k duplicitě timerEnableReceiveInterrupt() na ESP32-C3 (IRremote 3.3.2).
#define DISABLE_CODE_FOR_RECEIVER

#include <Arduino.h>

// Vyhneme se přímému includu <IRremote.hpp>, aby se definice globálních symbolů
// IRremote (timerEnableReceiveInterrupt, IrSender, ...) negenerovaly i v této
// překladové jednotce. Stačí nám pouze dopředná deklarace typů použitých v
// signatuře weak funkce níže.
enum decode_type_t : uint16_t;
class __FlashStringHelper;

// Volitelná diagnostika – je-li definována v projektu, použije se.
// (weak symbol, aby kompilace prošla i bez implementace v .ino / WebUI)
__attribute__((weak))
void recordIrTxDiagnostics(bool /*ok*/, decode_type_t /*protoCode*/, size_t /*pulses*/,
                           uint8_t /*freqKhz*/,
                           const __FlashStringHelper* /*methodLabel*/) {
  // default: no-op
}
