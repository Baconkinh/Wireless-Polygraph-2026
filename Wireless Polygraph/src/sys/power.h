// =====================================================================
//  power.h — การจัดการพลังงาน
//
//  โหมด          CPU     WiFi   RAM     ตื่นด้วย              ใช้เมื่อ
//  NORMAL        160MHz  เปิด   คงอยู่   -                    ใช้งานปกติ
//  ECO           80MHz   เปิด   คงอยู่   -                    ส่งข้อมูลช้าลง ประหยัดไฟ
//  LIGHT SLEEP   หยุด    ปิด    คงอยู่   ปุ่ม BOOT / timer      "พักชั่วคราว" baseline ยังอยู่ครบ
//  DEEP SLEEP    ปิด     ปิด    หาย*    timer (RTC)          เลิกใช้/แบตใกล้หมด
//      * เหลือแค่ RTC memory (ตัวนับใน RTC_DATA_ATTR) ตื่นมาคือบูตใหม่
//
//  STANDBY อัตโนมัติ ("ใส่แล้วตื่น"): ไม่มีใครใช้นาน -> deep sleep แล้วตื่นทุก 20 s
//  มาเช็คว่ามีผิวแตะเซนเซอร์ไหม (ใช้เวลาแค่ ~0.4 s ไม่เปิด WiFi) -> แตะ = บูตเต็มระบบ
//  (ESP32-C3 ปลุกจาก deep sleep ด้วย GPIO ได้เฉพาะขา 0–5 ซึ่งบนบอร์ดเราไม่มีปุ่ม
//   และปุ่ม BOOT คือ GPIO9 จึงใช้ timer + เช็คเซนเซอร์แทน)
// =====================================================================
#pragma once
#include <Arduino.h>

namespace power {

void handleWakeEarly();          // เรียกเป็นอย่างแรกใน setup(): ตรวจ standby/low-batt แล้วหลับต่อถ้าควร
void begin();

void setEco(bool on);
bool eco();

// คำขอจาก HTTP/CLI/ปุ่ม -> supervisor ทำให้ (ตอบ HTTP ทันก่อนเครื่องหลับ)
void requestLightSleep(uint32_t maxSec);       // 0 = จนกว่าจะกดปุ่ม BOOT
void requestDeepSleep(uint32_t sec);           // 0 = standby (ตื่นเมื่อใส่นาฬิกา)
void requestRestart(uint32_t plannedReason);

void service();                  // supervisor เรียกทุก 1 s: แบตต่ำ, auto-standby, คำขอที่ค้าง

uint32_t lightSleeps();
uint32_t lastLightSleepMs();     // ระยะเวลาหลับล่าสุด
const char* wakeCauseName(uint32_t cause);

}  // namespace power
