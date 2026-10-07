// =====================================================================
//  ppg.h — ประมวลผลสัญญาณ PPG (แสง IR สะท้อนจากผิว) -> ชีพจร, HRV, ความแรงชีพจร
//
//  หลักการ: เลือดเข้าหลอดเลือดมากขึ้นตอนหัวใจบีบ (systole) -> ดูดกลืนแสงมากขึ้น
//  -> แสงสะท้อนกลับ "ลดลง" ดังนั้นเราจะกลับเครื่องหมายสัญญาณให้จังหวะหัวใจเป็น "ขาขึ้น"
//
//  วิธีหาจังหวะ: Slope Sum Function (SSF) — แนวคิดจากงานตรวจจังหวะในสัญญาณความดันเลือด
//  (Zong et al., 2003) นำมาใช้กับ PPG: รวม "ความชันขาขึ้น" ในหน้าต่าง 120 ms
//   - ช่วงหัวใจบีบ สัญญาณพุ่งขึ้นชันมาก -> SSF สูง
//   - คลื่น dicrotic / การหายใจ / แรงกดเปลี่ยนช้า ๆ มีความชันน้อย -> SSF ต่ำ ไม่ถูกนับ
//   - ไม่ต้องใช้ high-pass จึงไม่บิดรูปคลื่นตอนชีพจรช้า (~40 bpm)
//  เกณฑ์ = 50% ของ SSF เฉลี่ยของจังหวะก่อน ๆ + ช่วงห้ามนับซ้ำ (refractory) ปรับตามชีพจร
//
//  ผลลัพธ์: HR (มัธยฐาน 5 จังหวะ), HRV แบบ RMSSD, แอมพลิจูดชีพจร, Perfusion Index
//  ไม่พึ่ง Arduino -> unit test บนคอมได้ (test/test_native)
// =====================================================================
#pragma once
#include <stdint.h>
#include "filters.h"

namespace dsp {

class PpgProcessor {
 public:
  void begin(float fs, uint32_t contactThreshold);
  void setContactThreshold(uint32_t thr);

  // ป้อน IR ดิบ 1 ค่า (18-bit) — คืน true ถ้าตรวจพบจังหวะหัวใจที่ sample นี้
  bool push(uint32_t irRaw);

  // ข้อมูลขาดช่วง (FIFO ล้น/อ่าน I2C ไม่ได้) -> ตัดสายประวัติจังหวะ กัน IBI ผิด
  void markGap();

  bool     contact()    const { return contact_; }
  bool     settled()    const { return contact_ && settle_ == 0; }
  float    wave()       const { return wave_; }      // สัญญาณกรอง 0.5–4 Hz (ไว้วาดกราฟ)
  float    hr()         const { return hr_; }        // bpm, 0 = ยังไม่มีค่า
  float    hrv()        const { return rmssd_; }     // RMSSD (ms), 0 = ข้อมูลไม่พอ
  float    lastIbi()    const { return lastIbi_; }   // ms
  float    amplitude()  const { return ampEst_; }    // แอมพลิจูดชีพจร (count)
  float    perfusion()  const;                       // Perfusion Index (%) = AC/DC
  uint32_t dc()         const { return (uint32_t)dc_.value(); }
  bool     saturated()  const { return saturated_; }
  uint32_t beatCount()  const { return beats_; }
  uint32_t rejected()   const { return rejected_; }

 private:
  void resetBeatState();
  void acceptIbi(float ibiMs);
  void recompute();
  float medianRecentIbi(int m) const;

  float fs_ = 100.0f;
  uint32_t onThr_ = 50000, offThr_ = 40000;

  Biquad lp_;                    // ตัวกรองสำหรับตรวจจังหวะ (low-pass อย่างเดียว)
  Biquad dispHp_, dispLp_;       // ตัวกรองสำหรับกราฟ (band-pass)
  Ema dc_;
  bool contact_ = false;
  uint32_t onCount_ = 0, offCount_ = 0;
  uint32_t settle_ = 0;
  bool saturated_ = false;

  uint32_t n_ = 0;               // ตัวนับ sample (ใช้เป็นฐานเวลา แม่นกว่า millis())
  float wave_ = 0.0f;
  float sPrev_ = 0.0f;           // สัญญาณ low-pass ตัวก่อน (คิดความชัน)

  static const int kSsfW = 12;   // 120 ms @100 Hz
  float slope_[kSsfW];
  int slopeHead_ = 0;
  float ssf_ = 0.0f, ssf1_ = 0.0f, ssf2_ = 0.0f;   // SSF[n], [n-1], [n-2]
  float ssfEst_ = 0.0f;          // SSF เฉลี่ยของจังหวะจริง -> ใช้ตั้งเกณฑ์
  float initMax_ = 0.0f;

  float trough_ = 0.0f;          // จุดต่ำสุดของสัญญาณก่อนจังหวะนี้
  float ampPeak_ = 0.0f, ampTrough_ = 0.0f;
  int ampSearch_ = 0;            // นับถอยหลังหา "ยอด" หลังตรวจพบจังหวะ
  float ampEst_ = 0.0f;

  double lastPeakT_ = -1.0;      // เวลา (หน่วย sample, ละเอียดกว่า sample) ของจังหวะล่าสุด
  uint32_t lastBeatN_ = 0;
  bool haveLastPeak_ = false;

  static const int kIbiN = 16;
  float ibi_[kIbiN];
  uint8_t chain_[kIbiN];         // IBI ที่ chain เดียวกัน = ต่อเนื่องกันจริง (ใช้คิด RMSSD)
  int ibiCount_ = 0, ibiHead_ = 0;
  uint8_t chainId_ = 0;
  int rejectStreak_ = 0;

  float hr_ = 0.0f, rmssd_ = 0.0f, lastIbi_ = 0.0f;
  uint32_t beats_ = 0, rejected_ = 0;
};

}  // namespace dsp
