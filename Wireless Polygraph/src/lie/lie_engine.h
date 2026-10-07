// =====================================================================
//  lie_engine.h — "สมอง" ของเครื่องจับเท็จ (รันบนนาฬิกาเอง = edge computing)
//
//  ใช้หลักการแบบ polygraph (Control Question Technique แบบย่อ):
//   1) BASELINE  : ให้ผู้ถูกทดสอบนั่งนิ่ง ~30 s เก็บค่าปกติของแต่ละคน
//   2) CONTROL   : ถามคำถามที่รู้คำตอบอยู่แล้ว (ตอบจริง 1 ข้อ / สั่งให้โกหก 1 ข้อ)
//                  -> ระบบเรียนรู้ว่า "คนนี้ตอนโกหก ร่างกายตอบสนองแรงแค่ไหน"
//   3) TEST      : ถามคำถามจริง -> เทียบการตอบสนองกับข้อ 1 และ 2 -> ตัดสิน
//
//  ต่อคำถาม 1 ข้อ ระบบดู 5 สัญญาณใน 12 วินาทีหลังเริ่มถาม เทียบกับ 3 วินาทีก่อนถาม:
//   F_GSR : เหงื่อ/ความนำไฟฟ้าผิวพุ่งขึ้นเท่าไร (µS)   <- ไม่ใช่ตัวหลักแล้ว (v2.1)
//           ทดสอบจริงพบว่าคนส่วนใหญ่ไม่ได้เหงื่อออกตลอด GSR จึงแบนเกือบทั้งช่วง
//           -> ลดน้ำหนักลง และ "ไม่มี GSR" ไม่ทำให้ผลใช้ไม่ได้อีกต่อไป
//   F_HR  : ชีพจรเร่งขึ้นสูงสุดกี่ bpm                   <- น้ำหนักมากสุด
//   F_AMP : แอมพลิจูดชีพจรลดลงกี่ % (หลอดเลือดปลายมือหดตัวตอนเครียด)
//   F_TRM : มือสั่นเพิ่มขึ้นเท่าไร (m/s²)
//   F_TMP : ผิวเย็นลงกี่ °C
//  แต่ละค่าแปลงเป็น z-score เทียบกับ "การแกว่งตามธรรมชาติตอนไม่ได้ถูกถาม"
//  (null distribution: เอาสูตรเดียวกันไปคิดบนหน้าต่างย่อยของช่วง baseline)
//  -> ถ่วงน้ำหนักรวมเป็นคะแนน S -> logistic -> P(โกหก) -> ตัดสิน
//
//  โหมด AI (v2.1): ถ้ามีโมเดลที่เทรนจากข้อมูลจริง (lie/ml_model.h) จะเรียกผ่าน
//  "scorer hook" ให้โมเดลเป็นคนคำนวณ P(โกหก) แทนสูตรถ่วงน้ำหนักด้านบน
//
//  ไม่พึ่ง Arduino -> unit test บนคอมได้ และ Polygraph Studio มีตัวเดียวกันที่พอร์ตเป็น
//  Python ไว้ใน "นาฬิกาจำลอง" (ทดสอบแล้วว่าให้ผลตรงกัน)
//
//  ข้อจำกัด (สำคัญ ต้องบอกผู้ใช้): การตอบสนองทางสรีระบอก "ความตื่นตัว/ความเครียด"
//  ไม่ได้บอก "การโกหก" โดยตรง คนพูดจริงแต่ตื่นเต้นก็อาจถูกตัดสินว่าโกหกได้
//  ผลจากเครื่องนี้จึงเป็นเพื่อการศึกษา/สาธิตเท่านั้น
// =====================================================================
#pragma once
#include <stdint.h>
#include <math.h>

