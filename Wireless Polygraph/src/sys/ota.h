// =====================================================================
//  ota.h — อัปเดตเฟิร์มแวร์ไร้สาย 2 ทาง + ป้องกันเฟิร์มแวร์เสียด้วย rollback
//
//   1) Web OTA : เปิด http://192.168.4.1/update (user admin / รหัส polygraph-ota)
//                หรือกดปุ่มในหน้า "ระบบ" ของ Polygraph Studio
//   2) ArduinoOTA (espota): PlatformIO env:esp32c3_ota -> Upload
//
//  การทำงาน (ใช้ flash map app0/app1/otadata):
//   เฟิร์มแวร์ใหม่ถูกเขียนลงช่องที่ "ไม่ได้รันอยู่" -> otadata ชี้ไปช่องใหม่ -> รีบูต
//   -> bootloader ตั้งสถานะ PENDING_VERIFY -> เฟิร์มแวร์ใหม่ต้องทำงานปกติ 20 s
//   แล้วเรียก esp_ota_mark_app_valid_cancel_rollback()
//   ถ้ารีเซ็ตก่อนยืนยัน (เช่นล่มวนลูป) -> bootloader ย้อนกลับไปช่องเดิมเอง
// =====================================================================
#pragma once
#include <Arduino.h>

class WebServer;
class Json;

namespace ota {

void begin(WebServer& server);    // ลงทะเบียน /update + เริ่ม ArduinoOTA + mDNS
void handle();                    // เรียกใน httpTask ทุกรอบ
void restartNetwork();            // เปิด ArduinoOTA + mDNS ใหม่หลัง WiFi รีสตาร์ท (เรียกจาก httpTask)
void serviceVerify();             // supervisor เรียกทุก 1 s
void scanSlots();                 // หาเวอร์ชันเฟิร์มแวร์ในช่อง app0/app1 (supervisor เรียกครั้งเดียว)
bool pendingVerify();
const char* runningLabel();
void appendJson(Json& j);         // ข้อมูล OTA/พาร์ทิชันสำหรับ /api/system

}  // namespace ota
