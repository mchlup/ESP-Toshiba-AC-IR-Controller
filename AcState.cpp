#include "AcState.h"

AcState gAcState;

void AcState_initDefaults() {
  gAcState.power       = "off";
  gAcState.temp        = 24;
  gAcState.mode        = "auto";
  gAcState.fan         = "auto";
  gAcState.swing       = "fix";
  gAcState.pure        = "off";
  // Výchozí stav bez HI-POWER: normální výkon = 75 %
  gAcState.powerSelect = "75%";
  gAcState.operation   = "normal";
  gAcState.wifiLed     = "off";
}

void AcState_toHvacSettings(hvacSettings &s) {
  s.state        = gAcState.power.c_str();
  s.setpoint     = gAcState.temp;
  s.mode         = gAcState.mode.c_str();
  s.swing        = gAcState.swing.c_str();
  s.fanMode      = gAcState.fan.c_str();
  s.pure         = gAcState.pure.c_str();
  s.powerSelect  = gAcState.powerSelect.c_str();
  s.operation    = gAcState.operation.c_str();
  s.wifiLed      = gAcState.wifiLed.c_str();
}
