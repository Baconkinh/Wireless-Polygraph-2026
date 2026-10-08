// =====================================================================
//  power.cpp — โหมดพลังงาน (ตารางอยู่ใน power.h): NORMAL / ECO / LIGHT SLEEP / DEEP SLEEP / STANDBY
//  ทำอะไร   : รับคำขอหลับจาก HTTP/CLI/ปุ่ม แล้วให้ supervisor ทำ (ตอบ HTTP ทันก่อนหลับ), หยุด sensor ที่จุดปลอดภัย,
//             ตั้งแหล่งปลุก (ปุ่ม/timer), หลับอัตโนมัติเมื่อไม่มีใครใช้ (ค่าเริ่มต้น = ปิด), ป้องกันแบตหมด (< 3.3 V)
//  เรียกจาก : tasks.cpp supervisorTask -> power::service() ทุก 1 วินาที, main.cpp handleWakeEarly()
//  วิชา     : Sleep modes, Wakeup sources, Power optimization
// =====================================================================
#include "power.h"
#include "esp_sleep.h"
#include "driver/gpio.h"
#include "../config.h"
#include "../app.h"
#include "../sensors.h"
#include "../net/wifi_ap.h"
#include "../net/web_server.h"
#include "../drivers/analog_in.h"
#include "storage.h"
#include "watchdog.h"
#include "ui.h"

// ค่าที่ต้องรู้ตอนตื่นจาก standby ก่อนโหลด Settings (เก็บใน RTC memory)
RTC_DATA_ATTR static uint32_t rtcContactThr = 50000;

