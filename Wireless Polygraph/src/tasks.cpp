// =====================================================================
//  tasks.cpp — สร้าง FreeRTOS task ทั้ง 6 ตัว + hardware timer 100 Hz (ตารางอยู่ใน tasks.h)
//  ทำอะไร   : onTick() (ISR) ปลุก sensorTask -> sensorTask อ่านเซนเซอร์ -> engineTask ป้อน frame ให้ LieEngine
//             -> ได้ผลแล้วส่ง eventQueue (ไป UDP) + logQueue (ไปเขียนแฟลช) -> supervisorTask เขียนแฟลช/เฝ้า task อื่น
//  ทำไม     : แยกงานตามความเร่งด่วน (priority) งานที่ต้องตรงเวลาไม่ถูกงานช้า (เขียนแฟลช/WiFi) ถ่วง
//             ISR สั้นที่สุด (แค่ส่ง notification) = deferred interrupt processing
//  เรียกจาก : main.cpp setup() -> tasks::startAll()
//  วิชา     : Interrupt, Hardware timer, RTOS task/priority/queue, Watchdog (ทุก task เรียก wdt::feed)
// =====================================================================
#include "tasks.h"
#include <time.h>
#include "esp_task_wdt.h"
#include "app.h"
#include "config.h"
#include "sensors.h"
#include "net/telemetry.h"
#include "net/web_server.h"
#include "sys/storage.h"
#include "sys/watchdog.h"
#include "sys/power.h"
#include "sys/ota.h"
#include "sys/sysinfo.h"
#include "sys/ui.h"
#include "sys/ml_runtime.h"
#if ARDUINO_USB_CDC_ON_BOOT && ARDUINO_USB_MODE
#include "HWCDC.h"
#endif

