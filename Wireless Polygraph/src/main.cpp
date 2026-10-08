// =====================================================================
//  Wireless Polygraph Watch — เฟิร์มแวร์ฉบับสมบูรณ์ (ESP32-C3 SuperMini)
//  ภาควิชาวิศวกรรมคอมพิวเตอร์ ม.เกษตรศาสตร์ วิทยาเขตศรีราชา — วิชา Embedded Systems
//
//  โครงสร้างโฟลเดอร์ src/
//    config.h            ค่าคงที่ทั้งหมด (ขา, เวลา, priority, เกณฑ์)
//    app.h/.cpp          ข้อมูลกลาง + วัตถุ FreeRTOS (queue, mutex, event group)
//    sensors.*           รวมเซนเซอร์ + DSP (ถูกเรียกจาก sensorTask 100 Hz)
//    tasks.*             สร้าง task ทั้งหมด + Hardware Timer ISR
//    cli.*               Serial console (พิมพ์ help)
//    drivers/            MAX30102, MPU6050, ADC, I2C bus recovery, สูตรแปลงหน่วย
//    dsp/                ตัวกรอง, PPG (ชีพจร/HRV), EDA (GSR/SCR), การสั่น
//    lie/                LieEngine — ตัดสิน โกหก / จริง
//    net/                WiFi AP, UDP telemetry, HTTP REST + หน้าเว็บ, JSON
//    sys/                NVS/EEPROM/RTC/LittleFS, watchdog, sleep, OTA, LED/ปุ่ม, sysinfo
//
//  ลำดับการบูต (setup):
//    1) ตื่นจาก standby? -> เช็คผิวแตะแวบเดียว ไม่มีใครใส่ = หลับต่อทันที (ไม่เปิด WiFi)
//    2) อ่าน "กล่องดำ": สาเหตุรีเซ็ต + RTC record + core dump ของการล่มครั้งก่อน
//    3) เปิดหน่วยความจำถาวร (NVS, EEPROM, LittleFS) + นับสถิติการบูต
//    4) เซนเซอร์ -> LieEngine -> WiFi AP -> Web/OTA -> Task WDT -> สร้าง task -> Timer WDT
// =====================================================================
#include <Arduino.h>
#include "config.h"
#include "app.h"
#include "sensors.h"
#include "tasks.h"
#include "cli.h"
#include "net/wifi_ap.h"
#include "net/web_server.h"
#include "sys/storage.h"
#include "sys/power.h"
#include "sys/watchdog.h"
#include "sys/ui.h"
#include "sys/ml_runtime.h"

// นับสถิติการบูตตามสาเหตุ (watchdog, ไฟตก, panic) แล้วเก็บใน EEPROM emulation
static void countBootReason() {
  const wdt::BootInfo& bi = wdt::bootInfo();
  Stats& st = storage::stats();
  st.bootCount++;
  if (wdt::isWdtReset(bi.reason)) {
    st.wdtResets++;
  } else if (bi.reason == ESP_RST_PANIC) {
    // timer WDT ของเรารีเซ็ตด้วย abort() (= PANIC) -> ดูกล่องดำเพื่อแยกว่าเป็น WDT
    const bool hwWdt = bi.planned && (bi.plannedReason == PR_HW_WDT || bi.plannedReason == PR_DEMO_HWWDT);
    if (hwWdt) st.wdtResets++;
    else st.panicResets++;
  } else if (bi.reason == ESP_RST_BROWNOUT) {
    st.brownouts++;
  }
  // ประวัติสาเหตุรีเซ็ต 8 ครั้งล่าสุด (คำสั่ง boots ใน Serial) -> ดูได้ว่าไฟตกวนลูปหรือไม่
  st.resetHist[st.resetHead % 8] = (uint8_t)bi.reason;
  st.resetHead = (uint8_t)((st.resetHead + 1) % 8);
  storage::saveStats();
}

// พิมพ์ข้อมูลเริ่มต้นทาง Serial: เวอร์ชัน, สาเหตุการรีเซ็ต, เซนเซอร์ที่เจอ, ชื่อ WiFi
static void printBanner(bool sensorsOk) {
  const wdt::BootInfo& bi = wdt::bootInfo();
  Serial.println();
  Serial.println(F("=================================================="));
  Serial.printf(" %s  v%s\n", FW_NAME, FW_VERSION);
  Serial.printf(" build %s | %s rev%d | %lu MHz\n", FW_BUILD, ESP.getChipModel(), ESP.getChipRevision(),
                (unsigned long)getCpuFrequencyMhz());
  Serial.printf(" boot #%lu | reset: %s (%s)\n", (unsigned long)storage::stats().bootCount,
                wdt::resetReasonName(bi.reason), wdt::resetReasonThai(bi.reason));
  if (bi.planned)
    Serial.printf(" black box: %s (task '%s', ran %lus before)\n", wdt::plannedReasonName(bi.plannedReason),
                  bi.plannedTask, (unsigned long)bi.prevUptimeS);
  if (bi.coredump) Serial.printf(" core dump: crashed in task '%s' at PC 0x%08lX\n", bi.cdTask, (unsigned long)bi.cdPc);
  Serial.printf(" sensors: %s | storage: NVS+EEPROM %s, LittleFS %s\n", sensorsOk ? "OK" : "CHECK WIRING",
                "OK", storage::fsOk() ? "OK" : "FAIL");
  Serial.printf(" WiFi: \"%s\" / \"%s\" -> http://192.168.4.1  (UDP %u)\n", AP_SSID, AP_PASS, UDP_PORT);
  Serial.println(F(" พิมพ์ help + Enter เพื่อดูคำสั่ง"));
  Serial.println(F("=================================================="));
}

