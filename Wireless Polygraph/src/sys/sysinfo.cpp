// =====================================================================
//  sysinfo.cpp — รวบรวมข้อมูลภายในระบบให้หน้า "ระบบ & อุปกรณ์" และ /api/system
//  ทำอะไร   : รายชื่อ task + priority + stack ที่เหลือ + %CPU, heap, แผนที่ partition, สถานะ WDT/OTA/พลังงาน
//  ทำไม     : พิสูจน์ได้ว่าใช้ RTOS/หน่วยความจำ/watchdog จริง (ค่าจาก API ของ ESP-IDF ณ เวลาที่ขอ)
//  เรียกจาก : web_server.cpp (/api/info, /api/system)
// =====================================================================
#include "sysinfo.h"
#include <WiFi.h>
#include <LittleFS.h>
#include <time.h>
#include "esp_heap_caps.h"
#include "esp_sleep.h"
#include "../app.h"
#include "../config.h"
#include "../sensors.h"
#include "../drivers/i2c_bus.h"
#include "../net/json_writer.h"
#include "../net/telemetry.h"
#include "../net/wifi_ap.h"
#include "storage.h"
#include "watchdog.h"
#include "power.h"
#include "ota.h"
#include "ui.h"

namespace sysinfo {

namespace {
uint32_t s_sketchSize = 0, s_sketchFree = 0;
const uint32_t kStack[T_COUNT] = {STACK_SENSOR, STACK_ENGINE, STACK_TELEMETRY,
                                  STACK_HTTP,   STACK_UI,     STACK_SUPERVISOR};
// task ของระบบที่สร้างโดย ESP-IDF/Arduino (ชื่อมาตรฐาน) — หาด้วย xTaskGetHandle()
const char* const kSysTasks[] = {"loopTask", "IDLE", "wifi", "tiT", "sys_evt",
                                 "esp_timer", "Tmr Svc", "arduino_events"};

void taskJson(Json& j, TaskHandle_t h, const char* name, bool ours, uint32_t stackSize,
              const TaskStat* st) {
  j.obj();
  j.kv("name", name);
  j.kv("ours", ours);
  j.kv("prio", (unsigned int)uxTaskPriorityGet(h));
  j.kv("state", taskStateName((int)eTaskGetState(h)));
  j.kv("stackFree", (unsigned int)uxTaskGetStackHighWaterMark(h));   // ไบต์ (ESP-IDF ใช้หน่วยไบต์)
  if (stackSize) j.kv("stack", (unsigned long)stackSize);
  if (st) {
    j.kv("cpu", st->cpuPct, 2);
    j.kv("beats", (unsigned long)st->beats);
    j.kv("lastBeatMs", (unsigned long)(millis() - st->lastBeatMs));
  }
  j.end();
}
}  // namespace

const char* taskStateName(int s) {
  switch (s) {
    case 0: return "running";
    case 1: return "ready";
    case 2: return "blocked";
    case 3: return "suspended";
    case 4: return "deleted";
    default: return "?";
  }
}

void cacheSlowValues() {
  // ESP.getSketchSize() ตรวจ hash ทั้ง image ใช้เวลาหลายร้อย ms -> ทำครั้งเดียวใน supervisor
  s_sketchSize = ESP.getSketchSize();
  s_sketchFree = ESP.getFreeSketchSpace();
}

void buildInfoJson(Json& j) {
  const wdt::BootInfo& bi = wdt::bootInfo();
  char buf[24];
  j.obj();
  j.kv("name", FW_NAME);
  j.kv("fw", FW_VERSION);
  j.kv("build", FW_BUILD);
  j.kv("id", net::deviceId());
  j.kv("mac", net::macStr());
  j.kv("chip", ESP.getChipModel());
  j.kv("chipRev", (int)ESP.getChipRevision());
  j.kv("cores", (int)ESP.getChipCores());
  j.kv("cpuMHz", (unsigned long)getCpuFrequencyMhz());
  j.kv("idf", ESP.getSdkVersion());
  snprintf(buf, sizeof(buf), "%d.%d.%d", ESP_ARDUINO_VERSION_MAJOR, ESP_ARDUINO_VERSION_MINOR,
           ESP_ARDUINO_VERSION_PATCH);
  j.kv("arduino", buf);
  j.kv("uptime", (unsigned long)(millis() / 1000));
  const time_t now = time(nullptr);
  j.kv("time", (long)(now > 1600000000 ? now : 0));
  j.kv("bootCount", (unsigned long)storage::stats().bootCount);
  j.obj("boot");
  j.kv("reason", wdt::resetReasonName(bi.reason));
  j.kv("reasonTh", wdt::resetReasonThai(bi.reason));
  j.kv("wake", power::wakeCauseName(bi.wakeCause));
  j.kv("planned", bi.planned ? wdt::plannedReasonName(bi.plannedReason) : "none");
  j.kv("plannedTask", bi.plannedTask);
  j.kv("prevUptime", (unsigned long)bi.prevUptimeS);
  j.kv("coredump", bi.coredump);
  j.kv("cdTask", bi.cdTask);
  snprintf(buf, sizeof(buf), "0x%08lX", (unsigned long)bi.cdPc);
  j.kv("cdPc", buf);
  j.end();
  j.end();
}

void buildSystemJson(Json& j) {
  const uint32_t now = millis();
  const uint32_t b = app::bits();
  j.obj();
  j.kv("uptime", (unsigned long)(now / 1000));

  // ---- ชิป ----
  j.obj("chip");
  j.kv("model", ESP.getChipModel());
  j.kv("rev", (int)ESP.getChipRevision());
  j.kv("cpuMHz", (unsigned long)getCpuFrequencyMhz());
  j.kv("tempC", sensors::chipTempC(), 1);
  j.kv("idf", ESP.getSdkVersion());
  j.end();

  // ---- RAM ----
  j.obj("memory");
  j.kv("heapTotal", (unsigned long)ESP.getHeapSize());
  j.kv("heapFree", (unsigned long)ESP.getFreeHeap());
  j.kv("heapMin", (unsigned long)ESP.getMinFreeHeap());          // ต่ำสุดตั้งแต่บูต
  j.kv("heapMaxBlock", (unsigned long)ESP.getMaxAllocHeap());    // ก้อนใหญ่สุดที่จองได้ (ดู fragmentation)
  j.kv("internalFree", (unsigned long)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
  j.kv("engineBytes", (unsigned long)sizeof(lie::Engine));       // RAM ที่ LieEngine ใช้ (static)
  j.end();

  // ---- แฟลช + flash map ----
  j.obj("flash");
  j.kv("size", (unsigned long)ESP.getFlashChipSize());
  j.kv("speedMHz", (unsigned long)(ESP.getFlashChipSpeed() / 1000000));
  j.kv("sketch", (unsigned long)s_sketchSize);
  j.kv("sketchFree", (unsigned long)s_sketchFree);
  j.end();
  ota::appendJson(j);   // "ota" + "partitions"

  j.obj("nvs");
  j.kv("used", (unsigned long)storage::nvsUsedEntries());
  j.kv("free", (unsigned long)storage::nvsFreeEntries());
  j.end();

  j.obj("fs");
  j.kv("ok", storage::fsOk());
  j.kv("used", (unsigned long)storage::fsUsed());
  j.kv("total", (unsigned long)storage::fsTotal());
  j.arr("files");
  if (storage::fsOk() && app::fsMutex && xSemaphoreTake(app::fsMutex, pdMS_TO_TICKS(200)) == pdTRUE) {
    File root = LittleFS.open("/");
    File f = root.openNextFile();
    int n = 0;
    while (f && n < 20) {
      j.obj().kv("name", f.name()).kv("size", (unsigned long)f.size()).end();
      f = root.openNextFile();
      n++;
    }
    xSemaphoreGive(app::fsMutex);
  }
  j.end();
  j.end();

  // ---- FreeRTOS tasks ----
  j.kv("taskCount", (unsigned long)uxTaskGetNumberOfTasks());
  j.arr("tasks");
  for (int i = 0; i < T_COUNT; i++)
    if (app::tasks[i].handle)
      taskJson(j, app::tasks[i].handle, app::tasks[i].name, true, kStack[i], &app::tasks[i]);
  for (const char* name : kSysTasks) {
    TaskHandle_t h = xTaskGetHandle(name);
    if (h) taskJson(j, h, name, false, 0, nullptr);
  }
  j.end();

  // ---- Watchdog ----
  j.obj("wdt");
  j.kv("twdtS", (unsigned long)TWDT_TIMEOUT_S);
  j.kv("iwdtMs", (unsigned long)CONFIG_ESP_INT_WDT_TIMEOUT_MS);
  j.kv("hwTimeoutMs", (unsigned long)HWWDT_TIMEOUT_MS);
  j.kv("hwRunning", wdt::hwRunning());
  j.kv("hwFeeds", (unsigned long)wdt::hwFeeds());
  j.kv("hwLastFeedMs", (unsigned long)(now - wdt::hwLastFeedMs()));
  j.kv("demo", wdt::demoName(wdt::pendingDemo()));
  j.kv("wdtResets", (unsigned long)storage::stats().wdtResets);
  j.kv("panicResets", (unsigned long)storage::stats().panicResets);
  j.kv("brownouts", (unsigned long)storage::stats().brownouts);
  j.end();

  // ---- พลังงาน ----
  const Vitals v = app::getVitals();
  j.obj("power");
  j.kv("eco", (b & EV_ECO) != 0);
  j.kv("usb", (b & EV_USB) != 0);
  j.kv("vbatMv", v.vbatMv, 0);
  j.kv("battPct", (int)v.battPct);
  j.kv("battPresent", v.battPresent);
  j.kv("lowBatt", (b & EV_LOW_BATT) != 0);
  j.kv("lightSleeps", (unsigned long)power::lightSleeps());
  j.kv("lastLightSleepMs", (unsigned long)power::lastLightSleepMs());
  j.kv("deepSleeps", (unsigned long)storage::stats().deepSleeps);
  j.kv("rtcWakeCount", (unsigned long)rtcWakeCount);
  j.kv("rtcStandbyChecks", (unsigned long)rtcStandbyChecks);
  j.kv("rtcSleepSeconds", (unsigned long)rtcSleepSeconds);
  j.kv("standbyMin", (int)storage::settings().standbyMin);
  j.kv("idleS", (unsigned long)((now - app::lastActivityMs()) / 1000));
  j.end();

  // ---- เครือข่าย ----
  j.obj("net");
  j.kv("ssid", AP_SSID);
  j.kv("ip", WiFi.softAPIP().toString());
  j.kv("mac", net::macStr());
  j.kv("channel", (int)AP_CHANNEL);
  j.kv("stations", (int)net::stations());
  j.kv("rssi", (int)net::bestRssi());
  j.kv("udpClients", (int)telemetry::clientCount());
  j.kv("udpSent", (unsigned long)telemetry::packetsSent());
  j.kv("udpErrors", (unsigned long)telemetry::sendErrors());
  j.end();

  // ---- เซนเซอร์ ----
  j.obj("sensors");
  j.kv("max30102", sensors::maxOk());
  j.kv("maxPart", (int)sensors::max30102().partId());
  j.kv("maxRev", (int)sensors::max30102().revId());
  j.kv("fifoOverflows", (unsigned long)sensors::max30102().overflows());
  j.kv("mpu", sensors::mpuOk());
  j.kv("mpuWho", (int)sensors::mpu6050().whoAmI());
  j.kv("mpuChip", sensors::mpu6050().chipName());
  j.kv("mpuAccelCfg2", sensors::mpu6050().hasAccelConfig2());
  j.kv("i2cErrors", (unsigned long)sensors::i2cErrors());
  j.kv("i2cRecoveries", (unsigned long)i2cbus::recoveries());
  j.kv("ticks", (unsigned long)sensors::ticks());
  j.kv("button", (unsigned long)ui::buttonPresses());
  j.end();

  // ---- สถิติสะสม (EEPROM) ----
  const Stats& s = storage::stats();
  j.obj("stats");
  j.kv("boots", (unsigned long)s.bootCount);
  j.kv("sessions", (unsigned long)s.sessions);
  j.kv("questions", (unsigned long)s.questions);
  j.kv("lies", (unsigned long)s.lies);
  j.kv("truths", (unsigned long)s.truths);
  j.kv("inconclusive", (unsigned long)s.inconclusive);
  j.kv("invalid", (unsigned long)s.invalid);
  j.kv("uptimeMin", (unsigned long)s.uptimeMin);
  j.kv("otaUpdates", (unsigned long)s.otaUpdates);
  j.end();
  j.end();
}

}  // namespace sysinfo
