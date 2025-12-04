#include "Logging.h"
#include <stdarg.h>

namespace Logging {

static Level currentLevel = LEVEL_INFO;

static const uint8_t LOG_BUFFER_SIZE = 32;
static const uint8_t LOG_LINE_MAX    = 120;

struct LogEntry {
  Level level;
  char  text[LOG_LINE_MAX];
};

static LogEntry buffer[LOG_BUFFER_SIZE];
static uint8_t  logCount   = 0;
static uint8_t  startIndex = 0;

const char* levelToString(Level level) {
  switch (level) {
    case LEVEL_ERROR:   return "ERROR";
    case LEVEL_WARN:    return "WARN";
    case LEVEL_INFO:    return "INFO";
    case LEVEL_DEBUG:   return "DEBUG";
    case LEVEL_VERBOSE: return "VERBOSE";
    default:            return "?";
  }
}

void begin(unsigned long baud, Level level) {
  Serial.begin(baud);
  currentLevel = level;
  logf(LEVEL_INFO, "LOG", "Logging started at level %s", levelToString(level));
}

void setLevel(Level level) {
  currentLevel = level;
  logf(LEVEL_INFO, "LOG", "Log level set to %s", levelToString(level));
}

Level getLevel() {
  return currentLevel;
}

bool isEnabled(Level level) {
  // ERROR(0)..INFO(2)..DEBUG(3)..VERBOSE(4)
  return level <= currentLevel;
}

static void addToBuffer(Level level, const char *line) {
  uint8_t idx;
  if (logCount < LOG_BUFFER_SIZE) {
    idx = (startIndex + logCount) % LOG_BUFFER_SIZE;
    logCount++;
  } else {
    idx = startIndex;
    startIndex = (startIndex + 1) % LOG_BUFFER_SIZE;
  }

  buffer[idx].level = level;
  strncpy(buffer[idx].text, line, LOG_LINE_MAX - 1);
  buffer[idx].text[LOG_LINE_MAX - 1] = '\0';
}

void logf(Level level, const char *tag, const char *fmt, ...) {
  if (!isEnabled(level)) return;

  char msg[LOG_LINE_MAX];
  va_list args;
  va_start(args, fmt);
  vsnprintf(msg, sizeof(msg), fmt, args);
  va_end(args);

  const char *lvlStr = levelToString(level);
  if (!tag) tag = "";

  // Výpis na Serial (živý debug)
  Serial.printf("[%s][%s] %s\n", lvlStr, tag, msg);

  // Uložení do kruhového bufferu pro WebUI
  char line[LOG_LINE_MAX];
  snprintf(line, sizeof(line), "[%s][%s] %s", lvlStr, tag, msg);
  addToBuffer(level, line);
}

uint8_t getLogCount() {
  return logCount;
}

bool getLogLine(uint8_t index, Level &levelOut, char *bufOut, size_t bufSize) {
  if (index >= logCount || !bufOut || bufSize == 0) return false;
  uint8_t idx = (startIndex + index) % LOG_BUFFER_SIZE;
  levelOut = buffer[idx].level;
  strncpy(bufOut, buffer[idx].text, bufSize - 1);
  bufOut[bufSize - 1] = '\0';
  return true;
}

} // namespace Logging
