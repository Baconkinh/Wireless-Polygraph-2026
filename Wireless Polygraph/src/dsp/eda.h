// =====================================================================
//  eda.h — ประมวลผล GSR / EDA (ความนำไฟฟ้าของผิว, หน่วย µS)
//
//  ตอนตื่นเต้น/เครียด ต่อมเหงื่อถูกกระตุ้นโดยระบบประสาทซิมพาเทติก -> ผิวนำไฟฟ้าดีขึ้น
//  สัญญาณแบ่งเป็น 2 ส่วน:
//    tonic  (SCL) : ระดับพื้นที่เปลี่ยนช้า ๆ ระดับนาที  -> EMA tau 15 s
//    phasic (SCR) : ยอดสั้น ๆ 1–5 s หลังถูกกระตุ้น      -> value - tonic
//  นับจำนวน SCR ต่อนาที (ยอดที่สูงขึ้น >= 0.05 µS) เป็นดัชนีความตื่นตัวอีกตัว
// =====================================================================
#pragma once
#include <stdint.h>
#include "filters.h"

namespace dsp {

class EdaProcessor {
 public:
  void begin(float fs);
  // fs ครั้ง/วินาที: ค่า µS (0 ถ้าไม่ได้แตะ) + สถานะการแตะ
  void push(float microSiemens, bool contact);

  bool     contact()   const { return contact_; }
  float    value()     const { return contact_ ? smooth_.value() : 0.0f; }
  float    tonic()     const { return contact_ ? tonic_.value() : 0.0f; }
  float    phasic()    const { return contact_ ? smooth_.value() - tonic_.value() : 0.0f; }
  float    slope()     const { return contact_ ? slope_.value() : 0.0f; }   // µS/s
  uint16_t scrPerMin() const;
  uint32_t scrTotal()  const { return scrTotal_; }

 private:
  float fs_ = 10.0f;
  Ema smooth_, tonic_, slope_;
  bool contact_ = false;
  uint32_t n_ = 0;
  float hist_[16];            // ค่าย้อนหลัง ~1.5 s ไว้คิดความชัน
  int histHead_ = 0, histCount_ = 0;
  float trough_ = 0.0f, peak_ = 0.0f;
  bool rising_ = false;
  uint32_t riseStart_ = 0;
  static const int kScrMax = 32;
  uint32_t scrAt_[kScrMax];   // เวลา (หน่วย sample) ของ SCR ล่าสุด
  int scrHead_ = 0, scrCount_ = 0;
  uint32_t scrTotal_ = 0;
};

}  // namespace dsp
