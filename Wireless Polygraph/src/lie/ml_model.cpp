// ml_model.cpp — ดูคำอธิบายใน ml_model.h
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

int featureFromName(const char* name) {
  if (!name) return -1;
  for (int i = 0; i < FEAT_COUNT; i++)
    if (!strcmp(name, kFeatureNames[i])) return i;
  return -1;
}

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

static uint32_t crc32(const uint8_t* p, size_t n) {
  uint32_t c = 0xFFFFFFFFu;
  while (n--) {
    c ^= *p++;
    for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
  }
  return ~c;
}

void clear(Model& m) { memset(&m, 0, sizeof(m)); }

void seal(Model& m) {
  m.magic = kMagic;
  m.version = 1;
  m.name[sizeof(m.name) - 1] = 0;
  m.crc = crc32(reinterpret_cast<const uint8_t*>(&m), offsetof(Model, crc));
}

bool valid(const Model& m) {
  if (m.magic != kMagic || m.n == 0 || m.n > kMaxFeatures) return false;
  if (m.crc != crc32(reinterpret_cast<const uint8_t*>(&m), offsetof(Model, crc))) return false;
  for (int i = 0; i < m.n; i++) {
    if (m.idx[i] >= FEAT_COUNT) return false;
    if (!(m.scale[i] > 0.0f) || isnan(m.w[i]) || isnan(m.mean[i])) return false;   // จับ NaN ด้วย
  }
  return !isnan(m.b);
}

float predict(const Model& m, const lie::Result& r) {
  float x[FEAT_COUNT];
  extract(r, x);
  double z = m.b;
  for (int i = 0; i < m.n; i++) z += (double)m.w[i] * ((x[m.idx[i]] - m.mean[i]) / m.scale[i]);
  if (z > 30.0) return 1.0f;           // กัน exp ล้น
  if (z < -30.0) return 0.0f;
  return (float)(1.0 / (1.0 + exp(-z)));
}

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
