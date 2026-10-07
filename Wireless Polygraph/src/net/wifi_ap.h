// =====================================================================
//  wifi_ap.h — นาฬิกาปล่อย WiFi เอง (SoftAP) ไม่ต้องพึ่งเราเตอร์
//  SSID "Polygraph-Watch" / รหัส "polygraph123" / IP นาฬิกา 192.168.4.1
//  TX power 8.5 dBm: ทดสอบแล้วว่าพอสำหรับในห้อง และลดกระแสพีคตอนส่ง
//  (กระแสพีคสูง + หน้าสัมผัสหลวม = ไฟตก -> brownout reset)
// =====================================================================
#pragma once
#include <Arduino.h>

namespace net {

// level: 0 = ต่ำ (5 dBm), 1 = กลาง (8.5 dBm), 2 = สูง (13 dBm), 3 = ต่ำสุด (2 dBm, โหมดกันไฟตก)
bool beginAp(uint8_t level = 1);
void setPowerLevel(uint8_t level);
const char* powerName(uint8_t level);
uint8_t powerLevel();
void stopAp();
bool apRunning();
uint8_t stations();          // จำนวนเครื่องที่ต่อ WiFi อยู่
int8_t bestRssi();           // สัญญาณของเครื่องที่แรงที่สุด (dBm), 0 = ไม่มี
const char* deviceId();      // "PW-XXXX" จาก MAC address
const char* macStr();

}  // namespace net
