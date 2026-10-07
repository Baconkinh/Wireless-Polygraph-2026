// ml_runtime.cpp — ดูคำอธิบายใน ml_runtime.h
#include "ml_runtime.h"
#include <time.h>
#include "../app.h"
#include "../net/json_writer.h"
#include "storage.h"

namespace mlrt {

namespace {
ml::Model s_model;
volatile bool s_loaded = false;
char s_subject[24] = "-";

// ถูกเรียกจากใน LieEngine (engineTask ถือ engineMutex อยู่แล้ว) -> อ่าน s_model ได้ปลอดภัย
bool scorer(const lie::Result& r, float& p, void*) {
  if (!s_loaded) return false;
  p = ml::predict(s_model, r);
  return true;
}
}  // namespace

void begin() {
  ml::Model m;
  if (storage::loadModel(m)) {
    s_model = m;
    s_loaded = true;
  }
  app::engine.setScorer(scorer, nullptr);
}

uint8_t mode() { return storage::settings().mode; }

void setMode(uint8_t m) {
  storage::settings().mode = m ? MODE_TRAIN : MODE_DETECT;
  storage::saveSettings();
}

const char* modeName(uint8_t m) { return m == MODE_TRAIN ? "train" : "detect"; }

bool hasModel() { return s_loaded; }

bool install(const ml::Model& in) {
  ml::Model m = in;
  ml::seal(m);
  if (!ml::valid(m)) return false;
  if (!storage::saveModel(m)) return false;
  // สลับโมเดลขณะ engine ไม่ได้คำนวณอยู่ (ถือ engineMutex)
  xSemaphoreTake(app::engineMutex, portMAX_DELAY);
  s_model = m;
  s_loaded = true;
  xSemaphoreGive(app::engineMutex);
  return true;
}

void clearModel() {
  xSemaphoreTake(app::engineMutex, portMAX_DELAY);
  s_loaded = false;
  ml::clear(s_model);
  xSemaphoreGive(app::engineMutex);
  storage::clearModel();
}

ml::Model modelCopy() {
  xSemaphoreTake(app::engineMutex, portMAX_DELAY);
  ml::Model m = s_model;
  xSemaphoreGive(app::engineMutex);
  return m;
}

void setSubject(const char* name) {
  size_t i = 0;
  // เก็บเฉพาะตัวอักษรที่ไม่ทำให้ CSV พัง (ตัด , " ขึ้นบรรทัดใหม่)
  for (; name && *name && i < sizeof(s_subject) - 1; name++) {
    const char c = *name;
    if (c == ',' || c == '"' || c == '\n' || c == '\r') continue;
    s_subject[i++] = c;
  }
  if (i == 0) s_subject[i++] = '-';
  s_subject[i] = 0;
  // ชื่อภาษาไทย 1 ตัวอักษร = 3 ไบต์ (UTF-8) ถ้าถูกตัดกลางตัวตอนเกินความยาว ไฟล์ CSV จะมีอักขระเสีย
  // -> ถอยไปหาไบต์นำของตัวสุดท้าย ถ้าไบต์ไม่ครบก็ตัดทั้งตัวทิ้ง
  size_t k = i;
  while (k > 0 && ((uint8_t)s_subject[k - 1] & 0xC0) == 0x80) k--;
  if (k > 0 && ((uint8_t)s_subject[k - 1] & 0x80)) {
    const uint8_t lead = (uint8_t)s_subject[k - 1];
    const size_t need = lead >= 0xF0 ? 4 : lead >= 0xE0 ? 3 : 2;
    if (i - (k - 1) < need) s_subject[k - 1] = 0;
  }
  if (!s_subject[0]) strcpy(s_subject, "-");
}

const char* subject() { return s_subject; }

bool makeTrainRow(const lie::Result& r, char* out, size_t n, int& label) {
  if (r.kind == lie::Kind::ControlTruth) label = 0;
  else if (r.kind == lie::Kind::ControlLie) label = 1;
  else return false;                                   // เก็บเฉพาะข้อที่รู้เฉลย
  if (r.verdict == lie::Verdict::Invalid) return false; // สัญญาณเสีย ไม่เอามาสอน AI
  float x[ml::FEAT_COUNT];
  ml::extract(r, x);
  const time_t now = time(nullptr);
  snprintf(out, n,
           "%ld,%s,%u,%d,%s,%u,%d,%d,%.3f,%.3f,%.3f,%.3f,%.3f,%.4f,%.2f,%.4f,%.4f,%.3f,%.3f,%u",
           (long)(now > 1600000000 ? now : 0), s_subject, r.qid, label, label ? "lie" : "truth",
           r.quality, (int)x[ml::GSR_OK], (int)x[ml::PPG_OK], (double)x[ml::Z_GSR], (double)x[ml::Z_HR],
           (double)x[ml::Z_AMP], (double)x[ml::Z_TRM], (double)x[ml::Z_TMP], (double)x[ml::D_GSR],
           (double)x[ml::D_HR], (double)x[ml::D_AMP], (double)x[ml::D_TRM], (double)x[ml::D_TMP],
           (double)r.pLie, r.source);
  return true;
}

void appendJson(Json& j) {
  uint32_t t = 0, l = 0;
  storage::trainCounts(t, l);
  j.kv("mode", modeName(mode()));
  j.kv("subject", s_subject);
  j.obj("data");
  j.kv("truth", (unsigned long)t);
  j.kv("lie", (unsigned long)l);
  j.kv("bytes", (unsigned long)storage::trainBytes());
  j.kv("max", (unsigned long)storage::TRAIN_MAX);
  j.end();
  const ml::Model m = modelCopy();
  j.obj("model");
  j.kv("loaded", (bool)s_loaded);
  if (s_loaded) {
    j.kv("name", m.name);
    j.kv("accuracy", m.accuracy, 3);
    j.kv("samples", (int)m.samples);
    j.kv("trainedAt", (unsigned long)m.trainedAt);
    j.kv("b", m.b, 4);
    j.arr("features");
    for (int i = 0; i < m.n; i++) j.v(ml::kFeatureNames[m.idx[i]]);
    j.end();
    j.arr("w");
    for (int i = 0; i < m.n; i++) j.v(m.w[i], 4);
    j.end();
  }
  j.end();
}

}  // namespace mlrt
