#include <Arduino.h>

#if defined(ESP32) && defined(ARDUINO_ARCH_ESP32)
#if defined(ESP_ARDUINO_VERSION_MAJOR) && (ESP_ARDUINO_VERSION_MAJOR >= 3)
#include <esp32-hal-timer.h>

// Compatibility wrappers for the old pre-IDF5 timer API used by IRremoteESP8266.
// The ESP32 Arduino core 3.x redesigned the timer API to be frequency based.
// Provide the legacy signatures so the library can link without modification.
extern "C" {

hw_timer_t *timerBegin(uint8_t timer_num, uint16_t divider, bool countUp) {
  (void)timer_num;  // Legacy API accepted timer number but it is unused now.
  if (divider == 0) {
    divider = 80;  // Default to 1 MHz tick if divider is zero.
  }
  const uint32_t base_clock_hz = 80000000UL;
  uint32_t frequency = base_clock_hz / divider;
  if (frequency == 0) {
    frequency = 1;  // Prevent division by zero inside the core implementation.
  }
  hw_timer_t *timer = ::timerBegin(frequency);
  if (!timer) {
    return nullptr;
  }
  ::timerSetCountUp(timer, countUp);
  ::timerSetAutoReload(timer, false);
  return timer;
}

void timerAttachInterrupt(hw_timer_t *timer, void (*userFunc)(void), bool edge) {
  (void)edge;  // The new API always uses level triggered interrupts.
  ::timerAttachInterrupt(timer, userFunc);
}

void timerAlarmWrite(hw_timer_t *timer, uint64_t alarm_value, bool autoreload) {
  ::timerSetAutoReload(timer, autoreload);
  ::timerSetAlarmValue(timer, alarm_value);
}

void timerAlarmEnable(hw_timer_t *timer) {
  ::timerEnableInterrupt(timer);
  ::timerStart(timer);
}

void timerAlarmDisable(hw_timer_t *timer) {
  ::timerDisableInterrupt(timer);
  ::timerStop(timer);
}

}  // extern "C"

#endif  // ESP_ARDUINO_VERSION_MAJOR >= 3
#endif  // ESP32 && ARDUINO_ARCH_ESP32
