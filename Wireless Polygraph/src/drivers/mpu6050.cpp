// =====================================================================
//  mpu6050.cpp — driver เซนเซอร์ความเร่ง MPU6050/MPU6500 (อ่านรีจิสเตอร์ตรง)
//  ทำอะไร   : อ่าน WHO_AM_I แยกรุ่นชิป (0x68 = MPU6050, 0x70 = MPU6500), ตั้ง ±2g + DLPF, อ่านความเร่ง 3 แกน
//  ทำไม     : บอร์ดที่ซื้อมาจริงเป็น MPU6500 (ตัวเลียนแบบ) -> ต้องรองรับทั้งสองรุ่น
//  เรียกจาก : sensors.cpp  ->  ค่า m/s² ไปที่ dsp/motion.cpp (แยกมือสั่น/การขยับตัว)
//  วิชา     : I2C
// =====================================================================
#include "mpu6050.h"

namespace {
constexpr uint8_t REG_SMPLRT_DIV    = 0x19;
constexpr uint8_t REG_CONFIG        = 0x1A;
constexpr uint8_t REG_GYRO_CONFIG   = 0x1B;
constexpr uint8_t REG_ACCEL_CONFIG  = 0x1C;
constexpr uint8_t REG_ACCEL_CONFIG2 = 0x1D;   // มีเฉพาะตระกูล MPU6500/9250
constexpr uint8_t REG_ACCEL_XOUT_H  = 0x3B;
constexpr uint8_t REG_PWR_MGMT_1    = 0x6B;
constexpr uint8_t REG_WHO_AM_I      = 0x75;
constexpr float   ACCEL_SCALE = 9.80665f / 16384.0f;   // ±2 g -> 16384 LSB/g -> m/s²
}  // namespace

bool Mpu6050::writeReg(uint8_t reg, uint8_t val) {
  w_->beginTransmission(addr_);
  w_->write(reg);
  w_->write(val);
  return w_->endTransmission() == 0;
}

bool Mpu6050::readRegs(uint8_t reg, uint8_t* buf, size_t len) {
  w_->beginTransmission(addr_);
  w_->write(reg);
  if (w_->endTransmission(false) != 0) return false;
  size_t got = w_->requestFrom((uint16_t)addr_, len, true);
  if (got != len) {
    while (w_->available()) w_->read();
    return false;
  }
  for (size_t i = 0; i < len; i++) buf[i] = (uint8_t)w_->read();
  return true;
}

bool Mpu6050::begin(TwoWire& wire, uint8_t addr) {
  w_ = &wire;
  addr_ = addr;
  ok_ = false;
  if (!readRegs(REG_WHO_AM_I, &who_, 1)) return false;   // ไม่ตอบ = สาย/ไฟมีปัญหา
  // ไม่บังคับค่า WHO_AM_I (ของแท้ 0x68, MPU6500 0x70, MPU9250 0x71, ฯลฯ)
  cfg2_ = (who_ == 0x70 || who_ == 0x71 || who_ == 0x73);

  bool ok = writeReg(REG_PWR_MGMT_1, 0x01);  // ปลุกชิป + ใช้นาฬิกา PLL ของ gyro (นิ่งกว่า RC ภายใน)
  delay(50);
  ok &= writeReg(REG_SMPLRT_DIV, 0x00);      // อัปเดตข้อมูล 1 kHz -> อ่านเมื่อไรก็ได้ค่าล่าสุด
  ok &= writeReg(REG_CONFIG, 0x04);          // DLPF ~21 Hz (MPU6050: ทั้ง accel+gyro)
  ok &= writeReg(REG_GYRO_CONFIG, 0x00);     // ±250 °/s
  ok &= writeReg(REG_ACCEL_CONFIG, 0x00);    // ±2 g ละเอียดสุด (มือสั่นมีขนาดเล็ก)
  if (cfg2_) ok &= writeReg(REG_ACCEL_CONFIG2, 0x04);   // MPU6500: accel DLPF 21.2 Hz
  ok_ = ok;
  return ok;
}

bool Mpu6050::readAccel(float& ax, float& ay, float& az) {
  if (!ok_) return false;
  uint8_t b[6];
  if (!readRegs(REG_ACCEL_XOUT_H, b, 6)) return false;
  // ประกอบไบต์จากอาร์เรย์ (ถ้าเขียน (read()<<8)|read() ลำดับการเรียกไม่รับประกันใน C++)
  ax = (int16_t)((b[0] << 8) | b[1]) * ACCEL_SCALE;
  ay = (int16_t)((b[2] << 8) | b[3]) * ACCEL_SCALE;
  az = (int16_t)((b[4] << 8) | b[5]) * ACCEL_SCALE;
  return true;
}

bool Mpu6050::sleep(bool on) {
  if (!w_) return false;
  return writeReg(REG_PWR_MGMT_1, on ? 0x40 : 0x01);   // bit6 = SLEEP
}

const char* Mpu6050::chipName() const {
  switch (who_) {
    case 0x68: return "MPU6050";
    case 0x70: return "MPU6500";
    case 0x71: return "MPU9250";
    case 0x73: return "MPU9255";
    case 0x72: return "MPU6050-clone";
    case 0x98: return "clone-0x98";
    default: return "unknown";
  }
}
