// =====================================================================
//  watchdog.h — Watchdog 3 ชั้น + "กล่องดำ" บอกสาเหตุการรีเซ็ตครั้งก่อน
//
//  ชั้น 1  Interrupt WDT (IWDT, ของ ESP-IDF, 300 ms)
//          จับ interrupt/critical section ที่ค้าง -> รีเซ็ตทันที
//  ชั้น 2  Task WDT (TWDT, 8 s, panic=true)
//          ทุก task ของเราต้องเรียก esp_task_wdt_reset() ทุกรอบ ถ้า task ไหนเงียบ 8 s
//          -> panic พิมพ์ชื่อ task ที่ค้าง -> บันทึก core dump -> รีเซ็ต
//  ชั้น 3  Timer-interrupt WDT (12 s) — แบบเดียวกับตัวอย่างในสไลด์บทที่ 12
//          ใช้ hardware timer 1: supervisor จะ "ป้อน" (timerWrite(t,0)) ก็ต่อเมื่อ
//          ทุก task รายงานตัว (heartbeat) ภายใน 5 s เท่านั้น -> ถ้าไม่ป้อน ISR จะรีเซ็ตเครื่อง
//
//  หลังรีเซ็ต: อ่าน esp_reset_reason() + RTC_NOINIT record + สรุป core dump จากแฟลช
//  -> แสดงบนแดชบอร์ดว่า "ครั้งก่อนรีเซ็ตเพราะอะไร task ไหน"
// =====================================================================
#pragma once
#include <Arduino.h>
#include "esp_system.h"

namespace wdt {

struct BootInfo {
  esp_reset_reason_t reason = ESP_RST_UNKNOWN;
  uint32_t wakeCause = 0;            // esp_sleep_wakeup_cause_t
  bool     planned = false;          // มีบันทึกใน RTC black box
  uint32_t plannedReason = 0;        // PlannedReason (storage.h)
  char     plannedTask[16] = {0};
  uint32_t prevUptimeS = 0;
  uint32_t plannedValue = 0;
  bool     coredump = false;         // มี core dump จากการล่มครั้งก่อน
  char     cdTask[16] = {0};
  uint32_t cdPc = 0;
  bool     otaInvalidExists = false; // เคยมีเฟิร์มแวร์ OTA ถูกตีว่าเสีย (rollback)
};

void captureBootInfo();                         // เรียกต้น setup() ก่อนสร้าง task
const BootInfo& bootInfo();
const char* resetReasonName(esp_reset_reason_t r);
const char* resetReasonThai(esp_reset_reason_t r);
const char* plannedReasonName(uint32_t pr);
bool isWdtReset(esp_reset_reason_t r);

// ---- Task WDT ----
void beginTaskWdt();
void subscribe();                               // ให้ TWDT เฝ้า task ที่เรียก
void feed();

// ---- Timer-interrupt WDT ----
void beginHwWdt();
void feedHw();
void setHwPaused(bool paused);                  // พักไว้ตอน light sleep
bool hwRunning();
uint32_t hwFeeds();
uint32_t hwLastFeedMs();
void setSuspect(const char* taskName);          // task ที่เงียบ (จดไว้ในกล่องดำถ้า ISR ยิง)

// ---- สาธิตการทำงานของ watchdog (สำหรับนำเสนอ) ----
enum Demo : uint8_t { DEMO_NONE = 0, DEMO_TWDT, DEMO_HWWDT, DEMO_PANIC, DEMO_INTWDT };
bool requestDemo(Demo d);                       // ตั้งธง -> task ที่เกี่ยวข้องจะลงมือ
Demo pendingDemo();
void clearDemo();
const char* demoName(Demo d);
bool demoFromName(const char* s, Demo& out);
[[noreturn]] void crashNow(Demo d, const char* task);   // ลงมือ (ไม่กลับมา)

}  // namespace wdt
