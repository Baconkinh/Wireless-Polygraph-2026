// =====================================================================
//  Unit tests ของโมดูลที่ไม่พึ่งฮาร์ดแวร์ (รันบนคอม)  —  pio test -e native
//  ใช้สัญญาณจำลองที่รู้ "คำตอบที่ถูก" อยู่แล้ว แล้วตรวจว่าอัลกอริทึมหาได้ใกล้เคียง
// =====================================================================
#include <unity.h>
#include <math.h>
#include <stdio.h>
#include "dsp/ppg.h"
#include "dsp/motion.h"
#include "dsp/eda.h"
#include "drivers/sensor_math.h"
#include "lie/lie_engine.h"
#include "lie/ml_model.h"

// ---------- ตัวสุ่มแบบกำหนดผลได้ (ให้ผลเหมือนกันทุกเครื่อง) ----------
static uint32_t g_seed = 12345;
static float urand() {            // [0,1)
  g_seed ^= g_seed << 13; g_seed ^= g_seed >> 17; g_seed ^= g_seed << 5;
  return (g_seed & 0xFFFFFF) / 16777216.0f;
}
static float nrand() {            // ~N(0,1) จากผลรวม 12 ค่า (Irwin–Hall)
  float s = 0; for (int i = 0; i < 12; i++) s += urand();
  return s - 6.0f;
}

void setUp() { g_seed = 12345; }
void tearDown() {}

// =====================================================================
//  sensor_math
// =====================================================================
void test_ntc_25c() {
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 25.0f, sensor::ntcCelsius(1650.0f, 3300.0f));
}
void test_ntc_35c() {
  // Rntc(35°C, B3950) = 6505 Ω -> V = 3300*10k/16505 = 1999.4 mV
  TEST_ASSERT_FLOAT_WITHIN(0.1f, 35.0f, sensor::ntcCelsius(1999.4f, 3300.0f));
}
void test_ntc_fault() {
  TEST_ASSERT_TRUE(isnan(sensor::ntcCelsius(10.0f, 3300.0f)));
  TEST_ASSERT_TRUE(isnan(sensor::ntcCelsius(3290.0f, 3300.0f)));
}
void test_gsr_2us() {
  // Rskin = 500k -> V = 3300*100k/700k = 471.43 mV -> 2.0 µS
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 2.0f, sensor::gsrMicroSiemens(471.43f, 3300.0f));
  TEST_ASSERT_EQUAL_FLOAT(0.0f, sensor::gsrMicroSiemens(5.0f, 3300.0f));
  TEST_ASSERT_TRUE(isnan(sensor::gsrMicroSiemens(2000.0f, 3300.0f)));
}
void test_battery_percent() {
  TEST_ASSERT_EQUAL_UINT8(100, sensor::batteryPercent(4250));
  TEST_ASSERT_EQUAL_UINT8(30, sensor::batteryPercent(3700));
  TEST_ASSERT_EQUAL_UINT8(40, sensor::batteryPercent(3750));
  TEST_ASSERT_EQUAL_UINT8(0, sensor::batteryPercent(3100));
}

// =====================================================================
//  PPG: สร้างคลื่นชีพจรจำลอง (ยอดหลัก + คลื่น dicrotic + การหายใจ + noise)
// =====================================================================
struct PpgSimResult { float hr; float hrv; uint32_t beats; uint32_t trueBeats; };

static PpgSimResult runPpg(float bpm, float seconds, float noise, float hrvMs) {
  dsp::PpgProcessor p;
  p.begin(100.0f, 50000);
  const float fs = 100.0f, dc = 120000.0f, ac = 1200.0f;
  float nextBeat = 0.5f, lastBeat = -10.0f, ibi = 60.0f / bpm;
  uint32_t trueBeats = 0, beatsAfterSettle = 0;
  const int N = (int)(seconds * fs);
  for (int n = 0; n < N; n++) {
    float t = n / fs;
    if (t >= nextBeat) {
      lastBeat = nextBeat;
      ibi = 60.0f / bpm + (hrvMs / 1000.0f) * nrand() * 0.7f;
      nextBeat += ibi;
      if (t > 3.0f) trueBeats++;
    }
    float tb = t - lastBeat;
    // ยอดหลัก (systolic) + คลื่น dicrotic เล็กกว่า ตามหลัง ~0.3 s
    float pulse = expf(-powf((tb - 0.12f) / 0.06f, 2)) + 0.4f * expf(-powf((tb - 0.38f) / 0.08f, 2));
    float resp = 0.5f * ac * sinf(2 * 3.14159f * 0.25f * t);   // การหายใจ 15 ครั้ง/นาที
    float ir = dc - ac * pulse + resp + noise * ac * nrand();
    bool beat = p.push((uint32_t)ir);
    if (beat && t > 3.0f) beatsAfterSettle++;
  }
  PpgSimResult r = {p.hr(), p.hrv(), beatsAfterSettle, trueBeats};
  return r;
}

