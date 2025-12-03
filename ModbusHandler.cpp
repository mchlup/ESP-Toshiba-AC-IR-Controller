#include "ModbusHandler.h"

static ModbusIP mb;
static ModbusConfig modbusCfg;
static AcStateChangedCallback stateChangedCb = nullptr;

// Adresy holding registrů
static const uint16_t HR_POWER = 0;   // 0=OFF,1=ON
static const uint16_t HR_MODE  = 1;   // 0=AUTO,1=COOL,2=DRY,3=HEAT,4=FAN
static const uint16_t HR_TEMP  = 2;   // 17–30
static const uint16_t HR_FAN   = 3;   // 0=AUTO,1=QUIET,2..6
static const uint16_t HR_SWING = 4;   // 0=FIX,1=V,2=H,3=HV
static const uint16_t HR_PURE  = 5;   // 0=OFF,1=ON
static const uint16_t HR_PSEL  = 6;   // 0=50%,1=75%,2=100%
// Stavový registr – jednoduchá zpětná vazba pro Loxone / SCADA
static const uint16_t HR_STATUS = 7;  // viz enum ModbusStatus

// poslední známé hodnoty
static uint16_t lastPower = 0;
static uint16_t lastMode  = 0;
static uint16_t lastTemp  = 24;
static uint16_t lastFan   = 0;
static uint16_t lastSwing = 0;
static uint16_t lastPure  = 0;
static uint16_t lastPsel  = 2;
static uint16_t lastStatus= MODBUS_STATUS_OK;

// Debounce pro hromadné zápisy – IR se odešle až po malé prodlevě
// od poslední změny registru.
static bool      pendingChange   = false;
static unsigned long lastChangeMs= 0;
// ~100–150 ms je obvykle víc než dost, aby Loxone / SCADA stihla
// odeslat všechny požadované zápisy.
static const uint16_t MODBUS_DEBOUNCE_MS = 120;

void ModbusHandler_init(const ModbusConfig &cfg, AcStateChangedCallback cb) {
  modbusCfg = cfg;
  stateChangedCb = cb;
}

void ModbusHandler_begin() {
  if (!modbusCfg.enabled) return;

  mb.server();  // = mb.server(MODBUSIP_PORT);

  // inicializace Hreg podle stavu
  lastPower = (gAcState.power == "on") ? 1 : 0;
  mb.addHreg(HR_POWER, lastPower);

  if      (gAcState.mode == "cool")     lastMode = 1;
  else if (gAcState.mode == "dry")      lastMode = 2;
  else if (gAcState.mode == "heat")     lastMode = 3;
  else if (gAcState.mode == "fan_only") lastMode = 4;
  else lastMode = 0;
  mb.addHreg(HR_MODE, lastMode);

  lastTemp = gAcState.temp;
  mb.addHreg(HR_TEMP, lastTemp);

  if      (gAcState.fan == "quiet") lastFan = 1;
  else if (gAcState.fan == "lvl_1") lastFan = 2;
  else if (gAcState.fan == "lvl_2") lastFan = 3;
  else if (gAcState.fan == "lvl_3") lastFan = 4;
  else if (gAcState.fan == "lvl_4") lastFan = 5;
  else if (gAcState.fan == "lvl_5") lastFan = 6;
  else lastFan = 0;
  mb.addHreg(HR_FAN, lastFan);

  if      (gAcState.swing == "v_swing")  lastSwing = 1;
  else if (gAcState.swing == "h_swing")  lastSwing = 2;
  else if (gAcState.swing == "hv_swing") lastSwing = 3;
  else lastSwing = 0;
  mb.addHreg(HR_SWING, lastSwing);

  lastPure = (gAcState.pure == "on") ? 1 : 0;
  mb.addHreg(HR_PURE, lastPure);

  if      (gAcState.powerSelect == "50%")  lastPsel = 0;
  else if (gAcState.powerSelect == "75%")  lastPsel = 1;
  else lastPsel = 2;
  mb.addHreg(HR_PSEL, lastPsel);

  // status na začátku = OK
  lastStatus = MODBUS_STATUS_OK;
  mb.addHreg(HR_STATUS, lastStatus);
}

const ModbusConfig &ModbusHandler_getConfig() {
  return modbusCfg;
}

void ModbusHandler_setConfig(const ModbusConfig &cfg) {
  modbusCfg = cfg;
}

void ModbusHandler_setStatus(uint16_t status) {
  lastStatus = status;
  if (!modbusCfg.enabled) return;
  mb.Hreg(HR_STATUS, lastStatus);
}

uint16_t ModbusHandler_getStatus() {
  return lastStatus;
}

