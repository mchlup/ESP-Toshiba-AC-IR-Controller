#include "AcState.h"

AcState gAcState;

void AcState_initDefaults() {
  gAcState.power       = "off";
  gAcState.temp        = 22;
  gAcState.mode        = "auto";
  gAcState.fan         = "auto";
  gAcState.swing       = "fix";
  gAcState.pure        = "off";
  // Výchozí stav bez HI-POWER: normální výkon = 75 %
  gAcState.powerSelect = "75%";
  gAcState.operation   = "normal";
  gAcState.wifiLed     = "off";
  AcState_normalize();
}

void AcState_normalize() {
  // Teplota – základní ochrana proti nesmyslům.
  if (gAcState.temp < 17) gAcState.temp = 17;
  if (gAcState.temp > 30) gAcState.temp = 30;

  gAcState.power.toLowerCase();
  if (gAcState.power != "on" && gAcState.power != "off") {
    gAcState.power = "off";
  }

  gAcState.mode.toLowerCase();
  if (gAcState.mode != "auto" &&
      gAcState.mode != "cool" &&
      gAcState.mode != "dry"  &&
      gAcState.mode != "heat" &&
      gAcState.mode != "fan_only") {
    gAcState.mode = "auto";
  }

  gAcState.fan.toLowerCase();
  if (gAcState.fan != "auto"  &&
      gAcState.fan != "quiet" &&
      gAcState.fan != "lvl_1" &&
      gAcState.fan != "lvl_2" &&
      gAcState.fan != "lvl_3" &&
      gAcState.fan != "lvl_4" &&
      gAcState.fan != "lvl_5") {
    gAcState.fan = "auto";
  }

  gAcState.swing.toLowerCase();
  if (gAcState.swing != "fix"      &&
      gAcState.swing != "v_swing"  &&
      gAcState.swing != "h_swing"  &&
      gAcState.swing != "hv_swing") {
    gAcState.swing = "fix";
  }

  gAcState.pure.toLowerCase();
  if (gAcState.pure != "on" && gAcState.pure != "off") {
    gAcState.pure = "off";
  }

  if (gAcState.powerSelect != "50%" &&
      gAcState.powerSelect != "75%" &&
      gAcState.powerSelect != "100%") {
    gAcState.powerSelect = "75%";
  }

  gAcState.operation.toLowerCase();
  if (gAcState.operation != "normal" &&
      gAcState.operation != "high_power") {
    gAcState.operation = "normal";
  }

  gAcState.wifiLed.toLowerCase();
  if (gAcState.wifiLed != "on" && gAcState.wifiLed != "off") {
    gAcState.wifiLed = "off";
  }
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