void test_ppg_72bpm() {
  PpgSimResult r = runPpg(72, 40, 0.03f, 30);
  printf("  72bpm: hr=%.1f hrv=%.1f beats=%u true=%u\n", r.hr, r.hrv, r.beats, r.trueBeats);
  TEST_ASSERT_FLOAT_WITHIN(3.0f, 72.0f, r.hr);
  TEST_ASSERT_INT_WITHIN(2, (int)r.trueBeats, (int)r.beats);
  TEST_ASSERT_TRUE(r.hrv > 5.0f && r.hrv < 120.0f);
}
void test_ppg_50bpm() {
  PpgSimResult r = runPpg(50, 40, 0.03f, 20);
  printf("  50bpm: hr=%.1f beats=%u true=%u\n", r.hr, r.beats, r.trueBeats);
  TEST_ASSERT_FLOAT_WITHIN(3.0f, 50.0f, r.hr);
  TEST_ASSERT_INT_WITHIN(2, (int)r.trueBeats, (int)r.beats);   // ต้องไม่นับ dicrotic ซ้ำ
}
void test_ppg_120bpm() {
  PpgSimResult r = runPpg(120, 40, 0.03f, 15);
  printf("  120bpm: hr=%.1f beats=%u true=%u\n", r.hr, r.beats, r.trueBeats);
  TEST_ASSERT_FLOAT_WITHIN(4.0f, 120.0f, r.hr);
  TEST_ASSERT_INT_WITHIN(3, (int)r.trueBeats, (int)r.beats);
}
void test_ppg_no_contact() {
  dsp::PpgProcessor p;
  p.begin(100.0f, 50000);
  for (int n = 0; n < 1000; n++) p.push(3000 + (n % 7));
  TEST_ASSERT_FALSE(p.contact());
  TEST_ASSERT_EQUAL_FLOAT(0.0f, p.hr());
}
void test_ppg_contact_release() {
  dsp::PpgProcessor p;
  p.begin(100.0f, 50000);
  for (int n = 0; n < 300; n++) p.push(120000);
  TEST_ASSERT_TRUE(p.contact());
  for (int n = 0; n < 50; n++) p.push(1000);
  TEST_ASSERT_FALSE(p.contact());
}

// =====================================================================
//  Motion
// =====================================================================
void test_tremor_8hz() {
  dsp::MotionProcessor m;
  m.begin(100.0f);
  for (int n = 0; n < 600; n++) {
    float t = n / 100.0f;
    m.push(0.0f, 0.0f, 9.81f + 0.5f * sinf(2 * 3.14159f * 8.0f * t));
  }
  printf("  tremor=%.3f motion=%.3f\n", m.tremor(), m.motion());
  TEST_ASSERT_FLOAT_WITHIN(0.06f, 0.354f, m.tremor());   // RMS ของไซน์ = A/√2
  TEST_ASSERT_TRUE(m.motion() < 0.1f);
}
void test_motion_1hz() {
  dsp::MotionProcessor m;
  m.begin(100.0f);
  for (int n = 0; n < 800; n++) {
    float t = n / 100.0f;
    m.push(0.0f, 0.0f, 9.81f + 1.0f * sinf(2 * 3.14159f * 1.0f * t));
  }
  printf("  motion=%.3f tremor=%.3f\n", m.motion(), m.tremor());
  TEST_ASSERT_FLOAT_WITHIN(0.12f, 0.707f, m.motion());
  TEST_ASSERT_TRUE(m.tremor() < 0.15f);
}