namespace lie {

enum class State : uint8_t { Idle = 0, Baseline = 1, Ready = 2, Question = 3 };
enum class Kind : uint8_t { Test = 0, ControlTruth = 1, ControlLie = 2, Warmup = 3 };
enum class Verdict : uint8_t { None = 0, Truth = 1, Lie = 2, Inconclusive = 3, Invalid = 4 };

enum Feature : uint8_t { F_GSR = 0, F_HR = 1, F_AMP = 2, F_TRM = 3, F_TMP = 4, F_COUNT = 5 };

// เหตุผลที่ผลเชื่อถือได้น้อย/ไม่ได้ (bitmask)
enum Reason : uint16_t {
  R_NONE        = 0,
  R_GSR_CONTACT = 1,   // แผ่น GSR ไม่มีสัญญาณ -> ใช้สัญญาณที่เหลือ (เตือนเฉย ๆ)
  R_PPG_CONTACT = 2,   // เซนเซอร์ชีพจรหลุด -> ใช้สัญญาณที่เหลือ (ความมั่นใจลดลง)
                       // ถ้าหลุดทั้ง GSR และชีพจร -> ตัดสินไม่ได้
  R_MOTION      = 4,   // ขยับตัวมาก -> สัญญาณปนเปื้อน ตัดสินไม่ได้
  R_SHORT_PRE   = 8,   // ข้อมูลก่อนถามไม่พอ
};

// ข้อมูล 1 frame (5 ครั้ง/วินาที) ที่ป้อนเข้า engine
struct Frame {
  float gsr = 0.0f;       // µS
  float hr = 0.0f;        // bpm
  float amp = 0.0f;       // แอมพลิจูดชีพจร (count)
  float tremor = 0.0f;    // m/s²
  float motion = 0.0f;    // m/s²
  float temp = NAN;       // °C (NAN = เสีย)
  bool  gsrOk = false;    // แผ่น GSR แตะผิว
  bool  ppgOk = false;    // เซนเซอร์ชีพจรแตะผิวและมีค่า HR
};

struct Config {
  float frameHz     = 5.0f;
  float baselineSec = 30.0f;   // 20..90
  float preSec      = 3.0f;    // 1..5
  float windowSec   = 12.0f;   // 6..20
  float recoverySec = 8.0f;    // หลังจบคำถาม ต้องรออย่างน้อยเท่านี้ก่อนบอกว่า "พร้อม"
  float weight[F_COUNT]  = {0.20f, 0.35f, 0.25f, 0.12f, 0.08f};   // GSR, HR, AMP, TRM, TMP
  float sdFloor[F_COUNT] = {0.05f, 1.5f, 0.04f, 0.02f, 0.03f};  // กันหารด้วยค่าเล็กเกินจริง
  float s0     = 2.0f;         // คะแนนที่ P(โกหก) = 50% (ก่อน calibrate)
  float k      = 1.5f;         // ความชันของ logistic
  float lieP   = 0.65f;        // P >= นี้ -> โกหก
  float truthP = 0.35f;        // P <= นี้ -> จริง (ระหว่างกลาง = ไม่แน่ชัด)
  float motionLimit   = 1.5f;  // m/s² ขยับแรงกว่านี้ = frame ปนเปื้อน
  float minGsrFrac    = 0.8f;
  float minPpgFrac    = 0.6f;
  float maxMotionFrac = 0.3f;
};

struct Baseline {
  bool  valid = false;
  float mean[F_COUNT];      // ค่าเฉลี่ยระดับ frame (gsr, hr, amp, tremor, temp)
  float sd[F_COUNT];
  float nullMean[F_COUNT];  // การกระจายของ "feature" ตอนไม่มีคำถาม
  float nullSd[F_COUNT];
  uint16_t nullN = 0;       // จำนวนหน้าต่างที่ใช้คิด null
  float durationSec = 0.0f;
  float gsrFrac = 0.0f, ppgFrac = 0.0f;
};

struct Calibration {
  uint8_t nTruth = 0, nLie = 0;
  bool  active = false;     // ใช้ค่าจาก calibration อยู่
  bool  weak = false;       // มีข้อควบคุมครบแต่แยกจริง/โกหกไม่ออก
  float meanTruth = 0.0f, meanLie = 0.0f, separation = 0.0f;
  float s0 = 0.0f, k = 0.0f;
  float weight[F_COUNT];
  uint8_t correct = 0, scored = 0;   // ข้อควบคุมที่ระบบตอบถูก / ทั้งหมด
};

struct Result {
  uint32_t seq = 0;         // ลำดับผลตั้งแต่เริ่ม (เพิ่มขึ้นเรื่อย ๆ)
  uint16_t qid = 0;
  Kind     kind = Kind::Test;
  Verdict  verdict = Verdict::None;
  Verdict  expected = Verdict::None;   // ข้อควบคุมเท่านั้น
  bool     correct = false;
  uint8_t  quality = 0;     // 0..100
  uint16_t reasons = 0;
  float pLie = 0.0f;        // 0..1
  uint8_t source = 0;       // ใครคำนวณ P: 0 = สูตรมาตรฐาน, 1 = สูตร+calibration, 2 = โมเดล AI
  float score = 0.0f;
  float feat[F_COUNT];      // ค่าจริงที่เปลี่ยน (หน่วยตามแต่ละ feature)
  float z[F_COUNT];
  float pre[F_COUNT];       // ค่าเฉลี่ยก่อนถาม
  uint8_t okMask = 0;       // bit i = feature i ใช้ได้
  float peakLatency = -1.0f;  // วินาทีที่ GSR สูงสุดหลังเริ่มถาม
  float answerAt = -1.0f;     // วินาทีที่กดบันทึกคำตอบ (-1 = ไม่ได้กด)
  int8_t answerYes = -1;      // 1 = ตอบใช่, 0 = ไม่ใช่, -1 = ไม่ได้บันทึก
  float tStart = 0.0f;        // เวลาเริ่ม (วินาทีของ engine)
};

struct Status {
  State    state;
  float    progress;      // 0..1 ของ baseline/คำถามปัจจุบัน
  float    elapsed;       // วินาทีในเฟสปัจจุบัน
  uint16_t qid;
  Kind     kind;
  bool     baselineValid;
  bool     calibrated;
  bool     calibWeak;
  int      stress;        // -1 = ยังไม่มี baseline, 0..100
  bool     settled;       // สัญญาณกลับมานิ่ง พร้อมถามข้อต่อไป
  uint32_t results;
  uint32_t revision;      // เปลี่ยนทุกครั้งที่มีอะไรใหม่ (UI ใช้รู้ว่าต้องโหลดข้อมูลใหม่)
};

// ฟังก์ชันภายนอก (เช่นโมเดล AI) ที่ขอคำนวณ P(โกหก) จากผลที่มี feature/z ครบแล้ว
// คืน true = ใช้ค่า pLie ที่ให้มา, false = ให้ engine ใช้สูตรของตัวเอง
typedef bool (*Scorer)(const struct Result& r, float& pLie, void* ctx);

class Engine {
 public:
  Engine();
  void setScorer(Scorer fn, void* ctx) { scorer_ = fn; scorerCtx_ = ctx; }
  void configure(const Config& c);           // ปรับค่าแล้วจำกัดให้อยู่ในช่วงปลอดภัย
  const Config& config() const { return cfg_; }

