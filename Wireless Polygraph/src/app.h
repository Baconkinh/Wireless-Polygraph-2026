// =====================================================================
//  app.h — ข้อมูลกลางที่ทุก task ใช้ร่วมกัน + วัตถุ FreeRTOS สำหรับสื่อสารระหว่าง task
//
//  แผนผังการไหลของข้อมูล (ESP32-C3 คอร์เดียว, priority สูง = ได้ CPU ก่อน):
//
//   [HW Timer ISR 100Hz] --notify--> sensorTask(5) --frameQueue 5Hz--> engineTask(4)
//                                        |  \--waveQueue 10Hz--+            |
//                                        |                     v            v
//                                   liveMutex -----------> telemetryTask(3) <--eventQueue
//                                        |                  (UDP -> โน้ตบุ๊ก)
//                                        v
//                                   httpTask(2): หน้าเว็บ/REST/OTA     uiTask(2): LED+ปุ่ม
//                                   supervisorTask(1): เขียนแฟลช (logQueue), ป้อน HW-WDT,
//                                                      เช็คแบต, sleep, ยืนยัน OTA
//
//  ใช้ครบ: Queue, Mutex, Event Group, Task Notification (จาก ISR และระหว่าง task)
// =====================================================================
#pragma once
#include <Arduino.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/event_groups.h"
#include "config.h"
#include "lie/lie_engine.h"

// ---------------- Event group bits (สถานะระบบแบบ on/off ที่หลาย task อยากรู้) ----------------
enum : uint32_t {
  EV_WIFI_UP     = 1u << 0,
  EV_CLIENT      = 1u << 1,   // มีเครื่องรับ UDP อยู่
  EV_SENSORS_OK  = 1u << 2,
  EV_PPG_CONTACT = 1u << 3,
  EV_GSR_CONTACT = 1u << 4,
  EV_LOW_BATT    = 1u << 5,
  EV_OTA_ACTIVE  = 1u << 6,
  EV_ENGINE_BUSY = 1u << 7,   // กำลังวัด baseline/คำถาม
  EV_ECO         = 1u << 8,
  EV_USB         = 1u << 9,   // ต่อสาย USB กับคอมอยู่ (ห้ามวัด GSR กับคน!)
  EV_TIME_SYNCED = 1u << 10,
};

// ---------------- Flags ใน telemetry (bitmask "fl") ----------------
enum : uint16_t {
  FL_MAX_FAIL   = 1u << 0,    // MAX30102 ไม่ตอบ
  FL_MPU_FAIL   = 1u << 1,    // MPU6050 ไม่ตอบ
  FL_NTC_FAULT  = 1u << 2,    // NTC สายขาด/ลัด
  FL_GSR_SHORT  = 1u << 3,    // แผ่น GSR แตะกันเอง
  FL_PPG_SAT    = 1u << 4,    // สัญญาณ IR ชนเพดาน
  FL_LOW_BATT   = 1u << 5,
  FL_CRIT_BATT  = 1u << 6,
  FL_I2C_RECOV  = 1u << 7,    // เพิ่งกู้บัส I2C (ภายใน 30 s)
  FL_USB        = 1u << 8,    // ต่อ USB อยู่
  FL_MOTION     = 1u << 9,    // ขยับแรงขณะนี้
  FL_OTA_VERIFY = 1u << 10,   // เฟิร์มแวร์ใหม่จาก OTA รอยืนยัน
  FL_TIME_SYNC  = 1u << 11,
  FL_FS_FAIL    = 1u << 12,
  FL_NO_BATT    = 1u << 13,   // ไม่มีแบต/สวิตช์ปิด (ใช้ไฟ USB)
};