// =====================================================================
//  EDA
// =====================================================================
void test_eda_scr_count() {
  dsp::EdaProcessor e;
  e.begin(10.0f);
  for (int n = 0; n < 900; n++) {               // 90 s
    float t = n / 10.0f;
    float v = 2.0f + 0.003f * nrand();
    // SCR 3 ครั้งที่ 40, 55, 70 s: ขึ้น 0.3 µS ใน ~2 s แล้วค่อย ๆ ลง
    const float at[3] = {40, 55, 70};
    for (int k = 0; k < 3; k++) {
      float d = t - at[k];
      if (d > 0) v += 0.3f * (1 - expf(-d / 0.8f)) * expf(-d / 5.0f);
    }
    e.push(v, true);
  }
  printf("  scr/min=%u total=%u phasic=%.3f\n", e.scrPerMin(), e.scrTotal(), e.phasic());
  TEST_ASSERT_EQUAL_UINT16(3, e.scrPerMin());
}
void test_eda_no_contact() {
  dsp::EdaProcessor e;
  e.begin(10.0f);
  for (int n = 0; n < 50; n++) e.push(0, false);
  TEST_ASSERT_FALSE(e.contact());
  TEST_ASSERT_EQUAL_FLOAT(0.0f, e.value());
}

// =====================================================================
//  LieEngine — จำลองการสอบสวนทั้งชุด
// =====================================================================
struct Subject {
  float t = 0;
  // การตอบสนองที่กำลังเกิด (0 = ไม่มี)
  float respStart = -1, respGain = 0;
  bool gsrOff = false;
  bool ppgOff = false;
  float motion = 0.01f;
};

static lie::Frame makeFrame(Subject& s) {
  lie::Frame f;
  float g = 2.0f + 0.002f * s.t + 0.008f * nrand();
  float hr = 72 + 3.0f * sinf(2 * 3.14159f * 0.25f * s.t) + 0.8f * nrand();
  float amp = 1000 * (1 + 0.02f * nrand());
  float trm = 0.04f + 0.004f * nrand();
  float tmp = 33.0f + 0.02f * nrand();
  if (s.respStart >= 0) {
    float d = s.t - s.respStart;
    if (d > 1.0f) {                          // แฝง ~1 s แล้วค่อยตอบสนอง
      float x = d - 1.0f;
      float shape = (1 - expf(-x / 1.0f)) * expf(-x / 8.0f);
      g += s.respGain * 0.5f * shape;        // µS
      hr += s.respGain * 10.0f * shape;      // bpm
      amp *= 1 - s.respGain * 0.3f * shape;  // หลอดเลือดหดตัว
      trm += s.respGain * 0.03f * shape;
      tmp -= s.respGain * 0.05f * shape;
    }
  }
  f.gsr = s.gsrOff ? 0.0f : g; f.gsrOk = !s.gsrOff;
  f.hr = s.ppgOff ? 0.0f : hr; f.ppgOk = !s.ppgOff; f.amp = s.ppgOff ? 0.0f : amp;
  f.tremor = trm; f.motion = s.motion; f.temp = tmp;
  s.t += 0.2f;
  return f;
}

static void feed(lie::Engine& e, Subject& s, float seconds) {
  int n = (int)(seconds * 5 + 0.5f);
  for (int i = 0; i < n; i++) e.push(makeFrame(s));
}

static lie::Result ask(lie::Engine& e, Subject& s, uint16_t qid, lie::Kind kind, float gain) {
  feed(e, s, 10);                         // พักให้นิ่งก่อนถาม
  TEST_ASSERT_TRUE(e.startQuestion(qid, kind));
  s.respStart = s.t; s.respGain = gain;
  feed(e, s, 12.2f);
  s.respStart = -1; s.respGain = 0;
  lie::Result r;
  TEST_ASSERT_TRUE(e.result(0, r));
  TEST_ASSERT_EQUAL_UINT16(qid, r.qid);
  printf("  q%u kind=%s gain=%.2f -> %s p=%.2f S=%.2f zG=%.1f zH=%.1f zA=%.1f q=%u\n", qid,
         lie::Engine::kindName(kind), gain, lie::Engine::verdictName(r.verdict), r.pLie, r.score,
         r.z[lie::F_GSR], r.z[lie::F_HR], r.z[lie::F_AMP], r.quality);
  return r;
}

