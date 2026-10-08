// =====================================================================
//  storage.cpp — หน่วยความจำทุกชนิด (ตารางอยู่ใน storage.h)
//  ทำอะไร   : NVS (Preferences) = ค่าตั้ง + โมเดล AI, EEPROM emulation = สถิติสะสม + CRC32,
//             RTC_DATA/RTC_NOINIT = ตัวนับตอนหลับ + กล่องดำ, LittleFS = log/ผล/ข้อมูลเทรน (หมุนไฟล์เมื่อใหญ่)
//  ทำไม     : ข้อมูลแต่ละแบบต้องการความทนทานต่างกัน (ทนไฟดับ / รอดรีเซ็ต / ไฟล์ใหญ่ดาวน์โหลดได้)
//  เรียกจาก : แทบทุกโมดูล; การเขียนแฟลชทำใน supervisorTask เท่านั้น (ผ่าน logQueue + fsMutex)
//  วิชา     : Memory system (Flash, NVS, SPIFFS/LittleFS, EEPROM, RTC memory)
// =====================================================================
#include "storage.h"
#include <Preferences.h>
#include <EEPROM.h>
#include <LittleFS.h>
#include <nvs.h>
#include <time.h>
#include "esp_attr.h"
#include "../app.h"
#include "watchdog.h"

// ---------------- RTC memory ----------------
// RTC_DATA_ATTR  : ค่าเริ่มต้นถูกโหลดใหม่ทุกครั้งที่บูต "ยกเว้น" ตื่นจาก deep sleep
// RTC_NOINIT_ATTR: ไม่ถูกแตะเลยตอนบูต -> รอดจาก software reset / WDT / panic
//                  (แต่เป็นค่าขยะหลังเสียบไฟใหม่ จึงต้องมี magic + check)
RTC_DATA_ATTR uint32_t rtcWakeCount = 0;
RTC_DATA_ATTR uint32_t rtcStandbyChecks = 0;
RTC_DATA_ATTR uint8_t  rtcSleepReason = SLP_NONE;
RTC_DATA_ATTR uint16_t rtcWakeIntervalS = 20;
RTC_DATA_ATTR uint32_t rtcSleepSeconds = 0;
RTC_NOINIT_ATTR RtcRecord rtcRecord;

