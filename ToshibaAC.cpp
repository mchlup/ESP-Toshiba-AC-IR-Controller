#include "ToshibaAC.h"

// Globální instance deklarovaná v hlavním .ino souboru.
extern ToshibaACIR toshiba;

// implementace je kompletně v hlavičce (header-only) kvůli optimalizaci a jednoduchosti
void sendHeat23Fan3() {
  ToshibaACIR::State s;
  s.powerOn = true;
  s.mode    = ToshibaACIR::Mode::HEAT;
  s.fan     = ToshibaACIR::Fan::F3;
  s.tempC   = 23;
  toshiba.send(s);
}
