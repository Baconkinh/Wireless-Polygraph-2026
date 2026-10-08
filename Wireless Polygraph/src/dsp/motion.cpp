// =====================================================================
//  motion.cpp — แยก "มือสั่น" (band-pass 3–15 Hz) กับ "การขยับตัว" (0.3–3 Hz) จากความเร่ง 3 แกน
//  ทำไม     : มือสั่นเป็นสัญญาณความเครียด แต่การขยับตัวทำให้สัญญาณอื่นเพี้ยน -> ใช้ตัดสินว่าข้อนั้น "ใช้ไม่ได้"
//  เรียกจาก : sensors.cpp  ->  frame.tremor / frame.motion ไปที่ LieEngine
// =====================================================================
#include "motion.h"
#include <math.h>

namespace dsp {

void MotionProcessor::begin(float fs) {
  fs_ = fs;
  trmHp_.highpass(fs, 3.0f);
  trmLp_.lowpass(fs, 15.0f);
  motHp_.highpass(fs, 0.3f);
  motLp_.lowpass(fs, 3.0f);
  trmPow_.setup(fs, 1.0f);
  motPow_.setup(fs, 1.0f);
  trmPow_.reset(0.0f);
  motPow_.reset(0.0f);
  init_ = false;
}

void MotionProcessor::push(float ax, float ay, float az) {
  mag_ = sqrtf(ax * ax + ay * ay + az * az);
  if (!init_) {
    // ตั้งตัวกรองให้นิ่งที่ค่าปัจจุบัน -> ไม่เกิด "การสั่นปลอม" ตอนเริ่ม
    trmHp_.reset(mag_);
    trmLp_.reset(0.0f);
    motHp_.reset(mag_);
    motLp_.reset(0.0f);
    init_ = true;
  }
  const float t = trmLp_.push(trmHp_.push(mag_));
  const float m = motLp_.push(motHp_.push(mag_));
  trmPow_.push(t * t);
  motPow_.push(m * m);
}

}  // namespace dsp
