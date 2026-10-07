// =====================================================================
//  sensor_math.h — แปลงแรงดัน ADC -> หน่วยจริง (ไม่พึ่ง Arduino -> เทสบนคอมได้)
//
//  NTC  : 3V3 -[NTC]- NTC_SENSE -[R1 10k]- GND
//         V = Vcc * R1 / (R1 + Rntc)  ->  Rntc = R1 * (Vcc - V) / V
//         แล้วใช้สมการ Beta: 1/T = 1/T25 + ln(Rntc/R25)/Beta
//  GSR  : 3V3 -[R2 100k]- ผิว -[R3 100k]- GND, วัดที่บน R3
//         V = Vcc * R3 / (R2 + Rskin + R3) -> Rskin = Vcc*R3/V - R2 - R3
//         ความนำไฟฟ้า G = 1/Rskin (หน่วยมาตรฐานของงาน EDA คือ µS)
//  VBAT : แบ่งแรงดัน 2:1 -> คูณ 2 กลับ
// =====================================================================
#pragma once
#include <stdint.h>

namespace sensor {

// ต่ำกว่านี้ถือว่าแผ่น GSR ไม่ได้แตะผิว (≈ ผิวต้านทาน > 16 MΩ หรือ < 0.06 µS)
constexpr float GSR_OPEN_MV  = 20.0f;
// สูงกว่านี้ = แผ่นแตะกันเอง/ลัดวงจร (Rskin < ~6 kΩ ผิวคนจริงไม่ต่ำขนาดนี้)
constexpr float GSR_SHORT_MV = 1600.0f;

// คืนค่า °C หรือ NAN ถ้าสายขาด/ลัดวงจร
float ntcCelsius(float vMv, float vccMv, float r1 = 10000.0f, float r25 = 10000.0f,
                 float beta = 3950.0f);

// คืนค่า µS; 0 = ไม่ได้แตะ, NAN = ลัดวงจร
float gsrMicroSiemens(float vMv, float vccMv, float r2 = 100000.0f, float r3 = 100000.0f);

inline bool gsrContact(float vMv) { return vMv >= GSR_OPEN_MV && vMv <= GSR_SHORT_MV; }

// เปอร์เซ็นต์แบต Li-Po จากแรงดัน (เส้นโค้งทั่วไปขณะโหลดเบา — เป็นค่าประมาณ)
uint8_t batteryPercent(float vbatMv);

}  // namespace sensor
