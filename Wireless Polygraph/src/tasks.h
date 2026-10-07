// =====================================================================
//  tasks.h — สร้าง FreeRTOS task ทั้งหมด + Hardware Timer ที่เป็นนาฬิกาจังหวะการสุ่มสัญญาณ
//
//  Task           Prio  Stack  งาน
//  sensor           5   4096   ถูกปลุกด้วย HW timer ISR ทุก 10 ms -> อ่านเซนเซอร์ + DSP
//  engine           4   6144   รับ frame 5 Hz จากคิว -> LieEngine -> ผล/เหตุการณ์
//  telemetry        3   4096   UDP: รับ hello, ส่งค่าสด/คลื่น/เหตุการณ์
//  http             2   8192   WebServer + REST + OTA
//  ui               2   3072   LED (PWM) + ปุ่ม BOOT
//  supervisor       1   6144   เขียน LittleFS, ป้อน HW-WDT, CPU%, แบต, sleep, ยืนยัน OTA
//  loopTask (Arduino) 1 8192   Serial console (พิมพ์ help)
// =====================================================================
#pragma once
#include <Arduino.h>

namespace tasks {

void startAll();          // สร้าง task + เริ่ม timer สุ่มสัญญาณ
uint32_t missedTicks();   // จำนวนครั้งที่ sensorTask รอ tick นานเกินคาด

}  // namespace tasks
