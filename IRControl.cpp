#include "IRControl.h"
#include "ToshibaIrGenerator.h"

// Globální instance Toshiba IR senderu (z IRremoteESP8266)
static IRToshibaAC irToshiba(0);   // pin nastavíme v IRControl_begin

void IRControl_begin(uint16_t irPin) {
  // Přenastavíme pin v instanci (konstruktor už proběhl)
  irToshiba = IRToshibaAC(irPin);
  irToshiba.begin();
}

// Mapování z našich stringů na stdAc opmode
stdAc::opmode_t IRControl_modeToStdAc(const String &m) {
  if (m == "cool")     return stdAc::opmode_t::kCool;
  if (m == "heat")     return stdAc::opmode_t::kHeat;
  if (m == "dry")      return stdAc::opmode_t::kDry;
  if (m == "fan_only") return stdAc::opmode_t::kFan;
  return stdAc::opmode_t::kAuto;
}

// Mapování z našich stringů na stdAc fanspeed
stdAc::fanspeed_t IRControl_fanToStdAc(const String &f) {
  if (f == "quiet") return stdAc::fanspeed_t::kMin;
  if (f == "lvl_1") return stdAc::fanspeed_t::kLow;
  if (f == "lvl_2") return stdAc::fanspeed_t::kMedium;
  if (f == "lvl_3") return stdAc::fanspeed_t::kHigh;
  if (f == "lvl_4") return stdAc::fanspeed_t::kMax;
  if (f == "lvl_5") return stdAc::fanspeed_t::kMax;
  return stdAc::fanspeed_t::kAuto;
}

// Swing – hrubé mapování na nativní 3bit "swing" kód
uint8_t IRControl_swingToNative(const String &s) {
  // 0 = step/fix, 1 = swing ON, 2 = swing OFF, 4 = toggle
  if (s == "auto") {
    return 1;   // swing ON (auto)
  }
  if (s.indexOf("swing") >= 0) {
    return 1;   // swing ON
  }
  return 2;     // swing OFF (fixní poloha)
}

void IRControl_sendFromState() {
  Serial.println(F("[IR] Building Toshiba IR state from gAcState..."));
  Serial.print(F("[IR] gAcState.power = "));
  Serial.println(gAcState.power);

  // Před generováním rámce pro jistotu srovnáme gAcState
  // (rozsahy teplot, povolené hodnoty).
  AcState_normalize();

  hvacSettings hvac;
  AcState_toHvacSettings(hvac);

  // Jednotný generátor pro ON i OFF:
  // - power ON/OFF se vyhodnotí uvnitř ToshibaIrGeneratoru
  //   z hvacSettings.state / operation.
  ToshibaIR::Frame72 frame = ToshibaIR::buildFromHvacSettings(hvac);

  Serial.print(F("[IR] Generated Toshiba 72-bit frame:"));
  for (uint8_t i = 0; i < 9; i++) {
    Serial.print(' ');
    if (frame.data[i] < 0x10) Serial.print('0');
    Serial.print(frame.data[i], HEX);
  }
  Serial.println();

  // Nahraj rámec do vnitřního bufferu IRToshibaAC a použij ho jen jako "sender".
  irToshiba.stateReset();
  uint8_t *state = irToshiba.getRaw();
  uint16_t stateLen = irToshiba.getStateLength();
  if (stateLen < 9) stateLen = 9; // bezpečnost

  uint16_t copyLen = 9;
  if (copyLen > stateLen) copyLen = stateLen;

  for (uint16_t i = 0; i < copyLen; i++) {
    state[i] = frame.data[i];
  }

  Serial.print(F("[IR] Toshiba state length (bytes): "));
  Serial.println(stateLen);
  Serial.print(F("[IR] Raw state to send:"));
  for (uint16_t i = 0; i < copyLen; i++) {
    Serial.print(' ');
    if (state[i] < 0x10) Serial.print('0');
    Serial.print(state[i], HEX);
  }
  Serial.println();

  // Skutečné vyslání IR – interní opakování řeší knihovna.
  irToshiba.send();
  Serial.println(F("[IR] Sent Toshiba frame via IRToshibaAC (repeat=2)."));
}
