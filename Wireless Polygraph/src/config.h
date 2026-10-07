// =====================================================================
//  config.h — ค่าคงที่ทั้งหมดของระบบรวมไว้ที่เดียว
//  ถ้าจะปรับฮาร์ดแวร์/เวลา/เกณฑ์ ให้แก้ที่ไฟล์นี้ก่อน
//  (ค่าที่ต้องปรับบ่อยขณะใช้งาน เช่น เกณฑ์ตัดสิน อยู่ใน Settings -> เก็บใน NVS
//   แก้ได้จากหน้าเว็บโดยไม่ต้องคอมไพล์ใหม่ ดู sys/storage.h)
// =====================================================================
#pragma once
#include <stdint.h>

// ---------------- ข้อมูลเฟิร์มแวร์ ----------------
#define FW_NAME     "Wireless Polygraph Watch"
#define FW_VERSION  "2.1.0"
#define FW_BUILD    __DATE__ " " __TIME__

// ---------------- ขา GPIO (PCB REV2 — ตรวจกับบอร์ดจริงแล้ว) ----------------
constexpr uint8_t PIN_SDA    = 6;   // J1: I2C data  (MAX30102 + MPU6050)
constexpr uint8_t PIN_SCL    = 7;   // J1: I2C clock
constexpr uint8_t PIN_NTC    = 0;   // J5: ADC1_CH0  NTC 10k (บน) + R1 10k (ล่าง)
constexpr uint8_t PIN_GSR    = 1;   // J5: ADC1_CH1  3V3-R2-ผิว-R3-GND
constexpr uint8_t PIN_VBAT   = 4;   // J5: ADC1_CH4  แบต/2 (R4=R5=100k)
constexpr uint8_t PIN_LED    = 8;   // LED สีฟ้าบนตัว SuperMini (ไม่ได้ต่อกับ PCB)
constexpr uint8_t PIN_BUTTON = 9;   // ปุ่ม BOOT บนตัว SuperMini (ไม่ได้ต่อกับ PCB)

// LED บน SuperMini ต่อระหว่าง 3V3 กับ GPIO8 -> เขียน LOW = ติด
// ถ้าบอร์ดล็อตของคุณกลับด้าน (ไฟติดตอนควรดับ) ให้เปลี่ยนเป็น false
constexpr bool LED_ACTIVE_LOW = true;

// ---------------- I2C ----------------
constexpr uint32_t I2C_FREQ_HZ    = 400000;  // ทั้งสองชิปรองรับ 400 kHz
constexpr uint16_t I2C_TIMEOUT_MS = 20;      // สั้นไว้: บัสค้างต้องไม่ลาก task อื่นค้างตาม
constexpr uint8_t  ADDR_MAX30102  = 0x57;
constexpr uint8_t  ADDR_MPU6050   = 0x68;    // AD0 ต่อ GND

// ---------------- ค่าวงจรอนาล็อก ----------------
constexpr float VCC_MV_DEFAULT = 3300.0f;    // ราง 3V3 จาก HT7333 (ปรับได้ใน Settings)
constexpr float R1_OHM   = 10000.0f;         // NTC_SENSE -> GND
constexpr float NTC_R25  = 10000.0f;         // NTC 10k @25°C
constexpr float NTC_BETA = 3950.0f;
constexpr float R2_OHM   = 100000.0f;        // 3V3 -> แผ่น GSR_A
constexpr float R3_OHM   = 100000.0f;        // แผ่น GSR_SENSE -> GND
constexpr float VBAT_DIV = 2.0f;             // R4 = R5 = 100k

// ---------------- จังหวะการสุ่มสัญญาณ ----------------
// Hardware timer ปลุก sensor task ทุก 10 ms (100 Hz)
constexpr uint32_t TICK_HZ        = 100;
constexpr uint32_t PPG_FS_HZ      = 100;     // MAX30102: 400 sps เฉลี่ย 4 = 100 sps
constexpr uint32_t ADC_EVERY_TICK = 10;      // อ่าน ADC ทุก 10 tick = 10 Hz
constexpr uint32_t FRAME_EVERY_TICK = 20;    // ส่ง frame ให้ LieEngine/UDP ทุก 200 ms = 5 Hz
constexpr uint8_t  WAVE_CHUNK     = 10;      // ส่งคลื่น PPG ทีละ 10 จุด (10 แพ็กเก็ต/วินาที)
constexpr uint8_t  ADC_OVERSAMPLE = 16;      // เฉลี่ย 16 ครั้ง ลดสัญญาณรบกวนของ ADC

