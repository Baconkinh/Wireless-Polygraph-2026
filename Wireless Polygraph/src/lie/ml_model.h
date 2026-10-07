// =====================================================================
//  ml_model.h — โมเดล AI (Logistic Regression) ที่รัน "บนนาฬิกาเอง"
//
//  ที่มาของโมเดล: เก็บข้อมูลในโหมด "เก็บข้อมูล (TRAIN)" -> ได้ไฟล์ CSV
//  -> เทรนบนคอมด้วย ml/train.py -> ได้ model.json -> อัปโหลดเข้านาฬิกา (เก็บใน NVS)
//
//  ทำไม Logistic Regression (ไม่ใช่ Random Forest แบบเดิมของเพื่อน):
//   - ข้อมูลน้อย (หลักสิบ-ร้อยข้อ) โมเดลง่ายจะ overfit น้อยกว่า
//   - ค่าน้ำหนักอ่านความหมายได้ (สัญญาณไหนสำคัญ) -> อธิบายอาจารย์ได้
//   - คำนวณแค่บวก-คูณ ~12 ครั้ง + sigmoid -> ใส่ใน ESP32-C3 ได้สบาย (edge AI)
//
//  สูตร:  x_i' = (x_i - mean_i) / scale_i          (standardize แบบเดียวกับตอนเทรน)
//         P(โกหก) = 1 / (1 + exp(-(b + Σ w_i x_i')))
//
//  feature ทุกตัวคำนวณจาก "การเปลี่ยนแปลงหลังถาม" เทียบกับช่วงก่อนถามของคนนั้นเอง
//  (z_* = เทียบกับการแกว่งตามธรรมชาติของคนนั้นตอน baseline, d_* = ค่าที่เปลี่ยนจริง)
//  ไม่ใช้ค่าสัมบูรณ์ (เช่น ชีพจร 90 bpm) เพราะต่างกันตามคน/ช่วงเวลา ไม่เกี่ยวกับการโกหก
//  ไม่พึ่ง Arduino -> unit test บนคอมได้
// =====================================================================
#pragma once
#include <stdint.h>
#include "lie_engine.h"

namespace ml {

enum FeatureId : uint8_t {
  Z_GSR = 0, Z_HR, Z_AMP, Z_TRM, Z_TMP,      // z-score เทียบ baseline ของคนนั้น
  D_GSR, D_HR, D_AMP, D_TRM, D_TMP,          // ค่าที่เปลี่ยนจริง (µS, bpm, สัดส่วน, m/s², °C)
  GSR_OK, PPG_OK,                            // 1 = สัญญาณนั้นใช้ได้ในข้อนี้
  FEAT_COUNT
};

constexpr int kMaxFeatures = 16;

extern const char* const kFeatureNames[FEAT_COUNT];
int featureFromName(const char* name);                 // -1 = ไม่รู้จัก

// ดึง feature ทั้งหมดจากผลของ LieEngine
void extract(const lie::Result& r, float out[FEAT_COUNT]);

struct Model {
  uint32_t magic;
  uint16_t version;
  uint8_t  n;                    // จำนวน feature ที่โมเดลใช้
  uint8_t  idx[kMaxFeatures];    // FeatureId ของแต่ละช่อง
  float    mean[kMaxFeatures];
  float    scale[kMaxFeatures];
  float    w[kMaxFeatures];
  float    b;
  float    accuracy;             // ความแม่นยำจาก cross-validation ตอนเทรน (0..1)
  uint16_t samples;              // จำนวนข้อที่ใช้เทรน
  uint32_t trainedAt;            // unix time
  char     name[24];
  uint32_t crc;
};

constexpr uint32_t kMagic = 0x4D4C3031;   // "ML01"

void clear(Model& m);
void seal(Model& m);                       // คำนวณ magic/crc ก่อนบันทึก
bool valid(const Model& m);
float predict(const Model& m, const lie::Result& r);   // P(โกหก) 0..1

// แปลงข้อความ "0.1,-2.5,3" -> float[]; คืนจำนวนที่อ่านได้ (-1 = รูปแบบผิด)
int parseFloats(const char* s, float* out, int maxN);
// แปลง "z_hr,z_amp,d_hr" -> FeatureId[]; คืนจำนวน (-1 = มีชื่อที่ไม่รู้จัก)
int parseFeatureList(const char* s, uint8_t* out, int maxN);

}  // namespace ml
