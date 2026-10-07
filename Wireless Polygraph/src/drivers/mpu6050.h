// =====================================================================
//  mpu6050.h — driver เซนเซอร์ความเร่ง MPU6050/MPU6500 แบบอ่านรีจิสเตอร์ตรง
//
//  โมดูล GY-521 ราคาถูกหลายล็อตใช้ชิปเลียนแบบ/MPU6500 (WHO_AM_I = 0x70 ฯลฯ)
//  ไลบรารีทั่วไปจะปฏิเสธชิปพวกนี้ จึงเขียนเองโดยยอมรับทุกค่า WHO_AM_I
//
//  จุดสำคัญที่พบจาก datasheet: MPU6500 ย้ายตัวกรอง low-pass ของ accelerometer
//  ไปไว้ที่รีจิสเตอร์ 0x1D (ACCEL_CONFIG2) ไม่ใช่ 0x1A เหมือน MPU6050
//  ถ้าไม่ตั้ง แบนด์วิดท์ accel จะเป็น ~218 Hz -> สุ่ม 100 Hz แล้วเกิด aliasing
// =====================================================================
#pragma once
#include <Arduino.h>
#include <Wire.h>

class Mpu6050 {
 public:
  bool begin(TwoWire& wire, uint8_t addr);
  bool present() const { return ok_; }
  bool readAccel(float& ax, float& ay, float& az);   // m/s²
  bool sleep(bool on);
  uint8_t whoAmI() const { return who_; }
  bool hasAccelConfig2() const { return cfg2_; }
  const char* chipName() const;

 private:
  bool writeReg(uint8_t reg, uint8_t val);
  bool readRegs(uint8_t reg, uint8_t* buf, size_t len);
  TwoWire* w_ = nullptr;
  uint8_t addr_ = 0x68;
  uint8_t who_ = 0;
  bool ok_ = false;
  bool cfg2_ = false;
};