  bool startBaseline(float sec = 0.0f);      // 0 = ใช้ค่าใน config
  bool startQuestion(uint16_t qid, Kind kind);
  bool markAnswer(bool yes);
  void abort();                              // ยกเลิก baseline/คำถามที่กำลังทำ
  void resetSession();                       // ล้าง baseline + calibration + ผลทั้งหมด

  void push(const Frame& f);                 // เรียก 5 ครั้ง/วินาที

  Status status() const;
  State state() const { return state_; }
  const Baseline& baseline() const { return base_; }
  const Calibration& calibration() const { return cal_; }
  uint32_t resultCount() const { return resultSeq_ - firstSeq_; }   // ในเซสชันนี้
  uint32_t lastSeq() const { return resultSeq_; }
  bool result(uint32_t newestIndex, Result& out) const;   // 0 = ล่าสุด
  bool resultBySeq(uint32_t seq, Result& out) const;
  float time() const { return frames_ / cfg_.frameHz; }
  uint32_t revision() const { return revision_; }
  const char* lastError() const { return err_; }

  static const char* stateName(State s);
  static const char* kindName(Kind k);
  static const char* verdictName(Verdict v);
  static bool kindFromName(const char* s, Kind& out);

  static const int kMaxBase = 450;     // 90 s @5 Hz
  static const int kMaxWin  = 100;     // 20 s
  static const int kMaxPre  = 25;      // 5 s
  static const int kHist    = 64;      // ~12.8 s
  static const int kMaxRes  = 24;
  static const int kMaxCtl  = 12;

 private:
  struct Features {
    float f[F_COUNT];
    bool  ok[F_COUNT];
    float pre[F_COUNT];
    float gsrFrac, ppgFrac, motionFrac;
    float peakLatency;
    bool  preOk;
  };
  struct Control {
    bool  lie;
    uint8_t okMask;
    float z[F_COUNT];
  };

  void computeFeatures(const Frame* pre, int nPre, const Frame* win, int nWin, Features& o) const;
  void finishBaseline();
  void finishQuestion();
  float scoreOf(const float* z, uint8_t okMask, const float* w) const;
  void recalibrate();
  void updateStress(const Frame& f);
  void updateSettled();
  void bump() { revision_++; }
  int histCount() const { return histN_; }
  const Frame& hist(int back) const;   // 0 = ล่าสุด

  Config cfg_;
  State state_ = State::Idle;
  uint32_t frames_ = 0;
  uint32_t phaseStart_ = 0;           // frame ที่เริ่มเฟสปัจจุบัน
  int baseTarget_ = 0, winTarget_ = 0, preFrames_ = 0;

  Frame baseBuf_[kMaxBase];
  int baseN_ = 0;
  Frame winBuf_[kMaxWin];
  int winN_ = 0;
  Frame preBuf_[kMaxPre];
  int preN_ = 0;
  Frame hist_[kHist];
  int histHead_ = 0, histN_ = 0;

  uint16_t qid_ = 0;
  Kind kind_ = Kind::Test;
  float answerAt_ = -1.0f;
  int8_t answerYes_ = -1;

  Baseline base_;
  Calibration cal_;
  Control ctl_[kMaxCtl];
  int ctlN_ = 0;

  Result res_[kMaxRes];
  uint32_t resultSeq_ = 0;
  uint32_t firstSeq_ = 0;            // ผลที่ seq <= ค่านี้ถูกล้างไปแล้ว (resetSession)

  Scorer scorer_ = nullptr;
  void* scorerCtx_ = nullptr;
  float refGsr_ = 0.0f;
  float stress_ = -1.0f;
  bool settled_ = false;
  uint32_t lastWindowEnd_ = 0;
  uint32_t revision_ = 0;
  const char* err_ = "";
};

}  // namespace lie