namespace tasks {

namespace {
hw_timer_t* s_tickTimer = nullptr;
volatile uint32_t s_missed = 0;

// ---------------- ISR ของ timer สุ่มสัญญาณ (100 Hz) ----------------
// งานใน ISR ต้องสั้นที่สุด: แค่ "ปลุก" sensorTask ด้วย task notification
// (deferred interrupt processing — งานหนักไปทำใน task ที่มี priority สูง)
void IRAM_ATTR onTick() {
  BaseType_t woken = pdFALSE;
  TaskHandle_t h = app::tasks[T_SENSOR].handle;
  if (h) vTaskNotifyGiveFromISR(h, &woken);
  if (woken) portYIELD_FROM_ISR();   // สลับไป sensorTask ทันทีหลังออกจาก ISR
}

// ---------------- sensorTask ----------------
void sensorTask(void*) {
  wdt::subscribe();
  for (;;) {
    if (ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(50)) == 0) s_missed++;   // timer ไม่มา = ผิดปกติ
    const uint32_t t0 = micros();
    wdt::feed();
    app::heartbeat(T_SENSOR);
    if (wdt::pendingDemo() == wdt::DEMO_TWDT) wdt::crashNow(wdt::DEMO_TWDT, "sensor");
    if (sensors::stopRequested()) {        // กำลังจะ deep sleep: หยุดที่จุดปลอดภัย
      esp_task_wdt_delete(NULL);           // เลิกให้ TWDT เฝ้า (ไม่งั้นจะนึกว่าค้าง)
      sensors::ackStop();
      vTaskSuspend(NULL);
    }
    sensors::tick();
    app::addBusy(T_SENSOR, micros() - t0);
  }
}

// ---------------- engineTask ----------------
void resultEvent(const lie::Result& r) {
  using lie::Engine;
  char js[sizeof(EventMsg::json)];
  snprintf(js, sizeof(js),
           "{\"t\":\"e\",\"eid\":%lu,\"ev\":\"result\",\"seq\":%lu,\"qid\":%u,\"kind\":\"%s\","
           "\"verdict\":\"%s\",\"exp\":\"%s\",\"cor\":%d,\"p\":%.3f,\"score\":%.2f,\"q\":%u,\"rs\":%u,"
           "\"ok\":%u,\"f\":[%.4f,%.2f,%.3f,%.3f,%.3f],\"z\":[%.2f,%.2f,%.2f,%.2f,%.2f],"
           "\"lat\":%.1f,\"ans\":%.1f,\"ay\":%d,\"src\":%u,\"md\":%u}",
           (unsigned long)app::nextEventId(), (unsigned long)r.seq, r.qid, Engine::kindName(r.kind),
           Engine::verdictName(r.verdict), Engine::verdictName(r.expected), r.correct ? 1 : 0,
           (double)r.pLie, (double)r.score, r.quality, r.reasons, r.okMask, (double)r.feat[0],
           (double)r.feat[1], (double)r.feat[2], (double)r.feat[3], (double)r.feat[4], (double)r.z[0],
           (double)r.z[1], (double)r.z[2], (double)r.z[3], (double)r.z[4], (double)r.peakLatency,
           (double)r.answerAt, (int)r.answerYes, r.source, mlrt::mode());
  app::pushEvent(js);
}

// แปลงผล 1 ข้อเป็นบรรทัด CSV แล้วส่งเข้า logQueue (supervisor เขียนลง /results.csv)
void resultCsv(const lie::Result& r) {
  using lie::Engine;
  LogMsg m;
  strcpy(m.type, "RESULT");
  const time_t now = time(nullptr);
  snprintf(m.text, sizeof(m.text),
           "%ld,%lu,%lu,%u,%s,%s,%.3f,%.2f,%u,%u,%.4f,%.2f,%.3f,%.3f,%.3f,%.2f,%.2f,%.2f,%.2f,%.2f",
           (long)(now > 1600000000 ? now : 0), (unsigned long)(millis() / 1000), (unsigned long)r.seq,
           r.qid, Engine::kindName(r.kind), Engine::verdictName(r.verdict), (double)r.pLie,
           (double)r.score, r.quality, r.reasons, (double)r.feat[0], (double)r.feat[1],
           (double)r.feat[2], (double)r.feat[3], (double)r.feat[4], (double)r.z[0], (double)r.z[1],
           (double)r.z[2], (double)r.z[3], (double)r.z[4]);
  xQueueSend(app::logQueue, &m, 0);
}

// นับสถิติสะสม: จำนวนคำถาม/โกหก/จริง/ไม่แน่ชัด
void countResult(const lie::Result& r) {
  Stats& s = storage::stats();
  s.questions++;
  switch (r.verdict) {
    case lie::Verdict::Lie: s.lies++; break;
    case lie::Verdict::Truth: s.truths++; break;
    case lie::Verdict::Inconclusive: s.inconclusive++; break;
    default: s.invalid++; break;
  }
}

// task ตัดสิน (priority 4): รับเฟรมจาก frameQueue -> LieEngine -> ถ้าได้ผลใหม่ ส่ง event/CSV/ข้อมูลเทรน/LED
void engineTask(void*) {
  wdt::subscribe();
  Vitals v;
  uint32_t lastSeq = 0;
  for (;;) {
    const bool got = xQueueReceive(app::frameQueue, &v, pdMS_TO_TICKS(500)) == pdTRUE;
    const uint32_t t0 = micros();
    wdt::feed();
    app::heartbeat(T_ENGINE);
    if (!got) continue;   // ไม่มี frame (เช่นช่วง light sleep) แค่รายงานตัวแล้วรอต่อ

    lie::Frame f;
    f.gsr = v.gsr;
    f.gsrOk = v.gsrContact;
    f.hr = v.hr;
    f.ppgOk = v.ppgContact && v.hr > 0.0f;
    f.amp = v.ppgAmp;
    f.tremor = v.tremor;
    f.motion = v.motion;
    f.temp = v.skinTemp;

    lie::Result fresh[3];
    int nFresh = 0;
    xSemaphoreTake(app::engineMutex, portMAX_DELAY);
    const lie::State before = app::engine.state();
    app::engine.push(f);
    const lie::Status st = app::engine.status();
    const uint32_t seq = app::engine.lastSeq();
    while (lastSeq < seq) {
      lastSeq++;
      if (nFresh < 3 && app::engine.resultBySeq(lastSeq, fresh[nFresh])) nFresh++;
    }
    const lie::Baseline bl = app::engine.baseline();
    const char* err = app::engine.lastError();
    xSemaphoreGive(app::engineMutex);

    EngineSnap s;
    s.state = (uint8_t)st.state;
    s.progress = st.progress;
    s.elapsed = st.elapsed;
    s.qid = st.qid;
    s.kind = (uint8_t)st.kind;
    s.stress = st.stress;
    s.settled = st.settled;
    s.baselineValid = st.baselineValid;
    s.calibrated = st.calibrated;
    s.calibWeak = st.calibWeak;
    s.results = st.results;
    s.revision = st.revision;
    s.lastSeq = seq;
    app::setEngineSnap(s);
    if (st.state == lie::State::Baseline || st.state == lie::State::Question) app::setBits(EV_ENGINE_BUSY);
    else app::clearBits(EV_ENGINE_BUSY);

    if (before == lie::State::Baseline && st.state == lie::State::Ready) {
      storage::stats().sessions++;
      char js[220];
      snprintf(js, sizeof(js),
               "{\"t\":\"e\",\"eid\":%lu,\"ev\":\"baseline_done\",\"gsr\":%.3f,\"hr\":%.1f,"
               "\"nullN\":%u,\"gsrFrac\":%.2f,\"ppgFrac\":%.2f}",
               (unsigned long)app::nextEventId(), (double)bl.mean[lie::F_GSR],
               (double)bl.mean[lie::F_HR], bl.nullN, (double)bl.gsrFrac, (double)bl.ppgFrac);
      app::pushEvent(js);
      ui::blink(2);
      app::logEvent("ENGINE", "baseline done: GSR %.3f uS, HR %.1f bpm, %u null windows",
                    (double)bl.mean[lie::F_GSR], (double)bl.mean[lie::F_HR], bl.nullN);
    } else if (before == lie::State::Baseline && st.state == lie::State::Idle) {
      char js[120];
      snprintf(js, sizeof(js), "{\"t\":\"e\",\"eid\":%lu,\"ev\":\"baseline_fail\",\"error\":\"%s\"}",
               (unsigned long)app::nextEventId(), err);
      app::pushEvent(js);
      ui::showVerdict(lie::Verdict::Invalid);
      app::logEvent("ENGINE", "baseline failed: %s", err);
    }
    for (int i = 0; i < nFresh; i++) {
      const lie::Result& r = fresh[i];
      resultEvent(r);
      resultCsv(r);
      countResult(r);
      if (mlrt::mode() == mlrt::MODE_TRAIN) {
        // โหมดเก็บข้อมูล: ข้อที่รู้เฉลย (จริง/โกหก) -> 1 แถวใน /train.csv (supervisor เป็นคนเขียนแฟลช)
        LogMsg tm;
        int label = -1;
        if (mlrt::makeTrainRow(r, tm.text, sizeof(tm.text), label)) {
          snprintf(tm.type, sizeof(tm.type), "TRAIN%d", label);
          xQueueSend(app::logQueue, &tm, 0);
        }
      }
      ui::showVerdict(r.verdict);
      app::logEvent("RESULTQ", "q%u %s -> %s p=%.2f quality=%u%%", r.qid, lie::Engine::kindName(r.kind),
                    lie::Engine::verdictName(r.verdict), (double)r.pLie, r.quality);
    }
    app::addBusy(T_ENGINE, micros() - t0);
  }
}

// ---------------- supervisorTask ----------------
void supervisorTask(void*) {
  wdt::subscribe();
  uint32_t lastTick = millis(), lastCpuUs = micros();
  uint32_t secCount = 0;
  const char* lastStale = nullptr;
  for (;;) {
    // 1) เขียน log ลงแฟลช (ทำที่นี่ที่เดียว: task priority ต่ำสุด การเขียนแฟลชช้าไม่กระทบการวัด)
    LogMsg m;
    if (xQueueReceive(app::logQueue, &m, pdMS_TO_TICKS(200)) == pdTRUE) {
      do {
        if (!strcmp(m.type, "RESULT")) storage::appendResult(m.text);
        else if (!strncmp(m.type, "TRAIN", 5)) {
          if (!storage::appendTrain(m.text, m.type[5] - '0'))
            app::logEvent("ML", "train.csv full or FS error - download and clear the data");
        }
        else storage::appendEvent(m.type, m.text);
        wdt::feed();
      } while (xQueueReceive(app::logQueue, &m, 0) == pdTRUE);
    }
    wdt::feed();
    const uint32_t now = millis();
    if (now - lastTick < 1000) continue;
    lastTick = (now - lastTick > 5000) ? now : lastTick + 1000;   // หลัง light sleep ไม่ต้องไล่ตาม
    secCount++;
    const uint32_t t0 = micros();
    app::heartbeat(T_SUPERVISOR);

    // 2) ทุก task ยังรายงานตัว? -> ป้อน timer watchdog (ไม่ครบ = ไม่ป้อน = ISR รีเซ็ตใน 12 s)
    const char* stale = nullptr;
    for (int i = 0; i < T_COUNT; i++) {
      if (i == T_SUPERVISOR || !app::tasks[i].handle) continue;
      if (now - app::tasks[i].lastBeatMs > HEARTBEAT_STALE_MS) { stale = app::tasks[i].name; break; }
    }
    if (!stale) wdt::feedHw();
    else wdt::setSuspect(stale);
    if (stale && stale != lastStale) app::logEvent("WDT", "task '%s' silent > %lums", stale, (unsigned long)HEARTBEAT_STALE_MS);
    lastStale = stale;

    // 3) CPU% ของแต่ละ task = เวลาที่ task วัดเองว่าทำงาน / เวลาจริงที่ผ่านไป
    const uint32_t nowUs = micros();
    const float elapsedUs = (float)(nowUs - lastCpuUs);
    lastCpuUs = nowUs;
    for (int i = 0; i < T_COUNT; i++) {
      const uint32_t busy = app::tasks[i].busyUs;
      app::tasks[i].cpuPct = elapsedUs > 0 ? 100.0f * (float)(busy - app::tasks[i].prevBusyUs) / elapsedUs : 0;
      app::tasks[i].prevBusyUs = busy;
    }

#if ARDUINO_USB_CDC_ON_BOOT && ARDUINO_USB_MODE
    // 4) ต่อสาย USB กับคอมอยู่ไหม (ตรวจจาก USB SOF) -> เตือนห้ามวัด GSR กับคน
    if (HWCDC::isPlugged()) app::setBits(EV_USB);
    else app::clearBits(EV_USB);
#endif

    // 5) พลังงาน (แบตต่ำ, auto-standby, คำขอ sleep/restart) + ยืนยันเฟิร์มแวร์ OTA
    power::service();
    ota::serviceVerify();

    // 6) งานเป็นรอบ
    if (secCount % 10 == 0) sensors::setChipTempC(temperatureRead());
    if (secCount == 5) {
      sysinfo::cacheSlowValues();
      ota::scanSlots();
    }
    if (secCount % 60 == 0) storage::stats().uptimeMin++;
    if (secCount % 600 == 0) storage::saveStats();   // EEPROM commit ทุก 10 นาที (ถนอมแฟลช)
    app::addBusy(T_SUPERVISOR, micros() - t0);
  }
}

// สร้าง FreeRTOS task 1 ตัว และจดเวลาเริ่ม (supervisor ใช้ตรวจว่า task ยังมีชีวิต)
void create(TaskFunction_t fn, TaskId id, uint32_t stack, UBaseType_t prio) {
  app::tasks[id].lastBeatMs = millis();
  xTaskCreate(fn, app::tasks[id].name, stack, nullptr, prio, &app::tasks[id].handle);
}
}  // namespace

