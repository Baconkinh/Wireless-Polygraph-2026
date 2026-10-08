// =====================================================================
//  web_server.cpp — backend ในนาฬิกา: HTTP server พอร์ต 80 (รายการ API ทั้งหมดอยู่ใน web_server.h)
//  ทำอะไร   : ส่งหน้าเว็บ (web_page.h), ตอบ REST API เป็น JSON: ค่าสด, สั่ง baseline/คำถาม, ตั้งค่า, AI,
//             การหลับ, WiFi, ดาวน์โหลด CSV, OTA — ทุกคำสั่งที่แตะ LieEngine ล็อก engineMutex ก่อน
//  เรียกจาก : tasks.cpp httpTask (เรียก server.handleClient() วนไป)
//  ผู้เรียก : หน้าเว็บ 192.168.4.1, Polygraph Studio (watch_link.py), ml/train.py (--upload)
//  วิชา     : WiFi (SoftAP + web server), JSON/REST
// =====================================================================
#include "web_server.h"
#include <WebServer.h>
#include <LittleFS.h>
#include <sys/time.h>
#include "../app.h"
#include "../config.h"
#include "../sensors.h"
#include "../sys/storage.h"
#include "../sys/power.h"
#include "../sys/watchdog.h"
#include "../sys/ota.h"
#include "../sys/sysinfo.h"
#include "../sys/ml_runtime.h"
#include "../lie/ml_model.h"
#include "json_writer.h"
#include "web_page.h"
#include "telemetry.h"
#include "wifi_ap.h"

