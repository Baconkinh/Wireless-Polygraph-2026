// =====================================================================
//  max30102.cpp — driver เซนเซอร์ชีพจร MAX30102 (เขียนเอง อ่าน/เขียนรีจิสเตอร์ผ่าน I2C ตรง ๆ)
//  ทำอะไร   : ตั้งค่า LED IR, sample rate 400 sps เฉลี่ย 4 = 100 sps, อ่าน FIFO ทีละหลายค่า
//  ทำไม     : ไม่ใช้ไลบรารีสำเร็จรูป จะได้คุมจังหวะอ่านเองและอธิบายได้ทุกบรรทัด
//  เรียกจาก : sensors.cpp readPpg()  ->  ค่า IR ดิบไปที่ dsp/ppg.cpp
//  วิชา     : I2C (register map, burst read), datasheet reading
// =====================================================================
#include "max30102.h"

namespace {
// ที่อยู่รีจิสเตอร์ (datasheet MAX30102, Register Maps)
constexpr uint8_t REG_INT_ENABLE1 = 0x02;
constexpr uint8_t REG_INT_ENABLE2 = 0x03;
constexpr uint8_t REG_FIFO_WR_PTR = 0x04;   // 0x04 WR_PTR, 0x05 OVF_COUNTER, 0x06 RD_PTR ติดกัน
constexpr uint8_t REG_OVF_COUNTER = 0x05;
constexpr uint8_t REG_FIFO_RD_PTR = 0x06;
constexpr uint8_t REG_FIFO_DATA   = 0x07;
constexpr uint8_t REG_FIFO_CONFIG = 0x08;
constexpr uint8_t REG_MODE_CONFIG = 0x09;
constexpr uint8_t REG_SPO2_CONFIG = 0x0A;
constexpr uint8_t REG_LED1_PA     = 0x0C;   // LED แดง
constexpr uint8_t REG_LED2_PA     = 0x0D;   // LED IR
constexpr uint8_t REG_REV_ID      = 0xFE;
constexpr uint8_t REG_PART_ID     = 0xFF;

constexpr uint8_t PART_ID_EXPECTED = 0x15;
constexpr uint8_t MODE_SHDN  = 0x80;
constexpr uint8_t MODE_RESET = 0x40;
constexpr uint8_t MODE_SPO2  = 0x03;
// SMP_AVE=4 (010<<5) | FIFO_ROLLOVER_EN (1<<4) | FIFO_A_FULL=15 (ไม่ได้ใช้ interrupt)
constexpr uint8_t FIFO_CFG = 0x40 | 0x10 | 0x0F;
// ADC_RGE=4096nA (01<<5) | SR=400sps (011<<2) | LED_PW=411us/18bit (11)
constexpr uint8_t SPO2_CFG = 0x20 | 0x0C | 0x03;

constexpr int BYTES_PER_SAMPLE = 6;          // แดง 3 ไบต์ + IR 3 ไบต์
constexpr int MAX_CHUNK = 21;                // 21*6 = 126 ไบต์ < บัฟเฟอร์ Wire 128 ไบต์
}  // namespace

// เขียนค่า 1 ไบต์ลง register ของ MAX30102
bool Max30102::writeReg(uint8_t reg, uint8_t val) {
  w_->beginTransmission(addr_);
  w_->write(reg);
  w_->write(val);
  return w_->endTransmission() == 0;
}

// อ่านหลายไบต์ต่อเนื่องจาก register
bool Max30102::readRegs(uint8_t reg, uint8_t* buf, size_t len) {
  w_->beginTransmission(addr_);
  w_->write(reg);
  if (w_->endTransmission(false) != 0) return false;   // repeated start: ไม่ปล่อยบัสระหว่างทาง
  size_t got = w_->requestFrom((uint16_t)addr_, len, true);
  if (got != len) {
    while (w_->available()) w_->read();                 // ทิ้งเศษให้บัฟเฟอร์สะอาด
    return false;
  }
  for (size_t i = 0; i < len; i++) buf[i] = (uint8_t)w_->read();
  return true;
}

