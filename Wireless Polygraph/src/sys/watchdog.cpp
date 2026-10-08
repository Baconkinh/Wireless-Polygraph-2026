// =====================================================================
//  watchdog.cpp — Watchdog 3 ชั้น + กล่องดำบอกสาเหตุการรีเซ็ต (ภาพรวมใน watchdog.h)
//  ทำอะไร   : ชั้น 1 Task WDT 8 s (ทุก task ต้อง feed), ชั้น 2 Interrupt WDT (ของ ESP-IDF),
//             ชั้น 3 hardware timer WDT 12 s ที่ supervisor ป้อนหลังตรวจ heartbeat ของทุก task,
//             captureBootInfo() อ่านเหตุผลรีเซ็ต + กล่องดำ RTC_NOINIT + core dump, ฟังก์ชันสาธิต
//  เรียกจาก : main.cpp, tasks.cpp (ทุก task), cli.cpp/web_server.cpp (สาธิต)
//  วิชา     : Watchdog (kicking the dog), Timer interrupt
// =====================================================================
#include "watchdog.h"
#include "esp_task_wdt.h"
#include "esp_sleep.h"
#include "esp_timer.h"
#include "esp_ota_ops.h"
#include "esp_core_dump.h"
#include "esp_rom_sys.h"
#include "storage.h"
#include "../config.h"

