// =====================================================================
//  sensors.h — รวมเซนเซอร์ทั้งหมด + DSP ไว้ที่เดียว (เจ้าของคือ sensorTask)
//
//  sensorTask ถูกปลุกด้วย Hardware Timer ทุก 10 ms แล้วเรียก sensors::tick()
//   ทุก tick   : อ่าน FIFO ของ MAX30102 (~1 sample) -> PPG DSP, อ่าน MPU6050 -> motion DSP
//   ทุก 10 tick: อ่าน ADC (NTC, GSR, VBAT) เฉลี่ย 16 ครั้ง -> EDA DSP (10 Hz)
//   ทุก 20 tick: สร้าง Vitals frame -> frameQueue (LieEngine) + live snapshot (5 Hz)
//   ทุก 10 PPG sample: WaveChunk -> waveQueue (กราฟคลื่นชีพจร)
// =====================================================================
#pragma once
#include <Arduino.h>
#include "app.h"
#include "drivers/max30102.h"
#include "drivers/mpu6050.h"

namespace sensors {

bool begin();                     // I2C + MAX30102 + MPU6050 + ADC + DSP
void tick();                      // เรียกจาก sensorTask เท่านั้น
void requestGap();                // ข้อมูลจะขาดช่วง (เช่นตื่นจาก light sleep) -> รีเซ็ตสายจังหวะ
void applySettings();             // อ่าน Settings ใหม่ (เกณฑ์แตะ, กระแส LED, Vcc)
bool stopForSleep(uint32_t waitMs); // ขอให้ sensorTask หยุดตรงจุดปลอดภัย (ไม่ค้างกลางธุรกรรม I2C)
bool stopRequested();             // sensorTask ถามทุกรอบ
void ackStop();                   // sensorTask ตอบรับแล้วหยุดตัวเอง
void shutdownForSleep();          // ปิดเซนเซอร์ก่อน deep sleep (ต้องหยุด sensorTask ก่อน)
bool quickContactCheck(uint32_t ms, uint32_t threshold);   // ใช้ตอน standby (ก่อนสร้าง task)

bool maxOk();
bool mpuOk();
const Max30102& max30102();
const Mpu6050& mpu6050();
uint32_t i2cErrors();
uint32_t ticks();
float chipTempC();                // อุณหภูมิในชิป ESP32-C3 (supervisor อัปเดต)
void setChipTempC(float t);

}  // namespace sensors