void ModbusHandler_loop() {
  if (!modbusCfg.enabled) return;

  mb.task();

  bool changed = false;
  uint16_t v;

  v = mb.Hreg(HR_POWER);
  if (v != lastPower) {
    lastPower = v;
    gAcState.power = (v ? "on" : "off");
    changed = true;
  }

  v = mb.Hreg(HR_MODE);
  if (v != lastMode) {
    lastMode = v;
    switch (v) {
      case 1: gAcState.mode = "cool";     break;
      case 2: gAcState.mode = "dry";      break;
      case 3: gAcState.mode = "heat";     break;
      case 4: gAcState.mode = "fan_only"; break;
      default:gAcState.mode = "auto";     break;
    }
    changed = true;
  }

  v = mb.Hreg(HR_TEMP);
  if (v != lastTemp) {
    if (v < 17) v = 17;
    if (v > 30) v = 30;
    lastTemp = v;
    gAcState.temp = (uint8_t)lastTemp;
    // zapiš zpět do Modbus registru oříznutou hodnotu,
    // aby Loxone/SCADA viděla skutečnou efektivní teplotu
    mb.Hreg(HR_TEMP, lastTemp);
    changed = true;
  }

  v = mb.Hreg(HR_FAN);
  if (v != lastFan) {
    lastFan = v;
    switch (v) {
      case 1: gAcState.fan = "quiet"; break;
      case 2: gAcState.fan = "lvl_1"; break;
      case 3: gAcState.fan = "lvl_2"; break;
      case 4: gAcState.fan = "lvl_3"; break;
      case 5: gAcState.fan = "lvl_4"; break;
      case 6: gAcState.fan = "lvl_5"; break;
      default:gAcState.fan = "auto";  break;
    }
    changed = true;
  }

  v = mb.Hreg(HR_SWING);
  if (v != lastSwing) {
    lastSwing = v;
    switch (v) {
      case 1: gAcState.swing = "v_swing";  break;
      case 2: gAcState.swing = "h_swing";  break;
      case 3: gAcState.swing = "hv_swing"; break;
      default:gAcState.swing = "fix";      break;
    }
    changed = true;
  }

  v = mb.Hreg(HR_PURE);
  if (v != lastPure) {
    lastPure = v;
    gAcState.pure = (v ? "on" : "off");
    changed = true;
  }

  v = mb.Hreg(HR_PSEL);
  if (v != lastPsel) {
    lastPsel = v;
    switch (v) {
      case 0: gAcState.powerSelect = "50%";  break;
      case 1: gAcState.powerSelect = "75%";  break;
      default:gAcState.powerSelect = "100%"; break;
    }
    changed = true;
  }

  // Pokud došlo k jedné nebo více změnám registrů, pouze si to
  // zapamatujeme a počkáme malou dobu, než skutečně odpálíme callback.
  if (changed) {
    pendingChange = true;
    lastChangeMs = millis();
  }

  if (pendingChange && stateChangedCb) {
    unsigned long now = millis();
    if ((uint32_t)(now - lastChangeMs) >= MODBUS_DEBOUNCE_MS) {
      pendingChange = false;
      stateChangedCb();               // → main: IR + MQTT state publish
      // po úspěšném callbacku nastavíme základní status OK
      ModbusHandler_setStatus(MODBUS_STATUS_OK);
    }
  }
}

void ModbusHandler_updateRegsFromState() {
  if (!modbusCfg.enabled) return;

  lastPower = (gAcState.power == "on") ? 1 : 0;
  mb.Hreg(HR_POWER, lastPower);

  if      (gAcState.mode == "cool")     lastMode = 1;
  else if (gAcState.mode == "dry")      lastMode = 2;
  else if (gAcState.mode == "heat")     lastMode = 3;
  else if (gAcState.mode == "fan_only") lastMode = 4;
  else lastMode = 0;
  mb.Hreg(HR_MODE, lastMode);

  lastTemp = gAcState.temp;
  mb.Hreg(HR_TEMP, lastTemp);

  if      (gAcState.fan == "quiet") lastFan = 1;
  else if (gAcState.fan == "lvl_1") lastFan = 2;
  else if (gAcState.fan == "lvl_2") lastFan = 3;
  else if (gAcState.fan == "lvl_3") lastFan = 4;
  else if (gAcState.fan == "lvl_4") lastFan = 5;
  else if (gAcState.fan == "lvl_5") lastFan = 6;
  else lastFan = 0;
  mb.Hreg(HR_FAN, lastFan);

  if      (gAcState.swing == "v_swing")  lastSwing = 1;
  else if (gAcState.swing == "h_swing")  lastSwing = 2;
  else if (gAcState.swing == "hv_swing") lastSwing = 3;
  else lastSwing = 0;
  mb.Hreg(HR_SWING, lastSwing);

  lastPure = (gAcState.pure == "on") ? 1 : 0;
  mb.Hreg(HR_PURE, lastPure);

  if      (gAcState.powerSelect == "50%")  lastPsel = 0;
  else if (gAcState.powerSelect == "75%")  lastPsel = 1;
  else lastPsel = 2;
  mb.Hreg(HR_PSEL, lastPsel);
}
