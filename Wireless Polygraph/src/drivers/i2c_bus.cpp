// i2c_bus.cpp — ดูคำอธิบายใน i2c_bus.h
#include "i2c_bus.h"
#include <Wire.h>
#include "../config.h"

namespace i2cbus {

static uint32_t s_recoveries = 0;

bool begin() {
  bool ok = Wire.begin((int)PIN_SDA, (int)PIN_SCL, I2C_FREQ_HZ);
  Wire.setTimeOut(I2C_TIMEOUT_MS);
  return ok;
}

bool recover() {
  s_recoveries++;
  Wire.end();
  pinMode(PIN_SDA, INPUT_PULLUP);
  pinMode(PIN_SCL, OUTPUT_OPEN_DRAIN);
  digitalWrite(PIN_SCL, HIGH);
  delayMicroseconds(10);
  // ส่ง clock จนกว่าชิปจะปล่อย SDA (สูงสุด 9 ลูก = 1 ไบต์ + ACK)
  for (int i = 0; i < 9 && digitalRead(PIN_SDA) == LOW; i++) {
    digitalWrite(PIN_SCL, LOW);
    delayMicroseconds(5);
    digitalWrite(PIN_SCL, HIGH);
    delayMicroseconds(5);
  }
  // STOP condition: SDA ขึ้นจาก LOW -> HIGH ขณะ SCL เป็น HIGH
  pinMode(PIN_SDA, OUTPUT_OPEN_DRAIN);
  digitalWrite(PIN_SDA, LOW);
  delayMicroseconds(5);
  digitalWrite(PIN_SCL, HIGH);
  delayMicroseconds(5);
  digitalWrite(PIN_SDA, HIGH);
  delayMicroseconds(5);
  pinMode(PIN_SDA, INPUT);
  pinMode(PIN_SCL, INPUT);
  return begin();
}

bool probe(uint8_t addr) {
  Wire.beginTransmission(addr);
  return Wire.endTransmission() == 0;
}

int scan(uint8_t* found, int maxN) {
  int n = 0;
  for (uint8_t a = 0x08; a < 0x78; a++) {
    if (probe(a)) {
      if (n < maxN) found[n] = a;
      n++;
    }
  }
  return n;
}

uint32_t recoveries() { return s_recoveries; }

}  // namespace i2cbus