// ---------------- WiFi / เครือข่าย ----------------
#define AP_SSID       "Polygraph-Watch"
#define AP_PASS       "polygraph123"         // อย่างน้อย 8 ตัว (WPA2)
#define MDNS_HOST     "polygraph"            // -> http://polygraph.local
constexpr uint8_t  AP_CHANNEL      = 1;
constexpr uint8_t  AP_MAX_CLIENTS  = 3;      // โน้ตบุ๊ก + มือถือ + สำรอง
constexpr uint16_t UDP_PORT        = 4210;
constexpr uint16_t HTTP_PORT       = 80;
constexpr uint8_t  UDP_MAX_CLIENTS = 3;
constexpr uint32_t CLIENT_TIMEOUT_MS = 10000; // ไม่ได้ "hello" 10 s = เลิกส่งให้เครื่องนั้น

// ---------------- OTA ----------------
#define OTA_USER      "admin"
#define OTA_PASSWORD  "polygraph-ota"        // ต้องตรงกับ upload_flags ใน platformio.ini
// เฟิร์มแวร์ใหม่ต้องทำงานปกติครบเวลานี้ก่อนจึง "ยืนยัน" ไม่งั้น bootloader ย้อนเวอร์ชัน
constexpr uint32_t OTA_VERIFY_AFTER_MS = 20000;

// ---------------- Watchdog ----------------
constexpr uint32_t TWDT_TIMEOUT_S     = 8;     // Task WDT: task ไหนไม่รายงานตัว 8 s -> panic+รีเซ็ต
constexpr uint32_t HWWDT_TIMEOUT_MS   = 12000; // Timer-interrupt WDT (แบบในสไลด์ บทที่ 12)
constexpr uint32_t HEARTBEAT_STALE_MS = 6500;  // supervisor ถือว่า task ตาย ถ้าเงียบเกินนี้
                                               // (> 5 s ที่ WebServer อาจรอ client ช้า)
constexpr uint32_t SENSOR_STALE_MS    = 4000;  // I2C อ่านไม่ได้ติดกันเกินนี้ -> กู้บัส

// ---------------- พลังงาน ----------------
constexpr float    BATT_PRESENT_MV = 2500.0f;  // ต่ำกว่านี้ = ไม่มีแบต/สวิตช์ปิด (ใช้ไฟ USB)
constexpr float    BATT_LOW_MV     = 3450.0f;  // เตือนแบตอ่อน
constexpr float    BATT_CRIT_MV    = 3300.0f;  // ต่ำกว่านี้ 10 s -> deep sleep ปกป้องแบต
constexpr uint32_t LOWBATT_SLEEP_S = 300;      // หลับแล้วตื่นมาเช็คแบตใหม่ทุก 5 นาที
constexpr uint8_t  CPU_MHZ_NORMAL  = 160;
constexpr uint8_t  CPU_MHZ_ECO     = 80;       // WiFi ต้องการอย่างน้อย 80 MHz

// ---------------- FreeRTOS: priority (สูง = สำคัญ) / stack (ไบต์) ----------------
// ESP32-C3 มีคอร์เดียว -> priority คือสิ่งที่ตัดสินว่าใครได้ CPU ก่อน
// WiFi (23) และ lwIP (18) ของระบบสูงกว่า task ของเราทั้งหมดอยู่แล้ว
constexpr uint8_t PRIO_SENSOR     = 5;   // ต้องตรงเวลาที่สุด (สุ่มสัญญาณ 100 Hz)
constexpr uint8_t PRIO_ENGINE     = 4;   // วิเคราะห์ 5 Hz
constexpr uint8_t PRIO_TELEMETRY  = 3;   // ส่ง UDP
constexpr uint8_t PRIO_HTTP       = 2;   // หน้าเว็บ/REST/OTA (ช้าได้)
constexpr uint8_t PRIO_UI         = 2;   // LED + ปุ่ม
constexpr uint8_t PRIO_SUPERVISOR = 1;   // งานเบื้องหลัง: เขียนแฟลช, เช็คแบต, ป้อน WDT

constexpr uint32_t STACK_SENSOR     = 4096;
constexpr uint32_t STACK_ENGINE     = 6144;  // LieEngine คำนวณ null distribution ใช้ stack มากหน่อย
constexpr uint32_t STACK_TELEMETRY  = 4096;
constexpr uint32_t STACK_HTTP       = 8192;  // สร้าง JSON ใหญ่ + OTA
constexpr uint32_t STACK_UI         = 3072;
constexpr uint32_t STACK_SUPERVISOR = 6144;  // LittleFS + core dump summary
