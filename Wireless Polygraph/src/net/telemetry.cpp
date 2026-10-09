// =====================================================================
//  telemetry.cpp — ส่งข้อมูลสดทาง UDP พอร์ต 4210 (ไม่ต้องรอ ACK = หน่วงต่ำ เหมาะกับกราฟสด)
//  ทำอะไร   : รับ "hello" จากคอม (จำ IP/พอร์ตไว้ 10 วินาที) แล้วส่ง: ค่าสด 'v' 5 Hz, คลื่น PPG 'w', เหตุการณ์ 'e'
//             (ผลการตัดสินส่งซ้ำ 2 รอบกันหาย), ตอบ ping ให้วัด latency, CSV แบบเก่าถ้าส่ง "hello" เฉย ๆ
//  เรียกจาก : tasks.cpp telemetryTask  — อ่านจาก eventQueue/waveQueue
//  ผู้รับ   : Polygraph-Studio/backend/watch_link.py, ml/collect.py, receive_data.py ของเพื่อน
//  วิชา     : WiFi, UDP socket
// =====================================================================
#include "telemetry.h"
#include <WiFi.h>
#include <WiFiUdp.h>
#include <sys/time.h>
#include <time.h>
#include "../app.h"
#include "../config.h"
#include "../sys/watchdog.h"
#include "../sys/storage.h"
#include "wifi_ap.h"
#include "../sys/ml_runtime.h"
#include "../lie/ml_model.h"

