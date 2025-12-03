#ifndef AC_STATE_H
#define AC_STATE_H

#include <Arduino.h>
#include "ToshibaCarrierHvac.h"

// Jednoduchá reprezentace posledního "logického" stavu AC.
// Z toho se generuje IR pattern, MQTT, Modbus registry i WebUI.
struct AcState {
  String power;        // "on"/"off"
  uint8_t temp;        // 17–30 °C
  String mode;         // "auto","cool","dry","heat","fan_only"
  String fan;          // "auto","quiet","lvl_1".."lvl_5"
  String swing;        // "fix","v_swing","h_swing","hv_swing"
  String pure;         // "on"/"off"
  String powerSelect;  // "50%","75%","100%"
  String operation;    // "normal","high_power",...
  String wifiLed;      // "on"/"off"
};

extern AcState gAcState;

// Inicializace výchozích hodnot.
void AcState_initDefaults();

// Normalizace/validace gAcState – ořezání rozsahu teploty,
// doplnění výchozích hodnot, kontrola povolených stringů.
// Můžeš ji volat po změnách z WebUI / MQTT / Modbusu.
void AcState_normalize();

// Zapsání gAcState do hvacSettings (pro funkce, co chtějí hvacSettings).
void AcState_toHvacSettings(hvacSettings &s);

#endif // AC_STATE_H
