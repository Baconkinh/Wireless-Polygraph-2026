// =====================================================================
//  filters.h — ตัวกรองดิจิทัลพื้นฐาน (header-only, ไม่พึ่ง Arduino -> เทสบนคอมได้)
//
//  ESP32-C3 ไม่มี FPU: float ทำด้วยซอฟต์แวร์ (~50-100 cycle ต่อการคูณ)
//  ตัวกรองที่นี่ใช้ไม่กี่การคูณต่อ sample จึงรัน 100 Hz ได้สบาย (<1% CPU)
// =====================================================================
#pragma once
#include <math.h>

namespace dsp {

constexpr float kPi = 3.14159265358979f;

// ค่า alpha ของ EMA สำหรับ time-constant tau วินาที ที่อัตราสุ่ม fs
// ใช้ 1-exp(-dt/tau) แทน dt/tau เพราะยังถูกต้องแม้ tau จะสั้นใกล้ dt
inline float emaAlpha(float fs, float tauSec) {
  if (tauSec <= 0.0f) return 1.0f;
  return 1.0f - expf(-1.0f / (fs * tauSec));
}

// Exponential moving average (low-pass อันดับ 1)
class Ema {
 public:
  void setup(float fs, float tauSec) { a_ = emaAlpha(fs, tauSec); }
  void reset(float v) { y_ = v; init_ = true; }
  void clear() { init_ = false; y_ = 0.0f; }
  float push(float x) {
    if (!init_) { y_ = x; init_ = true; }   // ตัวอย่างแรก: เริ่มที่ค่าจริง ไม่ไต่จาก 0
    else        { y_ += a_ * (x - y_); }
    return y_;
  }
  float value() const { return y_; }
  bool ready() const { return init_; }
 private:
  float a_ = 1.0f;
  float y_ = 0.0f;
  bool init_ = false;
};

// Biquad (IIR อันดับ 2) แบบ Direct Form II Transposed
// สูตรสัมประสิทธิ์จาก "RBJ Audio EQ Cookbook" — Q = 0.7071 คือ Butterworth (ไม่มีริปเปิล)
class Biquad {
 public:
  void lowpass(float fs, float fc, float q = 0.70710678f) {
    float w0 = 2.0f * kPi * fc / fs, c = cosf(w0), al = sinf(w0) / (2.0f * q);
    set((1 - c) / 2, 1 - c, (1 - c) / 2, 1 + al, -2 * c, 1 - al);
  }
  void highpass(float fs, float fc, float q = 0.70710678f) {
    float w0 = 2.0f * kPi * fc / fs, c = cosf(w0), al = sinf(w0) / (2.0f * q);
    set((1 + c) / 2, -(1 + c), (1 + c) / 2, 1 + al, -2 * c, 1 - al);
  }
  // ตั้งสถานะภายในให้เหมือนรับค่าคงที่ x0 มานานแล้ว
  // เหตุผล: ถ้าเริ่มจาก 0 แต่สัญญาณจริงมี DC ~150000 (PPG) ตัวกรองจะกระชากแรง
  // และกว่าจะนิ่งก็หลายวินาที -> ตรวจจังหวะหัวใจผิดช่วงแรก
  void reset(float x0) {
    float den = 1.0f + a1_ + a2_;
    float g = (den != 0.0f) ? (b0_ + b1_ + b2_) / den : 0.0f;  // DC gain
    float y0 = g * x0;
    z1_ = y0 - b0_ * x0;
    z2_ = b2_ * x0 - a2_ * y0;
  }
  float push(float x) {
    float y = b0_ * x + z1_;
    z1_ = b1_ * x - a1_ * y + z2_;
    z2_ = b2_ * x - a2_ * y;
    return y;
  }
 private:
  void set(float b0, float b1, float b2, float a0, float a1, float a2) {
    b0_ = b0 / a0; b1_ = b1 / a0; b2_ = b2 / a0; a1_ = a1 / a0; a2_ = a2 / a0;
    z1_ = z2_ = 0.0f;
  }
  float b0_ = 1, b1_ = 0, b2_ = 0, a1_ = 0, a2_ = 0;
  float z1_ = 0, z2_ = 0;
};

// ค่ามัธยฐานของอาร์เรย์เล็ก (n <= 16) — insertion sort ในสำเนา ไม่แตะต้นฉบับ
inline float median(const float* v, int n) {
  if (n <= 0) return 0.0f;
  float t[16];
  if (n > 16) n = 16;
  for (int i = 0; i < n; i++) {
    float x = v[i];
    int j = i - 1;
    while (j >= 0 && t[j] > x) { t[j + 1] = t[j]; j--; }
    t[j + 1] = x;
  }
  return (n & 1) ? t[n / 2] : 0.5f * (t[n / 2 - 1] + t[n / 2]);
}

inline float clampf(float x, float lo, float hi) { return x < lo ? lo : (x > hi ? hi : x); }

// Welford: mean/variance แบบสะสมทีละค่า เสถียรเชิงตัวเลขกว่า sum/sum²
struct RunningStats {
  int n = 0;
  float mean = 0.0f, m2 = 0.0f;
  void clear() { n = 0; mean = 0.0f; m2 = 0.0f; }
  void add(float x) {
    n++;
    float d = x - mean;
    mean += d / n;
    m2 += d * (x - mean);
  }
  float var() const { return n > 1 ? m2 / (n - 1) : 0.0f; }
  float sd() const { return sqrtf(var()); }
};

}  // namespace dsp