// ---------------- ค่าที่วัดได้ล่าสุด (sensorTask เขียน, คนอื่นอ่านผ่าน mutex) ----------------
struct Vitals {
  uint32_t ms = 0;          // uptime ตอนสร้าง frame
  uint32_t seq = 0;         // ลำดับ frame
  // PPG
  bool     ppgContact = false;
  float    hr = 0, hrv = 0, ibi = 0, pi = 0, ppgAmp = 0;
  uint32_t irDc = 0;
  uint32_t beats = 0;
  // EDA / GSR
  bool     gsrContact = false;
  float    gsr = 0, gsrTonic = 0, gsrPhasic = 0, gsrMv = 0;
  uint16_t scrPerMin = 0;
  // อุณหภูมิผิว
  float    skinTemp = NAN;
  // การเคลื่อนไหว
  float    tremor = 0, motion = 0;
  // พลังงาน
  float    vbatMv = 0;
  uint8_t  battPct = 0;
  bool     battPresent = false;
  uint16_t flags = 0;
};

// สถานะของ LieEngine ที่ engineTask คัดลอกออกมาให้ task อื่นอ่าน (ไม่ต้องแย่ง engineMutex)
struct EngineSnap {
  uint8_t  state = 0;       // lie::State
  float    progress = 0;
  float    elapsed = 0;
  uint16_t qid = 0;
  uint8_t  kind = 0;
  int      stress = -1;
  bool     settled = false;
  bool     baselineValid = false;
  bool     calibrated = false;
  bool     calibWeak = false;
  uint32_t results = 0;
  uint32_t revision = 0;
  uint32_t lastSeq = 0;
};

// คลื่น PPG 10 จุด (100 ms) สำหรับกราฟ oscilloscope
struct WaveChunk {
  uint32_t n0;              // ลำดับ sample แรก (ตรวจข้อมูลหายได้)
  uint8_t  count;
  uint16_t beatMask;        // bit i = จุดที่ i เป็นจังหวะหัวใจ
  int16_t  d[WAVE_CHUNK];
};

struct EventMsg { char json[480]; };
// type "RESULT" = บรรทัด CSV ลง /results.csv, อย่างอื่นลง /events.log
struct LogMsg { char type[12]; char text[200]; };

// ---------------- ข้อมูลรายงานตัวของแต่ละ task (heartbeat + CPU) ----------------
enum TaskId : uint8_t { T_SENSOR = 0, T_ENGINE, T_TELEMETRY, T_HTTP, T_UI, T_SUPERVISOR, T_COUNT };
struct TaskStat {
  const char*       name;
  TaskHandle_t      handle;
  volatile uint32_t beats;      // นับรอบการทำงาน
  volatile uint32_t lastBeatMs; // เวลาที่รายงานตัวล่าสุด
  volatile uint32_t busyUs;     // เวลาที่ใช้ CPU สะสม (µs, วนรอบได้)
  float             cpuPct;     // supervisor คำนวณทุก 1 s
  uint32_t          prevBusyUs;
};

namespace app {

extern SemaphoreHandle_t liveMutex;
extern SemaphoreHandle_t engineMutex;
extern SemaphoreHandle_t fsMutex;
extern QueueHandle_t frameQueue;
extern QueueHandle_t waveQueue;
extern QueueHandle_t eventQueue;
extern QueueHandle_t logQueue;
extern EventGroupHandle_t events;
extern TaskStat tasks[T_COUNT];
extern lie::Engine engine;

void createSyncObjects();

void setVitals(const Vitals& v);
Vitals getVitals();
void setEngineSnap(const EngineSnap& s);
EngineSnap getEngineSnap();

inline void heartbeat(TaskId id) {
  tasks[id].beats++;
  tasks[id].lastBeatMs = millis();
}
inline void addBusy(TaskId id, uint32_t us) { tasks[id].busyUs += us; }

// บันทึกเหตุการณ์: พิมพ์ทาง Serial + ส่งให้ supervisor เขียนลง LittleFS (/events.log)
void logEvent(const char* type, const char* fmt, ...) __attribute__((format(printf, 2, 3)));
// ส่ง JSON event ไปหาเครื่องที่รับ UDP (telemetryTask)
void pushEvent(const char* json);
uint32_t nextEventId();

// อ่าน/ตั้ง bit ใน event group (ห่อไว้ให้ปลอดภัยถ้ายังไม่ได้สร้าง)
void setBits(uint32_t b);
void clearBits(uint32_t b);
uint32_t bits();

// กิจกรรมล่าสุด (ใช้ตัดสินใจ auto-standby)
void touchActivity();
uint32_t lastActivityMs();

}  // namespace app
