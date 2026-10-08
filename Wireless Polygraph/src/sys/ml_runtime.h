// =====================================================================
//  ml_runtime.h — ตัวกลางระหว่างโมเดล AI กับ LieEngine บนนาฬิกา
//
//  - ตอนบูต: โหลดโมเดลจาก NVS (ถ้าเคยอัปโหลด) แล้วผูกเป็น "scorer" ของ LieEngine
//  - มีโมเดล  -> P(โกหก) มาจากโมเดล AI (source = 2)
//  - ไม่มีโมเดล -> ใช้สูตรมาตรฐาน/calibration ของ LieEngine เหมือนเดิม
//  - โหมด TRAIN: ทุกคำถามที่ติดป้าย "จริง/โกหก" จะถูกบันทึกเป็นแถวข้อมูลเทรนใน /train.csv
// =====================================================================
#pragma once
#include <Arduino.h>
#include "../lie/ml_model.h"

class Json;

namespace mlrt {

enum Mode : uint8_t { MODE_DETECT = 0, MODE_TRAIN = 1 };

void begin();                                  // เรียกหลัง storage::begin()
uint8_t mode();
void setMode(uint8_t m);                       // บันทึกลง NVS ด้วย
const char* modeName(uint8_t m);               // "detect" / "train"

bool hasModel();
bool install(const ml::Model& m);              // ตรวจ + บันทึก NVS + ใช้ทันที
void clearModel();
ml::Model modelCopy();

void setSubject(const char* name);             // ชื่อผู้ถูกทดสอบ (ใส่ในแถวข้อมูลเทรน)
const char* subject();

// สร้างแถว CSV ของข้อมูลเทรนจากผลคำถาม (คืน false ถ้าข้อนี้ไม่ควรเก็บ)
bool makeTrainRow(const lie::Result& r, char* out, size_t n, int& label);
// สร้างแถว CSV โดย "ระบุเฉลยเอง" (label 0 = จริง, 1 = โกหก) — ใช้กับโหมดใช้งานจริงบนมือถือ
// ที่ผู้ใช้บอกหลังได้ผลว่านาฬิกาตอบถูก/ผิด (POST /api/ml/feedback)
bool makeTrainRowAs(const lie::Result& r, int label, char* out, size_t n);

void appendJson(Json& j);                      // สำหรับ GET /api/ml

}  // namespace mlrt
