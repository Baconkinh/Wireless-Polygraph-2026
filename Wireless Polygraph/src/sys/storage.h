// =====================================================================
//  storage.h — หน่วยความจำทุกชนิดตาม flash map ที่อาจารย์สอน
//
//   ┌─────────────────────┬────────────────────────────────────────────────────┐
//   │ NVS (Preferences)   │ ค่าตั้งของผู้ใช้ (เกณฑ์ตัดสิน, เวลา baseline, eco ฯลฯ) │
//   │                     │ key-value, ทนไฟดับ, มี wear-leveling ในตัว            │
//   │ EEPROM emulation    │ สถิติสะสมแบบ struct ก้อนเดียว + CRC32                 │
//   │                     │ (Arduino-ESP32 2.x เก็บเป็น blob ใน NVS namespace     │
//   │                     │  "eeprom" — ต้อง commit() ถึงจะลงแฟลชจริง)            │
//   │ RTC_DATA_ATTR       │ ตัวนับการตื่นจาก deep sleep (หายเมื่อไฟดับ/รีเซ็ต)    │
//   │ RTC_NOINIT_ATTR     │ "กล่องดำ" เหตุผลการรีเซ็ต — รอดจาก WDT/panic/restart  │
//   │ LittleFS            │ log เหตุการณ์ + ผลการสอบสวน (ไฟล์ CSV ดาวน์โหลดได้)   │
//   │ OTA data/app0/app1  │ ดู sys/ota.h                                          │
//   │ coredump            │ ดู sys/watchdog.h                                     │
//   └─────────────────────┴────────────────────────────────────────────────────┘
// =====================================================================
#pragma once
#include <Arduino.h>
#include "../lie/lie_engine.h"
#include "../lie/ml_model.h"

struct Settings {
  // ---- LieEngine ----
  uint16_t baselineSec = 30;
  uint8_t  windowSec = 12;
  uint8_t  preSec = 3;
  float    s0 = 2.0f;
  float    k = 1.5f;
  float    lieP = 0.65f;
  float    truthP = 0.35f;
  float    w[lie::F_COUNT] = {0.20f, 0.35f, 0.25f, 0.12f, 0.08f};   // GSR, HR, AMP, TRM, TMP
  // ---- โหมดการทำงาน (v2.1) ----
  uint8_t  mode = 0;             // 0 = ใช้งานจริง (DETECT), 1 = เก็บข้อมูลเทรน AI (TRAIN)
  uint8_t  wifiPower = 1;        // 0 = ต่ำ 5 dBm, 1 = กลาง 8.5 dBm, 2 = สูง 13 dBm
  // ---- เซนเซอร์ ----
  uint32_t contactThr = 50000;   // IR ดิบที่ถือว่า "แตะผิว"
  uint8_t  irLed = 0x1F;         // กระแส LED IR (หน่วย 0.2 mA)
  uint16_t vccMv = 3300;         // ราง 3V3 จริง (วัดด้วยมิเตอร์แล้วใส่ -> NTC/GSR แม่นขึ้น)
  // ---- พลังงาน ----
  bool     eco = false;
  uint16_t standbyMin = 0;       // ไม่มีใครใช้นานเท่านี้ (นาที) -> deep sleep อัตโนมัติ
                                 // 0 = ปิด (ค่าเริ่มต้น) เพื่อไม่ให้บอร์ดหลับเองตอนทดสอบ/นำเสนอ
                                 // อยากเปิดประหยัดไฟ: set standbyMin 10 (ผ่าน Serial) หรือหน้า Settings
  uint16_t wakeCheckS = 20;      // ตอน standby ตื่นมาเช็คว่าใส่นาฬิกาหรือยังทุกกี่วินาที
  // ---- telemetry ----
  bool     waveform = true;      // ส่งคลื่น PPG 100 Hz ไปกราฟ
};

