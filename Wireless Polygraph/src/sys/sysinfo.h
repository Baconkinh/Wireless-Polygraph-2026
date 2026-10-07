// =====================================================================
//  sysinfo.h — รวบรวมข้อมูลภายในระบบสำหรับหน้า "ระบบ" (RTOS, หน่วยความจำ, WDT, พลังงาน)
//  ทุกค่ามาจาก API จริงของ ESP-IDF/FreeRTOS ณ ตอนที่ขอ ไม่ใช่ค่าที่เขียนตายตัว
// =====================================================================
#pragma once
#include <Arduino.h>

class Json;

namespace sysinfo {

void cacheSlowValues();             // ค่าที่คำนวณนาน (ขนาดเฟิร์มแวร์) — supervisor เรียกครั้งเดียว
void buildSystemJson(Json& j);      // /api/system
void buildInfoJson(Json& j);        // /api/info (ข้อมูลเครื่อง + การบูตครั้งนี้)
const char* taskStateName(int s);

}  // namespace sysinfo
