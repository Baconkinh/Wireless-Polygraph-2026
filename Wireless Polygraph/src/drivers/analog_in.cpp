// =====================================================================
//  analog_in.cpp — อ่าน ADC เป็นมิลลิโวลต์ (ใช้กับ GSR, NTC, แรงดันแบต)
//  ทำอะไร   : analogReadMilliVolts() ซ้ำ ADC_OVERSAMPLE ครั้งแล้วเฉลี่ย (ลดสัญญาณรบกวน)
//  ทำไม     : ค่า mV ที่คาลิเบรตจาก eFuse แม่นกว่าค่าดิบ 0–4095 และใช้สูตรฟิสิกส์ต่อได้เลย
//  เรียกจาก : sensors.cpp readAnalog()  ->  ต่อด้วย sensor_math.cpp (แปลงเป็น µS / °C / %)
//  วิชา     : ADC (attenuation, oversampling)
// =====================================================================
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
