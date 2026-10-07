// sensor_math.cpp — ดูสมการใน sensor_math.h
#include "sensor_math.h"
#include <math.h>

namespace sensor {

float ntcCelsius(float vMv, float vccMv, float r1, float r25, float beta) {
  // ใกล้ 0 V = NTC ขาด/R1 ลัด, ใกล้ Vcc = NTC ลัด/R1 ขาด -> ไม่คำนวณ (จะได้ค่าหลอก)
  if (vMv < 50.0f || vMv > vccMv - 50.0f) return NAN;
  const float rNtc = r1 * (vccMv - vMv) / vMv;
  const float invT = 1.0f / 298.15f + logf(rNtc / r25) / beta;
  return 1.0f / invT - 273.15f;
}

float gsrMicroSiemens(float vMv, float vccMv, float r2, float r3) {
  if (vMv < GSR_OPEN_MV) return 0.0f;
  if (vMv > GSR_SHORT_MV) return NAN;
  const float rSkin = vccMv * r3 / vMv - r2 - r3;
  if (rSkin <= 0.0f) return NAN;
  return 1e6f / rSkin;
}

uint8_t batteryPercent(float v) {
  // (mV, %) — จุดอ้างอิงทั่วไปของ Li-Po 1 เซลล์ ไม่ใช่ค่าที่วัดจากแบตก้อนนี้
  static const float pts[][2] = {{4200, 100}, {4100, 90}, {4000, 80}, {3900, 65},
                                 {3800, 50},  {3700, 30}, {3600, 15}, {3500, 5},
                                 {3300, 0}};
  const int n = sizeof(pts) / sizeof(pts[0]);
  if (v >= pts[0][0]) return 100;
  for (int i = 1; i < n; i++) {
    if (v >= pts[i][0]) {
      const float v1 = pts[i - 1][0], p1 = pts[i - 1][1];
      const float v2 = pts[i][0], p2 = pts[i][1];
      return (uint8_t)lroundf(p2 + (p1 - p2) * (v - v2) / (v1 - v2));
    }
  }
  return 0;
}

}  // namespace sensor