namespace wdt {

namespace {
BootInfo s_boot;
hw_timer_t* s_hwTimer = nullptr;
volatile bool s_hwPaused = false;
volatile uint32_t s_hwFeeds = 0;
volatile uint32_t s_hwLastFeed = 0;
const char* volatile s_suspect = "supervisor";
volatile uint8_t s_demo = DEMO_NONE;

// คัดลอกชื่อ task ลงกล่องดำแบบไม่ล้น buffer
void copyName(char* dst, size_t n, const char* src) {
  size_t i = 0;
  if (src)
    for (; i + 1 < n && src[i]; i++) dst[i] = src[i];
  dst[i] = 0;
}

// ISR ของ timer watchdog — ห้ามใช้ Serial/printf ธรรมดาใน ISR
// เขียนกล่องดำด้วยมือ แล้วสั่ง abort() (panic handler ทำงานได้จากทุก context และบันทึก core dump)
// ไม่ใช้ esp_restart() เหมือนในสไลด์ เพราะ esp_restart() จะเรียก esp_wifi_stop()
// ซึ่งรอ mutex -> ห้ามเรียกใน ISR (assert ล้มก่อนจะรีเซ็ตเรียบร้อย)
void IRAM_ATTR hwWdtIsr() {
  rtcRecord.magic = 0xC0FFEE42;
  rtcRecord.reason = (s_demo == DEMO_HWWDT) ? PR_DEMO_HWWDT : PR_HW_WDT;
  const char* s = s_suspect;
  size_t i = 0;
  for (; s && i < sizeof(rtcRecord.task) - 1 && s[i]; i++) rtcRecord.task[i] = s[i];
  rtcRecord.task[i] = 0;
  rtcRecord.uptimeS = (uint32_t)(esp_timer_get_time() / 1000000);
  rtcRecord.value = HWWDT_TIMEOUT_MS;
  rtcRecord.check = rtcRecord.magic ^ rtcRecord.reason ^ rtcRecord.uptimeS ^ rtcRecord.value;
  esp_rom_printf("\n[HW-WDT] timer watchdog fired (suspect: %s) -> reset\n", rtcRecord.task);
  abort();
}
}  // namespace

// =====================================================================
void captureBootInfo() {
  s_boot.reason = esp_reset_reason();
  s_boot.wakeCause = (uint32_t)esp_sleep_get_wakeup_cause();

  RtcRecord r;
  s_boot.planned = storage::takePlanned(r);
  if (s_boot.planned) {
    s_boot.plannedReason = r.reason;
    copyName(s_boot.plannedTask, sizeof(s_boot.plannedTask), r.task);
    s_boot.prevUptimeS = r.uptimeS;
    s_boot.plannedValue = r.value;
  }

#if CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH && CONFIG_ESP_COREDUMP_DATA_FORMAT_ELF
  // core dump ถูกเขียนลงพาร์ทิชัน "coredump" ตอน panic -> อ่านสรุปแล้วลบทิ้ง
  if (esp_core_dump_image_check() == ESP_OK) {
    esp_core_dump_summary_t* sum = (esp_core_dump_summary_t*)malloc(sizeof(esp_core_dump_summary_t));
    if (sum) {
      if (esp_core_dump_get_summary(sum) == ESP_OK) {
        s_boot.coredump = true;
        copyName(s_boot.cdTask, sizeof(s_boot.cdTask), sum->exc_task);
        s_boot.cdPc = sum->exc_pc;
      }
      free(sum);
    }
    esp_core_dump_image_erase();   // กันรายงานการล่มเดิมซ้ำทุกครั้งที่บูต
  }
#endif
  s_boot.otaInvalidExists = esp_ota_get_last_invalid_partition() != nullptr;
}

const BootInfo& bootInfo() { return s_boot; }

// สาเหตุการรีเซ็ตของชิปเป็นข้อความอังกฤษ
const char* resetReasonName(esp_reset_reason_t r) {
  switch (r) {
    case ESP_RST_POWERON:   return "POWERON";
    case ESP_RST_EXT:       return "EXTERNAL";
    case ESP_RST_SW:        return "SOFTWARE";
    case ESP_RST_PANIC:     return "PANIC";
    case ESP_RST_INT_WDT:   return "INT_WDT";
    case ESP_RST_TASK_WDT:  return "TASK_WDT";
    case ESP_RST_WDT:       return "OTHER_WDT";
    case ESP_RST_DEEPSLEEP: return "DEEPSLEEP";
    case ESP_RST_BROWNOUT:  return "BROWNOUT";
    case ESP_RST_SDIO:      return "SDIO";
    default:                return "UNKNOWN";
  }
}

// สาเหตุการรีเซ็ตเป็นคำอธิบายภาษาไทย (แสดงในหน้าเว็บ/Serial)
const char* resetReasonThai(esp_reset_reason_t r) {
  switch (r) {
    case ESP_RST_POWERON:   return "เพิ่งจ่ายไฟ (ถ้าเกิดเองระหว่างใช้ = ไฟกระตุก/หน้าสัมผัสหลวม)";
    case ESP_RST_EXT:       return "กดปุ่ม RESET";
    case ESP_RST_SW:        return "ซอฟต์แวร์สั่งรีสตาร์ท";
    case ESP_RST_PANIC:     return "โปรแกรมล่ม (exception/abort)";
    case ESP_RST_INT_WDT:   return "Interrupt Watchdog: interrupt ค้าง";
    case ESP_RST_TASK_WDT:  return "Task Watchdog: มี task ค้าง";
    case ESP_RST_WDT:       return "Watchdog อื่น";
    case ESP_RST_DEEPSLEEP: return "ตื่นจาก Deep Sleep";
    case ESP_RST_BROWNOUT:  return "ไฟตก (Brownout) — แบตอ่อน/สายไฟหลวม";
    default:                return "ไม่ทราบสาเหตุ";
  }
}

// สาเหตุที่ตั้งใจรีเซ็ต (จากกล่องดำ) เป็นข้อความ
const char* plannedReasonName(uint32_t pr) {
  switch (pr) {
    case PR_USER_RESTART: return "user_restart";
    case PR_OTA:          return "ota_update";
    case PR_HW_WDT:       return "hw_timer_wdt";
    case PR_DEMO_TWDT:    return "demo_task_wdt";
    case PR_DEMO_HWWDT:   return "demo_hw_timer_wdt";
    case PR_DEMO_PANIC:   return "demo_panic";
    case PR_DEMO_INTWDT:  return "demo_int_wdt";
    case PR_FACTORY:      return "factory_reset";
    case PR_CONFIG:       return "config_change";
    default:              return "none";
  }
}

// รีเซ็ตเพราะ watchdog ชั้นใดชั้นหนึ่งหรือไม่
bool isWdtReset(esp_reset_reason_t r) {
  return r == ESP_RST_INT_WDT || r == ESP_RST_TASK_WDT || r == ESP_RST_WDT;
}

// ---------------- Task WDT ----------------
void beginTaskWdt() {
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  esp_task_wdt_config_t c;
  c.timeout_ms = TWDT_TIMEOUT_S * 1000;
  c.idle_core_mask = 0;          // ไม่เฝ้า IDLE task (เราเฝ้า task ของเราเอง)
  c.trigger_panic = true;
  esp_task_wdt_reconfigure(&c);
#else
  // IDF 4.4: ถ้า TWDT ถูก init แล้ว ฟังก์ชันนี้จะ "ปรับ" timeout/panic ให้ (ดูคอมเมนต์ใน esp_task_wdt.h)
  esp_task_wdt_init(TWDT_TIMEOUT_S, true);
#endif
}

void subscribe() { esp_task_wdt_add(NULL); }
void feed() { esp_task_wdt_reset(); }

// ---------------- Timer-interrupt WDT ----------------
void beginHwWdt() {
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  s_hwTimer = timerBegin(1000000);                      // 1 MHz -> 1 tick = 1 µs
  timerAttachInterrupt(s_hwTimer, &hwWdtIsr);
  timerAlarm(s_hwTimer, (uint64_t)HWWDT_TIMEOUT_MS * 1000ULL, false, 0);
#else
  s_hwTimer = timerBegin(1, 80, true);                  // timer 1, APB 80 MHz / 80 = 1 MHz
  timerAttachInterrupt(s_hwTimer, &hwWdtIsr, false);
  timerAlarmWrite(s_hwTimer, (uint64_t)HWWDT_TIMEOUT_MS * 1000ULL, false);
  timerAlarmEnable(s_hwTimer);
#endif
  s_hwLastFeed = millis();
}

// ป้อน hardware timer watchdog (นับใหม่จาก 0) — supervisor เรียกเมื่อทุก task ยังมีชีวิต
void feedHw() {
  if (!s_hwTimer || s_hwPaused) return;
  if (s_demo == DEMO_HWWDT) return;                     // สาธิต: แกล้งไม่ป้อน
  timerWrite(s_hwTimer, 0);                             // "ป้อนหมา" = นับใหม่จาก 0
  s_hwFeeds++;
  s_hwLastFeed = millis();
}

// หยุด/เดิน hardware watchdog ชั่วคราว (ระหว่างหลับ)
void setHwPaused(bool paused) {
  if (!s_hwTimer) return;
  s_hwPaused = paused;
  if (paused) timerStop(s_hwTimer);
  else {
    timerWrite(s_hwTimer, 0);
    timerStart(s_hwTimer);
  }
}

bool hwRunning() { return s_hwTimer != nullptr && !s_hwPaused; }
uint32_t hwFeeds() { return s_hwFeeds; }
uint32_t hwLastFeedMs() { return s_hwLastFeed; }
void setSuspect(const char* name) { s_suspect = name; }

// ---------------- Demo ----------------
bool requestDemo(Demo d) {
  if (s_demo != DEMO_NONE || d == DEMO_NONE) return false;
  s_demo = d;
  return true;
}
Demo pendingDemo() { return (Demo)s_demo; }
void clearDemo() { s_demo = DEMO_NONE; }

// ชื่อการสาธิต watchdog เป็นข้อความ
const char* demoName(Demo d) {
  switch (d) {
    case DEMO_TWDT:   return "twdt";
    case DEMO_HWWDT:  return "hwwdt";
    case DEMO_PANIC:  return "panic";
    case DEMO_INTWDT: return "intwdt";
    default:          return "none";
  }
}

// แปลงชื่อการสาธิตจาก API เป็น enum
bool demoFromName(const char* s, Demo& out) {
  if (!s) return false;
  if (!strcmp(s, "twdt"))   { out = DEMO_TWDT; return true; }
  if (!strcmp(s, "hwwdt"))  { out = DEMO_HWWDT; return true; }
  if (!strcmp(s, "panic"))  { out = DEMO_PANIC; return true; }
  if (!strcmp(s, "intwdt")) { out = DEMO_INTWDT; return true; }
  return false;
}

// ทำให้ระบบค้าง/ล่มตามแบบที่สาธิต (เพื่อดู watchdog แต่ละชั้นทำงาน)
void crashNow(Demo d, const char* task) {
  Serial.flush();
  switch (d) {
    case DEMO_TWDT:
      storage::setPlanned(PR_DEMO_TWDT, task, TWDT_TIMEOUT_S);
      // วนค้างโดยไม่เรียก esp_task_wdt_reset() -> TWDT จะ panic ใน 8 s
      // (ต้องมี volatile ข้างใน: ลูปว่างไม่มี side effect เป็น undefined behavior ใน C++11)
      for (;;) {
        volatile uint32_t spin = 0;
        spin++;
      }
    case DEMO_PANIC:
      storage::setPlanned(PR_DEMO_PANIC, task, 0);
      // เขียนลงแอดเดรส 0 -> CPU exception "Store access fault" -> panic + core dump
      *(volatile uint32_t*)0 = 0xDEAD;
      break;
    case DEMO_INTWDT:
      storage::setPlanned(PR_DEMO_INTWDT, task, CONFIG_ESP_INT_WDT_TIMEOUT_MS);
      portDISABLE_INTERRUPTS();          // ปิด interrupt ทั้งหมดแล้ววนค้าง -> IWDT ยิงใน 300 ms
      for (;;) {
        volatile uint32_t spin = 0;
        spin++;
      }
    default:
      break;
  }
  abort();
}

}  // namespace wdt