void test_lie_requires_baseline() {
  lie::Engine e;
  TEST_ASSERT_FALSE(e.startQuestion(1, lie::Kind::Test));
  TEST_ASSERT_EQUAL_STRING("NO_BASELINE", e.lastError());
}

void test_lie_full_session() {
  lie::Engine e;
  Subject s;
  feed(e, s, 5);
  TEST_ASSERT_TRUE(e.startBaseline(30));
  feed(e, s, 30.2f);
  TEST_ASSERT_TRUE(e.baseline().valid);
  TEST_ASSERT_TRUE(e.state() == lie::State::Ready);
  TEST_ASSERT_TRUE(e.baseline().nullN >= 10);
  printf("  baseline: gsr=%.3f±%.3f hr=%.1f±%.1f nullN=%u nullSdG=%.3f nullSdH=%.2f\n",
         e.baseline().mean[0], e.baseline().sd[0], e.baseline().mean[1], e.baseline().sd[1],
         e.baseline().nullN, e.baseline().nullSd[0], e.baseline().nullSd[1]);

  int si = e.status().stress;
  printf("  stress at rest=%d\n", si);
  TEST_ASSERT_TRUE(si >= 5 && si <= 45);

  lie::Result r1 = ask(e, s, 1, lie::Kind::ControlTruth, 0.05f);
  TEST_ASSERT_TRUE(r1.verdict == lie::Verdict::Truth);
  TEST_ASSERT_TRUE(r1.correct);
  lie::Result r2 = ask(e, s, 2, lie::Kind::ControlLie, 1.0f);
  TEST_ASSERT_TRUE(r2.verdict == lie::Verdict::Lie);
  TEST_ASSERT_TRUE(e.calibration().active);
  TEST_ASSERT_EQUAL_UINT8(2, e.calibration().correct);
  printf("  calib: s0=%.2f k=%.2f sep=%.2f w=[%.2f %.2f %.2f %.2f %.2f]\n", e.calibration().s0,
         e.calibration().k, e.calibration().separation, e.calibration().weight[0],
         e.calibration().weight[1], e.calibration().weight[2], e.calibration().weight[3],
         e.calibration().weight[4]);

  lie::Result r3 = ask(e, s, 3, lie::Kind::Test, 0.8f);
  TEST_ASSERT_TRUE(r3.verdict == lie::Verdict::Lie);
  lie::Result r4 = ask(e, s, 4, lie::Kind::Test, 0.0f);
  TEST_ASSERT_TRUE(r4.verdict == lie::Verdict::Truth);

  // แผ่น GSR หลุดระหว่างถาม -> v2.1: ยังตัดสินได้จากชีพจร (แค่เตือน R_GSR_CONTACT)
  feed(e, s, 10);
  TEST_ASSERT_TRUE(e.startQuestion(5, lie::Kind::Test));
  s.gsrOff = true;
  feed(e, s, 12.2f);
  s.gsrOff = false;
  lie::Result r5;
  TEST_ASSERT_TRUE(e.result(0, r5));
  TEST_ASSERT_TRUE(r5.verdict != lie::Verdict::Invalid);
  TEST_ASSERT_TRUE(r5.reasons & lie::R_GSR_CONTACT);

  // ขยับตัวแรง -> ใช้ไม่ได้
  feed(e, s, 10);
  TEST_ASSERT_TRUE(e.startQuestion(6, lie::Kind::Test));
  s.motion = 3.0f;
  feed(e, s, 12.2f);
  s.motion = 0.01f;
  lie::Result r6;
  TEST_ASSERT_TRUE(e.result(0, r6));
  TEST_ASSERT_TRUE(r6.verdict == lie::Verdict::Invalid);
  TEST_ASSERT_TRUE(r6.reasons & lie::R_MOTION);

  // หลุดทั้ง GSR และชีพจร -> ตัดสินไม่ได้
  feed(e, s, 10);
  TEST_ASSERT_TRUE(e.startQuestion(7, lie::Kind::Test));
  s.gsrOff = true; s.ppgOff = true;
  feed(e, s, 12.2f);
  s.gsrOff = false; s.ppgOff = false;
  lie::Result r7;
  TEST_ASSERT_TRUE(e.result(0, r7));
  TEST_ASSERT_TRUE(r7.verdict == lie::Verdict::Invalid);

  TEST_ASSERT_EQUAL_UINT32(7, e.resultCount());
  feed(e, s, 14);
  TEST_ASSERT_TRUE(e.status().settled);
}

