// =====================================================================
//  motion.h — แยก "มือสั่น (tremor)" กับ "การขยับตัว (motion)" จากความเร่ง 3 แกน
//
//  ใช้ขนาดเวกเตอร์ |a| = sqrt(ax²+ay²+az²) -> ไม่ขึ้นกับว่าใส่นาฬิกาเอียงแบบไหน
//  ตอนนิ่ง |a| ≈ 9.81 m/s² (แรงโน้มถ่วง) ส่วนที่แกว่งรอบค่านั้นคือการเคลื่อนไหวจริง
//
//   tremor : band-pass 3–15 Hz  (การสั่นทางสรีระ/ความเครียดอยู่ในช่วงหลาย Hz)
//   motion : band-pass 0.3–3 Hz (ขยับแขน ขยับตัว — ใช้เป็นตัวบอกว่าสัญญาณอื่นเชื่อได้ไหม)
//  แล้วคิด RMS แบบเลื่อน (EMA ของกำลังสอง, tau 1 s)
// =====================================================================
#pragma once
#include "filters.h"

namespace dsp {

class MotionProcessor {
 public:
  void begin(float fs);
  void push(float ax, float ay, float az);   // หน่วย m/s²
  void markGap() { init_ = false; }           // ข้อมูลขาด -> เริ่มตัวกรองใหม่
  float tremor()    const { return sqrtf(trmPow_.value()); }  // m/s² RMS
  float motion()    const { return sqrtf(motPow_.value()); }  // m/s² RMS
  float magnitude() const { return mag_; }

 private:
  float fs_ = 100.0f;
  Biquad trmHp_, trmLp_, motHp_, motLp_;
  Ema trmPow_, motPow_;
  float mag_ = 0.0f;
  bool init_ = false;
};

}  // namespace dsp
