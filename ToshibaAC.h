#pragma once
#include <Arduino.h>
#include <IRremote.hpp>

// ====== Přehled ======
// Odesílá IR kódy pro Toshiba AC ve formátu 9 bajtů (72 bitů), odeslané 2×.
// Rámec: F2 0D 03 FC 01 [TEMP+PWR] [FAN+MODE] 00 [CHK]
// CHK = XOR předchozích 8 bajtů.
// Teplota 17–30 °C => vyšší nibble v bajtu[5] (0..13) + 0x00/0x20 pro ON/OFF.
// Režimy (MODE nibble): AUTO=0x0, COOL=0x1, DRY=0x2, HEAT=0x3.
// Ventilátor (FAN nibble): AUTO=0x0, 1=0x4, 2=0x6, 3=0x8, 4=0xA, 5=0xC.

class ToshibaACIR {
public:
  enum class Mode : uint8_t { AUTO=0x0, COOL=0x1, DRY=0x2, HEAT=0x3 };
  enum class Fan  : uint8_t { AUTO=0x0, F1=0x4, F2=0x6, F3=0x8, F4=0xA, F5=0xC };

  struct State {
    bool     powerOn   = true;
    Mode     mode      = Mode::AUTO;
    Fan      fan       = Fan::AUTO;
    uint8_t  tempC     = 23;   // 17..30
  };

  // Timing (Samsung-like)
  static constexpr uint16_t kCarrierKhz      = 38;
  static constexpr uint16_t HDR_MARK_US      = 4500;
  static constexpr uint16_t HDR_SPACE_US     = 4500;
  static constexpr uint16_t BIT_MARK_US      = 560;
  static constexpr uint16_t ONE_SPACE_US     = 1600;
  static constexpr uint16_t ZERO_SPACE_US    = 560;
  static constexpr uint16_t FRAME_GAP_US     = 5000; // mezera mezi 2×72b (bez nosné)

  explicit ToshibaACIR(uint8_t irSendPin) : _pin(irSendPin) {}

  void begin() {
#if defined(IR_SEND_PIN)
    // Pokud je IRremote zkonfigurován přes IR_SEND_PIN, nastaví se sám.
#else
    IrSender.begin(_pin, ENABLE_LED_FEEDBACK);
#endif
  }

  // Vytvoří a odešle příkaz podle stavu
  bool send(const State &s) {
    uint8_t frame[9];
    buildFrame(s, frame);
    return sendFrameTwice(frame);
  }

  // Utilita: sestavení rámce do bufferu (9 bajtů)
  static void buildFrame(const State &s, uint8_t out[9]) {
    out[0] = 0xF2;
    out[1] = 0x0D;
    out[2] = 0x03;
    out[3] = 0xFC;
    out[4] = 0x01;

    // Byte 5: TEMP (hi nibble) + POWER (lo nibble) — 00=ON, 02=OFF
    uint8_t t = constrain(s.tempC, (uint8_t)17, (uint8_t)30);
    uint8_t tempNibble = (t - 17) & 0x0F;
    uint8_t pwrNibble  = s.powerOn ? 0x00 : 0x02;
    out[5] = (uint8_t)((tempNibble << 4) | pwrNibble);

    // Byte 6: FAN (hi nibble) + MODE (lo nibble)
    uint8_t fanNib  = static_cast<uint8_t>(s.fan)  & 0x0F;
    uint8_t modeNib = static_cast<uint8_t>(s.mode) & 0x0F;
    out[6] = (uint8_t)((fanNib << 4) | modeNib);

    // Byte 7: 0x00 (v 144bit rámečku nevyužito)
    out[7] = 0x00;

    // Byte 8: XOR checksum of bytes [0..7]
    uint8_t x = 0;
    for (int i = 0; i < 8; ++i) x ^= out[i];
    out[8] = x;
  }

private:
  uint8_t _pin;

  // Sestaví RAW pulzy pro 72b a pošle je 2×
  bool sendFrameTwice(const uint8_t frame[9]) {
    // 72 bitů + úvodní hlavička + koncový MARK
    // Každý bit = MARK + (SPACE_0/1). Pulzů bude ~ (1 + 72)*2 + hlavičky atd.
    constexpr size_t MAX_PULSES = 2 * (1 + 72) * 2 + 20;
    static uint16_t raw[MAX_PULSES];
    size_t n = 0;

    auto emitHeader = [&]() {
      raw[n++] = HDR_MARK_US;
      raw[n++] = HDR_SPACE_US;
    };
    auto emitBit = [&](bool one) {
      raw[n++] = BIT_MARK_US;
      raw[n++] = one ? ONE_SPACE_US : ZERO_SPACE_US;
    };
    auto emitTrailMark = [&]() {
      raw[n++] = BIT_MARK_US; // závěrečný mark (běžné u NEC/Samsung stylu)
    };

    // jeden 72b rámec
    auto encode72 = [&](const uint8_t *b) {
      emitHeader();
      for (int i = 0; i < 9; ++i) {
        uint8_t val = b[i];
        // MSB-first (podle publikovaných analyzovaných rámců)
        for (int bit = 7; bit >= 0; --bit) {
          emitBit((val >> bit) & 0x01);
        }
      }
      emitTrailMark();
    };

    // 1. průchod
    encode72(frame);
    // vlož „ticho“ (mezera) mezi duplikovanými rámci
    raw[n++] = 0;               // IRremote očekává páry (MARK, SPACE).
    raw[n++] = FRAME_GAP_US;    // mezera bez nosné

    // 2. průchod
    encode72(frame);

    // Odeslat RAW (páry: MARK/SPACE; 38 kHz)
    IrSender.sendRaw(raw, (uint_fast8_t)n, kCarrierKhz);
    return true;
  }
};