// ผิวแห้ง: GSR ไม่มีสัญญาณตลอดการทดสอบ -> ต้องยังทำงานได้จากชีพจร/หลอดเลือด
void test_lie_works_without_gsr() {
  lie::Engine e;
  Subject s;
  s.gsrOff = true;
  feed(e, s, 5);
  TEST_ASSERT_TRUE(e.startBaseline(30));
  feed(e, s, 30.2f);
  TEST_ASSERT_TRUE(e.baseline().valid);
  lie::Result r1 = ask(e, s, 1, lie::Kind::ControlTruth, 0.05f);
  TEST_ASSERT_TRUE(r1.verdict == lie::Verdict::Truth);
  lie::Result r2 = ask(e, s, 2, lie::Kind::ControlLie, 1.0f);
  TEST_ASSERT_TRUE(r2.verdict == lie::Verdict::Lie);
  lie::Result r3 = ask(e, s, 3, lie::Kind::Test, 0.8f);
  TEST_ASSERT_TRUE(r3.verdict == lie::Verdict::Lie);
  lie::Result r4 = ask(e, s, 4, lie::Kind::Test, 0.0f);
  TEST_ASSERT_TRUE(r4.verdict == lie::Verdict::Truth);
  TEST_ASSERT_TRUE(r4.reasons & lie::R_GSR_CONTACT);
  // v2.0 บั๊ก: ไม่มี GSR -> settled = false ตลอด (หน้าเว็บรอ "สัญญาณนิ่ง" ไม่สิ้นสุด)
  feed(e, s, 14);
  TEST_ASSERT_TRUE(e.status().settled);
  // ชีพจรยังไต่ขึ้น 1 bpm/s (ร่างกายยังไม่กลับสู่ปกติ) -> ต้อง "ยังไม่นิ่ง"
  for (int i = 0; i < 50; i++) {
    lie::Frame f = makeFrame(s);
    f.hr += 0.2f * i;
    e.push(f);
  }
  TEST_ASSERT_FALSE(e.status().settled);
}

void test_lie_stress_rises() {
  lie::Engine e;
  Subject s;
  feed(e, s, 4);
  e.startBaseline(30);
  feed(e, s, 30.2f);
  e.startQuestion(1, lie::Kind::Test);
  s.respStart = s.t; s.respGain = 1.0f;
  feed(e, s, 5);
  int si = e.status().stress;
  printf("  stress during response=%d\n", si);
  TEST_ASSERT_TRUE(si > 60);
}

void test_lie_baseline_without_any_signal_fails() {
  lie::Engine e;
  Subject s;
  s.gsrOff = true;
  s.ppgOff = true;
  e.startBaseline(30);
  feed(e, s, 30.2f);
  TEST_ASSERT_FALSE(e.baseline().valid);
  TEST_ASSERT_TRUE(e.state() == lie::State::Idle);
  TEST_ASSERT_EQUAL_STRING("BASELINE_NO_SIGNAL", e.lastError());
}

// =====================================================================
//  ML model (โมเดล AI บนนาฬิกา)
// =====================================================================
void test_ml_parse() {
  float v[8];
  TEST_ASSERT_EQUAL_INT(3, ml::parseFloats("0.5,-1.25, 3", v, 8));
  TEST_ASSERT_FLOAT_WITHIN(1e-6, -1.25f, v[1]);
  TEST_ASSERT_EQUAL_INT(-1, ml::parseFloats("1,abc", v, 8));
  uint8_t id[8];
  TEST_ASSERT_EQUAL_INT(3, ml::parseFeatureList("z_hr,d_amp,ppg_ok", id, 8));
  TEST_ASSERT_EQUAL_INT(ml::Z_HR, id[0]);
  TEST_ASSERT_EQUAL_INT(ml::D_AMP, id[1]);
  TEST_ASSERT_EQUAL_INT(ml::PPG_OK, id[2]);
  TEST_ASSERT_EQUAL_INT(-1, ml::parseFeatureList("z_hr,bogus", id, 8));
}

