// =====================================================================
//  wifi_ap.cpp — เปิด WiFi ของนาฬิกาเอง (SoftAP "Polygraph-Watch", IP 192.168.4.1)
//  ทำอะไร   : ตั้งกำลังส่งก่อนเริ่มปล่อยสัญญาณ (ลดกระแสพีค), ตั้งระดับ ต่ำ/กลาง/สูง/ต่ำสุดกันไฟตก,
//             แจ้ง log เมื่อมีเครื่องเข้า/ออก
//  ทำไม     : ใช้ได้ทุกที่ไม่ต้องพึ่งเราเตอร์ และกำลังส่งต่ำ = กินไฟน้อย/ไม่ทำให้ไฟตกตอนใช้แบต
//  เรียกจาก : main.cpp setup(), power.cpp (เปิดใหม่หลัง light sleep), cli.cpp/web_server.cpp (ปรับระดับ)
//  วิชา     : WiFi AP mode, Power optimization
// =====================================================================
#include "wifi_ap.h"
#include <WiFi.h>
#include "esp_wifi.h"
#include "../config.h"
#include "../app.h"

namespace net {

namespace {
char s_id[12] = "PW-0000";
char s_mac[18] = "";
bool s_running = false;
uint8_t s_level = 1;

// ระดับ 0/1/2 -> กำลังส่ง 5/8.5/13 dBm (3 = 2 dBm ต่ำสุด ใช้ภายใน)
wifi_power_t toPower(uint8_t level) {
  switch (level) {
    case 0: return WIFI_POWER_5dBm;
    case 2: return WIFI_POWER_13dBm;
    case 3: return WIFI_POWER_2dBm;
    default: return WIFI_POWER_8_5dBm;
  }
}
bool s_eventsHooked = false;

// มีเครื่องเชื่อม/หลุดจาก WiFi ของนาฬิกา -> จด log (ทำสั้น ๆ เพราะรันใน task ของ WiFi)
// [เทคนิค: Event-driven WiFi callback] ระบบ WiFi เรียกเมื่อมีเครื่องเข้า/ออก -> ทำงานสั้น ๆ (จด log, ต่อเวลาก่อนหลับ)
void onWifiEvent(arduino_event_id_t ev, arduino_event_info_t info) {
  // callback นี้รันใน task ของ WiFi event -> ทำงานสั้น ๆ แล้วส่งต่อผ่านคิว
  if (ev == ARDUINO_EVENT_WIFI_AP_STACONNECTED) {
    const uint8_t* m = info.wifi_ap_staconnected.mac;
    app::logEvent("WIFI", "device joined %02X:%02X:%02X:%02X:%02X:%02X", m[0], m[1], m[2], m[3],
                  m[4], m[5]);
    app::touchActivity();
  } else if (ev == ARDUINO_EVENT_WIFI_AP_STADISCONNECTED) {
    app::logEvent("WIFI", "device left");
  }
}
}  // namespace

// เปิด WiFi แบบ SoftAP ("Polygraph-Watch", 192.168.4.1) ตามระดับกำลังส่งที่ตั้งไว้
// [เทคนิค: WiFi AP mode + TX power control] ตั้งกำลังส่งก่อนเปิด AP เพื่อลดกระแสพีค (สาเหตุ WiFi ไม่ขึ้นตอนใช้แบต)
bool beginAp(uint8_t level) {
  s_level = level;
  if (!s_eventsHooked) {
    WiFi.onEvent(onWifiEvent);
    s_eventsHooked = true;
  }
  WiFi.persistent(false);                       // ไม่เขียนค่า WiFi ลงแฟลชทุกครั้ง (ลดการสึก)
  WiFi.mode(WIFI_AP);
  // ลดกำลังส่ง "ก่อน" เริ่มปล่อย beacon: กระแสพีคช่วงเปิด WiFi สูงสุดของทั้งระบบ
  // ถ้าแหล่งจ่ายจากแบต (HT7333 + ขา socket) จ่ายไม่ทัน แรงดันจะตก -> brownout รีเซ็ตวน
  WiFi.setTxPower(toPower(level));
  bool ok = WiFi.softAP(AP_SSID, AP_PASS, AP_CHANNEL, 0, AP_MAX_CLIENTS);
  WiFi.setTxPower(toPower(level));     // บางเวอร์ชันรีเซ็ตค่าตอน softAP -> ตั้งซ้ำ
  uint8_t mac[6];
  WiFi.softAPmacAddress(mac);
  snprintf(s_id, sizeof(s_id), "PW-%02X%02X", mac[4], mac[5]);
  snprintf(s_mac, sizeof(s_mac), "%02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1], mac[2], mac[3],
           mac[4], mac[5]);
  s_running = ok;
  if (ok) app::setBits(EV_WIFI_UP);
  return ok;
}

// ปิด WiFi (ก่อนหลับ/ประหยัดไฟ)
void stopAp() {
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_OFF);
  s_running = false;
  app::clearBits(EV_WIFI_UP | EV_CLIENT);
}

bool apRunning() { return s_running; }

uint8_t stations() { return s_running ? WiFi.softAPgetStationNum() : 0; }

// ความแรงสัญญาณของเครื่องที่ต่ออยู่ (ดีที่สุด) — แสดงในหน้าระบบ
int8_t bestRssi() {
  if (!s_running) return 0;
  wifi_sta_list_t list;
  if (esp_wifi_ap_get_sta_list(&list) != ESP_OK || list.num == 0) return 0;
  int8_t best = -127;
  for (int i = 0; i < list.num; i++)
    if (list.sta[i].rssi > best) best = list.sta[i].rssi;
  return best;
}

// เปลี่ยนกำลังส่ง WiFi ทันที
void setPowerLevel(uint8_t level) {
  s_level = level;
  if (s_running) WiFi.setTxPower(toPower(level));
}

uint8_t powerLevel() { return s_level; }

// ชื่อระดับกำลังส่งเป็นข้อความ
const char* powerName(uint8_t level) {
  switch (level) {
    case 0: return "low (5 dBm)";
    case 2: return "high (13 dBm)";
    case 3: return "minimum (2 dBm, brownout-safe)";
    default: return "medium (8.5 dBm)";
  }
}

const char* deviceId() { return s_id; }
const char* macStr() { return s_mac; }

}  // namespace net