namespace power {

namespace {
volatile uint8_t s_req = 0;          // 0 ไม่มี, 1 light, 2 deep, 3 restart
volatile uint32_t s_reqSec = 0;
volatile uint32_t s_reqReason = 0;
volatile uint32_t s_reqAt = 0;
bool s_eco = false;
uint32_t s_lightSleeps = 0, s_lastLightMs = 0;
uint8_t s_lowCount = 0, s_critCount = 0;
bool s_lowLogged = false;

[[noreturn]] void sleepAgain(uint32_t sec) {
  esp_sleep_enable_timer_wakeup((uint64_t)sec * 1000000ULL);
  esp_deep_sleep_start();
}

void drainLogsToFlash() {
  // log ที่ค้างในคิวเขียนลงแฟลชก่อนหลับ (ไม่งั้นหายไปกับ RAM)
  LogMsg m;
  while (app::logQueue && xQueueReceive(app::logQueue, &m, 0) == pdTRUE)
    storage::appendEvent(m.type, m.text);
}

void doLightSleep(uint32_t maxSec) {
  app::logEvent("SLEEP", "light sleep (wake: BOOT button%s)", maxSec ? " or timer" : "");
  drainLogsToFlash();
  ui::ledOff();
  // ถ้ามาจากการกดค้าง ต้องรอปล่อยปุ่มก่อน ไม่งั้นตื่นทันที (ปลุกด้วยระดับ LOW)
  uint32_t t0 = millis();
  while (digitalRead(PIN_BUTTON) == LOW && millis() - t0 < 5000) {
    wdt::feed();
    delay(10);
  }
  delay(100);
  wdt::setHwPaused(true);
  net::stopAp();                                       // AP รักษาการเชื่อมต่อขณะหลับไม่ได้

  gpio_wakeup_enable((gpio_num_t)PIN_BUTTON, GPIO_INTR_LOW_LEVEL);
  esp_sleep_enable_gpio_wakeup();
  if (maxSec) esp_sleep_enable_timer_wakeup((uint64_t)maxSec * 1000000ULL);

  const uint32_t before = millis();
  Serial.flush();
  wdt::feed();
  esp_light_sleep_start();                             // CPU หยุดตรงนี้ RAM ยังอยู่ครบ
  const uint32_t slept = millis() - before;            // esp_timer ชดเชยเวลาหลับให้แล้ว

  esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
  gpio_wakeup_disable((gpio_num_t)PIN_BUTTON);
  wdt::feed();
  s_lightSleeps++;
  s_lastLightMs = slept;
  net::beginAp();
  web::notifyNetworkRestart();
  wdt::setHwPaused(false);
  sensors::requestGap();                               // ข้อมูลขาดช่วง -> ล้างสายจังหวะหัวใจ
  app::touchActivity();
  app::logEvent("WAKE", "light sleep %lu ms, cause=%s (baseline/results kept in RAM)",
                (unsigned long)slept, wakeCauseName((uint32_t)esp_sleep_get_wakeup_cause()));
  // รอให้ปล่อยปุ่ม (ถ้าปลุกด้วยปุ่ม) จะได้ไม่นับเป็นการกด
  t0 = millis();
  while (digitalRead(PIN_BUTTON) == LOW && millis() - t0 < 3000) {
    wdt::feed();
    delay(10);
  }
  ui::blink(1);
}

[[noreturn]] void doDeepSleep(uint32_t sec, uint8_t reason) {
  const Settings& st = storage::settings();
  if (reason == SLP_STANDBY && sec == 0) sec = st.wakeCheckS;
  if (sec == 0) sec = 60;
  app::logEvent("SLEEP", "deep sleep reason=%u wake in %lus", reason, (unsigned long)sec);
  storage::stats().deepSleeps++;
  storage::saveStats();
  drainLogsToFlash();
  // ให้ sensorTask หยุดเองที่จุดปลอดภัยก่อน แล้วค่อยใช้ I2C ปิดเซนเซอร์
  if (sensors::stopForSleep(300)) sensors::shutdownForSleep();   // MAX30102 ~0.7 µA, MPU sleep
  ui::ledOff();
  net::stopAp();
  rtcSleepReason = reason;
  rtcWakeIntervalS = (uint16_t)(sec > 65535 ? 65535 : sec);
  rtcContactThr = st.contactThr;
  Serial.flush();
  delay(50);
  sleepAgain(sec);
}
}  // namespace

// =====================================================================
void handleWakeEarly() {
  const esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
  if (cause != ESP_SLEEP_WAKEUP_TIMER) {
    rtcSleepReason = SLP_NONE;
    return;
  }
  rtcWakeCount++;
  if (rtcSleepReason == SLP_STANDBY) {
    rtcStandbyChecks++;
    // ตื่นมาดูแวบเดียว: มีผิวแตะเซนเซอร์ไหม? ไม่มี -> หลับต่อ (ไม่เปิด WiFi เลย ประหยัดสุด)
    if (!sensors::quickContactCheck(350, rtcContactThr)) {
      rtcSleepSeconds += rtcWakeIntervalS;
      sleepAgain(rtcWakeIntervalS);
    }
  } else if (rtcSleepReason == SLP_LOWBATT) {
    analog::begin();
    const float v = analog::readMv(PIN_VBAT, 16) * VBAT_DIV;
    // ยังไม่ได้ชาร์จ -> หลับต่อ; ถ้าไม่มีแบต (ใช้ USB) หรือแบตกลับมาพอแล้ว -> บูตต่อ
    if (v > BATT_PRESENT_MV && v < BATT_LOW_MV + 100.0f) {
      rtcSleepSeconds += LOWBATT_SLEEP_S;
      sleepAgain(LOWBATT_SLEEP_S);
    }
  } else if (rtcSleepReason == SLP_MANUAL) {
    rtcSleepSeconds += rtcWakeIntervalS;
  }
}

void begin() { setEco(storage::settings().eco); }

void setEco(bool on) {
  s_eco = on;
  setCpuFrequencyMhz(on ? CPU_MHZ_ECO : CPU_MHZ_NORMAL);
  if (on) app::setBits(EV_ECO);
  else app::clearBits(EV_ECO);
}

bool eco() { return s_eco; }

void requestLightSleep(uint32_t maxSec) {
  s_reqSec = maxSec;
  s_reqAt = millis();
  s_req = 1;
}

void requestDeepSleep(uint32_t sec) {
  s_reqSec = sec;
  s_reqAt = millis();
  s_req = 2;
}

void requestRestart(uint32_t plannedReason) {
  s_reqReason = plannedReason;
  s_reqAt = millis();
  s_req = 3;
}

void service() {
  const uint32_t now = millis();

  // ---- คำขอที่ค้าง (รอ 400 ms ให้ HTTP ส่งคำตอบออกไปก่อน) ----
  if (s_req && now - s_reqAt > 400) {
    const uint8_t r = s_req;
    s_req = 0;
    if (r == 1) doLightSleep(s_reqSec);
    else if (r == 2) doDeepSleep(s_reqSec, s_reqSec ? SLP_MANUAL : SLP_STANDBY);
    else if (r == 3) {
      storage::setPlanned(s_reqReason, "user", 0);
      storage::saveStats();
      drainLogsToFlash();
      Serial.flush();
      ESP.restart();
    }
  }

  const Vitals v = app::getVitals();
  // ---- แบตต่ำ (ต้องต่ำต่อเนื่อง 10 s กันค่ากระตุกตอน WiFi ส่ง) ----
  if (v.battPresent) {
    if (v.vbatMv < BATT_LOW_MV) {
      if (s_lowCount < 255) s_lowCount++;
    } else if (v.vbatMv > BATT_LOW_MV + 50.0f) {     // hysteresis 50 mV
      s_lowCount = 0;
      if (s_lowLogged) app::clearBits(EV_LOW_BATT);
      s_lowLogged = false;
    }
    if (s_lowCount >= 10 && !s_lowLogged) {
      s_lowLogged = true;
      app::setBits(EV_LOW_BATT);
      app::logEvent("BATTERY", "low battery %.0f mV", v.vbatMv);
      char js[96];
      snprintf(js, sizeof(js), "{\"t\":\"e\",\"eid\":%lu,\"ev\":\"low_batt\",\"mv\":%.0f}",
               (unsigned long)app::nextEventId(), v.vbatMv);
      app::pushEvent(js);
    }
    if (v.vbatMv < BATT_CRIT_MV) {
      if (++s_critCount >= 10) doDeepSleep(LOWBATT_SLEEP_S, SLP_LOWBATT);  // ปกป้องเซลล์ Li-Po
    } else {
      s_critCount = 0;
    }
  } else {
    s_lowCount = s_critCount = 0;
  }

  // ---- Auto-standby: ไม่มีใครใช้ (ไม่มีเครื่องเชื่อม, ไม่แตะ, ไม่กดปุ่ม) ----
  const Settings& st = storage::settings();
  const uint32_t b = app::bits();
  if (st.standbyMin > 0 && !(b & EV_USB) && !(b & EV_OTA_ACTIVE)) {
    const uint32_t idle = now - app::lastActivityMs();
    if (idle > (uint32_t)st.standbyMin * 60000UL) {
      app::logEvent("POWER", "idle %lu min -> standby", (unsigned long)(idle / 60000));
      doDeepSleep(st.wakeCheckS, SLP_STANDBY);
    }
  }
}

uint32_t lightSleeps() { return s_lightSleeps; }
uint32_t lastLightSleepMs() { return s_lastLightMs; }

const char* wakeCauseName(uint32_t cause) {
  switch ((esp_sleep_wakeup_cause_t)cause) {
    case ESP_SLEEP_WAKEUP_TIMER: return "timer";
    case ESP_SLEEP_WAKEUP_GPIO:  return "gpio";
    case ESP_SLEEP_WAKEUP_UART:  return "uart";
    case ESP_SLEEP_WAKEUP_WIFI:  return "wifi";
    case ESP_SLEEP_WAKEUP_UNDEFINED: return "none";
    default: return "other";
  }
}

}  // namespace power
