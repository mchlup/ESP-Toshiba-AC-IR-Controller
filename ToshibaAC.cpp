#include <Arduino.h>

enum decode_type_t : uint16_t;

// Volitelná diagnostika – je-li definována v projektu, použije se.
// (weak symbol, aby kompilace prošla i bez implementace v .ino / WebUI)
__attribute__((weak))
void recordIrTxDiagnostics(bool /*ok*/, decode_type_t /*protoCode*/, size_t /*pulses*/,
                           uint8_t /*freqKhz*/,
                           const __FlashStringHelper* /*methodLabel*/) {
  // default: no-op
}