static ml::Model makeModel() {
  ml::Model m;
  ml::clear(m);
  m.n = 2;
  m.idx[0] = ml::Z_HR; m.idx[1] = ml::D_HR;
  m.mean[0] = 0; m.mean[1] = 0; m.scale[0] = 1; m.scale[1] = 2;
  m.w[0] = 1.0f; m.w[1] = 0.5f; m.b = -1.0f;
  ml::seal(m);
  return m;
}

void test_ml_predict() {
  ml::Model m = makeModel();
  TEST_ASSERT_TRUE(ml::valid(m));
  lie::Result r;
  for (int i = 0; i < lie::F_COUNT; i++) { r.z[i] = 0; r.feat[i] = 0; }
  r.okMask = 1u << lie::F_HR;
  r.z[lie::F_HR] = 2.0f;
  r.feat[lie::F_HR] = 4.0f;
  // z = -1 + 1*2 + 0.5*(4/2) = 2 -> sigmoid(2) = 0.8808
  TEST_ASSERT_FLOAT_WITHIN(1e-4, 0.8808f, ml::predict(m, r));
  m.w[0] = 9.0f;                              // แก้ค่าโดยไม่ seal -> CRC ไม่ตรง
  TEST_ASSERT_FALSE(ml::valid(m));
}

static bool fixedScorer(const lie::Result&, float& p, void*) { p = 0.9f; return true; }

void test_ml_scorer_hook() {
  lie::Engine e;
  Subject s;
  e.setScorer(fixedScorer, nullptr);
  e.startBaseline(30);
  feed(e, s, 30.2f);
  lie::Result r = ask(e, s, 1, lie::Kind::Test, 0.0f);
  TEST_ASSERT_EQUAL_UINT8(2, r.source);
  TEST_ASSERT_FLOAT_WITHIN(1e-6, 0.9f, r.pLie);
  TEST_ASSERT_TRUE(r.verdict == lie::Verdict::Lie);
}

void test_lie_reset_session() {
  lie::Engine e;
  Subject s;
  e.startBaseline(30);
  feed(e, s, 30.2f);
  ask(e, s, 1, lie::Kind::Test, 0.0f);
  TEST_ASSERT_EQUAL_UINT32(1, e.resultCount());
  e.resetSession();
  TEST_ASSERT_EQUAL_UINT32(0, e.resultCount());
  TEST_ASSERT_FALSE(e.baseline().valid);
  lie::Result r;
  TEST_ASSERT_FALSE(e.result(0, r));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_ntc_25c);
  RUN_TEST(test_ntc_35c);
  RUN_TEST(test_ntc_fault);
  RUN_TEST(test_gsr_2us);
  RUN_TEST(test_battery_percent);
  RUN_TEST(test_ppg_72bpm);
  RUN_TEST(test_ppg_50bpm);
  RUN_TEST(test_ppg_120bpm);
  RUN_TEST(test_ppg_no_contact);
  RUN_TEST(test_ppg_contact_release);
  RUN_TEST(test_tremor_8hz);
  RUN_TEST(test_motion_1hz);
  RUN_TEST(test_eda_scr_count);
  RUN_TEST(test_eda_no_contact);
  RUN_TEST(test_lie_requires_baseline);
  RUN_TEST(test_lie_full_session);
  RUN_TEST(test_lie_stress_rises);
  RUN_TEST(test_lie_works_without_gsr);
  RUN_TEST(test_lie_baseline_without_any_signal_fails);
  RUN_TEST(test_ml_parse);
  RUN_TEST(test_ml_predict);
  RUN_TEST(test_ml_scorer_hook);
  RUN_TEST(test_lie_reset_session);
  return UNITY_END();
}
