// =====================================================================
//  sensors.cpp — อ่านเซนเซอร์ทุกตัว + ส่งต่อให้ DSP ทุกจังหวะ 10 ms (เจ้าของคือ sensorTask)
//  ทำอะไร   : tick() ถูกเรียก 100 ครั้ง/วินาที -> อ่าน FIFO ของ MAX30102 (I2C), อ่าน MPU6050 (I2C),
//             อ่าน ADC (GSR/NTC/แบต) ทุก 10 tick, แล้วรวมเป็น "frame" ส่งเข้า frameQueue ทุก 200 ms
//  ทำไม     : ให้การสุ่มสัญญาณตรงเวลาเป๊ะ (คุมด้วย hardware timer) -> คำนวณชีพจร/HRV ได้แม่น
//  เรียกจาก : tasks.cpp sensorTask (ถูกปลุกด้วย timer interrupt)
//  ใช้      : drivers/* (อ่านฮาร์ดแวร์), dsp/* (กรองสัญญาณ), app.h (คิว), ui.cpp (บอกจังหวะหัวใจให้ LED)
//  วิชา     : I2C, ADC, Timer interrupt, การกรองสัญญาณดิจิทัล
// =====================================================================
#include "sensors.h"
#include <Wire.h>
#include "config.h"
#include "drivers/i2c_bus.h"
#include "drivers/analog_in.h"
#include "drivers/sensor_math.h"
#include "dsp/ppg.h"
#include "dsp/motion.h"
#include "dsp/eda.h"
#include "sys/storage.h"

