// =====================================================================
//  ota.cpp — อัปเดตเฟิร์มแวร์ไร้สาย (หน้า /update และ espota) + rollback อัตโนมัติ
//  ทำอะไร   : เขียนไฟล์ใหม่ลง app slot ที่ไม่ได้ใช้ -> รีบูต -> serviceVerify() ถ้าทำงานปกติครบ 20 วินาที
//             จึง mark valid ไม่งั้น bootloader ย้อนกลับเวอร์ชันเดิม, scanSlots() อ่านเวอร์ชันในทั้งสอง slot
//  เรียกจาก : web_server.cpp (ลงทะเบียน /update), tasks.cpp (supervisor ตรวจ verify)
//  วิชา     : Memory (partition table, app0/app1, otadata)
// =====================================================================
#include "ota.h"
#include <WebServer.h>
#include <Update.h>
#include <ArduinoOTA.h>
#include <ESPmDNS.h>
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "../config.h"
#include "../app.h"
#include "../net/json_writer.h"
#include "../net/web_page.h"
#include "storage.h"
#include "watchdog.h"
#include "power.h"

// Arduino-ESP32 จะยืนยันเฟิร์มแวร์ OTA ให้ทันทีตอนบูต (ฟังก์ชัน weak คืน false)
// เรา override ให้คืน true = "เดี๋ยวยืนยันเอง" หลังพิสูจน์ว่าทำงานได้จริง 20 s
extern "C" bool verifyRollbackLater() { return true; }

// ป้ายเวอร์ชันฝังในเฟิร์มแวร์ (อยู่ใน .rodata ของไฟล์ .bin)
// ใช้ค้นหาว่าช่อง app อีกช่องเก็บเฟิร์มแวร์เวอร์ชันไหนไว้ (ข้อมูล esp_app_desc ของ Arduino
// เป็นของตัว core ที่คอมไพล์ไว้ก่อน ไม่ใช่ของโปรเจกต์เรา จึงใช้แยกเวอร์ชันไม่ได้)
extern const char kFwTag[];
const char kFwTag[] __attribute__((used)) = "@@PWFW@@" FW_VERSION "|" FW_BUILD "@@";

