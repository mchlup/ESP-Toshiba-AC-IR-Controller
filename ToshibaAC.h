#pragma once
#include <Arduino.h>

// ====== Přehled ======
// Odesílá IR kódy pro Toshiba AC ve formátu 9 bajtů (72 bitů), odeslané 2×.
// Rámec: F2 0D 03 FC 01 [TEMP+PWR] [FAN+MODE] 00 [CHK]
// CHK = XOR předchozích 8 bajtů.
// Teplota 17–30 °C => vyšší nibble v byte[5] (0..13) + 0x00/0x20 pro ON/OFF.
// Režimy (MODE nibble): AUTO=0x0, COOL=0x1, DRY=0x2, HEAT=0x3.
// Ventilátor (FAN nibble): AUTO=0x0, 1=0x4, 2=0x6, 3=0x8, 4=0xA, 5=0xC.
// (viz původní poznámky v projektu) 

class ToshibaACIR {
public:
  enum class Mode : uint8_t { AUTO=0x0, COOL=0x1, DRY=0x2, HEAT=0x3 };
  enum class Fan  : uint8_t { AUTO=0x0, F1=0x4, F2=0x6, F3=0x8, F4=0xA, F5=0xC };

  struct State {
    bool     powerOn   = true;
    Mode     mode      = Mode::AUTO;
    Fan      fan       = Fan::AUTO;
    uint8_t  tempC     = 24;   // 17..30
  };

  // Toshiba AC rámec
  static constexpr size_t   kFrameBytes       = 9;
  static constexpr uint16_t kBitsPerFrame     = 72;
  static_assert(kBitsPerFrame == kFrameBytes * 8, "Toshiba AC rámec musí mít 72 bitů");

  // Timings (NEC/Samsung-like; vyhovují stávající implementaci)
  static constexpr uint8_t  kCarrierKhz      = 38;
  static constexpr uint16_t HDR_MARK_US      = 4500;
  static constexpr uint16_t HDR_SPACE_US     = 4500;
  static constexpr uint16_t BIT_MARK_US      = 560;
  static constexpr uint16_t ONE_SPACE_US     = 1600;
  static constexpr uint16_t ZERO_SPACE_US    = 560;
  static constexpr uint16_t FRAME_GAP_US     = 5000; // mezera mezi 2×72b (bez nosné)

  // Odvozené počty pulsů pro sendRaw()
  static constexpr size_t   kFramePulseCount = 2 + (kBitsPerFrame * 2) + 1; // header + bity + trailing mark
  static constexpr size_t   kTotalPulseCount = (kFramePulseCount * 2) + 1;  // 2 rámce + gap
  static constexpr size_t   kRawBufferLen    = kTotalPulseCount + 10;       // rezerva

  explicit ToshibaACIR(int8_t irSendPin = -1) : _pin(irSendPin) {}

  void    setSendPin(int8_t irSendPin) { _pin = irSendPin; }
  int8_t  sendPin() const { return _pin; }

  // Inicializace IR odesílače (volat po nastavení TX pinu)
  void begin();

  // Vytvoří a odešle příkaz podle stavu
  bool send(const State &s);

  // Utilita: sestavení rámce do bufferu (9 bajtů)
  static void buildFrame(const State &s, uint8_t out[kFrameBytes]) {
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

    // Byte 7: 0x00 (nepoužito v této krátké variantě rámce)
    out[7] = 0x00;

    // Byte 8: XOR checksum of bytes [0..7]
    uint8_t x = 0;
    for (int i = 0; i < 8; ++i) x ^= out[i];
    out[8] = x;
  }

private:
  int8_t   _pin;
  struct IRsend;         // forward-declare z IRremote.hpp (nezatahujeme hlavičku do .h)
  IRsend*  _ir = nullptr;

  // Sestaví RAW pulzy pro 72b a pošle je 2×
  bool sendFrameTwice(const uint8_t frame[kFrameBytes]);
};
