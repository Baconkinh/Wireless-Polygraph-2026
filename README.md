# Wireless-Polygraph-2026

เครื่องจับเท็จไร้สายแบบสวมข้อมือ — ESP32-C3 SuperMini + MAX30102 (ชีพจร) + MPU6050 (มือสั่น) + NTC (อุณหภูมิผิว) + แผ่น GSR
วัดสัญญาณร่างกาย 5 ชนิด ตัดสิน "จริง / โกหก" บนนาฬิกาเอง (LieEngine + โมเดล AI Logistic Regression) และส่งข้อมูลสดผ่าน WiFi

รายวิชา 03603323 Introduction to Embedded Systems · ภาควิชาวิศวกรรมคอมพิวเตอร์ คณะวิศวกรรมศาสตร์ ศรีราชา มหาวิทยาลัยเกษตรศาสตร์
ผู้จัดทำ: อัจฉรา ดังดี 6730300655 · ปภากร จันทร์ดี 6730300809

## เริ่มตรงไหน

| อยากรู้/ทำ | เปิดไฟล์ |
|---|---|
| ใช้งานทั้งระบบ (ติดตั้ง, อัปโหลดเฟิร์มแวร์, หน้าเว็บ, แก้ปัญหา) | [`คู่มือการใช้งาน_Wireless_Polygraph.md`](คู่มือการใช้งาน_Wireless_Polygraph.md) |
| เก็บข้อมูล → เทรน AI → ส่งเข้านาฬิกา (ทีละคลิก) + AI ทำงานยังไง | [`TRAINING_GUIDE.md`](TRAINING_GUIDE.md) |
| ทุกไฟล์/โฟลเดอร์ทำอะไร เชื่อมกันยังไง | [`PROJECT_MAP.md`](PROJECT_MAP.md) |
| ผังการทำงาน (บูต, RTOS, สัญญาณ, ตัดสิน, watchdog, flash map ...) | [`FLOWCHARTS.md`](FLOWCHARTS.md) |
| ความหมายทุกคอลัมน์ใน CSV / ฐานข้อมูล | [`Polygraph-Studio/DATA_DICTIONARY.md`](Polygraph-Studio/DATA_DICTIONARY.md) |
| สไลด์นำเสนอ | [`presentation/`](presentation/) |

## โครงสร้างย่อ

- `Wireless Polygraph/` — เฟิร์มแวร์ (PlatformIO, Arduino-ESP32, FreeRTOS 6 task, watchdog 3 ชั้น, OTA, NVS/LittleFS)
- `Polygraph-Studio/` — โปรแกรมบนคอม (Python FastAPI + หน้าเว็บ): ดูค่าสด, ทดสอบแบบเซสชัน, ใช้งานจริง, เก็บข้อมูล, เทรน AI
  - ดับเบิลคลิก `install.bat` (ครั้งแรก) → ต่อ WiFi `Polygraph-Watch` → `run_studio.bat`
  - ไม่มีบอร์ด: `run_demo_simulator.bat`