// Arduino เรียกครั้งเดียวตอนเปิดเครื่อง: Serial -> watchdog -> NVS/LittleFS -> เซนเซอร์ -> WiFi -> สร้าง 6 task
void setup() {
  Serial.begin(115200);
#if ARDUINO_USB_CDC_ON_BOOT && ARDUINO_USB_MODE
  // USB CDC ของ ESP32-C3: รอส่งได้ไม่เกิน 50 ms ถ้าคอมไม่ได้อ่าน แล้วข้ามไป
  // (ห้ามตั้ง 0! ใน core 2.0.17 ค่า 0 ทำให้ HWCDC::write() ตั้ง connected=false
  //  ตั้งแต่ข้อความยาวครั้งแรก -> ข้อความตอบกลับใน Serial Monitor หายหมด — บั๊กของ v2.0.0)
  Serial.setTxTimeoutMs(50);
#endif

  power::handleWakeEarly();      // (1) อาจหลับต่อทันทีถ้าไม่มีใครใส่นาฬิกา
  wdt::captureBootInfo();        // (2) ต้องอ่านก่อนอย่างอื่นเขียนทับ RTC record
  storage::begin();              // (3)
  countBootReason();

  app::createSyncObjects();
  ui::begin();
  // ไฟ LED บอกสาเหตุการบูต (ดูได้แม้ไม่มีสาย USB): 2 ครั้ง = ปกติ, 4 = watchdog/ล่ม,
  // 6 ครั้งรัว = ไฟตก (brownout) -> ถ้าเห็น 6 ครั้งวนซ้ำเรื่อย ๆ = แหล่งจ่ายไฟจ่ายไม่พอ
  const esp_reset_reason_t rr = wdt::bootInfo().reason;
  const bool brownout = rr == ESP_RST_BROWNOUT;
  ui::bootBlink(brownout ? 6 : (wdt::isWdtReset(rr) || rr == ESP_RST_PANIC) ? 4 : 2);

  const bool sensorsOk = sensors::begin();   // (4)
  lie::Config cfg;
  storage::toEngineConfig(storage::settings(), cfg);
  app::engine.configure(cfg);
  mlrt::begin();                 // โหลดโมเดล AI จาก NVS (ถ้ามี) แล้วผูกกับ LieEngine
  power::begin();
  // เปิด WiFi: ถ้ารีเซ็ตครั้งก่อนเพราะไฟตก -> รอให้ไฟนิ่ง + ลด CPU + กำลังส่งต่ำสุดก่อน
  uint8_t wifiLevel = storage::settings().wifiPower;
  if (brownout) {
    wifiLevel = 3;               // 2 dBm (ระยะสั้นลง แต่กินกระแสพีคน้อยสุด)
    setCpuFrequencyMhz(80);
    delay(800);
  }
  const bool wifiOk = net::beginAp(wifiLevel);
  if (brownout && !storage::settings().eco) setCpuFrequencyMhz(CPU_MHZ_NORMAL);
  ui::wifiSignal(wifiOk);
  web::begin();
  wdt::beginTaskWdt();
  tasks::startAll();
  wdt::beginHwWdt();
  app::touchActivity();
  printBanner(sensorsOk);

  const wdt::BootInfo& bi = wdt::bootInfo();
  app::logEvent("BOOT", "fw %s boot#%lu reset=%s planned=%s wake=%s", FW_VERSION,
                (unsigned long)storage::stats().bootCount, wdt::resetReasonName(bi.reason),
                bi.planned ? wdt::plannedReasonName(bi.plannedReason) : "none",
                power::wakeCauseName(bi.wakeCause));
  if (bi.coredump) app::logEvent("CRASH", "previous crash in task '%s' PC=0x%08lX", bi.cdTask, (unsigned long)bi.cdPc);
  if (!sensorsOk)
    app::logEvent("SENSOR", "MAX30102=%s MPU=%s (will retry every 10 s)", sensors::maxOk() ? "ok" : "missing",
                  sensors::mpuOk() ? "ok" : "missing");
  if (brownout) app::logEvent("POWER", "previous reset was BROWNOUT -> WiFi started at minimum TX power");
  if (!wifiOk) app::logEvent("WIFI", "softAP failed to start");
}

// loop() = Arduino loopTask (priority 1): ใช้เป็น Serial console อย่างเดียว
// งานจริงทั้งหมดอยู่ใน FreeRTOS task ที่สร้างใน tasks::startAll()
void loop() {
  cli::poll();
  delay(20);
}