namespace sensors {

namespace {
Max30102 s_max;
Mpu6050 s_mpu;
dsp::PpgProcessor s_ppg;
dsp::MotionProcessor s_motion;
dsp::EdaProcessor s_eda;
dsp::Ema s_temp, s_vbat;

uint32_t s_tick = 0, s_frameSeq = 0, s_ppgSamples = 0;
uint32_t s_i2cErr = 0;
uint32_t s_lastMaxOk = 0, s_lastMpuOk = 0;
uint32_t s_lastRecover = 0, s_lastRetry = 0;
uint32_t s_lastOvf = 0;
volatile bool s_gapReq = false;
volatile bool s_settingsDirty = false;
volatile bool s_stopReq = false, s_stopped = false;
float s_gsrMv = 0, s_ntcMv = 0, s_tempC = NAN;
bool s_gsrContact = false, s_gsrShort = false, s_ntcFault = false;
float s_vccMv = VCC_MV_DEFAULT;
float s_chipTemp = NAN;
WaveChunk s_wave;

// เจอจังหวะหัวใจ -> ปลุก uiTask ให้กระพริบ LED
void notifyBeat() {
  // แจ้ง uiTask ให้กระพริบ LED ตามจังหวะหัวใจ (task notification = เบาสุดใน FreeRTOS)
  TaskHandle_t h = app::tasks[T_UI].handle;
  if (h) xTaskNotifyGive(h);
}

// อ่านค่าแสง IR/แดงจาก FIFO ของ MAX30102 (สูงสุด 32 ค่า) แล้วป้อน DSP ชีพจรทีละค่า
void readPpg(uint32_t now) {
  Max30102::Sample buf[32];
  const int n = s_max.read(buf, 32);
  if (n < 0) { s_i2cErr++; return; }
  s_lastMaxOk = now;
  if (s_max.overflows() != s_lastOvf) {   // FIFO ล้น = sample หาย -> ช่วงจังหวะเชื่อไม่ได้
    s_lastOvf = s_max.overflows();
    s_ppg.markGap();
  }
  for (int i = 0; i < n; i++) {
    const bool beat = s_ppg.push(buf[i].ir);
    if (s_wave.count == 0) s_wave.n0 = s_ppgSamples;
    float w = s_ppg.wave();
    if (w > 32767.0f) w = 32767.0f;
    if (w < -32768.0f) w = -32768.0f;
    s_wave.d[s_wave.count] = (int16_t)w;
    if (beat) {
      s_wave.beatMask |= (uint16_t)(1u << s_wave.count);
      notifyBeat();
    }
    s_wave.count++;
    s_ppgSamples++;
    if (s_wave.count >= WAVE_CHUNK) {
      xQueueSend(app::waveQueue, &s_wave, 0);   // คิวเต็ม = ไม่มีใครดูกราฟ ทิ้งได้
      s_wave.count = 0;
      s_wave.beatMask = 0;
    }
  }
}

// อ่าน ADC ของ NTC, GSR, แบต (เฉลี่ยหลายครั้งลด noise) แล้วแปลงเป็นหน่วยจริง
// [เทคนิค: ADC + voltage divider] อ่าน NTC (อุณหภูมิผิว), GSR (ความนำไฟฟ้าผิว), แบต/2 ด้วย ADC 12 บิต + oversampling 16 ครั้ง
//   แล้วแปลงแรงดันเป็นหน่วยจริงด้วยสูตรวงจรแบ่งแรงดัน (drivers/sensor_math.cpp)
void readAnalog() {
  s_ntcMv = analog::readMv(PIN_NTC, ADC_OVERSAMPLE);
  s_gsrMv = analog::readMv(PIN_GSR, ADC_OVERSAMPLE);
  const float vbat = analog::readMv(PIN_VBAT, ADC_OVERSAMPLE) * VBAT_DIV;

  const float t = sensor::ntcCelsius(s_ntcMv, s_vccMv, R1_OHM, NTC_R25, NTC_BETA);
  s_ntcFault = isnan(t);
  if (!s_ntcFault) s_tempC = s_temp.push(t);
  else s_tempC = NAN;

  const float g = sensor::gsrMicroSiemens(s_gsrMv, s_vccMv, R2_OHM, R3_OHM);
  s_gsrShort = isnan(g);
  s_gsrContact = sensor::gsrContact(s_gsrMv) && !s_gsrShort;
  s_eda.push(s_gsrContact ? g : 0.0f, s_gsrContact);
  s_vbat.push(vbat);
}

// รวมค่าทุกเซนเซอร์เป็น 1 เฟรม (5 ครั้ง/วินาที) -> ส่งเข้า frameQueue ให้ engineTask + เก็บเป็นค่าสดล่าสุด
void publishFrame(uint32_t now) {
  Vitals v;
  v.ms = now;
  v.seq = ++s_frameSeq;
  v.ppgContact = s_ppg.contact();
  v.hr = s_ppg.hr();
  v.hrv = s_ppg.hrv();
  v.ibi = s_ppg.lastIbi();
  v.pi = s_ppg.perfusion();
  v.ppgAmp = s_ppg.amplitude();
  v.irDc = s_ppg.dc();
  v.beats = s_ppg.beatCount();
  v.gsrContact = s_eda.contact();
  v.gsr = s_eda.value();
  v.gsrTonic = s_eda.tonic();
  v.gsrPhasic = s_eda.phasic();
  v.gsrMv = s_gsrMv;
  v.scrPerMin = s_eda.scrPerMin();
  v.skinTemp = s_tempC;
  v.tremor = s_motion.tremor();
  v.motion = s_motion.motion();
  v.vbatMv = s_vbat.value();
  v.battPresent = v.vbatMv > BATT_PRESENT_MV;
  v.battPct = v.battPresent ? sensor::batteryPercent(v.vbatMv) : 0;

  const uint32_t b = app::bits();
  uint16_t fl = 0;
  if (!s_max.present()) fl |= FL_MAX_FAIL;
  if (!s_mpu.present()) fl |= FL_MPU_FAIL;
  if (s_ntcFault) fl |= FL_NTC_FAULT;
  if (s_gsrShort) fl |= FL_GSR_SHORT;
  if (s_ppg.saturated()) fl |= FL_PPG_SAT;
  if (b & EV_LOW_BATT) fl |= FL_LOW_BATT;
  if (v.battPresent && v.vbatMv < BATT_CRIT_MV) fl |= FL_CRIT_BATT;
  if (s_lastRecover && now - s_lastRecover < 30000) fl |= FL_I2C_RECOV;
  if (b & EV_USB) fl |= FL_USB;
  if (v.motion > 1.5f) fl |= FL_MOTION;
  if (b & EV_TIME_SYNCED) fl |= FL_TIME_SYNC;
  if (!storage::fsOk()) fl |= FL_FS_FAIL;
  if (!v.battPresent) fl |= FL_NO_BATT;
  v.flags = fl;

  app::setVitals(v);
  xQueueSend(app::frameQueue, &v, 0);

  if (v.ppgContact) app::setBits(EV_PPG_CONTACT); else app::clearBits(EV_PPG_CONTACT);
  if (v.gsrContact) app::setBits(EV_GSR_CONTACT); else app::clearBits(EV_GSR_CONTACT);
  if (s_max.present() && s_mpu.present()) app::setBits(EV_SENSORS_OK);
  else app::clearBits(EV_SENSORS_OK);
  if (v.ppgContact || v.gsrContact) app::touchActivity();   // มีคนใส่อยู่ = ไม่ standby
}

// ตรวจสุขภาพเซนเซอร์: ชิปเงียบนาน = กู้บัส I2C, ตั้งธงเตือน (สายหลุด, แผ่น GSR ลัด, แบตอ่อน)
// [เทคนิค: Fault tolerance — I2C bus recovery] ชิปเงียบเกินเวลา = บัสค้าง -> ส่ง clock 9 ลูก + STOP แล้วเริ่มชิปใหม่
void checkHealth(uint32_t now) {
  const Settings& st = storage::settings();
  // ชิปเคยตอบแต่เงียบไปนาน -> บัสค้าง/สายหลวม -> กู้บัส (เว้นระยะ 10 s กันวนรัว)
  const bool maxStale = s_max.present() && now - s_lastMaxOk > SENSOR_STALE_MS;
  const bool mpuStale = s_mpu.present() && now - s_lastMpuOk > SENSOR_STALE_MS;
  if ((maxStale || mpuStale) && now - s_lastRecover > 10000) {
    s_lastRecover = now;
    app::logEvent("I2C", "sensor silent (max=%d mpu=%d) -> bus recovery", maxStale, mpuStale);
    i2cbus::recover();
    bool a = s_max.begin(Wire, ADDR_MAX30102, st.irLed, 0x0A);
    bool b = s_mpu.begin(Wire, ADDR_MPU6050);
    s_ppg.markGap();
    s_motion.markGap();
    s_lastMaxOk = s_lastMpuOk = now;
    app::logEvent("I2C", "after recovery: max=%s mpu=%s", a ? "ok" : "FAIL", b ? "ok" : "FAIL");
  }
  // ชิปไม่เจอตั้งแต่บูต -> ลองใหม่ทุก 10 s (เผื่อสายหลวมแล้วกลับมาแนบ)
  if ((!s_max.present() || !s_mpu.present()) && now - s_lastRetry > 10000) {
    s_lastRetry = now;
    if (!s_max.present() && s_max.begin(Wire, ADDR_MAX30102, st.irLed, 0x0A)) {
      s_lastMaxOk = now;
      app::logEvent("SENSOR", "MAX30102 found (late)");
    }
    if (!s_mpu.present() && s_mpu.begin(Wire, ADDR_MPU6050)) {
      s_lastMpuOk = now;
      app::logEvent("SENSOR", "MPU found (late) WHO_AM_I=0x%02X", s_mpu.whoAmI());
    }
  }
}
}  // namespace

// =====================================================================
bool begin() {
  i2cbus::begin();
  analog::begin();
  const Settings& st = storage::settings();
  s_vccMv = st.vccMv;
  const bool a = s_max.begin(Wire, ADDR_MAX30102, st.irLed, 0x0A);
  const bool b = s_mpu.begin(Wire, ADDR_MPU6050);
  s_ppg.begin((float)PPG_FS_HZ, st.contactThr);
  s_motion.begin((float)TICK_HZ);
  s_eda.begin((float)(TICK_HZ / ADC_EVERY_TICK));
  s_temp.setup((float)(TICK_HZ / ADC_EVERY_TICK), 2.0f);
  s_vbat.setup((float)(TICK_HZ / ADC_EVERY_TICK), 5.0f);
  s_wave.count = 0;
  s_wave.beatMask = 0;
  s_wave.n0 = 0;
  s_lastMaxOk = s_lastMpuOk = s_lastRetry = millis();
  // อ่าน ADC รอบแรกเลย -> ค่าแบต/อุณหภูมิมีทันทีไม่ต้องรอ EMA ไต่จาก 0
  readAnalog();
  return a && b;
}

// 1 รอบของ sensorTask (ทุก 10 ms จาก hardware timer): อ่าน MPU ทุกรอบ, PPG/ADC ตามรอบ, ส่งเฟรมทุก 200 ms
// [เทคนิค: Multi-rate sampling] ถูกเรียก 100 Hz จาก timer: MPU6050 ทุกรอบ (100 Hz), PPG อ่าน FIFO ทุกรอบ,
//   ADC ทุก 10 รอบ (10 Hz), ส่งเฟรมให้ LieEngine ทุก 20 รอบ (5 Hz), ตรวจสุขภาพเซนเซอร์ทุก 50 รอบ
void tick() {
  s_tick++;
  const uint32_t now = millis();

  if (s_gapReq) {
    s_gapReq = false;
    s_ppg.markGap();
    s_motion.markGap();
  }
  if (s_settingsDirty) {
    s_settingsDirty = false;
    const Settings& st = storage::settings();
    s_ppg.setContactThreshold(st.contactThr);
    s_max.setLedCurrent(st.irLed, 0x0A);
    s_vccMv = st.vccMv;
  }

  if (s_max.present()) readPpg(now);
  if (s_mpu.present()) {
    float ax, ay, az;
    if (s_mpu.readAccel(ax, ay, az)) {
      s_motion.push(ax, ay, az);
      s_lastMpuOk = now;
    } else {
      s_i2cErr++;
    }
  }
  if (s_tick % ADC_EVERY_TICK == 0) readAnalog();
  if (s_tick % FRAME_EVERY_TICK == 0) publishFrame(now);
  if (s_tick % 50 == 0) checkHealth(now);
}

void requestGap() { s_gapReq = true; }

// ขอให้ sensorTask หยุดเองหลังจบรอบ (ก่อนหลับ) แล้วรอได้ไม่เกิน waitMs
bool stopForSleep(uint32_t waitMs) {
  // ห้ามใช้ vTaskSuspend(sensorTask) ตรง ๆ: ถ้าบังเอิญหยุดตอนมันถือ mutex ของ Wire อยู่
  // task อื่นที่เรียก I2C ต่อจะค้างตลอดไป (deadlock) -> ให้มันหยุดเองหลังจบ tick
  s_stopReq = true;
  const uint32_t t0 = millis();
  while (!s_stopped && millis() - t0 < waitMs) delay(5);
  return s_stopped;
}
bool stopRequested() { return s_stopReq; }
void ackStop() { s_stopped = true; }
void applySettings() { s_settingsDirty = true; }

// สั่งเซนเซอร์เข้าโหมดประหยัดไฟก่อนหลับ
void shutdownForSleep() {
  s_max.shutdown(true);
  s_mpu.sleep(true);
}

// ตอนตื่นจาก standby: เปิด MAX30102 แป๊บเดียวเพื่อดูว่ามีคนใส่นาฬิกาไหม (ไม่มี = หลับต่อ)
bool quickContactCheck(uint32_t ms, uint32_t threshold) {
  // เรียกตอนตื่นจาก standby (ยังไม่มี task, ยังไม่เปิด WiFi) -> ต้องเร็วและกินไฟน้อย
  i2cbus::begin();
  bool contact = false;
  if (s_max.begin(Wire, ADDR_MAX30102, 0x1F, 0x00)) {
    delay(ms);                                   // ให้ FIFO สะสม ~30 sample
    Max30102::Sample buf[32];
    const int n = s_max.read(buf, 32);
    if (n > 0) {
      uint64_t sum = 0;
      for (int i = 0; i < n; i++) sum += buf[i].ir;
      contact = (sum / (uint32_t)n) > threshold;
    }
    s_max.shutdown(true);
  }
  analogReadResolution(12);
  if (sensor::gsrContact(analog::readMv(PIN_GSR, 8))) contact = true;
  return contact;
}

bool maxOk() { return s_max.present(); }
bool mpuOk() { return s_mpu.present(); }
const Max30102& max30102() { return s_max; }
const Mpu6050& mpu6050() { return s_mpu; }
uint32_t i2cErrors() { return s_i2cErr; }
uint32_t ticks() { return s_tick; }
float chipTempC() { return s_chipTemp; }
void setChipTempC(float t) { s_chipTemp = t; }

}  // namespace sensors
