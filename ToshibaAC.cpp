#include "ToshibaAC.h"
#include <IRremote.hpp>   // pouze v .cpp, abychom předešli multiple definition

// Volitelná diagnostika – je-li definována v projektu, použije se.
// (weak symbol, aby kompilace prošla i bez implementace v .ino / WebUI)
__attribute__((weak))
void recordIrTxDiagnostics(bool /*ok*/, int /*protoCode*/, size_t /*pulses*/,
                           uint8_t /*freqKhz*/,
                           const __FlashStringHelper* /*methodLabel*/) {
  // default: no-op
}

// Přemapování forward-declare na skutečný typ z IRremote.hpp
struct ToshibaACIR::IRsend : ::IRsend {};

// === Implementace ===

void ToshibaACIR::begin() {
  if (_pin < 0) {
    recordIrTxDiagnostics(false, -1, 0, kCarrierKhz, F("toshiba-ac:pin-not-set"));
    return;
  }
  if (_ir == nullptr) {
    _ir = new IRsend(); // alokujeme jednou na celý běh
  }
  _ir->begin(_pin, ENABLE_LED_FEEDBACK);
}

bool ToshibaACIR::send(const State &s) {
  uint8_t frame[kFrameBytes];
  buildFrame(s, frame);
  return sendFrameTwice(frame);
}

bool ToshibaACIR::sendFrameTwice(const uint8_t frame[kFrameBytes]) {
  if (_pin < 0 || _ir == nullptr) {
    recordIrTxDiagnostics(false, -1, 0, kCarrierKhz, F("toshiba-ac:not-initialized"));
    return false;
  }

  static uint16_t raw[kRawBufferLen];
  size_t n = 0;

  auto emitHeader = [&](void) {
    raw[n++] = HDR_MARK_US;
    raw[n++] = HDR_SPACE_US;
  };
  auto emitBit = [&](bool one) {
    raw[n++] = BIT_MARK_US;
    raw[n++] = one ? ONE_SPACE_US : ZERO_SPACE_US;
  };
  auto emitTrailMark = [&](void) {
    raw[n++] = BIT_MARK_US; // závěrečný mark (běžné u NEC/Samsung stylu)
  };

  auto encode72 = [&](const uint8_t *b) {
    emitHeader();
    for (size_t i = 0; i < kFrameBytes; ++i) {
      uint8_t val = b[i];
      // MSB-first
      for (int bit = 7; bit >= 0; --bit) {
        emitBit((val >> bit) & 0x01);
      }
    }
    emitTrailMark();
  };

  encode72(frame);
  raw[n++] = FRAME_GAP_US;    // mezera bez nosné (SPACE)
  encode72(frame);

  if (n > kRawBufferLen) {
    recordIrTxDiagnostics(false, -1, n, kCarrierKhz, F("toshiba-ac:overflow"));
    return false;
  }

  _ir->sendRaw(raw, static_cast<uint16_t>(n), kCarrierKhz);
  recordIrTxDiagnostics(true, -1, n, kCarrierKhz, F("toshiba-ac"));
  return true;
}