namespace telemetry {

namespace {
struct Client {
  IPAddress ip;
  uint16_t port;
  uint32_t lastHello;
  bool wave;
  bool active;
  bool csv;          // โหมดเข้ากันได้กับโค้ดเก่า: ส่งบรรทัด CSV 8 ช่อง วินาทีละครั้ง
};
struct Pending {
  EventMsg msg;
  uint32_t at;
  bool used;
};

Client s_clients[UDP_MAX_CLIENTS];
Pending s_pending[4];
WiFiUDP s_udp;
bool s_udpOn = false;
uint32_t s_sent = 0, s_err = 0, s_lastHello = 0, s_seqV = 0, s_seqW = 0;

// float -> ข้อความ (NaN/Inf ใช้ null เพราะ JSON ไม่มี NaN)
const char* f2s(char* buf, size_t n, float v, int dec) {
  if (isnan(v) || isinf(v)) snprintf(buf, n, "null");
  else snprintf(buf, n, "%.*f", dec, (double)v);
  return buf;
}

// ส่ง UDP 1 แพ็กเก็ตถึงผู้รับ 1 ราย (นับ error ถ้าส่งไม่ได้)
bool sendTo(const Client& c, const char* data, size_t len) {
  if (!s_udp.beginPacket(c.ip, c.port)) { s_err++; return false; }
  s_udp.write((const uint8_t*)data, len);
  if (!s_udp.endPacket()) { s_err++; return false; }
  s_sent++;
  return true;
}

// ส่งถึงทุกผู้รับที่ลงทะเบียน (waveClientsOnly = เฉพาะคนที่ขอคลื่นชีพจร)
void sendAll(const char* data, bool waveClientsOnly) {
  const size_t len = strlen(data);
  for (auto& c : s_clients)
    if (c.active && !c.csv && (!waveClientsOnly || c.wave)) sendTo(c, data, len);
}

// บรรทัด CSV แบบเฟิร์มแวร์รุ่นแรก: ms,bpm,finger,tremor_ms2,temp_C,gsr_uS,gsr_mV,vbat_mV
// ให้สคริปต์เดิม (udp_listen.py, receive_data.py, run_polygraph.py) ใช้ต่อได้โดยไม่ต้องแก้
void sendLegacyCsv() {
  bool any = false;
  for (auto& c : s_clients) any |= (c.active && c.csv);
  if (!any) return;
  const Vitals v = app::getVitals();
  char b[96];
  snprintf(b, sizeof(b), "%lu,%.0f,%d,%.3f,%.1f,%.2f,%.0f,%.0f\n", (unsigned long)v.ms,
           (double)(v.ppgContact ? v.hr : 0.0f), v.ppgContact ? 1 : 0, (double)v.tremor, (double)v.skinTemp,
           (double)(v.gsrContact ? v.gsr : 0.0f), (double)v.gsrMv, (double)v.vbatMv);
  const size_t len = strlen(b);
  for (auto& c : s_clients)
    if (c.active && c.csv) sendTo(c, b, len);
}

// มีผู้รับที่ขอคลื่นชีพจรอยู่ไหม (ไม่มี = ไม่ต้องเสียเวลาสร้างแพ็กเก็ตคลื่น)
bool anyWave() {
  for (auto& c : s_clients)
    if (c.active && c.wave) return true;
  return false;
}

// ส่งข้อความทักทาย (รหัสเครื่อง, เวอร์ชัน, เลขบูต, สาเหตุรีเซ็ต) ให้ผู้รับใหม่
void sendHi(const Client& c) {
  const wdt::BootInfo& bi = wdt::bootInfo();
  char b[320];
  snprintf(b, sizeof(b),
           "{\"t\":\"hi\",\"id\":\"%s\",\"fw\":\"%s\",\"build\":\"%s\",\"boot\":%lu,"
           "\"reason\":\"%s\",\"planned\":\"%s\",\"uptime\":%lu,\"ip\":\"192.168.4.1\",\"http\":%u}",
           net::deviceId(), FW_VERSION, FW_BUILD, (unsigned long)storage::stats().bootCount,
           wdt::resetReasonName(bi.reason),
           bi.planned ? wdt::plannedReasonName(bi.plannedReason) : "none",
           (unsigned long)(millis() / 1000), HTTP_PORT);
  sendTo(c, b, strlen(b));
}

// รับ "hello" จากคอม/มือถือ: ลงทะเบียนผู้รับ (สูงสุด 3 ราย), ตั้งเวลาจริง, เลือกส่ง JSON หรือ CSV แบบเก่า
// [เทคนิค: UDP + client registration] คอมส่ง "hello" มาที่พอร์ต 4210 -> นาฬิกาจำ IP/พอร์ต แล้วส่งค่าสดกลับ
//   UDP ไม่ต้องสร้างการเชื่อมต่อ หน่วงต่ำ เหมาะกับข้อมูลสดที่หายบ้างได้ (ผลคำถามสำคัญจึงดึงซ้ำผ่าน HTTP)
void handleHello(char* msg, IPAddress ip, uint16_t port) {
  s_lastHello = millis();
  app::touchActivity();
  bool wave = storage::settings().waveform;
  long epoch = 0;
  int tokens = 0;
  bool csv = false;
  // แยกคำด้วยช่องว่าง: hello t=1700000000 w=1
  // "hello" เปล่า ๆ (สคริปต์เก่า udp_listen.py / receive_data.py ของเพื่อน) -> ส่ง CSV แบบเดิม
  char* save = nullptr;
  for (char* tok = strtok_r(msg, " \r\n", &save); tok; tok = strtok_r(nullptr, " \r\n", &save)) {
    if (!strcmp(tok, "hello")) continue;
    tokens++;
    if (!strncmp(tok, "t=", 2)) epoch = atol(tok + 2);
    else if (!strncmp(tok, "w=", 2)) wave = tok[2] == '1';
    else if (!strcmp(tok, "csv=1")) csv = true;
  }
  if (tokens == 0) csv = true;
  if (epoch > 1600000000L) {
    // นาฬิกาไม่มี RTC แบตสำรอง -> ใช้เวลาจากคอมแทน (ไว้ประทับเวลาใน log)
    time_t now = time(nullptr);
    if (!(app::bits() & EV_TIME_SYNCED) || labs((long)now - epoch) > 2) {
      struct timeval tv;
      tv.tv_sec = epoch;
      tv.tv_usec = 0;
      settimeofday(&tv, nullptr);
      app::setBits(EV_TIME_SYNCED);
    }
  }
  Client* slot = nullptr;
  for (auto& c : s_clients)
    if (c.active && c.ip == ip && c.port == port) slot = &c;
  bool isNew = false;
  if (!slot) {
    for (auto& c : s_clients)
      if (!c.active) { slot = &c; break; }
    if (!slot) {   // เต็ม -> แทนที่เครื่องที่เงียบนานสุด
      slot = &s_clients[0];
      for (auto& c : s_clients)
        if (c.lastHello < slot->lastHello) slot = &c;
    }
    slot->ip = ip;
    slot->port = port;
    slot->active = true;
    isNew = true;
  }
  slot->lastHello = millis();
  slot->wave = wave && !csv;
  slot->csv = csv;
  if (isNew) {
    app::logEvent("UDP", "client %s:%u joined (%s)", ip.toString().c_str(), port, csv ? "legacy CSV" : "JSON");
    if (!csv) sendHi(*slot);
  }
}

// อ่านทุกแพ็กเก็ต UDP ที่เข้ามา (hello / ping) แล้วจัดการทีละอัน
void handleIncoming() {
  int len = s_udp.parsePacket();
  while (len > 0) {
    char buf[128];
    int n = s_udp.read(buf, sizeof(buf) - 1);
    if (n < 0) n = 0;
    buf[n] = 0;
    IPAddress ip = s_udp.remoteIP();
    uint16_t port = s_udp.remotePort();
    if (!strncmp(buf, "hello", 5)) {
      handleHello(buf, ip, port);
    } else if (!strncmp(buf, "ping", 4)) {
      // วัด round-trip latency ของ WiFi ได้จากฝั่งคอม
      char b[64];
      snprintf(b, sizeof(b), "{\"t\":\"pong\",\"n\":\"%.20s\",\"ms\":%lu}", buf + (n > 5 ? 5 : n),
               (unsigned long)millis());
      Client tmp = {ip, port, 0, false, true, false};
      sendTo(tmp, b, strlen(b));
    }
    len = s_udp.parsePacket();
  }
}

// ส่งค่าสด 1 ชุดเป็น JSON (5 ครั้ง/วินาที) — ค่าที่ใช้ไม่ได้ส่งเป็น null
// [เทคนิค: JSON over UDP] ค่าสด 5 ครั้ง/วินาที สร้างด้วย snprintf ลงบัฟเฟอร์บน stack (ไม่ใช้ heap = ไม่มี fragmentation)
void sendVitals() {
  const Vitals v = app::getVitals();
  const EngineSnap e = app::getEngineSnap();
  char hr[12], hrv[12], pi[12], gsr[12], gt[12], gp[12], tmp[12], trm[12], mot[12];
  char b[600];                        // 560 -> 600: เพิ่มคีย์ "mu" (กรณีเลขยาวสุดทุกช่อง ~551 ไบต์)
  snprintf(b, sizeof(b),
           "{\"t\":\"v\",\"id\":\"%s\",\"seq\":%lu,\"ms\":%lu,"
           "\"hr\":%s,\"hrv\":%s,\"ibi\":%.0f,\"con\":%d,\"pi\":%s,\"amp\":%.0f,\"ir\":%lu,\"beats\":%lu,"
           "\"gsr\":%s,\"gt\":%s,\"gp\":%s,\"gc\":%d,\"gmv\":%.0f,\"scr\":%u,"
           "\"tmp\":%s,\"trm\":%s,\"mot\":%s,"
           "\"vb\":%.0f,\"bp\":%u,\"bat\":%d,"
           "\"si\":%d,\"es\":%u,\"ep\":%.2f,\"eel\":%.1f,\"eq\":%u,\"ek\":%u,\"rv\":%lu,\"rs\":%lu,"
           "\"set\":%d,\"bl\":%d,\"cal\":%d,\"fl\":%u,\"cpu\":%lu,\"eco\":%d,\"md\":%u,\"ml\":%d,\"mu\":%d}",
           net::deviceId(), (unsigned long)++s_seqV, (unsigned long)v.ms,
           f2s(hr, sizeof hr, v.hr > 0 ? v.hr : NAN, 1), f2s(hrv, sizeof hrv, v.hrv > 0 ? v.hrv : NAN, 1),
           (double)v.ibi, v.ppgContact ? 1 : 0, f2s(pi, sizeof pi, v.pi, 2), (double)v.ppgAmp,
           (unsigned long)v.irDc, (unsigned long)v.beats,
           f2s(gsr, sizeof gsr, v.gsrContact ? v.gsr : NAN, 3),
           f2s(gt, sizeof gt, v.gsrContact ? v.gsrTonic : NAN, 3),
           f2s(gp, sizeof gp, v.gsrContact ? v.gsrPhasic : NAN, 3), v.gsrContact ? 1 : 0,
           (double)v.gsrMv, v.scrPerMin,
           f2s(tmp, sizeof tmp, v.skinTemp, 2), f2s(trm, sizeof trm, v.tremor, 3),
           f2s(mot, sizeof mot, v.motion, 3),
           (double)v.vbatMv, v.battPct, v.battPresent ? 1 : 0,
           e.stress, e.state, (double)e.progress, (double)e.elapsed, e.qid, e.kind,
           (unsigned long)e.revision, (unsigned long)e.lastSeq,
           e.settled ? 1 : 0, e.baselineValid ? 1 : 0, e.calibrated ? 1 : 0, v.flags,
           (unsigned long)getCpuFrequencyMhz(), (app::bits() & EV_ECO) ? 1 : 0, mlrt::mode(),
           mlrt::hasModel() ? 1 : 0, mlrt::modelActive() ? 1 : 0);   // mu = ตอนนี้ตัดสินด้วยโมเดล AI จริงไหม
  sendAll(b, false);
}

// ส่งคลื่นชีพจรช่วงสั้น ๆ (100 Hz) + ตำแหน่งจังหวะหัวใจ ให้หน้าเว็บวาดกราฟ
void sendWave(const WaveChunk& w) {
  char b[200];
  int n = snprintf(b, sizeof(b), "{\"t\":\"w\",\"seq\":%lu,\"n0\":%lu,\"fs\":%u,\"b\":%u,\"d\":[",
                   (unsigned long)++s_seqW, (unsigned long)w.n0, (unsigned)PPG_FS_HZ, w.beatMask);
  for (uint8_t i = 0; i < w.count && n < (int)sizeof(b) - 10; i++)
    n += snprintf(b + n, sizeof(b) - n, i ? ",%d" : "%d", w.d[i]);
  snprintf(b + n, sizeof(b) - n, "]}");
  sendAll(b, true);
}

// ลบผู้รับที่ไม่ได้ส่ง hello เกินกำหนด (ปิดแอป/ออกจาก WiFi แล้ว)
// [เทคนิค: Keep-alive timeout] เครื่องที่ไม่ส่ง hello ซ้ำเกินเวลา = เลิกส่งให้ (ไม่เปลืองแบนด์วิดท์/ไฟ)
void expireClients(uint32_t now) {
  bool any = false;
  for (auto& c : s_clients) {
    if (c.active && now - c.lastHello > CLIENT_TIMEOUT_MS) {
      c.active = false;
      app::logEvent("UDP", "client %s timed out", c.ip.toString().c_str());
    }
    any |= c.active;
  }
  if (any) app::setBits(EV_CLIENT);
  else app::clearBits(EV_CLIENT);
}
}  // namespace

// =====================================================================
void task(void*) {
  wdt::subscribe();
  uint32_t lastVitals = 0, lastCsv = 0;
  for (;;) {
    const uint32_t t0 = micros();
    wdt::feed();
    app::heartbeat(T_TELEMETRY);
    const uint32_t now = millis();
    const bool wifi = app::bits() & EV_WIFI_UP;

    // WiFi ถูกปิด/เปิดใหม่ (light sleep) -> socket เดิมใช้ไม่ได้ ต้องเปิดใหม่
    if (!wifi && s_udpOn) {
      s_udp.stop();
      s_udpOn = false;
      for (auto& c : s_clients) c.active = false;
    } else if (wifi && !s_udpOn) {
      s_udpOn = s_udp.begin(UDP_PORT);
    }

    WaveChunk w;
    if (s_udpOn) {
      handleIncoming();
      expireClients(now);
      const bool clients = app::bits() & EV_CLIENT;

      // คลื่น PPG: ระบายคิวเสมอ (ไม่มีคนดูก็ทิ้ง กันข้อมูลเก่าค้าง)
      const bool wantWave = clients && anyWave() && !(app::bits() & EV_ECO);
      while (xQueueReceive(app::waveQueue, &w, 0) == pdTRUE)
        if (wantWave) sendWave(w);

      // เหตุการณ์: ส่งทันที + จองส่งซ้ำอีกครั้งใน 150 ms
      EventMsg m;
      while (xQueueReceive(app::eventQueue, &m, 0) == pdTRUE) {
        if (clients) sendAll(m.json, false);
        for (auto& p : s_pending)
          if (!p.used) { p.msg = m; p.at = now + 150; p.used = true; break; }
      }
      for (auto& p : s_pending)
        if (p.used && (int32_t)(now - p.at) >= 0) {
          if (clients) sendAll(p.msg.json, false);
          p.used = false;
        }

      const uint32_t period = (app::bits() & EV_ECO) ? 500 : 200;
      if (clients && now - lastVitals >= period) {
        lastVitals = now;
        sendVitals();
      }
      if (clients && now - lastCsv >= 1000) {
        lastCsv = now;
        sendLegacyCsv();
      }
    } else {
      while (xQueueReceive(app::waveQueue, &w, 0) == pdTRUE) {}
    }
    app::addBusy(T_TELEMETRY, micros() - t0);
    vTaskDelay(pdMS_TO_TICKS(10));
  }
}

// จำนวนผู้รับที่ลงทะเบียนอยู่
uint8_t clientCount() {
  uint8_t n = 0;
  for (auto& c : s_clients) n += c.active ? 1 : 0;
  return n;
}
uint32_t packetsSent() { return s_sent; }
uint32_t sendErrors() { return s_err; }
uint32_t lastHelloMs() { return s_lastHello; }

}  // namespace telemetry
