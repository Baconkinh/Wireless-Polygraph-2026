// =====================================================================
//  max30102.h — driver เซนเซอร์ชีพจร MAX30102 ที่เขียนเอง (ไม่ใช้ไลบรารี SparkFun)
//
//  ทำไมเขียนเอง: ฟังก์ชัน check() ของ SparkFun เก็บข้อมูลไว้แค่ 4 sample
//  ถ้า task อ่านช้ากว่านั้นนิดเดียว sample จะหาย -> จังหวะหัวใจเพี้ยน
//  driver นี้อ่าน FIFO ทั้งหมดที่ค้างอยู่ทีเดียว (สูงสุด 32 sample = 320 ms)
//  และนับจำนวน sample ที่ล้น (OVF_COUNTER) ไว้ตรวจสอบได้
//
//  การตั้งค่า (อ้างอิง datasheet MAX30102 ตารางรีจิสเตอร์):
//   MODE        = SpO2 (LED แดง + IR)
//   SPO2_CONFIG = ADC 4096 nA, 400 sps, pulse 411 µs (18-bit)
//   FIFO_CONFIG = เฉลี่ย 4 sample (=> ได้ 100 sps), rollover เปิด
//   ค่าเดียวกับ setup() ค่าเริ่มต้นของ SparkFun ที่ทดสอบกับบอร์ดนี้แล้วว่าใช้ได้
// =====================================================================
#pragma once
#include <Arduino.h>
#include <Wire.h>

class Max30102 {
 public:
  struct Sample {
    uint32_t red;
    uint32_t ir;
  };

  // irLed/redLed: กระแส LED หน่วย 0.2 mA (0x1F = 6.2 mA)
  bool begin(TwoWire& wire, uint8_t addr, uint8_t irLed = 0x1F, uint8_t redLed = 0x0A);
  bool present() const { return ok_; }

  // อ่านทุก sample ที่ค้างใน FIFO (ไม่เกิน maxN) คืนจำนวนที่อ่านได้, -1 = I2C ผิดพลาด
  int read(Sample* out, int maxN);

  bool shutdown(bool off);            // ประหยัดไฟ (~0.7 µA) ก่อน deep sleep
  bool setLedCurrent(uint8_t ir, uint8_t red);
  uint8_t partId() const { return part_; }
  uint8_t revId() const { return rev_; }
  uint32_t overflows() const { return ovf_; }

 private:
  bool writeReg(uint8_t reg, uint8_t val);
  bool readRegs(uint8_t reg, uint8_t* buf, size_t len);

  TwoWire* w_ = nullptr;
  uint8_t addr_ = 0x57;
  bool ok_ = false;
  uint8_t part_ = 0, rev_ = 0;
  uint32_t ovf_ = 0;
};
