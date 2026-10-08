// lie_engine.cpp — ดูหลักการใน lie_engine.h
// หมายเหตุ: ไฟล์นี้ถูกพอร์ตเป็น Python ใน Polygraph-Studio/backend/lie_engine.py
//           ถ้าแก้สูตรที่นี่ ต้องแก้ที่นั่นด้วย (มีเทสเทียบผลสองฝั่ง)
#include "lie_engine.h"
#include <string.h>

namespace lie {

namespace {

inline float clampf(float x, float lo, float hi) { return x < lo ? lo : (x > hi ? hi : x); }

// 1/(1+e^-x) แปลงคะแนนเป็นความน่าจะเป็น 0-1
inline float sigmoid(float x) {
  if (x > 30.0f) return 1.0f;    // กัน expf ล้น
  if (x < -30.0f) return 0.0f;
  return 1.0f / (1.0f + expf(-x));
}

struct Stat {   // Welford mean/sd
  int n = 0;
  float mean = 0.0f, m2 = 0.0f;
  void add(float x) {
    n++;
    float d = x - mean;
    mean += d / n;
    m2 += d * (x - mean);
  }
  float sd() const { return n > 1 ? sqrtf(m2 / (n - 1)) : 0.0f; }
};

const float kDefaultWeight[F_COUNT] = {0.20f, 0.35f, 0.25f, 0.12f, 0.08f};

}  // namespace

// เริ่มต้นสถานะว่าง (ยังไม่มี baseline) และใช้น้ำหนักจากค่าตั้ง
Engine::Engine() {
  for (int i = 0; i < F_COUNT; i++) {
    base_.mean[i] = base_.sd[i] = base_.nullMean[i] = base_.nullSd[i] = 0.0f;
    cal_.weight[i] = cfg_.weight[i];
  }
  configure(cfg_);
}

// รับค่าตั้งใหม่ (บีบให้อยู่ในช่วงที่ปลอดภัยก่อนใช้)
void Engine::configure(const Config& c) {
  cfg_ = c;
  cfg_.frameHz = clampf(cfg_.frameHz, 1.0f, 5.0f);
  cfg_.preSec = clampf(cfg_.preSec, 1.0f, (float)kMaxPre / cfg_.frameHz);
  cfg_.windowSec = clampf(cfg_.windowSec, 6.0f, (float)kMaxWin / cfg_.frameHz);
  // baseline ต้องยาวพอให้มีหน้าต่างย่อยอย่างน้อย ~4 อันสำหรับคิด null distribution
  const float minBase = cfg_.preSec + cfg_.windowSec + 3.0f;
  cfg_.baselineSec = clampf(cfg_.baselineSec, minBase < 20.0f ? 20.0f : minBase,
                            (float)kMaxBase / cfg_.frameHz);
  cfg_.recoverySec = clampf(cfg_.recoverySec, 0.0f, 60.0f);

  float ws = 0.0f;
  for (int i = 0; i < F_COUNT; i++) {
    if (!(cfg_.weight[i] >= 0.0f)) cfg_.weight[i] = 0.0f;   // จับ NAN ด้วย
    ws += cfg_.weight[i];
    if (!(cfg_.sdFloor[i] > 1e-4f)) cfg_.sdFloor[i] = 1e-4f;
  }
  for (int i = 0; i < F_COUNT; i++)
    cfg_.weight[i] = (ws > 0.0f) ? cfg_.weight[i] / ws : kDefaultWeight[i];

  cfg_.k = clampf(cfg_.k, 0.1f, 10.0f);
  cfg_.s0 = clampf(cfg_.s0, -5.0f, 10.0f);
  cfg_.lieP = clampf(cfg_.lieP, 0.5f, 0.99f);
  cfg_.truthP = clampf(cfg_.truthP, 0.01f, 0.5f);
  cfg_.motionLimit = clampf(cfg_.motionLimit, 0.1f, 20.0f);
  cfg_.minGsrFrac = clampf(cfg_.minGsrFrac, 0.0f, 1.0f);
  cfg_.minPpgFrac = clampf(cfg_.minPpgFrac, 0.0f, 1.0f);
  cfg_.maxMotionFrac = clampf(cfg_.maxMotionFrac, 0.0f, 1.0f);

  if (!cal_.active)
    for (int i = 0; i < F_COUNT; i++) cal_.weight[i] = cfg_.weight[i];
  bump();
}

// ---------------------------------------------------------------- คำสั่ง
bool Engine::startBaseline(float sec) {
  if (state_ == State::Question) { err_ = "QUESTION_ACTIVE"; return false; }
  float s = (sec > 0.0f) ? sec : cfg_.baselineSec;
  const float minBase = cfg_.preSec + cfg_.windowSec + 3.0f;
  s = clampf(s, minBase < 20.0f ? 20.0f : minBase, (float)kMaxBase / cfg_.frameHz);
  baseTarget_ = (int)(s * cfg_.frameHz + 0.5f);
  baseN_ = 0;
  base_.valid = false;
  // baseline ใหม่ = สเกล z-score เปลี่ยน -> ข้อควบคุมเดิมใช้เทียบไม่ได้แล้ว ต้องทำใหม่
  ctlN_ = 0;
  cal_ = Calibration();
  for (int i = 0; i < F_COUNT; i++) cal_.weight[i] = cfg_.weight[i];
  stress_ = -1.0f;
  state_ = State::Baseline;
  phaseStart_ = frames_;
  err_ = "";
  bump();
  return true;
}

// เริ่มวัด 1 ข้อ (ต้องมี baseline แล้ว และไม่มีข้อค้าง) — จำ qid และชนิดข้อ
bool Engine::startQuestion(uint16_t qid, Kind kind) {
  if (state_ != State::Ready) {
    if (state_ == State::Question) err_ = "QUESTION_ACTIVE";
    else if (state_ == State::Baseline) err_ = "BASELINE_RUNNING";
    else err_ = "NO_BASELINE";
    return false;
  }
  preFrames_ = (int)(cfg_.preSec * cfg_.frameHz + 0.5f);
  if (preFrames_ < 1) preFrames_ = 1;
  if (preFrames_ > kMaxPre) preFrames_ = kMaxPre;
  winTarget_ = (int)(cfg_.windowSec * cfg_.frameHz + 0.5f);
  if (winTarget_ < 1) winTarget_ = 1;
  if (winTarget_ > kMaxWin) winTarget_ = kMaxWin;

  // ช่วง "ก่อนถาม" = frame ล่าสุดในประวัติ (เรียงเก่า -> ใหม่)
  preN_ = (histN_ < preFrames_) ? histN_ : preFrames_;
  for (int i = 0; i < preN_; i++) preBuf_[i] = hist(preN_ - 1 - i);

  winN_ = 0;
  qid_ = qid;
  kind_ = kind;
  answerAt_ = -1.0f;
  answerYes_ = -1;
  state_ = State::Question;
  phaseStart_ = frames_;
  err_ = "";
  bump();
  return true;
}

// บันทึกเวลาที่ผู้ตอบตอบ และตอบ ใช่/ไม่ใช่
bool Engine::markAnswer(bool yes) {
  if (state_ != State::Question) { err_ = "NO_QUESTION"; return false; }
  answerAt_ = (frames_ - phaseStart_) / cfg_.frameHz;
  answerYes_ = yes ? 1 : 0;
  bump();
  return true;
}

// ยกเลิก baseline หรือข้อที่กำลังวัด
void Engine::abort() {
  if (state_ == State::Baseline) state_ = State::Idle;
  else if (state_ == State::Question) state_ = State::Ready;
  lastWindowEnd_ = frames_;
  bump();
}

// เริ่มผู้ตอบคนใหม่: ล้าง baseline และข้อควบคุมทั้งหมด
void Engine::resetSession() {
  state_ = State::Idle;
  base_.valid = false;
  ctlN_ = 0;
  cal_ = Calibration();
  for (int i = 0; i < F_COUNT; i++) cal_.weight[i] = cfg_.weight[i];
  firstSeq_ = resultSeq_;
  stress_ = -1.0f;
  settled_ = false;
  err_ = "";
  bump();
}

// ---------------------------------------------------------------- รับข้อมูล
const Frame& Engine::hist(int back) const {
  return hist_[(histHead_ - 1 - back + kHist * 2) % kHist];
}

// ป้อน 1 เฟรม (5 ครั้ง/วินาที): เก็บประวัติ, สะสม baseline หรือจบข้อเมื่อครบเวลาแล้วคำนวณผล
void Engine::push(const Frame& f) {
  frames_++;
  hist_[histHead_] = f;
  histHead_ = (histHead_ + 1) % kHist;
  if (histN_ < kHist) histN_++;

  if (state_ == State::Baseline) {
    if (baseN_ < kMaxBase) baseBuf_[baseN_++] = f;
    if (baseN_ >= baseTarget_) finishBaseline();
  } else if (state_ == State::Question) {
    if (winN_ < kMaxWin) winBuf_[winN_++] = f;
    if (winN_ >= winTarget_) finishQuestion();
  }
  updateStress(f);
  updateSettled();
}

// ---------------------------------------------------------------- feature
void Engine::computeFeatures(const Frame* pre, int nPre, const Frame* win, int nWin,
                             Features& o) const {
  for (int i = 0; i < F_COUNT; i++) {
    o.f[i] = 0.0f;
    o.ok[i] = false;
    o.pre[i] = NAN;
  }
  o.gsrFrac = o.ppgFrac = o.motionFrac = 0.0f;
  o.peakLatency = -1.0f;
  o.preOk = nPre > 0;

  // ---- ค่าเฉลี่ยก่อนถาม (ใช้เฉพาะ frame ที่สัญญาณนั้นใช้ได้) ----
  float sG = 0, sH = 0, sA = 0, sT = 0, sP = 0;
  int cG = 0, cH = 0, cA = 0, cT = 0, cP = 0;
  for (int j = 0; j < nPre; j++) {
    const Frame& f = pre[j];
    if (f.gsrOk) { sG += f.gsr; cG++; }
    if (f.ppgOk && f.hr > 0.0f) {
      sH += f.hr; cH++;
      if (f.amp > 0.0f) { sA += f.amp; cA++; }
    }
    sT += f.tremor; cT++;
    if (!isnan(f.temp)) { sP += f.temp; cP++; }
  }
  const int half = (nPre + 1) / 2;
  const float preG = cG ? sG / cG : NAN, preH = cH ? sH / cH : NAN, preA = cA ? sA / cA : NAN;
  const float preT = cT ? sT / cT : NAN, preP = cP ? sP / cP : NAN;
  o.pre[F_GSR] = preG; o.pre[F_HR] = preH; o.pre[F_AMP] = preA;
  o.pre[F_TRM] = preT; o.pre[F_TMP] = preP;
  if (nWin <= 0) return;

  // ---- ช่วงตอบสนอง ----
  float maxG = -1e9f, maxH = -1e9f, minA = 1e9f, minP = 1e9f, sumT = 0.0f;
  int maxGi = -1, wG = 0, wH = 0, wA = 0, wT = 0, wP = 0, mot = 0;
  for (int j = 0; j < nWin; j++) {
    const Frame& f = win[j];
    if (f.gsrOk) {
      wG++;
      if (f.gsr > maxG) { maxG = f.gsr; maxGi = j; }
    }
    if (f.ppgOk && f.hr > 0.0f) {
      wH++;
      if (f.hr > maxH) maxH = f.hr;
      if (f.amp > 0.0f) { wA++; if (f.amp < minA) minA = f.amp; }
    }
    sumT += f.tremor; wT++;
    if (!isnan(f.temp)) { wP++; if (f.temp < minP) minP = f.temp; }
    if (f.motion > cfg_.motionLimit) mot++;
  }
  o.gsrFrac = (float)wG / nWin;
  o.ppgFrac = (float)wH / nWin;
  o.motionFrac = (float)mot / nWin;

  if (cG > 0 && cG >= half && o.gsrFrac >= cfg_.minGsrFrac) {
    o.f[F_GSR] = maxG - preG;                    // µS ที่พุ่งขึ้น
    o.ok[F_GSR] = true;
    o.peakLatency = maxGi / cfg_.frameHz;
  }
  if (cH > 0 && cH >= half && o.ppgFrac >= cfg_.minPpgFrac) {
    o.f[F_HR] = maxH - preH;                     // bpm ที่เร่งขึ้น
    o.ok[F_HR] = true;
  }
  if (cA > 0 && cA >= half && preA > 1.0f && (float)wA / nWin >= cfg_.minPpgFrac) {
    o.f[F_AMP] = (preA - minA) / preA;           // สัดส่วนที่ลดลง (0.2 = ลด 20%)
    o.ok[F_AMP] = true;
  }
  if (cT > 0 && wT > 0) {
    o.f[F_TRM] = sumT / wT - preT;               // m/s² ที่สั่นเพิ่ม
    o.ok[F_TRM] = true;
  }
  if (cP > 0 && cP >= half && wP >= (nWin + 1) / 2) {
    o.f[F_TMP] = preP - minP;                    // °C ที่เย็นลง
    o.ok[F_TMP] = true;
  }
}

// คะแนนรวม = ผลรวม z × น้ำหนัก ของสัญญาณที่ใช้ได้ หารด้วยน้ำหนักรวมของสัญญาณที่ใช้ได้
float Engine::scoreOf(const float* z, uint8_t okMask, const float* w) const {
  float s = 0.0f, ws = 0.0f;
  for (int i = 0; i < F_COUNT; i++) {
    if (okMask & (1u << i)) {
      // จำกัด z: ค่าติดลบเยอะ (สงบลง) ไม่ควรลบล้างสัญญาณตัวอื่น, ค่าบวกสุดโต่งมักเป็น artifact
      s += w[i] * clampf(z[i], -2.0f, 6.0f);
      ws += w[i];
    }
  }
  return ws > 0.0f ? s / ws : 0.0f;   // ถ่วงน้ำหนักเฉลี่ยเฉพาะสัญญาณที่ใช้ได้
}

// ---------------------------------------------------------------- จบ baseline
void Engine::finishBaseline() {
  const int n = baseN_;
  Stat st[F_COUNT];
  int gsrCnt = 0, ppgCnt = 0;
  for (int i = 0; i < n; i++) {
    const Frame& f = baseBuf_[i];
    if (f.gsrOk) { st[F_GSR].add(f.gsr); gsrCnt++; }
    if (f.ppgOk && f.hr > 0.0f) {
      st[F_HR].add(f.hr);
      ppgCnt++;
      if (f.amp > 0.0f) st[F_AMP].add(f.amp);
    }
    st[F_TRM].add(f.tremor);
    if (!isnan(f.temp)) st[F_TMP].add(f.temp);
  }
  base_.durationSec = n / cfg_.frameHz;
  base_.gsrFrac = n ? (float)gsrCnt / n : 0.0f;
  base_.ppgFrac = n ? (float)ppgCnt / n : 0.0f;

  if (base_.gsrFrac < 0.7f && base_.ppgFrac < 0.7f) {
    // ต้องมีอย่างน้อย 1 สัญญาณหลัก (ชีพจร หรือ GSR) ตลอด >= 70% ของเวลา baseline
    // (v2.1: เดิมบังคับ GSR อย่างเดียว แต่หลายคนผิวแห้งจน GSR แทบวัดไม่ได้)
    err_ = "BASELINE_NO_SIGNAL";
    base_.valid = false;
    state_ = State::Idle;
    bump();
    return;
  }
  for (int i = 0; i < F_COUNT; i++) {
    base_.mean[i] = st[i].mean;
    base_.sd[i] = st[i].sd();
  }

  // null distribution: ใช้สูตร feature เดียวกับตอนถามจริง กับหน้าต่างย่อยใน baseline
  // (เลื่อนทีละ 1 วินาที) -> รู้ว่า "ไม่มีใครถามอะไร ค่ามันแกว่งเองได้แค่ไหน"
  int P = (int)(cfg_.preSec * cfg_.frameHz + 0.5f);
  if (P < 1) P = 1;
  if (P > kMaxPre) P = kMaxPre;
  int W = (int)(cfg_.windowSec * cfg_.frameHz + 0.5f);
  if (W < 1) W = 1;
  if (W > kMaxWin) W = kMaxWin;
  int step = (int)(cfg_.frameHz + 0.5f);
  if (step < 1) step = 1;

  Stat ns[F_COUNT];
  int windows = 0;
  for (int s = P; s + W <= n; s += step) {
    Features fe;
    computeFeatures(&baseBuf_[s - P], P, &baseBuf_[s], W, fe);
    windows++;
    for (int i = 0; i < F_COUNT; i++)
      if (fe.ok[i]) ns[i].add(fe.f[i]);
  }
  base_.nullN = (uint16_t)windows;
  for (int i = 0; i < F_COUNT; i++) {
    if (ns[i].n >= 3) {
      base_.nullMean[i] = ns[i].mean;
      base_.nullSd[i] = ns[i].sd();
    } else {
      // หน้าต่างไม่พอ (เช่น ชีพจรหลุดตลอด baseline) -> ประมาณจากการแกว่งระดับ frame
      base_.nullMean[i] = 0.0f;
      base_.nullSd[i] = 2.0f * base_.sd[i];
    }
  }
  base_.valid = true;
  refGsr_ = base_.mean[F_GSR];
  state_ = State::Ready;
  lastWindowEnd_ = frames_;
  err_ = "";
  bump();
}

// ---------------------------------------------------------------- จบคำถาม
void Engine::finishQuestion() {
  Features fe;
  computeFeatures(preBuf_, preN_, winBuf_, winN_, fe);

  Result r;
  r.seq = ++resultSeq_;
  r.qid = qid_;
  r.kind = kind_;
  r.tStart = phaseStart_ / cfg_.frameHz;
  r.answerAt = answerAt_;
  r.answerYes = answerYes_;
  r.peakLatency = fe.ok[F_GSR] ? fe.peakLatency : -1.0f;

  uint8_t mask = 0;
  for (int i = 0; i < F_COUNT; i++) {
    r.pre[i] = fe.pre[i];
    if (fe.ok[i]) {
      const float sd = base_.nullSd[i] > cfg_.sdFloor[i] ? base_.nullSd[i] : cfg_.sdFloor[i];
      r.feat[i] = fe.f[i];
      r.z[i] = (fe.f[i] - base_.nullMean[i]) / sd;
      mask |= (uint8_t)(1u << i);
    } else {
      r.feat[i] = 0.0f;
      r.z[i] = 0.0f;
    }
  }
  r.okMask = mask;

  const float* w = cal_.active ? cal_.weight : cfg_.weight;
  const float s0 = cal_.active ? cal_.s0 : cfg_.s0;
  const float k = cal_.active ? cal_.k : cfg_.k;
  r.score = scoreOf(r.z, mask, w);
  r.pLie = sigmoid(k * (r.score - s0));
  r.source = cal_.active ? 1 : 0;
  float pExt = 0.0f;
  if (scorer_ && scorer_(r, pExt, scorerCtx_)) {   // มีโมเดล AI -> ให้โมเดลตัดสินแทน
    r.pLie = clampf(pExt, 0.0f, 1.0f);
    r.source = 2;
  }

  uint16_t reasons = 0;
  if (!fe.ok[F_GSR]) reasons |= R_GSR_CONTACT;
  if (!fe.ok[F_HR]) reasons |= R_PPG_CONTACT;
  if (fe.motionFrac > cfg_.maxMotionFrac) reasons |= R_MOTION;
  const int needPre = preFrames_ / 2 > 1 ? preFrames_ / 2 : 1;
  if (preN_ < needPre) reasons |= R_SHORT_PRE;
  r.reasons = reasons;
  r.quality = (uint8_t)clampf(
      100.0f * (0.3f * fe.gsrFrac + 0.5f * fe.ppgFrac + 0.2f * (1.0f - fe.motionFrac)) + 0.5f,
      0.0f, 100.0f);

  // ใช้ไม่ได้เมื่อ: ไม่มีทั้ง GSR และชีพจร / ขยับแรง / ข้อมูลก่อนถามไม่พอ
  const bool noMainSignal = !fe.ok[F_GSR] && !fe.ok[F_HR];
  if (noMainSignal || (reasons & (R_MOTION | R_SHORT_PRE))) r.verdict = Verdict::Invalid;
  else if (r.pLie >= cfg_.lieP) r.verdict = Verdict::Lie;
  else if (r.pLie <= cfg_.truthP) r.verdict = Verdict::Truth;
  else r.verdict = Verdict::Inconclusive;

  if (kind_ == Kind::ControlTruth) r.expected = Verdict::Truth;
  else if (kind_ == Kind::ControlLie) r.expected = Verdict::Lie;
  if (r.expected != Verdict::None && r.verdict != Verdict::Invalid) {
    // ตัดสินข้อควบคุมด้วยค่าก่อนหน้า (ยังไม่รวมข้อนี้) = วัดความแม่นแบบไม่โกง
    cal_.scored++;
    if (r.verdict == r.expected) { cal_.correct++; r.correct = true; }
  }
  res_[(r.seq - 1) % kMaxRes] = r;

  if ((kind_ == Kind::ControlTruth || kind_ == Kind::ControlLie) && r.verdict != Verdict::Invalid) {
    Control c;
    c.lie = (kind_ == Kind::ControlLie);
    c.okMask = mask;
    for (int i = 0; i < F_COUNT; i++) c.z[i] = r.z[i];
    if (ctlN_ < kMaxCtl) {
      ctl_[ctlN_++] = c;
    } else {
      for (int i = 1; i < kMaxCtl; i++) ctl_[i - 1] = ctl_[i];
      ctl_[kMaxCtl - 1] = c;
    }
    recalibrate();
  }
  state_ = State::Ready;
  lastWindowEnd_ = frames_;
  bump();
}

// ---------------------------------------------------------------- calibration
void Engine::recalibrate() {
  int nT = 0, nL = 0;
  for (int j = 0; j < ctlN_; j++) (ctl_[j].lie ? nL : nT)++;
  cal_.nTruth = (uint8_t)nT;
  cal_.nLie = (uint8_t)nL;
  if (nT == 0 || nL == 0) {
    cal_.active = false;
    cal_.weak = false;
    cal_.meanTruth = cal_.meanLie = cal_.separation = 0.0f;
    for (int i = 0; i < F_COUNT; i++) cal_.weight[i] = cfg_.weight[i];
    return;
  }

  // 1) feature ไหนแยก "โกหก" ออกจาก "จริง" ได้ดีสำหรับคนนี้ -> ให้น้ำหนักเพิ่ม
  float zT[F_COUNT] = {0}, zL[F_COUNT] = {0};
  int cT[F_COUNT] = {0}, cL[F_COUNT] = {0};
  for (int j = 0; j < ctlN_; j++) {
    for (int i = 0; i < F_COUNT; i++) {
      if (!(ctl_[j].okMask & (1u << i))) continue;
      if (ctl_[j].lie) { zL[i] += ctl_[j].z[i]; cL[i]++; }
      else             { zT[i] += ctl_[j].z[i]; cT[i]++; }
    }
  }
  float d[F_COUNT], dsum = 0.0f;
  for (int i = 0; i < F_COUNT; i++) {
    d[i] = 0.0f;
    if (cT[i] > 0 && cL[i] > 0) {
      float diff = zL[i] / cL[i] - zT[i] / cT[i];
      d[i] = diff > 0.0f ? diff : 0.0f;
    }
    dsum += d[i];
  }
  float w[F_COUNT];
  for (int i = 0; i < F_COUNT; i++)
    // ผสมครึ่ง-ครึ่งกับค่าเริ่มต้น: ข้อควบคุมมีแค่ไม่กี่ข้อ ถ้าเชื่อมันทั้งหมดจะ overfit
    w[i] = (dsum > 0.5f) ? 0.5f * cfg_.weight[i] + 0.5f * d[i] / dsum : cfg_.weight[i];

  // 2) คะแนนเฉลี่ยของข้อ "จริง" vs "โกหก" ด้วยน้ำหนักใหม่ -> ตั้งเส้นแบ่งตรงกลาง
  float sT = 0.0f, sL = 0.0f;
  for (int j = 0; j < ctlN_; j++) {
    float s = scoreOf(ctl_[j].z, ctl_[j].okMask, w);
    if (ctl_[j].lie) sL += s; else sT += s;
  }
  const float mT = sT / nT, mL = sL / nL, sep = mL - mT;
  cal_.meanTruth = mT;
  cal_.meanLie = mL;
  cal_.separation = sep;
  if (sep >= 0.75f) {
    cal_.active = true;
    cal_.weak = false;
    cal_.s0 = 0.5f * (mT + mL);
    // ตั้งความชันให้ค่าเฉลี่ยข้อโกหกได้ P ≈ 0.92 (sigmoid(2.5)) และข้อจริง ≈ 0.08
    // ขั้นต่ำ 1.0: ถ้าคนนี้ตอบสนองแรงมาก (sep กว้าง) เส้นโค้งจะไม่ราบจนตัดสินไม่ได้
    cal_.k = clampf(5.0f / sep, 1.0f, 6.0f);
    for (int i = 0; i < F_COUNT; i++) cal_.weight[i] = w[i];
  } else {
    // ตอบสนองต่อข้อโกหกไม่ต่างจากข้อจริง -> เชื่อ calibration ไม่ได้ ใช้ค่าเริ่มต้น
    cal_.active = false;
    cal_.weak = true;
    for (int i = 0; i < F_COUNT; i++) cal_.weight[i] = cfg_.weight[i];
  }
}

// ---------------------------------------------------------------- ดัชนีความเครียดสด
void Engine::updateStress(const Frame& f) {
  if (!base_.valid) { stress_ = -1.0f; return; }
  float num = 0.0f, den = 0.0f;
  if (f.gsrOk && base_.gsrFrac > 0.3f) {   // baseline ไม่มี GSR -> อย่าเอา GSR มาคิด (ค่าอ้างอิงไม่มี)
    float sd = base_.sd[F_GSR] > 0.05f ? base_.sd[F_GSR] : 0.05f;
    num += 0.6f * clampf((f.gsr - refGsr_) / sd, -3.0f, 8.0f);
    den += 0.6f;
  }
  if (f.ppgOk && f.hr > 0.0f && base_.ppgFrac > 0.3f) {
    float sd = base_.sd[F_HR] > 2.0f ? base_.sd[F_HR] : 2.0f;
    num += 0.3f * clampf((f.hr - base_.mean[F_HR]) / sd, -3.0f, 8.0f);
    den += 0.3f;
  }
  {
    float sd = base_.sd[F_TRM] > 0.02f ? base_.sd[F_TRM] : 0.02f;
    num += 0.1f * clampf((f.tremor - base_.mean[F_TRM]) / sd, -3.0f, 8.0f);
    den += 0.1f;
  }
  const float raw = den > 0.0f ? num / den : 0.0f;
  const float target = 100.0f * sigmoid(0.8f * raw - 1.2f);   // พักปกติ ≈ 23
  const float a1 = 1.0f - expf(-1.0f / (cfg_.frameHz * 1.0f)); // ทำให้เข็มเรียบ (tau 1 s)
  if (stress_ < 0.0f) stress_ = target;
  else stress_ += a1 * (target - stress_);

  // ระดับเหงื่อพื้นฐานลอยได้ตามเวลา -> ปรับค่าอ้างอิงช้า ๆ (tau 60 s) เฉพาะตอนพักนิ่ง
  if (state_ == State::Ready && settled_ && f.gsrOk) {
    const float a60 = 1.0f - expf(-1.0f / (cfg_.frameHz * 60.0f));
    refGsr_ += a60 * (f.gsr - refGsr_);
  }
}

// ร่างกายกลับสู่ปกติหลังข้อก่อนหรือยัง (พร้อมถามข้อถัดไป)
void Engine::updateSettled() {
  if (state_ != State::Ready) { settled_ = false; return; }
  if ((float)(frames_ - lastWindowEnd_) < cfg_.recoverySec * cfg_.frameHz) {
    settled_ = false;
    return;
  }
  // ใช้ GSR ถ้ามีครบ (ตอบสนองช้าที่สุด จึงบอกการ "กลับสู่ปกติ" ได้ดีที่สุด)
  //   เฉลี่ย 1 s เทียบกับ 3 s ก่อนหน้า: ชันไม่เกิน 0.03 µS/s
  // ถ้าไม่มี GSR (ผิวแห้ง/แผ่นไม่แตะ ซึ่งเกิดบ่อย) ใช้ชีพจรแทน — v2.0 ใช้ GSR อย่างเดียว
  //   ทำให้หน้าเว็บขึ้น "รอสัญญาณนิ่ง" ตลอดไปเมื่อไม่มีเหงื่อ
  //   เฉลี่ย 4 s เทียบกับ 4 s ก่อนหน้า: ต้องเฉลี่ยให้ครบ ~1 รอบหายใจ ไม่งั้นชีพจรที่แกว่งตาม
  //   การหายใจ (±3–5 bpm) จะทำให้สถานะกระพริบ พร้อม/ไม่พร้อม; ต่างกันไม่เกิน 3 bpm = นิ่ง
  // ไม่มีทั้งสองอย่าง = ถือว่าพร้อม (รอครบ recoverySec แล้ว) ปัญหาการแตะผิวแจ้งแยกอยู่แล้ว
  const int gAvg = (int)(cfg_.frameHz + 0.5f), gSpan = (int)(3.0f * cfg_.frameHz + 0.5f);
  const int hAvg = (int)(4.0f * cfg_.frameHz + 0.5f), hSpan = hAvg;
  bool gsrAll = histN_ >= gSpan + gAvg, ppgAll = histN_ >= hSpan + hAvg;
  float ga = 0.0f, gb = 0.0f, ha = 0.0f, hb = 0.0f;
  for (int i = 0; gsrAll && i < gAvg; i++) {
    const Frame& fa = hist(i);
    const Frame& fb = hist(gSpan + i);
    if (!fa.gsrOk || !fb.gsrOk) gsrAll = false;
    ga += fa.gsr;
    gb += fb.gsr;
  }
  for (int i = 0; ppgAll && i < hAvg; i++) {
    const Frame& fa = hist(i);
    const Frame& fb = hist(hSpan + i);
    if (!fa.ppgOk || !fb.ppgOk) ppgAll = false;
    ha += fa.hr;
    hb += fb.hr;
  }
  if (gsrAll) settled_ = fabsf((ga - gb) / gAvg / 3.0f) < 0.03f;   // µS/s
  else if (ppgAll) settled_ = fabsf((ha - hb) / hAvg) < 3.0f;     // bpm
  else settled_ = true;
}

// ---------------------------------------------------------------- สถานะ
Status Engine::status() const {
  Status s;
  s.state = state_;
  s.elapsed = 0.0f;
  s.progress = 0.0f;
  if (state_ == State::Baseline) {
    s.elapsed = (frames_ - phaseStart_) / cfg_.frameHz;
    s.progress = baseTarget_ > 0 ? (float)baseN_ / baseTarget_ : 0.0f;
  } else if (state_ == State::Question) {
    s.elapsed = (frames_ - phaseStart_) / cfg_.frameHz;
    s.progress = winTarget_ > 0 ? (float)winN_ / winTarget_ : 0.0f;
  }
  s.qid = (state_ == State::Question) ? qid_ : 0;
  s.kind = kind_;
  s.baselineValid = base_.valid;
  s.calibrated = cal_.active;
  s.calibWeak = cal_.weak;
  s.stress = stress_ < 0.0f ? -1 : (int)(stress_ + 0.5f);
  s.settled = settled_;
  s.results = resultSeq_ - firstSeq_;
  s.revision = revision_;
  return s;
}

// อ่านผลย้อนหลัง (0 = ล่าสุด) จากบัฟเฟอร์วงกลม
bool Engine::result(uint32_t newestIndex, Result& out) const {
  uint32_t avail = resultSeq_ - firstSeq_;
  if (avail > (uint32_t)kMaxRes) avail = kMaxRes;
  if (newestIndex >= avail) return false;
  const uint32_t seq = resultSeq_ - newestIndex;
  out = res_[(seq - 1) % kMaxRes];
  return true;
}

// อ่านผลตามเลข seq (ถ้ายังอยู่ในบัฟเฟอร์)
bool Engine::resultBySeq(uint32_t seq, Result& out) const {
  if (seq == 0 || seq > resultSeq_ || seq <= firstSeq_) return false;
  if (resultSeq_ - seq >= (uint32_t)kMaxRes) return false;
  out = res_[(seq - 1) % kMaxRes];
  return true;
}

// ชื่อสถานะเป็นข้อความ (ใช้ใน JSON/Serial)
const char* Engine::stateName(State s) {
  switch (s) {
    case State::Idle: return "idle";
    case State::Baseline: return "baseline";
    case State::Ready: return "ready";
    case State::Question: return "question";
  }
  return "?";
}

// ชื่อชนิดข้อเป็นข้อความ: test / truth / lie / warmup
const char* Engine::kindName(Kind k) {
  switch (k) {
    case Kind::Test: return "test";
    case Kind::ControlTruth: return "truth";
    case Kind::ControlLie: return "lie";
    case Kind::Warmup: return "warmup";
  }
  return "?";
}

// ชื่อคำตัดสินเป็นข้อความ: truth / lie / inconclusive / invalid
const char* Engine::verdictName(Verdict v) {
  switch (v) {
    case Verdict::None: return "none";
    case Verdict::Truth: return "truth";
    case Verdict::Lie: return "lie";
    case Verdict::Inconclusive: return "inconclusive";
    case Verdict::Invalid: return "invalid";
  }
  return "?";
}

// แปลงข้อความชนิดข้อจาก API เป็น enum (ไม่รู้จัก = false)
bool Engine::kindFromName(const char* s, Kind& out) {
  if (!s) return false;
  if (!strcmp(s, "test")) { out = Kind::Test; return true; }
  if (!strcmp(s, "truth")) { out = Kind::ControlTruth; return true; }
  if (!strcmp(s, "lie")) { out = Kind::ControlLie; return true; }
  if (!strcmp(s, "warmup")) { out = Kind::Warmup; return true; }
  return false;
}

}  // namespace lie