namespace web {

namespace {
WebServer server(HTTP_PORT);
uint32_t s_requests = 0;
uint16_t s_autoQid = 1;
volatile bool s_netRestart = false;

using lie::Engine;

// ---------------- ตัวช่วยตอบกลับ ----------------
void sendJson(int code, const String& s) { server.send(code, "application/json", s); }

// ตอบ {ok: true, msg}
void replyOk(const char* msg = nullptr) {
  Json j(96);
  j.obj().kv("ok", true);
  if (msg) j.kv("msg", msg);
  j.end();
  sendJson(200, j.str());
}

// ตอบ error {ok: false, error, msg} พร้อม HTTP status
void replyFail(int code, const char* err, const char* msg) {
  Json j(160);
  j.obj().kv("ok", false).kv("error", err).kv("msg", msg).end();
  sendJson(code, j.str());
}

// รหัส error ของ LieEngine -> ข้อความไทยสำหรับผู้ใช้
const char* engineErrThai(const char* e) {
  if (!strcmp(e, "NO_BASELINE")) return "ต้องวัด baseline ก่อน";
  if (!strcmp(e, "QUESTION_ACTIVE")) return "กำลังวัดคำถามอยู่ รอให้ครบเวลาก่อน";
  if (!strcmp(e, "BASELINE_RUNNING")) return "กำลังวัด baseline อยู่";
  if (!strcmp(e, "NO_QUESTION")) return "ยังไม่ได้เริ่มคำถาม";
  if (!strcmp(e, "BASELINE_NO_GSR")) return "แผ่น GSR ไม่แตะผิว baseline ใช้ไม่ได้";
  if (!strcmp(e, "BASELINE_NO_SIGNAL")) return "ไม่มีสัญญาณชีพจรและ GSR ตอน baseline — ใส่นาฬิกาให้แนบผิวแล้ววัดใหม่";
  return e;
}

// อ่าน argument แบบตรวจช่วงค่า: ไม่มี -> false (ไม่ใช่ error), ผิดช่วง -> ตอบ 400 แล้ว ok=false
bool argF(const char* name, float lo, float hi, float& out, bool& bad) {
  if (!server.hasArg(name)) return false;
  const float v = server.arg(name).toFloat();
  if (!(v >= lo && v <= hi)) {
    char msg[96];
    snprintf(msg, sizeof(msg), "%s ต้องอยู่ระหว่าง %.3g ถึง %.3g", name, (double)lo, (double)hi);
    replyFail(400, "BAD_VALUE", msg);
    bad = true;
    return false;
  }
  out = v;
  return true;
}

// ---------------- JSON builders ----------------
void resultJson(Json& j, const lie::Result& r) {
  j.obj();
  j.kv("seq", (unsigned long)r.seq);
  j.kv("qid", (int)r.qid);
  j.kv("kind", Engine::kindName(r.kind));
  j.kv("verdict", Engine::verdictName(r.verdict));
  j.kv("expected", Engine::verdictName(r.expected));
  j.kv("correct", r.correct);
  j.kv("p", r.pLie, 3);
  j.kv("score", r.score, 2);
  j.kv("quality", (int)r.quality);
  j.kv("src", (int)r.source);          // 0 = สูตรมาตรฐาน, 1 = สูตรที่ปรับจากข้อควบคุม, 2 = โมเดล AI
  j.kv("reasons", (int)r.reasons);
  j.kv("ok", (int)r.okMask);
  j.arr("feat");
  for (int i = 0; i < lie::F_COUNT; i++) j.v(r.feat[i], 4);
  j.end();
  j.arr("z");
  for (int i = 0; i < lie::F_COUNT; i++) j.v(r.z[i], 2);
  j.end();
  j.arr("pre");
  for (int i = 0; i < lie::F_COUNT; i++) j.v(r.pre[i], 3);
  j.end();
  j.kv("latency", r.peakLatency, 1);
  j.kv("answerAt", r.answerAt, 1);
  j.kv("answerYes", (int)r.answerYes);
  j.kv("tStart", r.tStart, 1);
  j.end();
}

// สร้าง JSON ค่าสดทุกเซนเซอร์ (GET /api/live และหน้าเว็บในนาฬิกา)
void liveJson(Json& j) {
  const Vitals v = app::getVitals();
  const EngineSnap e = app::getEngineSnap();
  j.obj();
  j.kv("id", net::deviceId());
  j.kv("fw", FW_VERSION);
  j.kv("uptime", (unsigned long)(millis() / 1000));
  j.kv("clients", (int)telemetry::clientCount());
  j.kv("seq", (unsigned long)v.seq);
  j.kv("ms", (unsigned long)v.ms);
  if (v.hr > 0) j.kv("hr", v.hr, 1); else j.kvNull("hr");
  if (v.hrv > 0) j.kv("hrv", v.hrv, 1); else j.kvNull("hrv");
  j.kv("ibi", v.ibi, 0);
  j.kv("con", v.ppgContact ? 1 : 0);
  j.kv("pi", v.pi, 2);
  j.kv("amp", v.ppgAmp, 0);
  j.kv("ir", (unsigned long)v.irDc);
  j.kv("beats", (unsigned long)v.beats);
  if (v.gsrContact) {
    j.kv("gsr", v.gsr, 3).kv("gt", v.gsrTonic, 3).kv("gp", v.gsrPhasic, 3);
  } else {
    j.kvNull("gsr").kvNull("gt").kvNull("gp");
  }
  j.kv("gc", v.gsrContact ? 1 : 0);
  j.kv("gmv", v.gsrMv, 0);
  j.kv("scr", (int)v.scrPerMin);
  j.kv("tmp", v.skinTemp, 2);
  j.kv("trm", v.tremor, 3);
  j.kv("mot", v.motion, 3);
  j.kv("vb", v.vbatMv, 0);
  j.kv("bp", (int)v.battPct);
  j.kv("bat", v.battPresent ? 1 : 0);
  j.kv("si", e.stress);
  j.kv("es", (int)e.state);
  j.kv("ep", e.progress, 2);
  j.kv("eel", e.elapsed, 1);
  j.kv("eq", (int)e.qid);
  j.kv("ek", (int)e.kind);
  j.kv("rv", (unsigned long)e.revision);
  j.kv("rs", (unsigned long)e.lastSeq);
  j.kv("set", e.settled ? 1 : 0);
  j.kv("bl", e.baselineValid ? 1 : 0);
  j.kv("cal", e.calibrated ? 1 : 0);
  j.kv("fl", (int)v.flags);
  j.kv("cpu", (unsigned long)getCpuFrequencyMhz());
  j.kv("eco", (app::bits() & EV_ECO) ? 1 : 0);
  j.kv("md", (int)mlrt::mode());
  j.kv("ml", mlrt::hasModel() ? 1 : 0);
  j.kv("sby", (int)storage::settings().standbyMin);
  j.kv("wp", (int)storage::settings().wifiPower);
  j.end();
}

// สร้าง JSON สถานะ LieEngine: baseline, การปรับเกณฑ์, ผล n ข้อล่าสุด (GET /api/lie)
void lieJson(Json& j, int n) {
  // ล็อก engine ตลอดการสร้าง JSON (~ไม่กี่ ms) เพื่อให้ข้อมูลทุกส่วนมาจาก "เวลาเดียวกัน"
  xSemaphoreTake(app::engineMutex, portMAX_DELAY);
  const lie::Status s = app::engine.status();
  const lie::Config& c = app::engine.config();
  const lie::Baseline& b = app::engine.baseline();
  const lie::Calibration& cal = app::engine.calibration();
  j.obj();
  j.kv("state", Engine::stateName(s.state));
  j.kv("progress", s.progress, 2);
  j.kv("elapsed", s.elapsed, 1);
  j.kv("qid", (int)s.qid);
  j.kv("kind", Engine::kindName(s.kind));
  j.kv("stress", s.stress);
  j.kv("settled", s.settled);
  j.kv("revision", (unsigned long)s.revision);
  j.kv("resultCount", (unsigned long)s.results);
  j.kv("error", app::engine.lastError());
  j.kv("time", app::engine.time(), 1);

  j.obj("config");
  j.kv("baselineSec", c.baselineSec, 0).kv("windowSec", c.windowSec, 0).kv("preSec", c.preSec, 0);
  j.kv("s0", c.s0, 2).kv("k", c.k, 2).kv("lieP", c.lieP, 2).kv("truthP", c.truthP, 2);
  j.arr("w");
  for (int i = 0; i < lie::F_COUNT; i++) j.v(c.weight[i], 3);
  j.end();
  j.end();

  j.obj("baseline");
  j.kv("valid", b.valid);
  j.kv("duration", b.durationSec, 1);
  j.kv("gsrFrac", b.gsrFrac, 2);
  j.kv("ppgFrac", b.ppgFrac, 2);
  j.kv("nullN", (int)b.nullN);
  j.arr("mean");
  for (int i = 0; i < lie::F_COUNT; i++) j.v(b.mean[i], 3);
  j.end();
  j.arr("sd");
  for (int i = 0; i < lie::F_COUNT; i++) j.v(b.sd[i], 4);
  j.end();
  j.arr("nullMean");
  for (int i = 0; i < lie::F_COUNT; i++) j.v(b.nullMean[i], 4);
  j.end();
  j.arr("nullSd");
  for (int i = 0; i < lie::F_COUNT; i++) j.v(b.nullSd[i], 4);
  j.end();
  j.end();

  j.obj("calibration");
  j.kv("active", cal.active).kv("weak", cal.weak);
  j.kv("nTruth", (int)cal.nTruth).kv("nLie", (int)cal.nLie);
  j.kv("meanTruth", cal.meanTruth, 2).kv("meanLie", cal.meanLie, 2).kv("separation", cal.separation, 2);
  j.kv("s0", cal.s0, 2).kv("k", cal.k, 2);
  j.kv("correct", (int)cal.correct).kv("scored", (int)cal.scored);
  j.arr("w");
  for (int i = 0; i < lie::F_COUNT; i++) j.v(cal.weight[i], 3);
  j.end();
  j.end();

  j.arr("results");
  lie::Result r;
  for (int i = 0; i < n && app::engine.result(i, r); i++) resultJson(j, r);
  j.end();
  j.end();
  xSemaphoreGive(app::engineMutex);
}

// สร้าง JSON ค่าตั้งทั้งหมด (GET /api/config)
void configJson(Json& j) {
  const Settings& s = storage::settings();
  j.obj();
  j.kv("baselineSec", (int)s.baselineSec).kv("windowSec", (int)s.windowSec).kv("preSec", (int)s.preSec);
  j.kv("s0", s.s0, 2).kv("k", s.k, 2).kv("lieP", s.lieP, 2).kv("truthP", s.truthP, 2);
  j.arr("w");
  for (int i = 0; i < lie::F_COUNT; i++) j.v(s.w[i], 3);
  j.end();
  j.kv("contactThr", (unsigned long)s.contactThr).kv("irLed", (int)s.irLed).kv("vccMv", (int)s.vccMv);
  j.kv("eco", s.eco).kv("standbyMin", (int)s.standbyMin).kv("wakeCheckS", (int)s.wakeCheckS);
  j.kv("waveform", s.waveform);
  j.end();
}

// นำค่าตั้งใน NVS ไปใช้กับ LieEngine/เซนเซอร์/WiFi ทันที
void applySettings() {
  const Settings& s = storage::settings();
  xSemaphoreTake(app::engineMutex, portMAX_DELAY);
  lie::Config c = app::engine.config();
  storage::toEngineConfig(s, c);
  app::engine.configure(c);
  xSemaphoreGive(app::engineMutex);
  sensors::applySettings();
  if (s.eco != power::eco()) power::setEco(s.eco);
}

// ส่งเหตุการณ์ไปคอม/มือถือทาง UDP (เช่น model, mode)
void emitEvent(const char* ev, const char* extra) {
  char js[200];
  snprintf(js, sizeof(js), "{\"t\":\"e\",\"eid\":%lu,\"ev\":\"%s\"%s%s}",
           (unsigned long)app::nextEventId(), ev, extra && *extra ? "," : "", extra ? extra : "");
  app::pushEvent(js);
}

// ---------------- handlers ----------------
void hRoot() { server.send_P(200, "text/html", INDEX_HTML); }

// GET /api/info: ข้อมูลเครื่อง
void hInfo() {
  Json j(900);
  sysinfo::buildInfoJson(j);
  sendJson(200, j.str());
}

// GET /api/live: ค่าสด
void hLive() {
  Json j(900);
  liveJson(j);
  sendJson(200, j.str());
}

// GET /api/lie?n=24: สถานะ LieEngine + ผลล่าสุด
void hLie() {
  int n = server.hasArg("n") ? server.arg("n").toInt() : 24;
  if (n < 0) n = 0;
  if (n > Engine::kMaxRes) n = Engine::kMaxRes;
  Json j(1800 + n * 330);
  lieJson(j, n);
  sendJson(200, j.str());
}

// POST /api/lie/baseline: เริ่มวัดค่าปกติ
void hBaseline() {
  const float sec = server.hasArg("sec") ? server.arg("sec").toFloat() : 0.0f;
  xSemaphoreTake(app::engineMutex, portMAX_DELAY);
  const bool ok = app::engine.startBaseline(sec);
  const char* err = app::engine.lastError();
  const float real = ok ? (sec > 0 ? sec : app::engine.config().baselineSec) : 0;
  xSemaphoreGive(app::engineMutex);
  if (!ok) return replyFail(409, err, engineErrThai(err));
  char extra[40];
  snprintf(extra, sizeof(extra), "\"sec\":%.0f", (double)real);
  emitEvent("baseline_start", extra);
  app::logEvent("ENGINE", "baseline start %.0fs (http)", (double)real);
  replyOk("baseline started");
}

// POST /api/lie/question?qid=&kind=: เริ่มวัด 1 ข้อ
void hQuestion() {
  lie::Kind kind = lie::Kind::Test;
  if (server.hasArg("kind") && !Engine::kindFromName(server.arg("kind").c_str(), kind))
    return replyFail(400, "BAD_KIND", "kind ต้องเป็น test, truth, lie หรือ warmup");
  long qid = server.hasArg("qid") ? server.arg("qid").toInt() : s_autoQid;
  if (qid <= 0 || qid > 65535) return replyFail(400, "BAD_QID", "qid ต้องอยู่ระหว่าง 1-65535");
  xSemaphoreTake(app::engineMutex, portMAX_DELAY);
  const bool ok = app::engine.startQuestion((uint16_t)qid, kind);
  const char* err = app::engine.lastError();
  xSemaphoreGive(app::engineMutex);
  if (!ok) return replyFail(409, err, engineErrThai(err));
  s_autoQid = (uint16_t)(qid + 1);
  char extra[64];
  snprintf(extra, sizeof(extra), "\"qid\":%ld,\"kind\":\"%s\"", qid, Engine::kindName(kind));
  emitEvent("q_start", extra);
  Json j(64);
  j.obj().kv("ok", true).kv("qid", (int)qid).end();
  sendJson(200, j.str());
}

// POST /api/lie/answer?ans=yes|no: บันทึกคำตอบ
void hAnswer() {
  const bool yes = server.arg("ans") != "no";
  xSemaphoreTake(app::engineMutex, portMAX_DELAY);
  const bool ok = app::engine.markAnswer(yes);
  const char* err = app::engine.lastError();
  xSemaphoreGive(app::engineMutex);
  if (!ok) return replyFail(409, err, engineErrThai(err));
  replyOk(yes ? "answer yes" : "answer no");
}

// POST /api/lie/abort: ยกเลิก
void hAbort() {
  xSemaphoreTake(app::engineMutex, portMAX_DELAY);
  app::engine.abort();
  xSemaphoreGive(app::engineMutex);
  emitEvent("abort", "");
  replyOk("aborted");
}

// POST /api/lie/reset: เริ่มผู้ตอบคนใหม่
void hReset() {
  xSemaphoreTake(app::engineMutex, portMAX_DELAY);
  app::engine.resetSession();
  xSemaphoreGive(app::engineMutex);
  emitEvent("session_reset", "");
  app::logEvent("ENGINE", "session reset (http)");
  replyOk("session reset");
}

// GET /api/config: ค่าตั้ง
void hConfigGet() {
  Json j(700);
  configJson(j);
  sendJson(200, j.str());
}

// POST /api/config: เปลี่ยนค่าตั้ง (ตรวจทุกค่าก่อน ผิดตัวเดียว = ไม่เปลี่ยนเลย)
void hConfigSet() {
  Settings n = storage::settings();   // แก้สำเนาก่อน ผ่านการตรวจทุกตัวแล้วค่อยบันทึก
  bool bad = false;
  float f;
  if (argF("baselineSec", 20, 90, f, bad)) n.baselineSec = (uint16_t)f;
  if (!bad && argF("windowSec", 6, 20, f, bad)) n.windowSec = (uint8_t)f;
  if (!bad && argF("preSec", 1, 5, f, bad)) n.preSec = (uint8_t)f;
  if (!bad && argF("s0", -5, 10, f, bad)) n.s0 = f;
  if (!bad && argF("k", 0.1f, 10, f, bad)) n.k = f;
  if (!bad && argF("lieP", 0.5f, 0.99f, f, bad)) n.lieP = f;
  if (!bad && argF("truthP", 0.01f, 0.5f, f, bad)) n.truthP = f;
  char key[4] = {'w', '0', 0, 0};
  for (int i = 0; i < lie::F_COUNT && !bad; i++) {
    key[1] = (char)('0' + i);
    if (argF(key, 0, 1, f, bad)) n.w[i] = f;
  }
  if (!bad && argF("contactThr", 5000, 250000, f, bad)) n.contactThr = (uint32_t)f;
  if (!bad && argF("irLed", 1, 255, f, bad)) n.irLed = (uint8_t)f;
  if (!bad && argF("vccMv", 3000, 3600, f, bad)) n.vccMv = (uint16_t)f;
  if (!bad && argF("eco", 0, 1, f, bad)) n.eco = f > 0.5f;
  if (!bad && argF("standbyMin", 0, 240, f, bad)) n.standbyMin = (uint16_t)f;
  if (!bad && argF("wakeCheckS", 5, 600, f, bad)) n.wakeCheckS = (uint16_t)f;
  if (!bad && argF("waveform", 0, 1, f, bad)) n.waveform = f > 0.5f;
  if (bad) return;   // ตอบ 400 ไปแล้ว
  if (n.truthP >= n.lieP) return replyFail(400, "BAD_VALUE", "truthP ต้องน้อยกว่า lieP");
  storage::settings() = n;
  storage::saveSettings();
  applySettings();
  app::logEvent("CONFIG", "settings updated (NVS)");
  Json j(700);
  configJson(j);
  sendJson(200, j.str());
}

// POST /api/config/reset: คืนค่าเริ่มต้น
void hConfigReset() {
  storage::resetSettings();
  applySettings();
  app::logEvent("CONFIG", "factory defaults restored");
  Json j(700);
  configJson(j);
  sendJson(200, j.str());
}

// GET /api/system: FreeRTOS task, watchdog, แฟลช, พลังงาน
void hSystem() {
  Json j(5000);
  sysinfo::buildSystemJson(j);
  sendJson(200, j.str());
}

// GET /api/logs?file=: อ่านไฟล์ log ใน LittleFS
void hLogs() {
  const String which = server.hasArg("file") ? server.arg("file") : String("events");
  const char* path = "/events.log";
  if (which == "events1") path = "/events.1";
  else if (which == "results") path = "/results.csv";
  else if (which == "results1") path = "/results.1";
  String out;
  if (!storage::readFile(path, out, 16384)) return replyFail(404, "NO_FILE", "ยังไม่มีไฟล์นี้");
  server.send(200, "text/plain; charset=utf-8", out);
}

// POST /api/logs/clear: ลบไฟล์ log
void hLogsClear() {
  storage::clearLogs();
  replyOk("logs cleared");
}

// POST /api/stats/reset: ล้างสถิติสะสม
void hStatsReset() {
  storage::resetStats();
  replyOk("stats reset");
}

// POST /api/power: โหมด ECO/ปกติ หรือสั่งหลับแบบ light/deep
void hPower() {
  const String mode = server.arg("mode");
  const uint32_t sec = server.hasArg("sec") ? (uint32_t)server.arg("sec").toInt() : 0;
  if (mode == "eco" || mode == "normal") {
    const bool on = mode == "eco";
    power::setEco(on);
    storage::settings().eco = on;
    storage::saveSettings();
    return replyOk(on ? "eco on (CPU 80 MHz)" : "normal (CPU 160 MHz)");
  }
  if (mode == "light") {
    power::requestLightSleep(sec > 3600 ? 3600 : sec);
    return replyOk("light sleep: กดปุ่ม BOOT เพื่อปลุก (WiFi จะหลุดชั่วคราว)");
  }
  if (mode == "deep") {
    power::requestDeepSleep(sec > 86400 ? 86400 : sec);
    return replyOk(sec ? "deep sleep: จะตื่นเองตามเวลา" : "standby: ใส่นาฬิกา/แตะเซนเซอร์เพื่อปลุก");
  }
  replyFail(400, "BAD_MODE", "mode ต้องเป็น normal, eco, light หรือ deep");
}

// POST /api/restart: รีสตาร์ท (บันทึกสาเหตุไว้ในกล่องดำ)
void hRestart() {
  power::requestRestart(PR_USER_RESTART);
  replyOk("restarting");
}

// POST /api/demo: สาธิต watchdog/panic (ต้อง confirm=yes)
void hDemo() {
  wdt::Demo d;
  if (!wdt::demoFromName(server.arg("type").c_str(), d))
    return replyFail(400, "BAD_TYPE", "type ต้องเป็น twdt, hwwdt, panic หรือ intwdt");
  if (server.arg("confirm") != "yes")
    return replyFail(400, "NEED_CONFIRM", "เครื่องจะรีเซ็ต! เพิ่ม confirm=yes เพื่อยืนยัน");
  if (!wdt::requestDemo(d)) return replyFail(409, "BUSY", "มีการสาธิตค้างอยู่แล้ว");
  app::logEvent("DEMO", "watchdog demo requested: %s", wdt::demoName(d));
  const char* msg = "";
  switch (d) {
    case wdt::DEMO_TWDT: msg = "sensor task จะค้าง -> Task WDT รีเซ็ตใน ~8 s"; break;
    case wdt::DEMO_HWWDT: msg = "supervisor หยุดป้อน timer WDT -> รีเซ็ตใน ~12 s"; break;
    case wdt::DEMO_PANIC: msg = "เขียนลงแอดเดรส 0 -> panic + core dump -> รีเซ็ตทันที"; break;
    case wdt::DEMO_INTWDT: msg = "ปิด interrupt แล้ววนค้าง -> Interrupt WDT รีเซ็ตใน ~0.3 s"; break;
    default: break;
  }
  replyOk(msg);
}

// ทุก request: นับ + ถือว่ามีคนใช้งาน (เลื่อน auto-standby)
template <void (*Handler)()>
// ตัวห่อทุก handler: นับ request, ต่อเวลาก่อนหลับ (มีคนใช้งานอยู่) แล้วเรียก handler จริง
void W() {
  s_requests++;
  app::touchActivity();
  Handler();
}

// ---------------- AI / Machine learning ----------------
void hMlGet() {
  Json j(900);
  j.obj().kv("ok", true);
  mlrt::appendJson(j);
  j.end();
  sendJson(200, j.str());
}

// POST /api/ml/mode: สลับโหมดเก็บข้อมูล/ใช้งานจริง
void hMlMode() {
  const String m = server.arg("mode");
  if (m != "train" && m != "detect") return replyFail(400, "BAD_MODE", "mode ต้องเป็น train หรือ detect");
  mlrt::setMode(m == "train" ? mlrt::MODE_TRAIN : mlrt::MODE_DETECT);
  app::logEvent("ML", "mode -> %s", m.c_str());
  emitEvent("mode", m == "train" ? "\"mode\":\"train\"" : "\"mode\":\"detect\"");
  replyOk(m == "train" ? "โหมดเก็บข้อมูล (TRAIN)" : "โหมดใช้งานจริง (DETECT)");
}

// POST /api/ml/subject: ตั้งชื่อผู้ตอบ (เขียนลงข้อมูลเทรน)
void hMlSubject() {
  mlrt::setSubject(server.arg("name").c_str());
  replyOk(mlrt::subject());
}

// POST /api/ml/model: รับโมเดล AI จากคอม -> ตรวจ -> เก็บลง NVS -> ใช้ทันที
void hMlModel() {
  ml::Model m;
  ml::clear(m);
  const int n = ml::parseFeatureList(server.arg("features").c_str(), m.idx, ml::kMaxFeatures);
  if (n <= 0) return replyFail(400, "BAD_FEATURES", "รายชื่อ feature ไม่ถูกต้อง");
  m.n = (uint8_t)n;
  if (ml::parseFloats(server.arg("mean").c_str(), m.mean, ml::kMaxFeatures) != n ||
      ml::parseFloats(server.arg("scale").c_str(), m.scale, ml::kMaxFeatures) != n ||
      ml::parseFloats(server.arg("w").c_str(), m.w, ml::kMaxFeatures) != n || !server.hasArg("b"))
    return replyFail(400, "BAD_MODEL", "จำนวนค่า mean/scale/w ไม่เท่ากับจำนวน feature");
  m.b = server.arg("b").toFloat();
  m.accuracy = server.arg("acc").toFloat();
  m.samples = (uint16_t)server.arg("samples").toInt();
  m.trainedAt = (uint32_t)strtoul(server.arg("trained").c_str(), nullptr, 10);
  strncpy(m.name, server.arg("name").c_str(), sizeof(m.name) - 1);
  if (!mlrt::install(m)) return replyFail(400, "BAD_MODEL", "โมเดลไม่ผ่านการตรวจ (ค่า scale ต้องมากกว่า 0)");
  app::logEvent("ML", "model installed: %d features, acc %.2f, %u samples", n, (double)m.accuracy, m.samples);
  emitEvent("model", "\"loaded\":1");
  replyOk("ติดตั้งโมเดล AI แล้ว");
}

// POST /api/ml/model/clear: ลบโมเดล กลับไปใช้สูตร
void hMlModelClear() {
  mlrt::clearModel();
  app::logEvent("ML", "model removed");
  replyOk("ลบโมเดลแล้ว กลับไปใช้สูตรมาตรฐาน");
}

// GET /api/ml/data.csv: ส่งไฟล์ /train.csv ทีละ 1 KB (ไม่ถือ mutex ระหว่างส่ง กัน watchdog)
void hMlData() {
  if (!storage::fsOk()) return replyFail(500, "FS", "ระบบไฟล์ใช้งานไม่ได้");
  size_t size = 0;
  xSemaphoreTake(app::fsMutex, portMAX_DELAY);
  File f = LittleFS.open(storage::kTrainPath, "r");
  if (f) {
    size = f.size();
    f.close();
  }
  xSemaphoreGive(app::fsMutex);
  if (!size)
    return replyFail(404, "NO_DATA", "ยังไม่มีข้อมูลเทรน — เปลี่ยนเป็นโหมดเก็บข้อมูลแล้วถามคำถามที่รู้เฉลยก่อน");

  // ส่งทีละ 1 KB และถือ fsMutex เฉพาะตอน "อ่านไฟล์" ไม่ถือระหว่างส่งทาง WiFi:
  // ถ้ามือถือรับช้า/สัญญาณอ่อน การส่ง 180 KB อาจนานหลายวินาที และ supervisor ที่รอเขียน log
  // จะค้างจน Task WDT รีเซ็ตเครื่อง (เคยใช้ streamFile ที่ถือ mutex ตลอด)
  server.sendHeader("Content-Disposition", "attachment; filename=\"polygraph_train.csv\"");
  server.setContentLength(size);
  server.send(200, "text/csv", "");
  static char buf[1024];             // static: ไม่กิน stack ของ httpTask (มีแค่ task นี้ที่ใช้)
  size_t off = 0;
  while (off < size) {
    size_t n = 0;
    xSemaphoreTake(app::fsMutex, portMAX_DELAY);
    File g = LittleFS.open(storage::kTrainPath, "r");
    if (g && g.seek(off)) n = g.read((uint8_t*)buf, (size - off) < sizeof(buf) ? (size - off) : sizeof(buf));
    if (g) g.close();
    xSemaphoreGive(app::fsMutex);
    if (n == 0) break;               // ไฟล์ถูกล้างระหว่างดาวน์โหลด -> หยุด (client จะเห็นว่าไม่ครบ)
    server.sendContent(buf, n);
    off += n;
    wdt::feed();
  }
}

// POST /api/ml/data/clear: ล้างข้อมูลเทรนในนาฬิกา (ต้อง confirm=yes)
void hMlDataClear() {
  if (server.arg("confirm") != "yes") return replyFail(400, "NEED_CONFIRM", "ข้อมูลเทรนจะหายทั้งหมด! เพิ่ม confirm=yes");
  storage::clearTrain();
  app::logEvent("ML", "training data cleared");
  replyOk("ล้างข้อมูลเทรนแล้ว");
}

// ---------------- Sleep / WiFi power (ตั้งค่าแบบง่าย) ----------------
void hSleep() {
  Settings& st = storage::settings();
  if (server.hasArg("auto")) {
    const long m = server.arg("auto").toInt();
    if (m < 0 || m > 240) return replyFail(400, "BAD_VALUE", "auto ต้องเป็น 0 (ปิด) ถึง 240 นาที");
    st.standbyMin = (uint16_t)m;
    storage::saveSettings();
    app::logEvent("POWER", "auto-standby = %ld min", m);
  }
  const String now = server.arg("now");
  const uint32_t sec = server.hasArg("sec") ? (uint32_t)server.arg("sec").toInt() : 0;
  if (now == "light") power::requestLightSleep(sec > 3600 ? 3600 : sec);
  else if (now == "deep") power::requestDeepSleep(sec > 86400 ? 86400 : sec);
  Json j(160);
  j.obj().kv("ok", true).kv("autoMin", (int)st.standbyMin).kv("now", now.c_str()).end();
  sendJson(200, j.str());
}

// POST /api/wifi?level=0-2: ระดับกำลังส่ง WiFi
void hWifiPower() {
  const long lv = server.arg("level").toInt();
  if (!server.hasArg("level") || lv < 0 || lv > 2) return replyFail(400, "BAD_VALUE", "level ต้องเป็น 0 (ต่ำ), 1 (กลาง) หรือ 2 (สูง)");
  storage::settings().wifiPower = (uint8_t)lv;
  storage::saveSettings();
  net::setPowerLevel((uint8_t)lv);
  replyOk(net::powerName((uint8_t)lv));
}

// มือถือ/คอมส่งเวลาปัจจุบันมาให้: นาฬิกาไม่มี RTC สำรองไฟ และ WiFi ของนาฬิกาไม่มีเน็ตให้ใช้ NTP
// -> คอลัมน์ time ในไฟล์ข้อมูลเทรนจะเป็นเวลาจริงแทน 0
// ESP-IDF 4.4 ใช้ time_t 32 บิต -> รับได้ถึงปี 2038
void hTime() {
  const long long e = atoll(server.arg("epoch").c_str());
  if (e < 1600000000LL || e > 2147483000LL) return replyFail(400, "BAD_VALUE", "epoch ไม่ถูกต้อง");
  const time_t now = time(nullptr);
  if (!(app::bits() & EV_TIME_SYNCED) || llabs((long long)now - e) > 2) {
    struct timeval tv;
    tv.tv_sec = (time_t)e;
    tv.tv_usec = 0;
    settimeofday(&tv, nullptr);
    app::setBits(EV_TIME_SYNCED);
  }
  replyOk("time set");
}

// ไม่มี path นี้: ตอบ CORS preflight หรือ 404 เป็น JSON
void hNotFound() {
  if (server.method() == HTTP_OPTIONS) {   // CORS preflight
    server.send(204);
    return;
  }
  replyFail(404, "NOT_FOUND", "ไม่มี API นี้");
}
}  // namespace

// =====================================================================
void begin() {
  server.enableCORS(true);      // ให้หน้าเว็บจากเครื่องอื่น (เช่น Studio) เรียกตรงได้
  server.on("/", HTTP_GET, W<hRoot>);
  server.on("/api/info", HTTP_GET, W<hInfo>);
  server.on("/api/live", HTTP_GET, W<hLive>);
  server.on("/api/lie", HTTP_GET, W<hLie>);
  server.on("/api/lie/baseline", HTTP_POST, W<hBaseline>);
  server.on("/api/lie/question", HTTP_POST, W<hQuestion>);
  server.on("/api/lie/answer", HTTP_POST, W<hAnswer>);
  server.on("/api/lie/abort", HTTP_POST, W<hAbort>);
  server.on("/api/lie/reset", HTTP_POST, W<hReset>);
  server.on("/api/config", HTTP_GET, W<hConfigGet>);
  server.on("/api/config", HTTP_POST, W<hConfigSet>);
  server.on("/api/config/reset", HTTP_POST, W<hConfigReset>);
  server.on("/api/system", HTTP_GET, W<hSystem>);
  server.on("/api/logs", HTTP_GET, W<hLogs>);
  server.on("/api/logs/clear", HTTP_POST, W<hLogsClear>);
  server.on("/api/stats/reset", HTTP_POST, W<hStatsReset>);
  server.on("/api/power", HTTP_POST, W<hPower>);
  server.on("/api/restart", HTTP_POST, W<hRestart>);
  server.on("/api/demo", HTTP_POST, W<hDemo>);
  server.on("/api/ml", HTTP_GET, W<hMlGet>);
  server.on("/api/ml/mode", HTTP_POST, W<hMlMode>);
  server.on("/api/ml/subject", HTTP_POST, W<hMlSubject>);
  server.on("/api/ml/model", HTTP_POST, W<hMlModel>);
  server.on("/api/ml/model/clear", HTTP_POST, W<hMlModelClear>);
  server.on("/api/ml/data.csv", HTTP_GET, W<hMlData>);
  server.on("/api/ml/data/clear", HTTP_POST, W<hMlDataClear>);
  server.on("/api/sleep", HTTP_POST, W<hSleep>);
  server.on("/api/wifi", HTTP_POST, W<hWifiPower>);
  server.on("/api/time", HTTP_POST, W<hTime>);
  server.onNotFound(W<hNotFound>);
  ota::begin(server);
  server.begin();
}

// httpTask (priority 2): รับ request ของเว็บเซิร์ฟเวอร์ + OTA วนตลอด และป้อน watchdog
void task(void*) {
  wdt::subscribe();
  for (;;) {
    const uint32_t t0 = micros();
    wdt::feed();
    app::heartbeat(T_HTTP);
    if (s_netRestart) {         // ทำใน task นี้เท่านั้น เพราะ WebServer ไม่ thread-safe
      s_netRestart = false;
      server.stop();
      server.begin();
      ota::restartNetwork();
    }
    server.handleClient();      // ตอบ request ที่ค้าง (ถ้าไม่มีจะคืนทันที)
    ota::handle();
    // การสาธิต panic / interrupt WDT ทำใน task นี้ หลังส่งคำตอบ HTTP ไปแล้ว
    const wdt::Demo d = wdt::pendingDemo();
    if (d == wdt::DEMO_PANIC || d == wdt::DEMO_INTWDT) {
      delay(300);
      wdt::crashNow(d, "http");
    }
    app::addBusy(T_HTTP, micros() - t0);
    vTaskDelay(pdMS_TO_TICKS(2));
  }
}

uint32_t requests() { return s_requests; }
void notifyNetworkRestart() { s_netRestart = true; }

}  // namespace web