// สถิติสะสม (เก็บใน EEPROM emulation)
struct Stats {
  uint32_t magic;
  uint16_t version;
  uint16_t size;
  uint32_t bootCount;
  uint32_t wdtResets;      // Task WDT / Interrupt WDT / HW-timer WDT
  uint32_t panicResets;
  uint32_t brownouts;
  uint32_t sessions;       // จำนวน baseline ที่สำเร็จ
  uint32_t questions;
  uint32_t lies;
  uint32_t truths;
  uint32_t inconclusive;
  uint32_t invalid;
  uint32_t uptimeMin;      // รวมทุกครั้งที่เปิดเครื่อง
  uint32_t deepSleeps;
  uint32_t otaUpdates;
  uint8_t  resetHist[8];   // สาเหตุรีเซ็ต 8 ครั้งล่าสุด (esp_reset_reason_t) ไว้ดูอาการไฟตกวนลูป
  uint8_t  resetHead;
  uint8_t  pad[3];
  uint32_t crc;            // CRC32 ของทุกไบต์ก่อนหน้า
};

// "กล่องดำ" ใน RTC memory ที่ไม่ถูกล้างตอนรีเซ็ตแบบ software/WDT
enum PlannedReason : uint32_t {
  PR_NONE = 0,
  PR_USER_RESTART = 1,
  PR_OTA = 2,
  PR_HW_WDT = 3,          // HW-timer watchdog ยิง (ไม่ใช่การสาธิต)
  PR_DEMO_TWDT = 4,
  PR_DEMO_HWWDT = 5,
  PR_DEMO_PANIC = 6,
  PR_DEMO_INTWDT = 7,
  PR_FACTORY = 8,
  PR_CONFIG = 9,
};
struct RtcRecord {
  uint32_t magic;
  uint32_t reason;        // PlannedReason
  char     task[16];      // task ที่เกี่ยวข้อง
  uint32_t uptimeS;
  uint32_t value;
  uint32_t check;         // magic ^ reason ^ uptimeS ^ value (ตรวจว่าไม่ใช่ขยะ)
};

// ข้อมูลใน RTC slow memory ที่รอด deep sleep (ประกาศใน storage.cpp)
enum SleepReason : uint8_t { SLP_NONE = 0, SLP_MANUAL = 1, SLP_STANDBY = 2, SLP_LOWBATT = 3 };
extern uint32_t rtcWakeCount;
extern uint32_t rtcStandbyChecks;
extern uint8_t  rtcSleepReason;
extern uint16_t rtcWakeIntervalS;
extern uint32_t rtcSleepSeconds;      // เวลาหลับสะสม (โดยประมาณจากค่าที่ตั้ง)
extern RtcRecord rtcRecord;

namespace storage {

bool begin();                         // เปิด NVS, EEPROM, LittleFS

// ---- Settings (NVS) ----
Settings& settings();
bool saveSettings();                  // เขียนเฉพาะ key ที่เปลี่ยน (ลดการสึกของแฟลช)
void resetSettings();                 // คืนค่าโรงงาน (ลบ namespace)
void toEngineConfig(const Settings& s, lie::Config& c);
uint32_t nvsUsedEntries();
uint32_t nvsFreeEntries();

// ---- Stats (EEPROM) ----
Stats& stats();
bool saveStats();
void resetStats();

// ---- RTC black box ----
void setPlanned(uint32_t reason, const char* task, uint32_t value);
bool takePlanned(RtcRecord& out);     // อ่านแล้วล้าง (ใช้ตอนบูต)

// ---- LittleFS ----
bool fsOk();
size_t fsUsed();
size_t fsTotal();
void appendEvent(const char* type, const char* text);   // /events.log (หมุนไฟล์เมื่อเกิน 24 KB)
void appendResult(const char* csvLine);                 // /results.csv (หมุนไฟล์เมื่อเกิน 48 KB)
bool readFile(const char* path, String& out, size_t maxBytes);
bool clearLogs();

// ---- ข้อมูลเทรน AI (/train.csv บน LittleFS) ----
extern const char* const kTrainPath;
extern const char* const kTrainHeader;
bool appendTrain(const char* csvLine, int label);    // label 0 = จริง, 1 = โกหก
void trainCounts(uint32_t& truth, uint32_t& lie);
size_t trainBytes();
bool clearTrain();
constexpr size_t TRAIN_MAX = 180 * 1024;              // กันพื้นที่ LittleFS เต็ม (~1,300 ข้อ)

// ---- โมเดล AI (NVS namespace "ml") ----
bool loadModel(ml::Model& m);
bool saveModel(const ml::Model& m);
void clearModel();

}  // namespace storage
