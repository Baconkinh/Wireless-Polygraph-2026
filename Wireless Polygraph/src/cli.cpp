// cli.cpp — Serial console (ผ่านสาย USB)   พิมพ์ help แล้วกด Enter เพื่อดูคำสั่งทั้งหมด
//
// v2.1:
//  - แก้บั๊กพิมพ์แล้วไม่มีอะไรตอบ (ต้นเหตุคือ setTxTimeoutMs(0) ใน main.cpp — แก้ที่นั่นแล้ว)
//  - ตัวอักษรที่พิมพ์จะแสดงทันที (echo จากบอร์ด) + มี prompt "> " บอกว่าพร้อมรับคำสั่ง
//  - เปิด Serial Monitor เมื่อไรก็เห็นข้อความต้อนรับ (ไม่ต้องรอจังหวะบูต)
//  - คำสั่งใหม่: mode / ask truth|lie / model / data / subject / sleep / wifi / boots
#include "cli.h"
#include <Wire.h>
#include <LittleFS.h>
#include "esp_partition.h"
#include "app.h"
#include "config.h"
#include "sensors.h"
#include "tasks.h"
#include "drivers/i2c_bus.h"
#include "net/wifi_ap.h"
#include "net/telemetry.h"
#include "sys/storage.h"
#include "sys/power.h"
#include "sys/watchdog.h"
#include "sys/ota.h"
#include "sys/sysinfo.h"
#include "sys/ml_runtime.h"

