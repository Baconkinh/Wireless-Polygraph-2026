// =====================================================================
//  app.cpp — "กระดานกลาง" ของระบบ: วัตถุ FreeRTOS ที่ทุก task ใช้คุยกัน
//  ทำอะไร   : createSyncObjects() สร้างคิว 4 ตัว (frame/wave/event/log), mutex 3 ตัว (live/engine/fs)
//             และ event group (บิตสถานะ เช่น WiFi ขึ้น, เสียบ USB, แบตต่ำ) + ฟังก์ชัน setVitals/getVitals
//  ทำไม     : task หลายตัวทำงานพร้อมกัน ถ้าอ่าน/เขียนตัวแปรเดียวกันตรง ๆ จะได้ข้อมูลครึ่ง ๆ กลาง ๆ (race condition)
//             จึงส่งข้อมูลผ่านคิว (copy ทั้งก้อน) และล็อกด้วย mutex
//  เรียกจาก : main.cpp setup() (สร้าง) แล้วทุก task ใช้ผ่าน namespace app::
//  วิชา     : Real-time OS — queue, mutex, event group, shared state
// =====================================================================
#include "app.h"
#include <stdarg.h>

namespace app {

SemaphoreHandle_t liveMutex = nullptr;
SemaphoreHandle_t engineMutex = nullptr;
SemaphoreHandle_t fsMutex = nullptr;
QueueHandle_t frameQueue = nullptr;
QueueHandle_t waveQueue = nullptr;
QueueHandle_t eventQueue = nullptr;
QueueHandle_t logQueue = nullptr;
EventGroupHandle_t events = nullptr;
lie::Engine engine;

TaskStat tasks[T_COUNT] = {
    {"sensor", nullptr, 0, 0, 0, 0.0f, 0},     {"engine", nullptr, 0, 0, 0, 0.0f, 0},
    {"telemetry", nullptr, 0, 0, 0, 0.0f, 0},  {"http", nullptr, 0, 0, 0, 0.0f, 0},
    {"ui", nullptr, 0, 0, 0, 0.0f, 0},         {"supervisor", nullptr, 0, 0, 0, 0.0f, 0},
};

static Vitals s_live;
static EngineSnap s_snap;
static volatile uint32_t s_eventId = 0;
static volatile uint32_t s_lastActivity = 0;

void createSyncObjects() {
  liveMutex = xSemaphoreCreateMutex();
  engineMutex = xSemaphoreCreateMutex();
  fsMutex = xSemaphoreCreateMutex();
  frameQueue = xQueueCreate(10, sizeof(Vitals));      // 2 s เผื่อ engine ช้า
  waveQueue = xQueueCreate(8, sizeof(WaveChunk));
  eventQueue = xQueueCreate(6, sizeof(EventMsg));
  logQueue = xQueueCreate(16, sizeof(LogMsg));
  events = xEventGroupCreate();
}

void setVitals(const Vitals& v) {
  if (!liveMutex) { s_live = v; return; }
  xSemaphoreTake(liveMutex, portMAX_DELAY);
  s_live = v;
  xSemaphoreGive(liveMutex);
}

Vitals getVitals() {
  Vitals v;
  if (!liveMutex) return s_live;
  xSemaphoreTake(liveMutex, portMAX_DELAY);
  v = s_live;
  xSemaphoreGive(liveMutex);
  return v;
}

void setEngineSnap(const EngineSnap& s) {
  if (!liveMutex) { s_snap = s; return; }
  xSemaphoreTake(liveMutex, portMAX_DELAY);
  s_snap = s;
  xSemaphoreGive(liveMutex);
}

EngineSnap getEngineSnap() {
  EngineSnap s;
  if (!liveMutex) return s_snap;
  xSemaphoreTake(liveMutex, portMAX_DELAY);
  s = s_snap;
  xSemaphoreGive(liveMutex);
  return s;
}

void logEvent(const char* type, const char* fmt, ...) {
  LogMsg m;
  strncpy(m.type, type, sizeof(m.type) - 1);
  m.type[sizeof(m.type) - 1] = 0;
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(m.text, sizeof(m.text), fmt, ap);
  va_end(ap);
  Serial.printf("[%7.1fs] %-9s %s\n", millis() / 1000.0f, m.type, m.text);
  // ไม่รอ: ถ้าคิวเต็ม (แฟลชช้า) ยอมทิ้ง log ดีกว่าทำให้ task สำคัญค้าง
  if (logQueue) xQueueSend(logQueue, &m, 0);
}

uint32_t nextEventId() { return ++s_eventId; }

void pushEvent(const char* json) {
  if (!eventQueue) return;
  EventMsg m;
  strncpy(m.json, json, sizeof(m.json) - 1);
  m.json[sizeof(m.json) - 1] = 0;
  xQueueSend(eventQueue, &m, 0);
}

void setBits(uint32_t b) { if (events) xEventGroupSetBits(events, (EventBits_t)b); }
void clearBits(uint32_t b) { if (events) xEventGroupClearBits(events, (EventBits_t)b); }
uint32_t bits() { return events ? (uint32_t)xEventGroupGetBits(events) : 0; }

void touchActivity() { s_lastActivity = millis(); }
uint32_t lastActivityMs() { return s_lastActivity; }

}  // namespace app
