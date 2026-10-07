// =====================================================================
//  web_server.h — HTTP server พอร์ต 80: หน้าเว็บในนาฬิกา + REST API + OTA
//
//  GET  /                         หน้าเว็บ (มือถือ)
//  GET  /api/info                 ข้อมูลเครื่อง + สาเหตุการบูตครั้งนี้
//  GET  /api/live                 ค่าที่วัดได้ล่าสุด + สถานะ LieEngine
//  GET  /api/lie?n=24             สถานะ engine, baseline, calibration, ผลคำถาม
//  POST /api/lie/baseline?sec=30
//  POST /api/lie/question?qid=3&kind=test|truth|lie|warmup
//  POST /api/lie/answer?ans=yes|no
//  POST /api/lie/abort            POST /api/lie/reset
//  GET  /api/config               POST /api/config?key=value&...   POST /api/config/reset
//  GET  /api/system               RTOS tasks, RAM, flash map, WDT, พลังงาน, เครือข่าย
//  GET  /api/logs?file=events|events1|results|results1     POST /api/logs/clear
//  POST /api/stats/reset
//  POST /api/power?mode=normal|eco|light|deep&sec=N
//  POST /api/restart
//  POST /api/demo?type=twdt|hwwdt|panic|intwdt&confirm=yes   (สาธิต watchdog — เครื่องจะรีเซ็ต!)
//  --- v2.1: AI / การหลับ / WiFi ---
//  GET  /api/ml                   โหมด, ชื่อผู้ถูกทดสอบ, จำนวนข้อมูลเทรน, ข้อมูลโมเดล
//  POST /api/ml/mode?mode=train|detect
//  POST /api/ml/subject?name=Somchai
//  GET  /api/ml/data.csv          ดาวน์โหลดข้อมูลเทรนทั้งหมด (CSV)
//  POST /api/ml/data/clear?confirm=yes
//  POST /api/ml/model             ติดตั้งโมเดล (form: features, mean, scale, w, b, acc, samples, trained, name)
//  POST /api/ml/model/clear
//  POST /api/sleep?auto=0..240    ตั้งเวลาหลับอัตโนมัติ (0 = ปิด)
//  POST /api/sleep?now=light|deep&sec=N
//  POST /api/wifi?level=0|1|2     กำลังส่ง WiFi ต่ำ/กลาง/สูง
//  POST /api/time?epoch=1791400000  ตั้งเวลาจากมือถือ/คอม (ใช้ประทับเวลาในไฟล์ CSV)
//  GET/POST /update               OTA ผ่านเว็บ (Basic auth)
//  ทุกคำตอบเป็น JSON: {"ok":true,...} หรือ {"ok":false,"error":"CODE","msg":"ข้อความไทย"}
// =====================================================================
#pragma once
#include <Arduino.h>

namespace web {

void begin();
void task(void*);          // httpTask
void notifyNetworkRestart();   // WiFi เพิ่งเปิดใหม่ (หลัง light sleep) -> เปิด server/OTA/mDNS ใหม่
uint32_t requests();

}  // namespace web
