// =====================================================================
//  analog_in.h — อ่าน ADC เป็นมิลลิโวลต์ (ใช้ค่าคาลิเบรตจาก eFuse ของชิป)
//  ESP32-C3: ADC1 ที่ attenuation 11 dB วัดได้ ~0–2500 mV
//  (VBAT/2 สูงสุด 2.1 V, NTC ที่ 40°C ≈ 2.16 V -> ยังอยู่ในช่วง)
// =====================================================================
#pragma once
#include <Arduino.h>

namespace analog {

void begin();
// เฉลี่ย n ครั้ง — ADC ของ ESP32 มี noise หลาย mV การเฉลี่ยช่วยได้มาก
float readMv(uint8_t pin, uint8_t n);

}  // namespace analog
