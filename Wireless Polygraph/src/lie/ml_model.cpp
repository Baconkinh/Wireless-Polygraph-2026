// =====================================================================
//  ml_model.cpp — โมเดล AI (Logistic Regression) ที่คำนวณบนนาฬิกาเอง
//  ทำอะไร   : extract() ดึง feature 12 ตัวจากผลของ LieEngine, predict() = sigmoid(b + Σ w·(x−mean)/scale),
//             seal()/valid() ใส่และตรวจ CRC32 ก่อนเก็บลง NVS (กันโมเดลเสีย)
//  เรียกจาก : sys/ml_runtime.cpp (ผูกเป็น scorer ของ LieEngine), web_server.cpp (รับโมเดลที่อัปโหลด)
//  คู่กัน   : Polygraph-Studio/ml/polyml.py (ฝั่งเทรนบนคอม ใช้สูตรเดียวกัน)
// =====================================================================
#include "ml_model.h"
#include <math.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

namespace ml {

const char* const kFeatureNames[FEAT_COUNT] = {
    "z_gsr", "z_hr", "z_amp", "z_trm", "z_tmp",
    "d_gsr", "d_hr", "d_amp", "d_trm", "d_tmp",
    "gsr_ok", "ppg_ok"};

// ชื่อ feature (เช่น "z_hr") -> ลำดับ 0-11 (ไม่รู้จัก = -1)
int featureFromName(const char* name) {
  if (!name) return -1;
  for (int i = 0; i < FEAT_COUNT; i++)
    if (!strcmp(name, kFeatureNames[i])) return i;
  return -1;
}

// ดึง feature 12 ตัวจากผลของ LieEngine (สัญญาณที่ใช้ไม่ได้ = 0) — ลำดับเดียวกับ polyml.FEATURES ฝั่งคอม
void extract(const lie::Result& r, float out[FEAT_COUNT]) {
  for (int i = 0; i < lie::F_COUNT; i++) {
    const bool ok = (r.okMask >> i) & 1u;
    // สัญญาณที่ใช้ไม่ได้ในข้อนี้ = "ไม่เปลี่ยน" (0) — สอดคล้องกับตอนเทรนที่ทำแบบเดียวกัน
    out[Z_GSR + i] = ok ? r.z[i] : 0.0f;
    out[D_GSR + i] = ok ? r.feat[i] : 0.0f;
  }
  out[GSR_OK] = (r.okMask & (1u << lie::F_GSR)) ? 1.0f : 0.0f;
  out[PPG_OK] = (r.okMask & (1u << lie::F_HR)) ? 1.0f : 0.0f;
}

// CRC32 ของโมเดล — ใช้ตรวจว่าโมเดลใน NVS ไม่เสีย
// [เทคนิค: Data integrity — CRC32] ตรวจโมเดลที่เก็บใน NVS ว่าไม่เสีย/ไม่ถูกแก้ ก่อนเอามาใช้ (ไม่ตรง = ไม่ใช้)
static uint32_t crc32(const uint8_t* p, size_t n) {
  uint32_t c = 0xFFFFFFFFu;
  while (n--) {
    c ^= *p++;
    for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
  }
  return ~c;
}

void clear(Model& m) { memset(&m, 0, sizeof(m)); }

// ใส่ magic + version + CRC32 ก่อนบันทึกโมเดลลง NVS
void seal(Model& m) {
  m.magic = kMagic;
  m.version = 1;
  m.name[sizeof(m.name) - 1] = 0;
  m.crc = crc32(reinterpret_cast<const uint8_t*>(&m), offsetof(Model, crc));
}

// ตรวจโมเดล: magic, จำนวน feature, CRC32, scale > 0, ไม่มี NaN — ไม่ผ่าน = ไม่ใช้
bool valid(const Model& m) {
  if (m.magic != kMagic || m.n == 0 || m.n > kMaxFeatures) return false;
  if (m.crc != crc32(reinterpret_cast<const uint8_t*>(&m), offsetof(Model, crc))) return false;
  for (int i = 0; i < m.n; i++) {
    if (m.idx[i] >= FEAT_COUNT) return false;
    if (!(m.scale[i] > 0.0f) || isnan(m.w[i]) || isnan(m.mean[i])) return false;   // จับ NaN ด้วย
  }
  return !isnan(m.b);
}

// ทำนายโอกาสโกหกของ 1 ข้อ (AI ทำงานยังไง) — ทุกบรรทัดตรงกับ Model.prob_row() ใน polyml.py ฝั่งคอม
//   1) extract()  : ดึง feature 12 ตัวของข้อนี้ (z และ d ของ 5 สัญญาณ + มี GSR/ชีพจรไหม)
//   2) z = b      : เริ่มจาก bias (ค่าคงที่ที่ได้ตอนเทรน)
//   3) วนทุก feature ที่โมเดลใช้: (x - mean) / scale = แปลงเป็นสเกลมาตรฐานแบบเดียวกับตอนเทรน
//      แล้วคูณน้ำหนัก w (บวก = ยิ่งมากยิ่งน่าจะโกหก, ลบ = ตรงข้าม) บวกสะสมใน z
//   4) sigmoid(z) = 1 / (1 + e^-z) แปลงคะแนนเป็นความน่าจะเป็น 0-1 = p(โกหก)
//   ใช้ double ตอนบวกสะสม (ESP32-C3 ไม่มี FPU double แต่ 12 ครั้งต่อข้อเร็วพอ) และตัดที่ ±30 กัน exp ล้น
//   LieEngine นำ p ไปเทียบเกณฑ์ lieP/truthP ต่อ -> โกหก / จริง / ไม่แน่ชัด
// [เทคนิค: On-device ML inference — Logistic Regression] P = 1/(1+e^−(b + Σ w·(x−mean)/scale)) ใช้ float บน MCU
//   ไม่ต้องต่อคอมตอนใช้งาน (edge inference)
float predict(const Model& m, const lie::Result& r) {
  float x[FEAT_COUNT];
  extract(r, x);
  double z = m.b;
  for (int i = 0; i < m.n; i++) z += (double)m.w[i] * ((x[m.idx[i]] - m.mean[i]) / m.scale[i]);
  if (z > 30.0) return 1.0f;           // กัน exp ล้น
  if (z < -30.0) return 0.0f;
  return (float)(1.0 / (1.0 + exp(-z)));
}

// แปลงข้อความ "1.2,3.4,..." (จากฟอร์มอัปโหลดโมเดล) เป็น array float
int parseFloats(const char* s, float* out, int maxN) {
  if (!s) return -1;
  int n = 0;
  const char* p = s;
  while (*p) {
    while (*p == ' ' || *p == ',') p++;
    if (!*p) break;
    if (n >= maxN) return -1;
    char* end = nullptr;
    const double v = strtod(p, &end);
    if (end == p) return -1;           // ไม่ใช่ตัวเลข
    out[n++] = (float)v;
    p = end;
    while (*p == ' ') p++;
    if (*p && *p != ',') return -1;
  }
  return n;
}

// แปลงรายชื่อ feature "z_gsr,z_hr,..." เป็นลำดับ index
int parseFeatureList(const char* s, uint8_t* out, int maxN) {
  if (!s) return -1;
  int n = 0;
  char tok[16];
  const char* p = s;
  while (*p) {
    while (*p == ' ' || *p == ',') p++;
    if (!*p) break;
    int k = 0;
    while (*p && *p != ',' && *p != ' ' && k < (int)sizeof(tok) - 1) tok[k++] = *p++;
    tok[k] = 0;
    while (*p && *p != ',') p++;       // ข้ามตัวอักษรเกิน (ชื่อยาวผิดปกติ)
    const int id = featureFromName(tok);
    if (id < 0 || n >= maxN) return -1;
    out[n++] = (uint8_t)id;
  }
  return n;
}

}  // namespace ml
