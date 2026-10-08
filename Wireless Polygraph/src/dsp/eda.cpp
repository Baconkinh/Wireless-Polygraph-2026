// =====================================================================
//  eda.cpp — ประมวลผล GSR (EDA): แยกระดับพื้น (tonic) กับยอดตอบสนอง (phasic) + นับ SCR
//  ทำอะไร   : กรองค่า µS ด้วย EMA, tonic = EMA ช้า (15 s), phasic = ค่าปัจจุบัน − tonic, ตรวจการแตะผิว/ลัดวงจร
//  ทำไม     : การโกหกทำให้ "phasic" พุ่งชั่วคราว ส่วน tonic ลอยช้า ๆ ตามอุณหภูมิ/เวลา ต้องแยกออกจากกัน
//  เรียกจาก : sensors.cpp  ->  frame.gsr ไปที่ LieEngine
// =====================================================================
#include "eda.h"

namespace dsp {

namespace {
constexpr float kScrMinRise = 0.05f;   // µS: ยอดเล็กกว่านี้ถือเป็นสัญญาณรบกวนของ ADC
constexpr float kScrDrop    = 0.01f;   // µS: ลดลงจากยอดเท่านี้ = ยอดจบแล้ว
}

void EdaProcessor::begin(float fs) {
  fs_ = fs;
  smooth_.setup(fs, 0.4f);   // เรียบพอตัดสัญญาณรบกวน แต่ยังเห็นยอด SCR (กว้าง 1–5 s)
  tonic_.setup(fs, 15.0f);
  slope_.setup(fs, 1.0f);
  contact_ = false;
  n_ = 0;
  histHead_ = histCount_ = 0;
  scrHead_ = scrCount_ = 0;
  scrTotal_ = 0;
  rising_ = false;
}

void EdaProcessor::push(float uS, bool contact) {
  n_++;
  if (!contact) {
    contact_ = false;
    smooth_.clear();
    tonic_.clear();
    slope_.clear();
    histHead_ = histCount_ = 0;
    rising_ = false;
    return;
  }
  if (!contact_) {
    // เพิ่งแตะ: เริ่ม tonic ที่ค่าปัจจุบันเลย ไม่งั้น phasic จะพุ่งปลอมไปหลายสิบวินาที
    contact_ = true;
    smooth_.reset(uS);
    tonic_.reset(uS);
    slope_.reset(0.0f);
    trough_ = peak_ = uS;
    rising_ = false;
  }
  const float v = smooth_.push(uS);
  tonic_.push(v);

  // ความชัน (µS/s) จากค่าเมื่อ ~1 วินาทีก่อน
  const int lag = (int)fs_;
  hist_[histHead_] = v;
  histHead_ = (histHead_ + 1) % 16;
  if (histCount_ < 16) histCount_++;
  if (histCount_ > lag && lag < 16) {
    float old = hist_[(histHead_ - 1 - lag + 16) % 16];
    slope_.push((v - old) * fs_ / lag);
  }

  // ตรวจยอด SCR: ไต่ขึ้นจากจุดต่ำ >= 0.05 µS แล้วเริ่มลง = นับ 1 ครั้ง
  if (v < trough_ && !rising_) trough_ = v;
  if (v > peak_) peak_ = v;
  if (!rising_ && v - trough_ >= kScrMinRise * 0.5f) {
    rising_ = true;
    riseStart_ = n_;
    peak_ = v;
  }
  if (rising_ && n_ - riseStart_ > (uint32_t)(6.0f * fs_)) {
    // ไต่ขึ้นนานเกิน 6 s = ระดับพื้น (tonic) ลอยขึ้นช้า ๆ ไม่ใช่ยอด SCR -> เริ่มนับใหม่
    rising_ = false;
    trough_ = v;
    peak_ = v;
  }
  if (rising_ && v < peak_ - kScrDrop) {
    if (peak_ - trough_ >= kScrMinRise) {
      scrAt_[scrHead_] = n_;
      scrHead_ = (scrHead_ + 1) % kScrMax;
      if (scrCount_ < kScrMax) scrCount_++;
      scrTotal_++;
    }
    rising_ = false;
    trough_ = v;
    peak_ = v;
  }
}

uint16_t EdaProcessor::scrPerMin() const {
  const uint32_t win = (uint32_t)(60.0f * fs_);
  uint16_t c = 0;
  for (int i = 0; i < scrCount_; i++) {
    uint32_t t = scrAt_[(scrHead_ - 1 - i + kScrMax) % kScrMax];
    if (n_ - t <= win) c++;
    else break;  // เรียงจากใหม่ไปเก่า เจอเก่ากว่า 60 s ก็หยุดได้
  }
  return c;
}

}  // namespace dsp