namespace cli {

namespace {
char s_line[96];
size_t s_len = 0;
bool s_raw = false;
uint32_t s_lastRaw = 0;
uint16_t s_qid = 800;      // คำถามที่สั่งจาก Serial ใช้เลข 800+ (ไม่ชนกับ Studio/ปุ่ม)
bool s_host = false;       // มีโปรแกรมบนคอมเปิดพอร์ตอยู่
bool s_welcomed = false;

using lie::Engine;

// ไม่ใช้ "> " เฉย ๆ เพราะ filter "time" ของ PlatformIO ขึ้นต้นบรรทัดด้วย "เวลา > " อยู่แล้ว
void prompt() { Serial.print("PW> "); }

void printWelcome() {
  Serial.printf("\n=== %s v%s  (%s) ===\n", FW_NAME, FW_VERSION, net::deviceId());
  Serial.printf("WiFi \"%s\" รหัส \"%s\" -> เปิด http://192.168.4.1\n", AP_SSID, AP_PASS);
  Serial.printf("โหมดตอนนี้: %s | โมเดล AI: %s\n",
                mlrt::mode() == mlrt::MODE_TRAIN ? "เก็บข้อมูล (TRAIN)" : "ใช้งานจริง (DETECT)",
                mlrt::hasModel() ? "มี" : "ยังไม่มี (ใช้สูตรมาตรฐาน)");
  Serial.println("พิมพ์ help แล้วกด Enter เพื่อดูคำสั่งทั้งหมด");
  s_welcomed = true;
}

void printHelp() {
  Serial.println(F("\n================ คำสั่ง Serial ================"));
  Serial.println(F("[ทั่วไป]"));
  Serial.println(F("  status            สรุปสถานะทั้งหมด"));
  Serial.println(F("  live              ค่าที่วัดได้ตอนนี้"));
  Serial.println(F("  raw on | raw off  พิมพ์ค่าทุก 0.2 วินาที (ดูกราฟด้วย Serial Plotter)"));
  Serial.println(F("  i2c               สแกนเซนเซอร์ (ต้องเจอ 0x57 และ 0x68)"));
  Serial.println(F("[ทดสอบ / AI]"));
  Serial.println(F("  mode              ดูโหมดปัจจุบัน"));
  Serial.println(F("  mode train        เปลี่ยนเป็นโหมดเก็บข้อมูลเทรน AI"));
  Serial.println(F("  mode detect       เปลี่ยนเป็นโหมดใช้งานจริง"));
  Serial.println(F("  subject <ชื่อ>    ตั้งชื่อผู้ถูกทดสอบ (เก็บลงไฟล์ข้อมูล)"));
  Serial.println(F("  baseline          วัดค่าปกติ ~30 วินาที (ต้องทำก่อนถาม)"));
  Serial.println(F("  ask truth         ถามข้อที่ให้ตอบ 'จริง'   (เฉลย = จริง)"));
  Serial.println(F("  ask lie           ถามข้อที่สั่งให้ 'โกหก' (เฉลย = โกหก)"));
  Serial.println(F("  ask               ถามคำถามจริง (ไม่รู้เฉลย -> ให้ระบบตัดสิน)"));
  Serial.println(F("  ans yes | ans no  บันทึกว่าผู้ถูกทดสอบตอบแล้ว"));
  Serial.println(F("  abort | reset     ยกเลิกข้อนี้ | ล้าง baseline และผลทั้งหมด"));
  Serial.println(F("  data              ดูจำนวนข้อมูลเทรนที่เก็บแล้ว"));
  Serial.println(F("  data show         แสดงข้อมูลเทรนท้ายไฟล์"));
  Serial.println(F("  data clear        ลบข้อมูลเทรนทั้งหมด"));
  Serial.println(F("  model             ดูโมเดล AI ที่ติดตั้ง"));
  Serial.println(F("  model clear       ลบโมเดล (กลับไปใช้สูตรมาตรฐาน)"));
  Serial.println(F("[พลังงาน]"));
  Serial.println(F("  sleep             ดูการตั้งค่าการหลับ"));
  Serial.println(F("  sleep off         ปิดการหลับอัตโนมัติ (ค่าเริ่มต้น)"));
  Serial.println(F("  sleep auto 10     หลับอัตโนมัติเมื่อไม่มีใครใช้ 10 นาที"));
  Serial.println(F("  sleep light       พักเครื่องทันที (กดปุ่ม BOOT เพื่อปลุก)"));
  Serial.println(F("  sleep deep 60     หลับลึก 60 วินาทีแล้วตื่นเอง"));
  Serial.println(F("  eco on | eco off  ประหยัดไฟ (CPU 80 MHz)"));
  Serial.println(F("  wifi low|mid|high กำลังส่ง WiFi (low ช่วยถ้าใช้แบตแล้ว WiFi หลุด)"));
  Serial.println(F("[ระบบ / สาธิต]"));
  Serial.println(F("  tasks | mem | flash   RTOS task, RAM, แผนที่แฟลช"));
  Serial.println(F("  boots | stats         ประวัติการรีเซ็ต 8 ครั้งล่าสุด | สถิติสะสม"));
  Serial.println(F("  ls | log | cat <ไฟล์> ไฟล์ใน LittleFS"));
  Serial.println(F("  cfg | set <key> <ค่า> ค่าตั้งใน NVS"));
  Serial.println(F("  wdt twdt confirm      สาธิต watchdog (twdt|hwwdt|panic|intwdt) เครื่องจะรีเซ็ต"));
  Serial.println(F("  reboot | factory      รีสตาร์ท | คืนค่าโรงงาน"));
  Serial.println(F("==============================================="));
}

void printLive() {
  const Vitals v = app::getVitals();
  const EngineSnap e = app::getEngineSnap();
  Serial.printf("ชีพจร %s | HRV %.0f ms | GSR %s (%.0f mV) | ผิว %.2f C | สั่น %.3f | ขยับ %.3f | แบต %.2f V %u%% | เครียด %d | %s %.0f%%\n",
                v.ppgContact ? (v.hr > 0 ? String((int)(v.hr + 0.5f)).c_str() : "...") : "ไม่แตะ",
                v.hrv, v.gsrContact ? String(v.gsr, 3).c_str() : "ไม่มีสัญญาณ", v.gsrMv, v.skinTemp, v.tremor,
                v.motion, v.vbatMv / 1000.0f, v.battPct, e.stress, Engine::stateName((lie::State)e.state),
                e.progress * 100);
}

void printStatus() {
  const wdt::BootInfo& bi = wdt::bootInfo();
  Serial.printf("\n%s v%s (%s)  id=%s\n", FW_NAME, FW_VERSION, FW_BUILD, net::deviceId());
  Serial.printf("uptime %lus  boot#%lu  reset=%s (%s)  planned=%s\n", (unsigned long)(millis() / 1000),
                (unsigned long)storage::stats().bootCount, wdt::resetReasonName(bi.reason),
                wdt::resetReasonThai(bi.reason),
                bi.planned ? wdt::plannedReasonName(bi.plannedReason) : "none");
  if (bi.coredump) Serial.printf("last crash: task '%s' PC=0x%08lX\n", bi.cdTask, (unsigned long)bi.cdPc);
  Serial.printf("WiFi '%s' (%s) stations=%u udpClients=%u  CPU %lu MHz %s  heap free %lu\n", AP_SSID,
                net::powerName(net::powerLevel()), net::stations(), telemetry::clientCount(),
                (unsigned long)getCpuFrequencyMhz(), power::eco() ? "ECO" : "", (unsigned long)ESP.getFreeHeap());
  Serial.printf("sensors: MAX30102=%s MPU=%s(0x%02X %s) i2cErr=%lu recover=%lu\n",
                sensors::maxOk() ? "ok" : "FAIL", sensors::mpuOk() ? "ok" : "FAIL",
                sensors::mpu6050().whoAmI(), sensors::mpu6050().chipName(),
                (unsigned long)sensors::i2cErrors(), (unsigned long)i2cbus::recoveries());
  uint32_t t = 0, l = 0;
  storage::trainCounts(t, l);
  Serial.printf("mode=%s  model=%s  train data: truth %lu / lie %lu  auto-sleep=%s\n",
                mlrt::modeName(mlrt::mode()), mlrt::hasModel() ? "loaded" : "none", (unsigned long)t,
                (unsigned long)l, storage::settings().standbyMin ? String(storage::settings().standbyMin).c_str() : "off");
  printLive();
}

void printBoots() {
  const Stats& s = storage::stats();
  Serial.printf("รีเซ็ตทั้งหมด %lu ครั้ง | ไฟตก (brownout) %lu | watchdog %lu | panic %lu\n",
                (unsigned long)s.bootCount, (unsigned long)s.brownouts, (unsigned long)s.wdtResets,
                (unsigned long)s.panicResets);
  Serial.print("ล่าสุด -> เก่าสุด: ");
  for (int i = 1; i <= 8; i++) {
    const uint8_t r = s.resetHist[(s.resetHead + 8 - i) % 8];
    if (r == 0) continue;
    Serial.printf("%s ", wdt::resetReasonName((esp_reset_reason_t)r));
  }
  Serial.println();
  if (s.brownouts > 0)
    Serial.println("ถ้า BROWNOUT ขึ้นหลายครั้งตอนใช้แบต = ไฟเลี้ยงจ่ายกระแสไม่พอ (ดูคู่มือหัวข้อ WiFi ไม่ขึ้นตอนใช้แบต)");
}

void printTasks() {
  static const char* sys[] = {"loopTask", "IDLE", "wifi", "tiT", "sys_evt", "esp_timer", "Tmr Svc"};
  Serial.printf("%-12s %4s %-9s %9s %6s\n", "task", "prio", "state", "stackFree", "CPU%");
  for (int i = 0; i < T_COUNT; i++) {
    TaskHandle_t h = app::tasks[i].handle;
    if (!h) continue;
    Serial.printf("%-12s %4u %-9s %9u %6.2f\n", app::tasks[i].name, (unsigned)uxTaskPriorityGet(h),
                  sysinfo::taskStateName((int)eTaskGetState(h)), (unsigned)uxTaskGetStackHighWaterMark(h),
                  app::tasks[i].cpuPct);
  }
  for (const char* n : sys) {
    TaskHandle_t h = xTaskGetHandle(n);
    if (h)
      Serial.printf("%-12s %4u %-9s %9u %6s\n", n, (unsigned)uxTaskPriorityGet(h),
                    sysinfo::taskStateName((int)eTaskGetState(h)), (unsigned)uxTaskGetStackHighWaterMark(h), "-");
  }
  Serial.printf("total tasks: %lu\n", (unsigned long)uxTaskGetNumberOfTasks());
}

void printFlash() {
  Serial.printf("%-9s %-5s %8s %9s\n", "label", "type", "address", "size");
  for (int t = 0; t <= 1; t++) {
    esp_partition_iterator_t it = esp_partition_find(t == 0 ? ESP_PARTITION_TYPE_APP : ESP_PARTITION_TYPE_DATA,
                                                     ESP_PARTITION_SUBTYPE_ANY, NULL);
    for (; it; it = esp_partition_next(it)) {
      const esp_partition_t* p = esp_partition_get(it);
      Serial.printf("%-9s %-5s 0x%06lX %9lu %s\n", p->label, t == 0 ? "app" : "data",
                    (unsigned long)p->address, (unsigned long)p->size,
                    strcmp(p->label, ota::runningLabel()) == 0 ? "<- running" : "");
    }
  }
  Serial.printf("NVS entries used %lu free %lu | LittleFS %u / %u bytes\n",
                (unsigned long)storage::nvsUsedEntries(), (unsigned long)storage::nvsFreeEntries(),
                (unsigned)storage::fsUsed(), (unsigned)storage::fsTotal());
}

void printFile(const char* name, size_t maxBytes = 4096) {
  char path[40];
  snprintf(path, sizeof(path), "%s%s", name[0] == '/' ? "" : "/", name);
  String s;
  if (!storage::readFile(path, s, maxBytes)) {
    Serial.printf("ไม่พบไฟล์ %s\n", path);
    return;
  }
  Serial.print(s);
  Serial.println();
}

void printCfg() {
  const Settings& s = storage::settings();
  Serial.printf("baselineSec=%u windowSec=%u preSec=%u s0=%.2f k=%.2f lieP=%.2f truthP=%.2f\n",
                s.baselineSec, s.windowSec, s.preSec, s.s0, s.k, s.lieP, s.truthP);
  Serial.printf("w(GSR,HR,AMP,TRM,TMP)=[%.2f %.2f %.2f %.2f %.2f] contactThr=%lu irLed=%u vccMv=%u\n", s.w[0],
                s.w[1], s.w[2], s.w[3], s.w[4], (unsigned long)s.contactThr, s.irLed, s.vccMv);
  Serial.printf("eco=%d standbyMin=%u wakeCheckS=%u waveform=%d mode=%u wifiPower=%u\n", s.eco, s.standbyMin,
                s.wakeCheckS, s.waveform, s.mode, s.wifiPower);
}

bool setCfg(const char* key, const char* val) {
  Settings n = storage::settings();
  const float v = atof(val);
  struct R { const char* k; float lo, hi; };
  static const R ranges[] = {{"baselineSec", 20, 90}, {"windowSec", 6, 20}, {"preSec", 1, 5},
                             {"s0", -5, 10},          {"k", 0.1f, 10},      {"lieP", 0.5f, 0.99f},
                             {"truthP", 0.01f, 0.5f}, {"contactThr", 5000, 250000},
                             {"irLed", 1, 255},       {"vccMv", 3000, 3600}, {"standbyMin", 0, 240},
                             {"wakeCheckS", 5, 600},  {"eco", 0, 1},        {"waveform", 0, 1}};
  const R* r = nullptr;
  for (const R& x : ranges)
    if (!strcmp(x.k, key)) r = &x;
  if (!r) { Serial.println("ไม่รู้จัก key นี้ (พิมพ์ cfg ดูรายการ)"); return false; }
  if (!(v >= r->lo && v <= r->hi)) {
    Serial.printf("%s ต้องอยู่ระหว่าง %g ถึง %g\n", key, (double)r->lo, (double)r->hi);
    return false;
  }
  if (!strcmp(key, "baselineSec")) n.baselineSec = (uint16_t)v;
  else if (!strcmp(key, "windowSec")) n.windowSec = (uint8_t)v;
  else if (!strcmp(key, "preSec")) n.preSec = (uint8_t)v;
  else if (!strcmp(key, "s0")) n.s0 = v;
  else if (!strcmp(key, "k")) n.k = v;
  else if (!strcmp(key, "lieP")) n.lieP = v;
  else if (!strcmp(key, "truthP")) n.truthP = v;
  else if (!strcmp(key, "contactThr")) n.contactThr = (uint32_t)v;
  else if (!strcmp(key, "irLed")) n.irLed = (uint8_t)v;
  else if (!strcmp(key, "vccMv")) n.vccMv = (uint16_t)v;
  else if (!strcmp(key, "standbyMin")) n.standbyMin = (uint16_t)v;
  else if (!strcmp(key, "wakeCheckS")) n.wakeCheckS = (uint16_t)v;
  else if (!strcmp(key, "eco")) n.eco = v > 0.5f;
  else if (!strcmp(key, "waveform")) n.waveform = v > 0.5f;
  if (n.truthP >= n.lieP) { Serial.println("truthP ต้องน้อยกว่า lieP"); return false; }
  storage::settings() = n;
  storage::saveSettings();
  xSemaphoreTake(app::engineMutex, portMAX_DELAY);
  lie::Config c = app::engine.config();
  storage::toEngineConfig(n, c);
  app::engine.configure(c);
  xSemaphoreGive(app::engineMutex);
  sensors::applySettings();
  if (n.eco != power::eco()) power::setEco(n.eco);
  Serial.println("บันทึกลง NVS แล้ว");
  return true;
}

void engineCmd(const char* cmd, const char* arg) {
  bool ok = false;
  const char* what = "";
  xSemaphoreTake(app::engineMutex, portMAX_DELAY);
  if (!strcmp(cmd, "baseline")) {
    ok = app::engine.startBaseline(arg ? atof(arg) : 0.0f);
    what = "เริ่มวัด baseline — นั่งนิ่ง ๆ หายใจปกติ";
  } else if (!strcmp(cmd, "ask")) {
    lie::Kind k = lie::Kind::Test;
    if (arg && !Engine::kindFromName(arg, k)) k = lie::Kind::Test;
    ok = app::engine.startQuestion(s_qid, k);
    if (ok) s_qid = (s_qid >= 899) ? 800 : s_qid + 1;
    what = k == lie::Kind::ControlTruth ? "ถามได้เลย (ข้อนี้ให้ตอบตามจริง) — รอผล 12 วินาที"
         : k == lie::Kind::ControlLie   ? "ถามได้เลย (ข้อนี้สั่งให้โกหก) — รอผล 12 วินาที"
                                        : "ถามได้เลย — รอผล 12 วินาที";
  } else if (!strcmp(cmd, "ans")) {
    ok = app::engine.markAnswer(!(arg && !strcmp(arg, "no")));
    what = "บันทึกเวลาที่ตอบแล้ว";
  } else if (!strcmp(cmd, "abort")) {
    app::engine.abort();
    ok = true;
    what = "ยกเลิกแล้ว";
  } else if (!strcmp(cmd, "reset")) {
    app::engine.resetSession();
    ok = true;
    what = "ล้าง baseline และผลทั้งหมดแล้ว";
  }
  const char* err = app::engine.lastError();
  xSemaphoreGive(app::engineMutex);
  if (ok) Serial.printf("OK: %s\n", what);
  else Serial.printf("ทำไม่ได้: %s\n", err);
  if (ok && !strcmp(cmd, "ask") && mlrt::mode() == mlrt::MODE_DETECT && arg &&
      (!strcmp(arg, "truth") || !strcmp(arg, "lie")))
    Serial.println("หมายเหตุ: โหมดใช้งานจริงจะไม่บันทึกข้อนี้เป็นข้อมูลเทรน (พิมพ์ mode train ถ้าต้องการเก็บ)");
}

void cmdMode(int argc, char** argv) {
  if (argc > 1) {
    if (!strcmp(argv[1], "train")) mlrt::setMode(mlrt::MODE_TRAIN);
    else if (!strcmp(argv[1], "detect")) mlrt::setMode(mlrt::MODE_DETECT);
    else { Serial.println("ใช้: mode train | mode detect"); return; }
  }
  if (mlrt::mode() == mlrt::MODE_TRAIN)
    Serial.println("โหมด: เก็บข้อมูล (TRAIN) — baseline แล้วใช้ ask truth / ask lie ทุกข้อจะถูกบันทึกลง /train.csv");
  else
    Serial.println("โหมด: ใช้งานจริง (DETECT) — baseline แล้วใช้ ask เพื่อให้ระบบตัดสิน");
}

void cmdData(int argc, char** argv) {
  if (argc > 1 && !strcmp(argv[1], "clear")) {
    if (argc < 3 || strcmp(argv[2], "confirm")) { Serial.println("ข้อมูลจะหายทั้งหมด! พิมพ์: data clear confirm"); return; }
    storage::clearTrain();
    Serial.println("ล้างข้อมูลเทรนแล้ว");
    return;
  }
  if (argc > 1 && !strcmp(argv[1], "show")) { printFile(storage::kTrainPath, 2048); return; }
  uint32_t t = 0, l = 0;
  storage::trainCounts(t, l);
  Serial.printf("ข้อมูลเทรน: ตอบจริง %lu ข้อ | โกหก %lu ข้อ | ไฟล์ %u ไบต์ (ดาวน์โหลดได้ที่ http://192.168.4.1/api/ml/data.csv)\n",
                (unsigned long)t, (unsigned long)l, (unsigned)storage::trainBytes());
  if (t + l < 20) Serial.println("แนะนำ: เก็บอย่างน้อยอย่างละ 15-20 ข้อ จากหลาย ๆ คน ก่อนเทรน");
}

void cmdModel(int argc, char** argv) {
  if (argc > 1 && !strcmp(argv[1], "clear")) {
    mlrt::clearModel();
    Serial.println("ลบโมเดลแล้ว — กลับไปใช้สูตรมาตรฐาน");
    return;
  }
  if (!mlrt::hasModel()) {
    Serial.println("ยังไม่มีโมเดล AI — เทรนบนคอมด้วย ml/train.py แล้วอัปโหลด (ดูคู่มือ)");
    return;
  }
  const ml::Model m = mlrt::modelCopy();
  Serial.printf("โมเดล \"%s\": เทรนจาก %u ข้อ, ความแม่นยำ (cross-validation) %.0f%%\n", m.name, m.samples,
                m.accuracy * 100);
  for (int i = 0; i < m.n; i++) Serial.printf("  %-7s น้ำหนัก %+.3f\n", ml::kFeatureNames[m.idx[i]], m.w[i]);
  Serial.printf("  bias    %+.3f\n", m.b);
}

void cmdSleep(int argc, char** argv) {
  Settings& st = storage::settings();
  if (argc > 1) {
    if (!strcmp(argv[1], "off")) {
      st.standbyMin = 0;
      storage::saveSettings();
    } else if (!strcmp(argv[1], "auto") && argc > 2) {
      const int m = atoi(argv[2]);
      if (m < 1 || m > 240) { Serial.println("ใส่ 1-240 นาที (หรือ sleep off เพื่อปิด)"); return; }
      st.standbyMin = (uint16_t)m;
      storage::saveSettings();
    } else if (!strcmp(argv[1], "light")) {
      power::requestLightSleep(argc > 2 ? (uint32_t)atol(argv[2]) : 0);
      Serial.println("กำลังพักเครื่อง (light sleep) — WiFi จะหลุดชั่วคราว กดปุ่ม BOOT เพื่อปลุก");
      return;
    } else if (!strcmp(argv[1], "deep")) {
      const uint32_t sec = argc > 2 ? (uint32_t)atol(argv[2]) : 60;
      power::requestDeepSleep(sec ? sec : 60);
      Serial.printf("กำลังหลับลึก %lu วินาที แล้วจะตื่นเอง (บูตใหม่)\n", (unsigned long)(sec ? sec : 60));
      return;
    } else {
      Serial.println("ใช้: sleep off | sleep auto <นาที> | sleep light | sleep deep <วินาที>");
      return;
    }
  }
  if (st.standbyMin) Serial.printf("หลับอัตโนมัติ: เปิด — ไม่มีใครใช้ %u นาทีแล้วหลับ (ปิดด้วย: sleep off)\n", st.standbyMin);
  else Serial.println("หลับอัตโนมัติ: ปิด (นาฬิกาจะไม่หลับเอง)");
}

void cmdWifi(int argc, char** argv) {
  if (argc > 1) {
    uint8_t lv = 1;
    if (!strcmp(argv[1], "low")) lv = 0;
    else if (!strcmp(argv[1], "mid")) lv = 1;
    else if (!strcmp(argv[1], "high")) lv = 2;
    else { Serial.println("ใช้: wifi low | wifi mid | wifi high"); return; }
    storage::settings().wifiPower = lv;
    storage::saveSettings();
    net::setPowerLevel(lv);
  }
  Serial.printf("กำลังส่ง WiFi: %s\n", net::powerName(net::powerLevel()));
}

void execute(char* line) {
  char* argv[4] = {nullptr, nullptr, nullptr, nullptr};
  int argc = 0;
  char* save = nullptr;
  for (char* t = strtok_r(line, " \t", &save); t && argc < 4; t = strtok_r(nullptr, " \t", &save)) argv[argc++] = t;
  if (!argc) {
    Serial.println("(พิมพ์ help แล้วกด Enter เพื่อดูคำสั่ง)");
    return;
  }
  const char* c = argv[0];
  app::touchActivity();
  if (!strcmp(c, "help") || !strcmp(c, "?")) printHelp();
  else if (!strcmp(c, "status")) printStatus();
  else if (!strcmp(c, "live")) printLive();
  else if (!strcmp(c, "raw")) {
    s_raw = argc > 1 && !strcmp(argv[1], "on");
    if (s_raw) Serial.println("ms,hr,gsr_uS,skin_C,tremor,motion,vbat_mV,stress");
  } else if (!strcmp(c, "mode")) cmdMode(argc, argv);
  else if (!strcmp(c, "subject")) {
    if (argc > 1) mlrt::setSubject(argv[1]);
    Serial.printf("ผู้ถูกทดสอบ: %s\n", mlrt::subject());
  } else if (!strcmp(c, "data")) cmdData(argc, argv);
  else if (!strcmp(c, "model")) cmdModel(argc, argv);
  else if (!strcmp(c, "sleep")) cmdSleep(argc, argv);
  else if (!strcmp(c, "standby")) {
    power::requestDeepSleep(0);
    Serial.println("standby: แตะเซนเซอร์/ใส่นาฬิกาเพื่อปลุก");
  } else if (!strcmp(c, "wifi")) cmdWifi(argc, argv);
  else if (!strcmp(c, "boots")) printBoots();
  else if (!strcmp(c, "tasks")) printTasks();
  else if (!strcmp(c, "mem")) {
    Serial.printf("heap total %lu free %lu min %lu maxBlock %lu | LieEngine %u bytes\n",
                  (unsigned long)ESP.getHeapSize(), (unsigned long)ESP.getFreeHeap(),
                  (unsigned long)ESP.getMinFreeHeap(), (unsigned long)ESP.getMaxAllocHeap(),
                  (unsigned)sizeof(lie::Engine));
  } else if (!strcmp(c, "flash")) printFlash();
  else if (!strcmp(c, "ls")) {
    File root = LittleFS.open("/");
    for (File f = root.openNextFile(); f; f = root.openNextFile())
      Serial.printf("  %-14s %6u bytes\n", f.name(), (unsigned)f.size());
  } else if (!strcmp(c, "cat") && argc > 1) printFile(argv[1]);
  else if (!strcmp(c, "log")) printFile("events.log");
  else if (!strcmp(c, "cfg")) printCfg();
  else if (!strcmp(c, "set") && argc > 2) setCfg(argv[1], argv[2]);
  else if (!strcmp(c, "baseline") || !strcmp(c, "ask") || !strcmp(c, "ans") || !strcmp(c, "abort") ||
           !strcmp(c, "reset"))
    engineCmd(c, argc > 1 ? argv[1] : nullptr);
  else if (!strcmp(c, "eco") && argc > 1) {
    const bool on = !strcmp(argv[1], "on");
    power::setEco(on);
    storage::settings().eco = on;
    storage::saveSettings();
    Serial.printf("ECO %s (CPU %lu MHz)\n", on ? "on" : "off", (unsigned long)getCpuFrequencyMhz());
  } else if (!strcmp(c, "wdt") && argc > 1) {
    wdt::Demo d;
    if (!wdt::demoFromName(argv[1], d)) Serial.println("ชนิด: twdt hwwdt panic intwdt");
    else if (argc < 3 || strcmp(argv[2], "confirm")) Serial.println("เครื่องจะรีเซ็ต! พิมพ์ต่อท้ายว่า confirm");
    else {
      Serial.printf("สาธิต %s ...\n", argv[1]);
      if (d == wdt::DEMO_PANIC || d == wdt::DEMO_INTWDT) wdt::crashNow(d, "loopTask");
      wdt::requestDemo(d);
    }
  } else if (!strcmp(c, "stats")) {
    const Stats& s = storage::stats();
    Serial.printf("boots %lu | sessions %lu | questions %lu (lie %lu, truth %lu, inconcl %lu, invalid %lu)\n"
                  "WDT resets %lu | panics %lu | brownouts %lu | deep sleeps %lu | OTA %lu | uptime %lu min\n",
                  (unsigned long)s.bootCount, (unsigned long)s.sessions, (unsigned long)s.questions,
                  (unsigned long)s.lies, (unsigned long)s.truths, (unsigned long)s.inconclusive,
                  (unsigned long)s.invalid, (unsigned long)s.wdtResets, (unsigned long)s.panicResets,
                  (unsigned long)s.brownouts, (unsigned long)s.deepSleeps, (unsigned long)s.otaUpdates,
                  (unsigned long)s.uptimeMin);
  } else if (!strcmp(c, "statsreset")) {
    storage::resetStats();
    Serial.println("ล้างสถิติแล้ว");
  } else if (!strcmp(c, "i2c")) {
    uint8_t found[16];
    const int n = i2cbus::scan(found, 16);
    Serial.printf("พบ %d อุปกรณ์:", n);
    for (int i = 0; i < n && i < 16; i++) Serial.printf(" 0x%02X", found[i]);
    Serial.println("  (0x57 = MAX30102 ชีพจร, 0x68 = MPU6050 การสั่น)");
  } else if (!strcmp(c, "reboot")) {
    Serial.println("กำลังรีสตาร์ท...");
    power::requestRestart(PR_USER_RESTART);
  } else if (!strcmp(c, "factory")) {
    storage::resetSettings();
    Serial.println("คืนค่าตั้งเริ่มต้นแล้ว -> รีสตาร์ท");
    power::requestRestart(PR_FACTORY);
  } else {
    Serial.printf("ไม่รู้จักคำสั่ง '%s' — พิมพ์ help\n", c);
  }
}
}  // namespace

namespace {
uint32_t s_lastRx = 0;     // เวลาที่ได้ตัวอักษรล่าสุด
bool s_burst = false;      // บรรทัดนี้มาเป็นก้อน (Serial Monitor ส่งทั้งคำ) ไม่ใช่พิมพ์ทีละตัว

void runLine() {
  Serial.println();
  s_line[s_len] = 0;
  execute(s_line);
  s_len = 0;
  s_burst = false;
  prompt();
}
}  // namespace

void poll() {
  // เปิด Serial Monitor เมื่อไร -> แสดงข้อความต้อนรับ (คอมเริ่มอ่านข้อมูล = connected)
  const bool host = (bool)Serial;
  if (host && !s_host) {
    delay(30);
    printWelcome();
    prompt();
  }
  s_host = host;

  int got = 0;                                    // จำนวนตัวอักษรที่เข้ามาในรอบนี้
  while (Serial.available()) {
    const int ch = Serial.read();
    s_lastRx = millis();
    if (!s_welcomed) { printWelcome(); prompt(); }
    if (ch == '\r' || ch == '\n') {
      if (ch == '\n' && s_len == 0) continue;     // CRLF: \n ที่ตามหลัง \r ไม่ต้องทำอะไร
      runLine();
    } else if ((ch == 8 || ch == 127) && s_len) {
      s_len--;
      Serial.print("\b \b");                      // ลบตัวอักษรบนจอ
    } else if (ch >= 32 && s_len < sizeof(s_line) - 1) {
      s_line[s_len++] = (char)ch;
      Serial.write((uint8_t)ch);                  // echo ให้เห็นสิ่งที่พิมพ์
      got++;
    }
  }
  // Serial Monitor บางตัว (เช่น ของ VS Code ที่ตั้ง Line ending = None) ส่งทั้งคำมาก้อนเดียว
  // โดยไม่มี Enter ตาม -> ถ้าได้มาเป็นก้อน (>= 2 ตัวในรอบเดียว) แล้วเงียบไป 300 ms ให้ถือว่าจบคำสั่ง
  // (คนพิมพ์ทีละตัวใน PlatformIO จะได้ทีละ 1 ตัว จึงไม่โดนตัดคำกลางคัน)
  if (got >= 2) s_burst = true;
  if (s_burst && s_len && millis() - s_lastRx > 300) runLine();
  if (s_raw && millis() - s_lastRaw >= 200) {
    s_lastRaw = millis();
    const Vitals v = app::getVitals();
    Serial.printf("%lu,%.1f,%.3f,%.2f,%.3f,%.3f,%.0f,%d\n", (unsigned long)v.ms, v.hr, v.gsr, v.skinTemp,
                  v.tremor, v.motion, v.vbatMv, app::getEngineSnap().stress);
  }
}

}  // namespace cli
