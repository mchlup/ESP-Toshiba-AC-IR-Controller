#ifndef IR_CONTROL_H
#define IR_CONTROL_H

#include <Arduino.h>
#include <IRremoteESP8266.h>
#include <ir_Toshiba.h>
#include <IRac.h>

#include "AcState.h"

// Inicializace IR vysílače Toshiba na daném pinu.
void IRControl_begin(uint16_t irPin);

// Odešle IR pattern podle aktuálního gAcState.
void IRControl_sendFromState();

// Pomocné mapovací funkce (pokud je potřebuješ i jinde).
stdAc::opmode_t IRControl_modeToStdAc(const String &m);
stdAc::fanspeed_t IRControl_fanToStdAc(const String &f);
uint8_t IRControl_swingToNative(const String &s);

#endif // IR_CONTROL_H