namespace ota {

namespace {
WebServer* s_srv = nullptr;
char s_slotTag[2][40] = {"", ""};    // เวอร์ชันในแต่ละช่อง app (app0, app1)
bool s_scanned = false;

// ค้นหา "@@PWFW@@...@@" ในพาร์ทิชัน (อ่านทีละ 1 KB เก็บส่วนท้ายไว้กันข้อความขาดคร่อมบล็อก)
bool findFwTag(const esp_partition_t* p, char* out, size_t outLen) {
  static const char kMagic[] = "@@PWFW@@";
  const size_t mlen = sizeof(kMagic) - 1;
  const size_t kChunk = 1024, kKeep = 64;
  // ไบต์แรกของ image เฟิร์มแวร์ ESP32 ต้องเป็น 0xE9 — ไม่ใช่ = ช่องว่าง/เสีย ไม่ต้องค้น
  uint8_t magic = 0;
  if (esp_partition_read(p, 0, &magic, 1) != ESP_OK || magic != 0xE9) return false;
  uint8_t* buf = (uint8_t*)malloc(kChunk + kKeep);
  if (!buf) return false;
  size_t keep = 0;
  bool found = false;
  for (size_t off = 0; off < p->size && !found; off += kChunk) {
    if ((off & 0xFFFF) == 0) wdt::feed();
    const size_t n = (p->size - off) < kChunk ? (p->size - off) : kChunk;
    if (esp_partition_read(p, off, buf + keep, n) != ESP_OK) break;
    const size_t total = keep + n;
    for (size_t i = 0; i + mlen <= total; i++) {
      if (buf[i] == '@' && memcmp(buf + i, kMagic, mlen) == 0) {
        size_t j = i + mlen, k = 0;
        while (j + 1 < total && k + 1 < outLen && !(buf[j] == '@' && buf[j + 1] == '@')) out[k++] = (char)buf[j++];
        out[k] = 0;
        found = k > 0 && j + 1 < total;   // ต้องเจอ @@ ปิดท้ายในบัฟเฟอร์
        if (found) break;
      }
    }
    if (!found) {
      keep = total < kKeep ? total : kKeep;
      memmove(buf, buf + total - keep, keep);
    }
  }
  free(buf);
  return found;
}
bool s_authOk = false;
bool s_uploadOk = false;
size_t s_uploadSize = 0;
bool s_pending = false;
bool s_checked = false;

// สถานะของเฟิร์มแวร์ใน slot OTA เป็นข้อความ
const char* stateName(esp_ota_img_states_t s) {
  switch (s) {
    case ESP_OTA_IMG_NEW: return "new";
    case ESP_OTA_IMG_PENDING_VERIFY: return "pending_verify";
    case ESP_OTA_IMG_VALID: return "valid";
    case ESP_OTA_IMG_INVALID: return "invalid";
    case ESP_OTA_IMG_ABORTED: return "aborted";
    default: return "undefined";
  }
}

// อัปโหลดเฟิร์มแวร์เสร็จ: ตอบผล แล้วรีบูตเข้า slot ใหม่ (ถ้าสำเร็จ)
void handleUpdateDone() {
  if (!s_authOk) {
    s_srv->send(401, "application/json", "{\"ok\":false,\"error\":\"AUTH\"}");
    return;
  }
  const bool ok = s_uploadOk && !Update.hasError();
  s_srv->sendHeader("Connection", "close");
  if (ok) {
    s_srv->send(200, "application/json", "{\"ok\":true,\"msg\":\"rebooting\"}");
    storage::stats().otaUpdates++;
    app::logEvent("OTA", "web upload ok (%u bytes) -> reboot", (unsigned)s_uploadSize);
    power::requestRestart(PR_OTA);
  } else {
    String js = String("{\"ok\":false,\"error\":\"") + Update.errorString() + "\"}";
    s_srv->send(500, "application/json", js);
    app::logEvent("OTA", "web upload failed: %s", Update.errorString());
  }
}

// รับไฟล์เฟิร์มแวร์ทีละช่วงแล้วเขียนลง slot OTA ที่ไม่ได้ใช้อยู่ (ตรวจรหัสผ่านก่อน)
void handleUpload() {
  HTTPUpload& up = s_srv->upload();
  if (up.status == UPLOAD_FILE_START) {
    // header ถูกอ่านครบแล้วตอนเริ่ม body -> ตรวจรหัสผ่านได้ตรงนี้
    s_authOk = s_srv->authenticate(OTA_USER, OTA_PASSWORD);
    s_uploadSize = 0;
    if (!s_authOk) return;
    app::setBits(EV_OTA_ACTIVE);
    app::logEvent("OTA", "web upload start: %s", up.filename.c_str());
    s_uploadOk = Update.begin(UPDATE_SIZE_UNKNOWN, U_FLASH);
  } else if (up.status == UPLOAD_FILE_WRITE) {
    if (s_authOk && s_uploadOk) {
      if (Update.write(up.buf, up.currentSize) != up.currentSize) s_uploadOk = false;
      s_uploadSize += up.currentSize;
    }
    // อัปโหลด ~1 MB ใช้หลายวินาทีภายใน handleClient() ครั้งเดียว -> ต้องป้อน WDT ระหว่างทาง
    wdt::feed();
    app::heartbeat(T_HTTP);
  } else if (up.status == UPLOAD_FILE_END) {
    if (s_authOk && s_uploadOk) s_uploadOk = Update.end(true);
    app::clearBits(EV_OTA_ACTIVE);
  } else if (up.status == UPLOAD_FILE_ABORTED) {
    Update.abort();
    s_uploadOk = false;
    app::clearBits(EV_OTA_ACTIVE);
    app::logEvent("OTA", "web upload aborted");
  }
}
}  // namespace

// ลงทะเบียนหน้า /update (ต้องใส่รหัส) + ArduinoOTA + ตรวจว่าเฟิร์มแวร์ใหม่รอยืนยันหรือไม่
void begin(WebServer& server) {
  s_srv = &server;
  server.on("/update", HTTP_GET, []() {
    if (!s_srv->authenticate(OTA_USER, OTA_PASSWORD)) return s_srv->requestAuthentication();
    s_srv->send_P(200, "text/html", UPDATE_HTML);
  });
  server.on("/update", HTTP_POST, handleUpdateDone, handleUpload);

  ArduinoOTA.setHostname(MDNS_HOST);
  ArduinoOTA.setPassword(OTA_PASSWORD);
  ArduinoOTA.setPort(3232);
  ArduinoOTA.onStart([]() {
    app::setBits(EV_OTA_ACTIVE);
    app::logEvent("OTA", "espota start (%s)", ArduinoOTA.getCommand() == U_FLASH ? "firmware" : "filesystem");
  });
  ArduinoOTA.onProgress([](unsigned int, unsigned int) {
    wdt::feed();                      // handle() วนรับข้อมูลจนจบ -> ป้อน WDT ระหว่างทาง
    app::heartbeat(T_HTTP);
  });
  ArduinoOTA.onEnd([]() {
    storage::stats().otaUpdates++;
    storage::saveStats();
    storage::setPlanned(PR_OTA, "espota", 0);
  });
  ArduinoOTA.onError([](ota_error_t e) {
    app::clearBits(EV_OTA_ACTIVE);
    app::logEvent("OTA", "espota error %d", (int)e);
  });
  ArduinoOTA.begin();                 // เริ่ม mDNS "polygraph.local" ให้ด้วย
  MDNS.addService("http", "tcp", HTTP_PORT);

  const esp_partition_t* run = esp_ota_get_running_partition();
  esp_ota_img_states_t st;
  if (run && esp_ota_get_state_partition(run, &st) == ESP_OK && st == ESP_OTA_IMG_PENDING_VERIFY) {
    s_pending = true;
    app::logEvent("OTA", "new firmware on %s waiting for verification (%lus)", run->label,
                  (unsigned long)(OTA_VERIFY_AFTER_MS / 1000));
  }
  s_checked = true;
}

void handle() { ArduinoOTA.handle(); }

// เปิด ArduinoOTA/mDNS ใหม่หลัง WiFi เริ่มใหม่ (เช่น ตื่นจากหลับ)
void restartNetwork() {
  ArduinoOTA.end();                   // ปิด socket เดิม + MDNS.end()
  ArduinoOTA.begin();                 // เปิดใหม่บน WiFi รอบใหม่
  MDNS.addService("http", "tcp", HTTP_PORT);
}

// ดูว่า slot ไหนกำลังรัน / slot ไหนรอยืนยัน (ทำครั้งเดียว)
void scanSlots() {
  if (s_scanned) return;
  s_scanned = true;
  const esp_partition_t* run = esp_ota_get_running_partition();
  for (int slot = 0; slot < 2; slot++) {
    const esp_partition_t* p = esp_partition_find_first(
        ESP_PARTITION_TYPE_APP, (esp_partition_subtype_t)(ESP_PARTITION_SUBTYPE_APP_OTA_0 + slot), NULL);
    if (!p) continue;
    if (run && p->address == run->address) {
      snprintf(s_slotTag[slot], sizeof(s_slotTag[slot]), "%s|%s", FW_VERSION, FW_BUILD);
    } else if (!findFwTag(p, s_slotTag[slot], sizeof(s_slotTag[slot]))) {
      strcpy(s_slotTag[slot], "");    // ว่าง หรือไม่ใช่เฟิร์มแวร์ของโปรเจกต์นี้
    }
  }
}

// เฟิร์มแวร์ใหม่ทำงานได้ครบ 20 วินาที + WiFi ใช้ได้ -> ยืนยัน (ไม่งั้นบูตหน้าจะย้อนกลับเวอร์ชันเดิม)
void serviceVerify() {
  if (!s_checked || !s_pending) return;
  // เงื่อนไข "ทำงานได้จริง": บูตผ่านมาครบเวลา + WiFi ทำงาน + task หลักยังรายงานตัว
  const uint32_t now = millis();
  const bool tasksAlive = now - app::tasks[T_SENSOR].lastBeatMs < 2000 &&
                          now - app::tasks[T_ENGINE].lastBeatMs < 3000;
  if (now > OTA_VERIFY_AFTER_MS && (app::bits() & EV_WIFI_UP) && tasksAlive) {
    if (esp_ota_mark_app_valid_cancel_rollback() == ESP_OK) {
      s_pending = false;
      app::logEvent("OTA", "firmware verified -> rollback cancelled");
    }
  }
}

bool pendingVerify() { return s_pending; }

// ชื่อ slot ที่กำลังรัน (app0 / app1)
const char* runningLabel() {
  const esp_partition_t* run = esp_ota_get_running_partition();
  return run ? run->label : "?";
}

// ส่วน JSON สถานะ OTA ในหน้าระบบ
void appendJson(Json& j) {
  const esp_partition_t* run = esp_ota_get_running_partition();
  const esp_partition_t* boot = esp_ota_get_boot_partition();
  j.obj("ota");
  j.kv("running", run ? run->label : "?");
  j.kv("boot", boot ? boot->label : "?");
  j.kv("pendingVerify", s_pending);
  j.kv("rollbackPossible", esp_ota_check_rollback_is_possible());
  j.kv("invalidExists", wdt::bootInfo().otaInvalidExists);
  j.kv("updates", (unsigned long)storage::stats().otaUpdates);
  j.end();

  // flash map: ทุกพาร์ทิชันพร้อมแอดเดรส/ขนาด + เวอร์ชันเฟิร์มแวร์ในแต่ละช่อง app
  j.arr("partitions");
  for (int type = 0; type <= 1; type++) {
    esp_partition_iterator_t it = esp_partition_find(
        type == 0 ? ESP_PARTITION_TYPE_APP : ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, NULL);
    for (; it; it = esp_partition_next(it)) {
      const esp_partition_t* p = esp_partition_get(it);
      j.obj();
      j.kv("label", p->label);
      j.kv("type", type == 0 ? "app" : "data");
      j.kv("subtype", (int)p->subtype);
      j.kv("addr", (unsigned long)p->address);
      j.kv("size", (unsigned long)p->size);
      j.kv("running", run && p->address == run->address);
      if (type == 0) {
        esp_ota_img_states_t st;
        j.kv("state", esp_ota_get_state_partition(p, &st) == ESP_OK ? stateName(st) : "undefined");
        const int slot = (int)p->subtype - (int)ESP_PARTITION_SUBTYPE_APP_OTA_0;
        if (slot >= 0 && slot < 2) j.kv("firmware", s_slotTag[slot]);   // "2.0.0|Oct  6 2026 10:00:00"
      }
      j.end();
    }
    if (it) esp_partition_iterator_release(it);
  }
  j.end();
}

}  // namespace ota