namespace storage {

namespace {
constexpr const char* NVS_NS = "poly";
constexpr uint32_t STATS_MAGIC = 0x504C5931;   // "PLY1"
constexpr uint16_t STATS_VERSION = 2;          // v2.1: เพิ่มประวัติสาเหตุรีเซ็ต
constexpr size_t   EEPROM_SIZE = 128;           // >= sizeof(Stats)
constexpr uint32_t RTC_MAGIC = 0xC0FFEE42;
constexpr size_t   EVENTS_MAX = 24 * 1024;
constexpr size_t   RESULTS_MAX = 48 * 1024;

Preferences prefs;
Preferences mlPrefs;
uint32_t s_trainTruth = 0, s_trainLie = 0;
Settings s_settings;
Settings s_saved;          // ค่าที่อยู่ในแฟลชตอนนี้ (เทียบเพื่อเขียนเฉพาะที่เปลี่ยน)
Stats s_stats;
bool s_fsOk = false;
bool s_prefsOk = false;

// CRC32 (IEEE 802.3) แบบบิตต่อบิต — ข้อมูลแค่ ~60 ไบต์ ไม่ต้องใช้ตาราง
uint32_t crc32(const uint8_t* p, size_t n) {
  uint32_t c = 0xFFFFFFFFu;
  while (n--) {
    c ^= *p++;
    for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
  }
  return ~c;
}

// CRC32 ของสถิติสะสม (ตรวจว่าข้อมูลใน EEPROM ไม่เสีย)
uint32_t statsCrc(const Stats& s) {
  return crc32(reinterpret_cast<const uint8_t*>(&s), offsetof(Stats, crc));
}

void lockFs() { if (app::fsMutex) xSemaphoreTake(app::fsMutex, portMAX_DELAY); }
void unlockFs() { if (app::fsMutex) xSemaphoreGive(app::fsMutex); }

// อ่านค่าตั้งจาก NVS (ไม่มี = ค่าเริ่มต้น) — อัปเกรดค่าจากเวอร์ชันเก่าให้อัตโนมัติ
void loadSettings() {
  Settings d;   // ค่าเริ่มต้น
  // v2.1 เปลี่ยนน้ำหนักเริ่มต้น (ลด GSR) -> ถ้าเป็นค่าตั้งจากเวอร์ชันเก่า ให้ลบน้ำหนักเดิมทิ้ง
  if (prefs.getUChar("ver", 1) < 2) {
    const char* old[] = {"w0", "w1", "w2", "w3", "w4", "stby"};
    for (const char* k : old) prefs.remove(k);
    prefs.putUChar("ver", 2);
  }
  s_settings.baselineSec = prefs.getUShort("base_s", d.baselineSec);
  s_settings.windowSec = prefs.getUChar("win_s", d.windowSec);
  s_settings.preSec = prefs.getUChar("pre_s", d.preSec);
  s_settings.s0 = prefs.getFloat("s0", d.s0);
  s_settings.k = prefs.getFloat("k", d.k);
  s_settings.lieP = prefs.getFloat("lieP", d.lieP);
  s_settings.truthP = prefs.getFloat("truthP", d.truthP);
  char key[4] = {'w', '0', 0, 0};
  for (int i = 0; i < lie::F_COUNT; i++) {
    key[1] = (char)('0' + i);
    s_settings.w[i] = prefs.getFloat(key, d.w[i]);
  }
  s_settings.contactThr = prefs.getUInt("thr", d.contactThr);
  s_settings.irLed = prefs.getUChar("irled", d.irLed);
  s_settings.vccMv = prefs.getUShort("vcc", d.vccMv);
  s_settings.eco = prefs.getBool("eco", d.eco);
  s_settings.standbyMin = prefs.getUShort("stby", d.standbyMin);
  s_settings.wakeCheckS = prefs.getUShort("wake", d.wakeCheckS);
  s_settings.waveform = prefs.getBool("wave", d.waveform);
  s_settings.mode = prefs.getUChar("mode", d.mode) ? 1 : 0;
  s_settings.wifiPower = prefs.getUChar("wifipw", d.wifiPower);
  if (s_settings.wifiPower > 2) s_settings.wifiPower = 1;
  s_saved = s_settings;
}
}  // namespace

// =====================================================================
bool begin() {
  // ---- NVS / Preferences ----
  s_prefsOk = prefs.begin(NVS_NS, false);
  if (s_prefsOk) loadSettings();

  // ---- EEPROM emulation ----
  EEPROM.begin(EEPROM_SIZE);
  EEPROM.get(0, s_stats);
  if (s_stats.magic != STATS_MAGIC || s_stats.version != STATS_VERSION ||
      s_stats.size != sizeof(Stats) || s_stats.crc != statsCrc(s_stats)) {
    // ครั้งแรก หรือข้อมูลเสีย (CRC ไม่ตรง) -> เริ่มนับใหม่
    memset(&s_stats, 0, sizeof(s_stats));
    s_stats.magic = STATS_MAGIC;
    s_stats.version = STATS_VERSION;
    s_stats.size = sizeof(Stats);
  }

  // ---- LittleFS (format อัตโนมัติถ้ายังไม่เคยใช้ -> ไม่ต้อง Upload Filesystem ก่อน) ----
  s_fsOk = LittleFS.begin(true);

  // นับข้อมูลเทรนที่มีอยู่แล้ว (คอลัมน์ที่ 4 = label 0/1)
  if (s_fsOk && LittleFS.exists(kTrainPath)) {
    File f = LittleFS.open(kTrainPath, "r");
    if (f) {
      f.readStringUntil('\n');                // ข้ามหัวตาราง
      while (f.available()) {
        String line = f.readStringUntil('\n');
        int c = 0, i = 0;
        for (; i < (int)line.length() && c < 3; i++) if (line[i] == ',') c++;
        if (c == 3 && i < (int)line.length()) {
          if (line[i] == '0') s_trainTruth++;
          else if (line[i] == '1') s_trainLie++;
        }
      }
      f.close();
    }
  }
  mlPrefs.begin("ml", false);
  return s_prefsOk && s_fsOk;
}

// ---------------- Settings ----------------
Settings& settings() { return s_settings; }

// บันทึกเฉพาะค่าที่เปลี่ยน ลง NVS (ลดการเขียนแฟลช)
bool saveSettings() {
  if (!s_prefsOk) return false;
  const Settings& n = s_settings;
  Settings& o = s_saved;
  if (n.baselineSec != o.baselineSec) prefs.putUShort("base_s", n.baselineSec);
  if (n.windowSec != o.windowSec) prefs.putUChar("win_s", n.windowSec);
  if (n.preSec != o.preSec) prefs.putUChar("pre_s", n.preSec);
  if (n.s0 != o.s0) prefs.putFloat("s0", n.s0);
  if (n.k != o.k) prefs.putFloat("k", n.k);
  if (n.lieP != o.lieP) prefs.putFloat("lieP", n.lieP);
  if (n.truthP != o.truthP) prefs.putFloat("truthP", n.truthP);
  char key[4] = {'w', '0', 0, 0};
  for (int i = 0; i < lie::F_COUNT; i++) {
    key[1] = (char)('0' + i);
    if (n.w[i] != o.w[i]) prefs.putFloat(key, n.w[i]);
  }
  if (n.contactThr != o.contactThr) prefs.putUInt("thr", n.contactThr);
  if (n.irLed != o.irLed) prefs.putUChar("irled", n.irLed);
  if (n.vccMv != o.vccMv) prefs.putUShort("vcc", n.vccMv);
  if (n.eco != o.eco) prefs.putBool("eco", n.eco);
  if (n.standbyMin != o.standbyMin) prefs.putUShort("stby", n.standbyMin);
  if (n.wakeCheckS != o.wakeCheckS) prefs.putUShort("wake", n.wakeCheckS);
  if (n.waveform != o.waveform) prefs.putBool("wave", n.waveform);
  if (n.mode != o.mode) prefs.putUChar("mode", n.mode);
  if (n.wifiPower != o.wifiPower) prefs.putUChar("wifipw", n.wifiPower);
  s_saved = s_settings;
  return true;
}

// ล้างค่าตั้งทั้งหมดกลับเป็นค่าเริ่มต้น
void resetSettings() {
  if (s_prefsOk) {
    prefs.clear();
    prefs.putUChar("ver", 2);
  }
  s_settings = Settings();
  s_saved = s_settings;
}

// แปลงค่าตั้งที่เก็บใน NVS เป็น lie::Config ของ LieEngine
void toEngineConfig(const Settings& s, lie::Config& c) {
  c.baselineSec = s.baselineSec;
  c.windowSec = s.windowSec;
  c.preSec = s.preSec;
  c.s0 = s.s0;
  c.k = s.k;
  c.lieP = s.lieP;
  c.truthP = s.truthP;
  for (int i = 0; i < lie::F_COUNT; i++) c.weight[i] = s.w[i];
}

// จำนวนช่องที่ใช้ใน NVS (แสดงในหน้าระบบ)
uint32_t nvsUsedEntries() {
  nvs_stats_t st;
  return nvs_get_stats(NULL, &st) == ESP_OK ? (uint32_t)st.used_entries : 0;
}
// จำนวนช่องว่างใน NVS
uint32_t nvsFreeEntries() {
  nvs_stats_t st;
  return nvs_get_stats(NULL, &st) == ESP_OK ? (uint32_t)st.free_entries : 0;
}

// ---------------- Stats ----------------
Stats& stats() { return s_stats; }

// บันทึกสถิติสะสมลง EEPROM emulation (ต้อง commit ถึงจะลงแฟลชจริง)
bool saveStats() {
  s_stats.crc = statsCrc(s_stats);
  EEPROM.put(0, s_stats);
  return EEPROM.commit();   // ไม่ commit = อยู่แค่ใน RAM, ไฟดับก็หาย
}

// ล้างสถิติสะสม (ยกเว้นจำนวนบูต)
void resetStats() {
  uint32_t boots = s_stats.bootCount;
  memset(&s_stats, 0, sizeof(s_stats));
  s_stats.magic = STATS_MAGIC;
  s_stats.version = STATS_VERSION;
  s_stats.size = sizeof(Stats);
  s_stats.bootCount = boots;   // จำนวนบูตเก็บไว้ (ใช้ดูว่ารีเซ็ตบ่อยไหม)
  saveStats();
}

// ---------------- RTC black box ----------------
void setPlanned(uint32_t reason, const char* task, uint32_t value) {
  rtcRecord.magic = RTC_MAGIC;
  rtcRecord.reason = reason;
  size_t i = 0;
  if (task) for (; i < sizeof(rtcRecord.task) - 1 && task[i]; i++) rtcRecord.task[i] = task[i];
  rtcRecord.task[i] = 0;
  rtcRecord.uptimeS = millis() / 1000;
  rtcRecord.value = value;
  rtcRecord.check = rtcRecord.magic ^ rtcRecord.reason ^ rtcRecord.uptimeS ^ rtcRecord.value;
}

// อ่านแล้วล้าง "กล่องดำ" ใน RTC memory: สาเหตุรีเซ็ตที่ตั้งใจ (เช่น สาธิต watchdog)
bool takePlanned(RtcRecord& out) {
  bool valid = rtcRecord.magic == RTC_MAGIC &&
               rtcRecord.check == (rtcRecord.magic ^ rtcRecord.reason ^ rtcRecord.uptimeS ^
                                   rtcRecord.value);
  if (valid) {
    out = rtcRecord;
    out.task[sizeof(out.task) - 1] = 0;
  }
  rtcRecord.magic = 0;   // ใช้ครั้งเดียว
  return valid;
}

// ---------------- LittleFS ----------------
bool fsOk() { return s_fsOk; }
size_t fsUsed() { return s_fsOk ? LittleFS.usedBytes() : 0; }
size_t fsTotal() { return s_fsOk ? LittleFS.totalBytes() : 0; }

// ไฟล์ log ใหญ่เกินกำหนด -> เปลี่ยนชื่อเป็นไฟล์เก่า แล้วเริ่มไฟล์ใหม่ (กันแฟลชเต็ม)
static void rotateIfBig(const char* path, const char* old, size_t maxBytes) {
  File f = LittleFS.open(path, "r");
  if (!f) return;
  size_t sz = f.size();
  f.close();
  if (sz < maxBytes) return;
  if (LittleFS.exists(old)) LittleFS.remove(old);
  LittleFS.rename(path, old);
}

// ต่อท้าย 1 บรรทัดลง /events.log
void appendEvent(const char* type, const char* text) {
  if (!s_fsOk) return;
  lockFs();
  rotateIfBig("/events.log", "/events.1", EVENTS_MAX);
  File f = LittleFS.open("/events.log", "a");
  if (f) {
    time_t now = time(nullptr);
    f.printf("%lu,%ld,%s,%s\n", (unsigned long)(millis() / 1000),
             (long)(now > 1600000000 ? now : 0), type, text);
    f.close();
  }
  unlockFs();
}

// ต่อท้ายผล 1 ข้อลง /results.csv
void appendResult(const char* csvLine) {
  if (!s_fsOk) return;
  lockFs();
  rotateIfBig("/results.csv", "/results.1", RESULTS_MAX);
  bool isNew = !LittleFS.exists("/results.csv");
  File f = LittleFS.open("/results.csv", "a");
  if (f) {
    if (isNew)
      f.print("epoch,uptime_s,seq,qid,kind,verdict,p_lie,score,quality,reasons,"
              "d_gsr_uS,d_hr_bpm,d_amp_frac,d_tremor,d_temp_C,z_gsr,z_hr,z_amp,z_trm,z_tmp\n");
    f.print(csvLine);
    f.print('\n');
    f.close();
  }
  unlockFs();
}

// อ่านไฟล์จาก LittleFS (ไม่เกิน maxBytes)
bool readFile(const char* path, String& out, size_t maxBytes) {
  if (!s_fsOk) return false;
  lockFs();
  File f = LittleFS.open(path, "r");
  bool ok = false;
  if (f) {
    size_t sz = f.size();
    // ไฟล์ log ยาว -> ส่งเฉพาะส่วนท้าย (เหตุการณ์ล่าสุดสำคัญกว่า)
    if (sz > maxBytes) f.seek(sz - maxBytes);
    out.reserve(sz > maxBytes ? maxBytes : sz);
    while (f.available()) {
      char buf[256];
      int n = f.read((uint8_t*)buf, sizeof(buf));
      if (n <= 0) break;
      out.concat(buf, n);
    }
    f.close();
    ok = true;
  }
  unlockFs();
  return ok;
}

const char* const kTrainPath = "/train.csv";
const char* const kTrainHeader =
    "time,subject,qid,label,label_name,quality,gsr_ok,ppg_ok,"
    "z_gsr,z_hr,z_amp,z_trm,z_tmp,d_gsr,d_hr,d_amp,d_trm,d_tmp,p_model,source";

// ต่อท้ายข้อมูลเทรน 1 ข้อลง /train.csv (เต็ม = ไม่เขียน แจ้งให้ดาวน์โหลดแล้วล้าง)
bool appendTrain(const char* csvLine, int label) {
  if (!s_fsOk) return false;
  bool ok = false;
  lockFs();
  const bool isNew = !LittleFS.exists(kTrainPath);
  size_t sz = 0;
  if (!isNew) {
    File r = LittleFS.open(kTrainPath, "r");
    if (r) { sz = r.size(); r.close(); }
  }
  if (sz < TRAIN_MAX) {              // ไม่หมุนไฟล์ทิ้ง: ข้อมูลเทรนมีค่า ให้ผู้ใช้ดาวน์โหลดแล้วล้างเอง
    File f = LittleFS.open(kTrainPath, "a");
    if (f) {
      if (isNew) { f.print(kTrainHeader); f.print('\n'); }
      f.print(csvLine);
      f.print('\n');
      f.close();
      ok = true;
      if (label == 0) s_trainTruth++;
      else if (label == 1) s_trainLie++;
    }
  }
  unlockFs();
  return ok;
}

// จำนวนข้อมูลเทรนในนาฬิกา (ตอบจริง, โกหก)
void trainCounts(uint32_t& truth, uint32_t& lie) {
  truth = s_trainTruth;
  lie = s_trainLie;
}

// ขนาดไฟล์ /train.csv
size_t trainBytes() {
  if (!s_fsOk) return 0;
  lockFs();
  size_t sz = 0;
  File f = LittleFS.open(kTrainPath, "r");
  if (f) { sz = f.size(); f.close(); }
  unlockFs();
  return sz;
}

// ลบ /train.csv
bool clearTrain() {
  if (!s_fsOk) return false;
  lockFs();
  if (LittleFS.exists(kTrainPath)) LittleFS.remove(kTrainPath);
  s_trainTruth = s_trainLie = 0;
  unlockFs();
  return true;
}

// แถวนี้ตรงกับที่มือถือขอลบไหม: คอลัมน์ 1 (time) และคอลัมน์ 3 (qid) ต้องตรง
static bool trainLineMatches(const String& line, const char* expectTime, long expectQid) {
  const int c1 = line.indexOf(',');
  if (c1 < 0) return false;
  const int c2 = line.indexOf(',', c1 + 1);
  const int c3 = c2 < 0 ? -1 : line.indexOf(',', c2 + 1);
  if (c3 < 0) return false;
  if (line.substring(0, c1) != expectTime) return false;
  return line.substring(c2 + 1, c3).toInt() == expectQid;
}

// ลบ 1 แถวโดยคัดลอกทุกแถวยกเว้นแถวนั้นไปไฟล์ชั่วคราว แล้วเปลี่ยนชื่อทับ (LittleFS rename = แทนที่แบบ atomic
// ไฟล์เดิมไม่เสียครึ่ง ๆ กลาง ๆ ถ้าไฟดับระหว่างทาง) — ไฟล์ใหญ่สุด 180 KB ใช้เวลาไม่ถึง 1-2 วินาที
int deleteTrainRow(uint32_t row, const char* expectTime, long expectQid) {
  if (!s_fsOk) return 2;
  static const char* const kTmp = "/train.tmp";
  lockFs();
  File in = LittleFS.open(kTrainPath, "r");
  if (!in) { unlockFs(); return 1; }
  File out = LittleFS.open(kTmp, "w");
  if (!out) { in.close(); unlockFs(); return 2; }
  uint32_t idx = 0;          // 0 = หัวตาราง, 1 = แถวข้อมูลแรก (นับเฉพาะบรรทัดที่ไม่ว่าง เหมือนหน้าเว็บ)
  int removedLabel = -1;
  bool removed = false;
  while (in.available()) {
    String line = in.readStringUntil('\n');
    if (line.length() == 0) continue;
    if (!removed && idx == row && row > 0 && trainLineMatches(line, expectTime, expectQid)) {
      removed = true;
      int c = 0, i = 0;                                  // คอลัมน์ที่ 4 = label 0/1
      for (; i < (int)line.length() && c < 3; i++) if (line[i] == ',') c++;
      if (i < (int)line.length()) removedLabel = line[i] - '0';
    } else {
      out.print(line);
      out.print('\n');
    }
    idx++;
    if ((idx & 31) == 0) wdt::feed();                    // ไฟล์ยาว: ป้อน watchdog ระหว่างทาง
  }
  in.close();
  out.close();
  if (!removed) { LittleFS.remove(kTmp); unlockFs(); return 1; }
  if (!LittleFS.rename(kTmp, kTrainPath)) {              // บางเวอร์ชัน rename ทับไฟล์เดิมไม่ได้
    LittleFS.remove(kTrainPath);
    if (!LittleFS.rename(kTmp, kTrainPath)) { unlockFs(); return 2; }
  }
  if (removedLabel == 0 && s_trainTruth) s_trainTruth--;
  else if (removedLabel == 1 && s_trainLie) s_trainLie--;
  unlockFs();
  return 0;
}

// อ่านโมเดล AI จาก NVS (ขนาดไม่ตรง = ไม่มีโมเดล)
bool loadModel(ml::Model& m) {
  ml::clear(m);
  if (mlPrefs.getBytesLength("model") != sizeof(ml::Model)) return false;
  mlPrefs.getBytes("model", &m, sizeof(m));
  return ml::valid(m);
}

// บันทึกโมเดล AI ลง NVS (อยู่ถาวรแม้ปิดเครื่อง)
bool saveModel(const ml::Model& m) {
  return mlPrefs.putBytes("model", &m, sizeof(m)) == sizeof(m);
}

void clearModel() { mlPrefs.remove("model"); }

// ลบไฟล์ log ทั้งหมด
bool clearLogs() {
  if (!s_fsOk) return false;
  lockFs();
  const char* files[] = {"/events.log", "/events.1", "/results.csv", "/results.1"};
  for (const char* p : files)
    if (LittleFS.exists(p)) LittleFS.remove(p);
  unlockFs();
  return true;
}

}  // namespace storage
