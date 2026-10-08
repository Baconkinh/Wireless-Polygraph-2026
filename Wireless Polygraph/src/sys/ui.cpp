// =====================================================================
//  ui.cpp — LED (PWM ด้วย LEDC) + ปุ่ม BOOT (debounce, กด 1/2/3 ครั้ง, กดค้าง 2/5/10 วินาที)
//  ทำอะไร   : computeLed() เลือกรูปแบบไฟตามสถานะ (ความหมายอยู่ใน ui.h), pollButton() แยกท่ากด
//  เรียกจาก : tasks.cpp uiTask (ถูกปลุกเมื่อหัวใจเต้นเพื่อกระพริบตรงจังหวะ), main.cpp (ไฟบอกสาเหตุบูต)
//  วิชา     : GPIO input/output, PWM, debounce
// =====================================================================
#include "ui.h"
#include <math.h>
#include "../app.h"
#include "../config.h"
#include "storage.h"
#include "power.h"
#include "watchdog.h"
#include "ml_runtime.h"

namespace ui {

namespace {
constexpr uint8_t LEDC_CH = 0;
constexpr uint32_t DEBOUNCE_MS = 30;
constexpr uint32_t MULTI_CLICK_MS = 400;

volatile uint8_t s_verdict = 0;
volatile uint32_t s_verdictAt = 0;
volatile uint8_t s_blinkReq = 0;
uint8_t s_blinkCount = 0;
uint32_t s_blinkStart = 0;
uint32_t s_beatUntil = 0;
volatile bool s_off = false;

bool s_raw = false, s_stable = false;
uint32_t s_rawChange = 0, s_pressStart = 0, s_lastRelease = 0;
uint8_t s_clicks = 0, s_holdLevel = 0;
uint32_t s_presses = 0;
uint16_t s_buttonQid = 900;    // คำถามที่เริ่มจากปุ่มบนนาฬิกาใช้เลข 900+

void ledWrite(uint8_t duty) {
  const uint8_t hw = LED_ACTIVE_LOW ? (uint8_t)(255 - duty) : duty;
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  ledcWrite(PIN_LED, hw);
#else
  ledcWrite(LEDC_CH, hw);
#endif
}

uint8_t computeLed(uint32_t now) {
  if (s_off) return 0;
  const uint32_t b = app::bits();
  const uint8_t full = (b & EV_ECO) ? 60 : 255;   // ECO = หรี่ไฟ

  if (b & EV_OTA_ACTIVE) return ((now / 50) & 1) ? full : 0;

  if (s_blinkReq) {             // คำขอใหม่จาก task อื่น
    s_blinkCount = s_blinkReq;
    s_blinkReq = 0;
    s_blinkStart = now;
  }
  if (s_blinkCount) {
    const uint32_t t = now - s_blinkStart;
    if (t < (uint32_t)s_blinkCount * 300) return (t % 300) < 120 ? full : 0;
    s_blinkCount = 0;
  }

  if (s_verdict) {
    const uint32_t t = now - s_verdictAt;
    switch ((lie::Verdict)s_verdict) {
      case lie::Verdict::Lie:
        if (t < 3000) return ((now / 60) & 1) ? full : 0;
        break;
      case lie::Verdict::Truth:
        if (t < 1500) return full;
        break;
      case lie::Verdict::Inconclusive:
        if (t < 1600) return (t % 800) < 400 ? full : 0;
        break;
      case lie::Verdict::Invalid:
        if (t < 1200) return (t % 300) < 120 ? full : 0;
        break;
      default:
        break;
    }
    s_verdict = 0;
  }

  if (b & EV_LOW_BATT) {
    const uint32_t t = now % 2000;
    if (t < 900) return (t % 300) < 100 ? full : 0;
  }

  const bool beat = now < s_beatUntil;
  const EngineSnap es = app::getEngineSnap();
  if (es.state == (uint8_t)lie::State::Baseline) {
    // หายใจเข้า-ออก (รอบละ 2 s) = กำลังวัด baseline ให้นั่งนิ่ง ๆ
    const float x = 0.5f - 0.5f * cosf(2.0f * 3.14159f * (now % 2000) / 2000.0f);
    return (uint8_t)(full * x);
  }
  if (es.state == (uint8_t)lie::State::Question) return beat ? full : full / 6;
  if (beat) return full;
  if (b & EV_PPG_CONTACT) return 0;              // แตะอยู่: กระพริบเฉพาะตอนหัวใจเต้น
  return (now % 2000) < 60 ? full : 0;           // รอ: ติดสั้น ๆ ทุก 2 วินาที (มองเห็นชัด = เฟิร์มแวร์ยังทำงาน)
}

const char* buttonAction() {
  const char* act = "none";
  xSemaphoreTake(app::engineMutex, portMAX_DELAY);
  const lie::State st = app::engine.state();
  if (st == lie::State::Idle) {
    if (app::engine.startBaseline(0)) act = "baseline";
  } else if (st == lie::State::Ready) {
    if (app::engine.startQuestion(s_buttonQid, lie::Kind::Test)) {
      act = "question";
      s_buttonQid = (s_buttonQid >= 999) ? 900 : s_buttonQid + 1;
    }
  } else if (st == lie::State::Question) {
    if (app::engine.markAnswer(true)) act = "answer";
  }
  xSemaphoreGive(app::engineMutex);
  return act;
}

void sendButtonEvent(const char* gesture, const char* act) {
  char js[128];
  snprintf(js, sizeof(js), "{\"t\":\"e\",\"eid\":%lu,\"ev\":\"button\",\"g\":\"%s\",\"act\":\"%s\"}",
           (unsigned long)app::nextEventId(), gesture, act);
  app::pushEvent(js);
  app::logEvent("BUTTON", "%s -> %s", gesture, act);
}

void handleClicks(uint8_t n) {
  app::touchActivity();
  if (n == 1) {
    sendButtonEvent("click", buttonAction());
  } else if (n == 2) {
    const uint8_t m = mlrt::mode() == mlrt::MODE_TRAIN ? mlrt::MODE_DETECT : mlrt::MODE_TRAIN;
    mlrt::setMode(m);
    s_blinkReq = m == mlrt::MODE_TRAIN ? 1 : 2;   // 1 ครั้ง = เก็บข้อมูล, 2 ครั้ง = ใช้งานจริง
    sendButtonEvent("double", m == mlrt::MODE_TRAIN ? "mode_train" : "mode_detect");
  } else {
    const Vitals v = app::getVitals();
    uint8_t bars = v.battPresent ? (uint8_t)((v.battPct + 19) / 20) : 0;
    if (bars == 0) bars = 1;
    s_blinkReq = bars;
    sendButtonEvent("triple", "battery");
  }
}

void handleHold(uint8_t level) {
  app::touchActivity();
  if (level == 1) {
    xSemaphoreTake(app::engineMutex, portMAX_DELAY);
    if (app::engine.state() == lie::State::Question) app::engine.abort();
    bool ok = app::engine.startBaseline(0);
    xSemaphoreGive(app::engineMutex);
    sendButtonEvent("hold2s", ok ? "baseline" : "busy");
  } else if (level == 2) {
    sendButtonEvent("hold5s", "light_sleep");
    power::requestLightSleep(0);
  } else {
    sendButtonEvent("hold10s", "standby");
    power::requestDeepSleep(0);
  }
}

void pollButton(uint32_t now) {
  const bool raw = digitalRead(PIN_BUTTON) == LOW;   // กด = LOW (ปุ่มต่อลง GND)
  if (raw != s_raw) {
    s_raw = raw;
    s_rawChange = now;
  }
  if (now - s_rawChange < DEBOUNCE_MS) return;       // หน้าสัมผัสยังเด้งอยู่
  if (raw != s_stable) {
    s_stable = raw;
    if (raw) {                                       // เพิ่งกด
      s_pressStart = now;
      s_holdLevel = 0;
      s_presses++;
    } else {                                         // เพิ่งปล่อย
      if (s_holdLevel == 0) {
        s_clicks++;
        s_lastRelease = now;
      } else {
        handleHold(s_holdLevel);
        s_clicks = 0;
      }
    }
  }
  if (s_stable) {                                    // กดค้างอยู่: บอกระดับด้วยไฟ
    const uint32_t held = now - s_pressStart;
    const uint8_t lvl = held >= 10000 ? 3 : held >= 5000 ? 2 : held >= 2000 ? 1 : 0;
    if (lvl > s_holdLevel) {
      s_holdLevel = lvl;
      s_blinkReq = lvl;
    }
  }
  if (s_clicks && !s_stable && now - s_lastRelease > MULTI_CLICK_MS) {
    handleClicks(s_clicks);
    s_clicks = 0;
  }
}
}  // namespace

// =====================================================================
void begin() {
  pinMode(PIN_BUTTON, INPUT_PULLUP);
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  ledcAttach(PIN_LED, 5000, 8);
#else
  ledcSetup(LEDC_CH, 5000, 8);      // 5 kHz, 8 บิต (0–255)
  ledcAttachPin(PIN_LED, LEDC_CH);
#endif
  ledWrite(0);
}

void task(void*) {
  wdt::subscribe();
  for (;;) {
    // รอ notification จาก sensorTask (หัวใจเต้น) แต่ไม่เกิน 20 ms -> LED ตรงจังหวะจริง
    const uint32_t beats = ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(20));
    const uint32_t t0 = micros();
    wdt::feed();
    app::heartbeat(T_UI);
    const uint32_t now = millis();
    if (beats) s_beatUntil = now + 70;
    pollButton(now);
    ledWrite(computeLed(now));
    app::addBusy(T_UI, micros() - t0);
  }
}

void bootBlink(uint8_t times) {
  for (uint8_t i = 0; i < times; i++) {
    ledWrite(255);
    delay(times > 4 ? 60 : 120);
    ledWrite(0);
    delay(times > 4 ? 90 : 180);
  }
}

void wifiSignal(bool ok) {
  if (ok) {
    ledWrite(255);
    delay(600);
    ledWrite(0);
  } else {
    for (int i = 0; i < 10; i++) {
      ledWrite(255);
      delay(50);
      ledWrite(0);
      delay(50);
    }
  }
}

void showVerdict(lie::Verdict v) {
  s_verdictAt = millis();
  s_verdict = (uint8_t)v;
}

void blink(uint8_t times) { s_blinkReq = times; }

void ledOff() {
  s_off = true;
  ledWrite(0);
}

uint32_t buttonPresses() { return s_presses; }

}  // namespace ui
