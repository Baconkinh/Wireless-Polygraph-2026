// =====================================================================
//  ppg.cpp — แปลงคลื่นแสง IR (PPG 100 Hz) เป็น ชีพจร, HRV (RMSSD), แรงชีพจร + ตรวจการแตะผิว
//  ทำอะไร   : กรอง DC/สัญญาณรบกวน -> หาจุดยอดของแต่ละครั้งที่หัวใจเต้น (ประมาณตำแหน่งด้วยพาราโบลา)
//             -> ช่วงห่างระหว่างครั้ง (IBI) -> bpm และ RMSSD
//  เรียกจาก : sensors.cpp readPpg() (ทุก sample) และแจ้ง ui.cpp ให้ LED กระพริบตามจังหวะหัวใจ
//  วิชา     : การประมวลผลสัญญาณ (filter, peak detection)
// =====================================================================
#include "ppg.h"
#include <math.h>

namespace dsp {

// ตั้งตัวกรองชีพจร: low-pass 6 Hz (ตัด noise), high-pass 0.5 Hz (ตัด DC สำหรับกราฟ), เกณฑ์ว่าแตะผิว
void PpgProcessor::begin(float fs, uint32_t contactThreshold) {
  fs_ = fs;
  lp_.lowpass(fs, 6.0f);        // ตัดสัญญาณรบกวนความถี่สูง แต่ยังเก็บขาขึ้นที่ชันของ systole
  dispHp_.highpass(fs, 0.5f);   // กราฟ: ตัด DC/การหายใจออกให้คลื่นอยู่กลางจอ
  dispLp_.lowpass(fs, 4.0f);
  dc_.setup(fs, 1.0f);
  setContactThreshold(contactThreshold);
  contact_ = false;
  onCount_ = offCount_ = 0;
  settle_ = 0;
  n_ = 0;
  beats_ = rejected_ = 0;
  resetBeatState();
}

// ตั้งเกณฑ์แสง IR ที่ถือว่า "แตะผิว" (ปล่อยที่ 80% กันสถานะกระพริบ)
void PpgProcessor::setContactThreshold(uint32_t thr) {
  onThr_ = thr;
  offThr_ = thr - thr / 5;  // ปล่อยที่ 80% ของเกณฑ์แตะ: กันสถานะกระพริบตอนค่าวนอยู่แถวเกณฑ์
}

// ดัชนี perfusion (%) = แอมพลิจูดชีพจร / ระดับ DC — บอกว่าเลือดไหลเวียนปลายนิ้วมากน้อย
float PpgProcessor::perfusion() const {
  float d = dc_.value();
  return (contact_ && d > 1.0f) ? 100.0f * ampEst_ / d : 0.0f;
}

// ล้างสถานะการหาจังหวะ (เริ่มใหม่หลังสัญญาณหลุด)
void PpgProcessor::resetBeatState() {
  wave_ = 0.0f;
  for (int i = 0; i < kSsfW; i++) slope_[i] = 0.0f;
  slopeHead_ = 0;
  ssf_ = ssf1_ = ssf2_ = 0.0f;
  ssfEst_ = 0.0f;
  initMax_ = 0.0f;
  trough_ = 1e9f;
  ampSearch_ = 0;
  ampEst_ = 0.0f;
  haveLastPeak_ = false;
  lastPeakT_ = -1.0;
  lastBeatN_ = n_;
  ibiCount_ = 0;
  ibiHead_ = 0;
  chainId_++;
  rejectStreak_ = 0;
  hr_ = rmssd_ = lastIbi_ = 0.0f;
}

// ข้อมูลขาดช่วง (อ่าน FIFO ไม่ทัน/หลับ) -> ไม่ใช้ช่วงห่างที่คร่อมช่องว่างคำนวณชีพจร
void PpgProcessor::markGap() {
  // ตัวกรองยังใช้ต่อได้ แต่ช่วงเวลาระหว่างจังหวะที่คร่อมช่องว่างเชื่อไม่ได้
  haveLastPeak_ = false;
  chainId_++;
}

// มัธยฐานของช่วงห่างระหว่างจังหวะ m ค่าล่าสุด (ทนค่าหลุดกว่าค่าเฉลี่ย)
float PpgProcessor::medianRecentIbi(int m) const {
  float tmp[8];
  if (m > ibiCount_) m = ibiCount_;
  if (m > 8) m = 8;
  for (int i = 0; i < m; i++) tmp[i] = ibi_[(ibiHead_ - 1 - i + kIbiN) % kIbiN];
  return median(tmp, m);
}

// ป้อนแสง IR 1 ค่า (100 Hz): ตรวจการแตะผิว -> กรอง -> หาจังหวะหัวใจด้วยความชัน (SSF) -> คืน true เมื่อเจอจังหวะ
// [เทคนิค: Signal processing — PPG peak detection] ตรวจแตะผิว (hysteresis) -> low-pass 6 Hz -> SSF (ผลรวมความชันขาขึ้น 120 ms)
//   -> เกณฑ์ปรับตัวเอง -> หาจุดยอดแต่ละครั้งที่หัวใจเต้น -> IBI -> bpm, HRV (RMSSD), แรงชีพจร
bool PpgProcessor::push(uint32_t irRaw) {
  n_++;
  saturated_ = irRaw >= 260000;  // 18-bit เต็มสเกล = 262143 -> ใกล้เพดาน = ยอดคลื่นถูกตัด
  const float x = (float)irRaw;
  dc_.push(x);

  // ---- 1) ตรวจการแตะผิว แบบ hysteresis + หน่วงเวลา ----
  if (!contact_) {
    if (irRaw > onThr_) {
      if (++onCount_ >= (uint32_t)(0.3f * fs_)) {   // ต้องแตะนิ่ง 0.3 s ถึงนับว่าแตะจริง
        contact_ = true;
        offCount_ = 0;
        resetBeatState();
        lp_.reset(-x);       // เริ่มตัวกรองที่สถานะนิ่ง -> ไม่กระชาก
        dispHp_.reset(-x);
        dispLp_.reset(0.0f);
        dc_.reset(x);
        sPrev_ = -x;
        settle_ = (uint32_t)(2.0f * fs_);
      }
    } else {
      onCount_ = 0;
    }
  } else {
    if (irRaw < offThr_) {
      if (++offCount_ >= (uint32_t)(0.1f * fs_)) {
        contact_ = false;
        onCount_ = 0;
        resetBeatState();
      }
    } else {
      offCount_ = 0;
    }
  }
  if (!contact_) {
    wave_ = 0.0f;
    return false;
  }

  // ---- 2) กลับเครื่องหมาย + กรอง ----
  const float s = lp_.push(-x);                  // ใช้ตรวจจังหวะ
  wave_ = dispLp_.push(dispHp_.push(-x));        // ใช้วาดกราฟ

  // ---- 3) SSF = ผลรวมความชันขาขึ้นใน 120 ms ล่าสุด ----
  float d = s - sPrev_;
  sPrev_ = s;
  if (d < 0.0f) d = 0.0f;
  ssf_ += d - slope_[slopeHead_];
  slope_[slopeHead_] = d;
  slopeHead_ = (slopeHead_ + 1) % kSsfW;
  if (ssf_ < 0.0f) ssf_ = 0.0f;                  // กันค่าติดลบจากการปัดเศษสะสม

  if (s < trough_) trough_ = s;

  // หลังพบจังหวะ: เดินต่อ ~150 ms เพื่อหายอดของคลื่น -> แอมพลิจูด = ยอด - จุดต่ำก่อนขึ้น
  if (ampSearch_ > 0) {
    if (s > ampPeak_) ampPeak_ = s;
    if (--ampSearch_ == 0) {
      const float a = ampPeak_ - ampTrough_;
      if (a > 0.0f) {
        if (ampEst_ <= 0.0f) ampEst_ = a;
        else ampEst_ += 0.25f * (clampf(a, 0.5f * ampEst_, 2.0f * ampEst_) - ampEst_);
      }
      trough_ = s;   // เริ่มหาจุดต่ำสุดของจังหวะถัดไป
    }
  }

  if (settle_ > 0) {
    settle_--;
    // 1.5 วินาทีสุดท้ายของช่วงรอนิ่ง: จำ SSF สูงสุดไว้เป็นค่าเริ่มของเกณฑ์
    if (settle_ < (uint32_t)(1.5f * fs_) && ssf_ > initMax_) initMax_ = ssf_;
    if (settle_ == 0) {
      ssfEst_ = initMax_;
      lastBeatN_ = n_;
      trough_ = s;
    }
    ssf2_ = ssf1_;
    ssf1_ = ssf_;
    return false;
  }

  // ---- 4) จังหวะ = local max ของ SSF ที่ n-1 ซึ่งเกินเกณฑ์ ----
  bool beat = false;
  const int32_t sinceBeat = (int32_t)((n_ - 1) - lastBeatN_);
  if (ssf1_ > ssf2_ && ssf1_ >= ssf_ && ssf1_ >= 0.5f * ssfEst_ && ssf1_ > 0.0f) {
    float refractory = 0.30f * fs_;               // ขั้นต่ำ 300 ms (200 bpm)
    if (ibiCount_ >= 3) {
      float r = 0.6f * medianRecentIbi(5) * fs_ / 1000.0f;
      if (r > refractory) refractory = r;
    }
    if (sinceBeat >= (int32_t)refractory) {
      ssfEst_ += 0.25f * (clampf(ssf1_, 0.5f * ssfEst_, 2.0f * ssfEst_) - ssfEst_);

      // จุดยอดจริงอยู่ระหว่าง sample — ประมาณด้วยพาราโบลาผ่าน 3 จุด (ช่วยให้ HRV แม่นขึ้น)
      const float den = ssf2_ - 2.0f * ssf1_ + ssf_;
      const float delta = (den < 0.0f) ? clampf(0.5f * (ssf2_ - ssf_) / den, -0.5f, 0.5f) : 0.0f;
      const double tPeak = (double)(n_ - 1) + delta;

      if (haveLastPeak_) {
        const float ibiMs = (float)((tPeak - lastPeakT_) * 1000.0 / fs_);
        if (ibiMs >= 300.0f && ibiMs <= 1600.0f) acceptIbi(ibiMs);
        else chainId_++;  // นานเกิน = พลาดจังหวะ, สั้นเกิน = สัญญาณรบกวน
      }
      lastPeakT_ = tPeak;
      haveLastPeak_ = true;
      lastBeatN_ = n_ - 1;
      ampTrough_ = trough_;
      ampPeak_ = s;
      ampSearch_ = (int)(0.15f * fs_);
      beats_++;
      beat = true;
    }
  }
  ssf2_ = ssf1_;
  ssf1_ = ssf_;

  // ---- 5) ไม่เจอจังหวะนาน ----
  const int32_t quiet = (int32_t)(n_ - lastBeatN_);
  if (quiet > (int32_t)(2.0f * fs_)) {
    ssfEst_ *= (1.0f - 0.7f / fs_);   // เกณฑ์สูงเกินจริง (เช่นหลังขยับแรง) -> ค่อย ๆ ลดลง
  }
  if (quiet > (int32_t)(3.0f * fs_)) {
    hr_ = 0.0f;
    rmssd_ = 0.0f;
  }
  if (quiet > (int32_t)(6.0f * fs_) && ibiCount_ > 0) {
    ibiCount_ = 0;
    ibiHead_ = 0;
    chainId_++;
    haveLastPeak_ = false;
  }
  return beat;
}

// รับช่วงห่างระหว่างจังหวะใหม่ (ต่างจากมัธยฐานเกิน 30% = น่าจะผิด ไม่รับ)
void PpgProcessor::acceptIbi(float ibiMs) {
  if (ibiCount_ >= 3) {
    const float med = medianRecentIbi(5);
    if (fabsf(ibiMs - med) > 0.3f * med) {
      rejected_++;
      if (++rejectStreak_ < 3) {
        chainId_++;     // จังหวะผิดปกติคั่นอยู่ -> ห้ามเอาไปคิด successive difference
        return;
      }
      // ผิดจากเดิมติดกัน 3 ครั้ง = ชีพจรเปลี่ยนจริง (เช่นตกใจ) -> เริ่มประวัติใหม่
      ibiCount_ = 0;
      ibiHead_ = 0;
      chainId_++;
    }
  }
  rejectStreak_ = 0;
  ibi_[ibiHead_] = ibiMs;
  chain_[ibiHead_] = chainId_;
  ibiHead_ = (ibiHead_ + 1) % kIbiN;
  if (ibiCount_ < kIbiN) ibiCount_++;
  lastIbi_ = ibiMs;
  recompute();
}

// คำนวณชีพจร (bpm) และ HRV (RMSSD) ใหม่จากช่วงห่างล่าสุด
void PpgProcessor::recompute() {
  // HR = 60000 / มัธยฐานของ IBI ล่าสุด 5 ค่า (ทนค่าหลุดได้ดีกว่าค่าเฉลี่ย)
  hr_ = (ibiCount_ >= 2) ? 60000.0f / medianRecentIbi(5) : 0.0f;

  // RMSSD = sqrt(mean((IBI[i] - IBI[i-1])^2)) เฉพาะคู่ที่ต่อเนื่องกันจริง
  const int k = ibiCount_ < 12 ? ibiCount_ : 12;
  double ss = 0.0;
  int cnt = 0;
  for (int i = k - 1; i >= 1; i--) {
    const int prev = (ibiHead_ - 1 - i + kIbiN) % kIbiN;
    const int cur = (ibiHead_ - i + kIbiN) % kIbiN;
    if (chain_[prev] == chain_[cur]) {
      const double dd = (double)ibi_[cur] - (double)ibi_[prev];
      ss += dd * dd;
      cnt++;
    }
  }
  rmssd_ = (cnt >= 4) ? (float)sqrt(ss / cnt) : 0.0f;
}

}  // namespace dsp