// ตรวจ PART_ID, รีเซ็ต, ตั้งโหมด SpO2 (IR + แดง) 100 Hz, ความแรง LED, ล้าง FIFO
bool Max30102::begin(TwoWire& wire, uint8_t addr, uint8_t irLed, uint8_t redLed) {
  w_ = &wire;
  addr_ = addr;
  ok_ = false;
  if (!readRegs(REG_PART_ID, &part_, 1)) return false;
  if (part_ != PART_ID_EXPECTED) return false;       // ไม่ใช่ MAX30102/30105
  readRegs(REG_REV_ID, &rev_, 1);

  // soft reset แล้วรอบิต RESET กลับเป็น 0 (datasheet: ชิปเคลียร์เองเมื่อพร้อม)
  if (!writeReg(REG_MODE_CONFIG, MODE_RESET)) return false;
  uint32_t t0 = millis();
  uint8_t mode = MODE_RESET;
  while ((mode & MODE_RESET) && millis() - t0 < 100) {
    delay(2);
    if (!readRegs(REG_MODE_CONFIG, &mode, 1)) return false;
  }

  bool ok = true;
  ok &= writeReg(REG_INT_ENABLE1, 0x00);              // ใช้วิธี polling ไม่ใช้ขา INT
  ok &= writeReg(REG_INT_ENABLE2, 0x00);
  ok &= writeReg(REG_FIFO_CONFIG, FIFO_CFG);
  ok &= writeReg(REG_SPO2_CONFIG, SPO2_CFG);
  ok &= writeReg(REG_LED1_PA, redLed);
  ok &= writeReg(REG_LED2_PA, irLed);
  ok &= writeReg(REG_MODE_CONFIG, MODE_SPO2);
  // ล้าง FIFO ให้เริ่มนับจากศูนย์
  ok &= writeReg(REG_FIFO_WR_PTR, 0);
  ok &= writeReg(REG_OVF_COUNTER, 0);
  ok &= writeReg(REG_FIFO_RD_PTR, 0);
  ok_ = ok;
  return ok;
}

// อ่านค่าที่สะสมใน FIFO (แสง IR + แดง) ออกมาทั้งหมด — คืนจำนวนค่า (-1 = อ่านไม่ได้)
int Max30102::read(Sample* out, int maxN) {
  if (!ok_) return -1;
  uint8_t ptr[3];
  if (!readRegs(REG_FIFO_WR_PTR, ptr, 3)) return -1;  // WR, OVF, RD อ่านทีเดียว
  const uint8_t wr = ptr[0] & 0x1F, ovf = ptr[1] & 0x1F, rd = ptr[2] & 0x1F;
  int avail = (wr - rd) & 0x1F;
  if (ovf) {
    // FIFO เต็มแล้ว (WR == RD) และทับข้อมูลเก่าไป ovf ตัว -> อ่านได้ทั้ง 32
    ovf_ += ovf;
    avail = 32;
  }
  if (avail == 0) return 0;
  if (avail > maxN) avail = maxN;   // ที่เหลืออ่านรอบหน้า (pointer ของชิปจัดการให้)

  int got = 0;
  uint8_t buf[MAX_CHUNK * BYTES_PER_SAMPLE];
  while (got < avail) {
    int chunk = avail - got;
    if (chunk > MAX_CHUNK) chunk = MAX_CHUNK;
    if (!readRegs(REG_FIFO_DATA, buf, (size_t)chunk * BYTES_PER_SAMPLE)) return got > 0 ? got : -1;
    for (int i = 0; i < chunk; i++) {
      const uint8_t* b = &buf[i * BYTES_PER_SAMPLE];
      // 3 ไบต์ big-endian, ข้อมูลจริง 18 บิตล่าง (pulse width 411 µs)
      out[got + i].red = (((uint32_t)b[0] << 16) | ((uint32_t)b[1] << 8) | b[2]) & 0x3FFFF;
      out[got + i].ir  = (((uint32_t)b[3] << 16) | ((uint32_t)b[4] << 8) | b[5]) & 0x3FFFF;
    }
    got += chunk;
  }
  return got;
}

// เปิด/ปิดโหมดประหยัดไฟของชิป (ปิด LED) ก่อนหลับ
bool Max30102::shutdown(bool off) {
  if (!w_) return false;
  uint8_t mode;
  if (!readRegs(REG_MODE_CONFIG, &mode, 1)) return false;
  mode = off ? (mode | MODE_SHDN) : (mode & ~MODE_SHDN);
  return writeReg(REG_MODE_CONFIG, mode);
}

// ตั้งกระแส LED IR/แดง (แรงเกิน = สัญญาณชนเพดาน, อ่อนเกิน = จับชีพจรไม่ได้)
bool Max30102::setLedCurrent(uint8_t ir, uint8_t red) {
  if (!ok_) return false;
  return writeReg(REG_LED2_PA, ir) && writeReg(REG_LED1_PA, red);
}
