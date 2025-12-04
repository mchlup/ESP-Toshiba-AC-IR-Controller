#ifndef LOGGING_H
#define LOGGING_H

#include <Arduino.h>

namespace Logging {

enum Level : uint8_t {
  LEVEL_ERROR   = 0,
  LEVEL_WARN    = 1,
  LEVEL_INFO    = 2,
  LEVEL_DEBUG   = 3,
  LEVEL_VERBOSE = 4
};

// Inicializace logování (volá Serial.begin)
void begin(unsigned long baud = 115200, Level level = LEVEL_INFO);

// Nastavení / načtení log levelu
void setLevel(Level level);
Level getLevel();
bool isEnabled(Level level);

// printf-like logování: tag = např. "MAIN","WEB","IR",...
void logf(Level level, const char *tag, const char *fmt, ...);

// Převod levelu na text ("INFO", ...)
const char* levelToString(Level level);

// In‑memory log buffer pro WebUI
uint8_t getLogCount();
// index 0 = nejstarší; vrací false, pokud index mimo rozsah
bool getLogLine(uint8_t index, Level &levelOut, char *buf, size_t bufSize);

} // namespace Logging

#endif // LOGGING_H
