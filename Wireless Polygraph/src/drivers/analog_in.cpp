// analog_in.cpp
#include "analog_in.h"
#include "../config.h"

namespace analog {

void begin() {
  analogReadResolution(12);
  analogSetPinAttenuation(PIN_NTC, ADC_11db);
  analogSetPinAttenuation(PIN_GSR, ADC_11db);
  analogSetPinAttenuation(PIN_VBAT, ADC_11db);
}

float readMv(uint8_t pin, uint8_t n) {
  if (n == 0) n = 1;
  uint32_t s = 0;
  for (uint8_t i = 0; i < n; i++) s += analogReadMilliVolts(pin);
  return (float)s / n;
}

}  // namespace analog
