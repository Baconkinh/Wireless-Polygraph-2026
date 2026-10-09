// =====================================================================
//  json_writer.h — ตัวช่วยสร้าง JSON แบบเบา ๆ (ไม่ต้องพึ่ง ArduinoJson)
//  - จัดการคอมม่า/escape string ให้อัตโนมัติ -> JSON ถูกรูปแบบเสมอ
//  - NaN/Inf กลายเป็น null (JSON มาตรฐานไม่มี NaN)
//  ตัวอย่าง:  Json j; j.obj().kv("hr", 72.5f, 1).arr("x").v(1).v(2).end().end();
// =====================================================================
#pragma once
#include <Arduino.h>
#include <math.h>

// [เทคนิค: Lightweight serializer] สร้าง JSON เองโดยไม่ใช้ไลบรารี: จัดคอมม่า/escape/NaN->null ให้ ลดขนาดเฟิร์มแวร์
class Json {
 public:
  explicit Json(size_t reserve = 1024) { s_.reserve(reserve); }

  Json& obj(const char* key = nullptr) { open(key, '{'); return *this; }
  Json& arr(const char* key = nullptr) { open(key, '['); return *this; }
  Json& end() {
    if (depth_ > 0) {
      s_ += closer_[depth_ - 1];
      depth_--;
    }
    return *this;
  }

  Json& kv(const char* key, const char* v) { k(key); str(v); return *this; }
  Json& kv(const char* key, const String& v) { return kv(key, v.c_str()); }
  Json& kv(const char* key, bool v) { k(key); s_ += v ? "true" : "false"; return *this; }
  Json& kv(const char* key, int v) { k(key); s_ += v; return *this; }
  Json& kv(const char* key, long v) { k(key); s_ += v; return *this; }
  Json& kv(const char* key, unsigned int v) { k(key); s_ += v; return *this; }
  Json& kv(const char* key, unsigned long v) { k(key); s_ += v; return *this; }
  Json& kv(const char* key, float v, int dec = 3) { k(key); num(v, dec); return *this; }
  Json& kv(const char* key, double v, int dec = 3) { k(key); num((float)v, dec); return *this; }
  Json& kvNull(const char* key) { k(key); s_ += "null"; return *this; }
  Json& kvRaw(const char* key, const char* raw) { k(key); s_ += raw; return *this; }

  Json& v(const char* x) { sep(); str(x); return *this; }
  Json& v(int x) { sep(); s_ += x; return *this; }
  Json& v(unsigned long x) { sep(); s_ += x; return *this; }
  Json& v(float x, int dec = 3) { sep(); num(x, dec); return *this; }

  const String& str() const { return s_; }

 private:
  void sep() {
    if (depth_ > 0) {
      if (!first_[depth_ - 1]) s_ += ',';
      first_[depth_ - 1] = false;
    }
  }
  void k(const char* key) {
    sep();
    str(key);
    s_ += ':';
  }
  void open(const char* key, char c) {
    if (key) k(key);
    else sep();
    s_ += c;
    if (depth_ < kMax) {
      closer_[depth_] = (c == '{') ? '}' : ']';
      first_[depth_] = true;
      depth_++;
    }
  }
  void num(float v, int dec) {
    if (isnan(v) || isinf(v)) { s_ += "null"; return; }
    char b[24];
    snprintf(b, sizeof(b), "%.*f", dec, (double)v);
    s_ += b;
  }
  void str(const char* x) {
    s_ += '"';
    for (const char* p = x ? x : ""; *p; p++) {
      const char c = *p;
      if (c == '"' || c == '\\') { s_ += '\\'; s_ += c; }
      else if ((uint8_t)c < 0x20) {
        char b[8];
        snprintf(b, sizeof(b), "\\u%04x", (uint8_t)c);
        s_ += b;
      } else {
        s_ += c;   // UTF-8 (ภาษาไทย) ผ่านได้ตรง ๆ
      }
    }
    s_ += '"';
  }

  static const int kMax = 10;
  String s_;
  char closer_[kMax];
  bool first_[kMax];
  int depth_ = 0;
};