// สร้างทั้ง 6 task ตามลำดับ priority (เรียกครั้งเดียวจาก setup)
void startAll() {
  create(sensorTask, T_SENSOR, STACK_SENSOR, PRIO_SENSOR);
  create(engineTask, T_ENGINE, STACK_ENGINE, PRIO_ENGINE);
  create(telemetry::task, T_TELEMETRY, STACK_TELEMETRY, PRIO_TELEMETRY);
  create(web::task, T_HTTP, STACK_HTTP, PRIO_HTTP);
  create(ui::task, T_UI, STACK_UI, PRIO_UI);
  create(supervisorTask, T_SUPERVISOR, STACK_SUPERVISOR, PRIO_SUPERVISOR);

  // Hardware timer 0: 100 Hz = จังหวะการสุ่มสัญญาณที่แม่นกว่า vTaskDelay
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  s_tickTimer = timerBegin(1000000);
  timerAttachInterrupt(s_tickTimer, &onTick);
  timerAlarm(s_tickTimer, 1000000 / TICK_HZ, true, 0);
#else
  s_tickTimer = timerBegin(0, 80, true);                 // APB 80 MHz / 80 = 1 MHz
  timerAttachInterrupt(s_tickTimer, &onTick, false);
  timerAlarmWrite(s_tickTimer, 1000000 / TICK_HZ, true);  // 10000 µs, auto-reload
  timerAlarmEnable(s_tickTimer);
#endif
}

uint32_t missedTicks() { return s_missed; }

}  // namespace tasks
