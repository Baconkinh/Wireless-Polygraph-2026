# แผนที่โปรเจกต์ (PROJECT_MAP) — ทุก path ทำอะไร เพื่ออะไร เชื่อมกับอะไร ใช้ตอนไหน

อ่านคู่กับ: `TRAINING_GUIDE.md` (ขั้นตอนเทรน AI) · `FLOWCHARTS.md` (ผังการทำงาน 14 รูป) ·
`Polygraph-Studio/DATA_DICTIONARY.md` (ความหมายทุกคอลัมน์) · `คู่มือการใช้งาน_Wireless_Polygraph.md` (คู่มือผู้ใช้)

ระบบมี 2 ฝั่งที่คุยกันผ่าน WiFi ของนาฬิกา (`Polygraph-Watch`, 192.168.4.1):

```
┌──────────── นาฬิกา (ESP32-C3) ── Wireless Polygraph/ ────────────┐        ┌──── คอม ── Polygraph-Studio/ ────┐
│ เซนเซอร์ → DSP → LieEngine (+ โมเดล AI) → ผลจริง/โกหก              │  UDP   │ backend (Python FastAPI)          │
│ เว็บเซิร์ฟเวอร์ในตัว (หน้าเว็บ + REST API) · Serial console          │ ─────► │   ↕ WebSocket / REST              │
│ แฟลช: NVS (ค่าตั้ง/โมเดล) · LittleFS (log, train.csv) · coredump    │ ◄───── │ frontend (หน้าเว็บ Studio)         │
└──────────────────────────────────────────────────────────────────┘  HTTP  │ ml/ (เก็บข้อมูล, เทรน AI) · data/   │
                                                                             └────────────────────────────────────┘
```

---

## 1. โครงสร้างโฟลเดอร์

```
Wireless-Polygraph-2026/
├─ README.md                      หน้าแรกของ repo (ลิงก์ไปเอกสารทั้งหมด)
├─ PROJECT_MAP.md                 ไฟล์นี้
├─ TRAINING_GUIDE.md              ขั้นตอนเทรน AI ทีละคลิก + AI ทำงานยังไง
├─ FLOWCHARTS.md                  ผังการทำงาน (mermaid) 14 รูป
├─ คู่มือการใช้งาน_Wireless_Polygraph.md   คู่มือผู้ใช้ภาษาไทย
├─ presentation/                  สไลด์นำเสนอ (.pptx สำหรับ Canva, .pdf) + ภาพ flowchart
├─ todopls/                       โน้ตงานที่ต้องทำ (ของผู้ใช้)
├─ .vscode/                       ค่าตั้ง VS Code ของ repo
├─ Wireless Polygraph/            ── เฟิร์มแวร์นาฬิกา (PlatformIO) ──
│  ├─ platformio.ini              วิธี build/upload (USB, OTA, unit test)
│  ├─ partitions_polygraph.csv    แผนที่แฟลช 4 MB
│  ├─ src/                        โค้ดเฟิร์มแวร์ (ดูข้อ 4)
│  ├─ test/test_native/           unit test รันบนคอม
│  ├─ include/, lib/              โฟลเดอร์มาตรฐานของ PlatformIO (ว่าง — driver เขียนเองใน src/drivers)
│  └─ diagram.json, wokwi.toml    ไฟล์จำลองวงจรบน Wokwi (รุ่นทดลองแรก ใช้บอร์ด ESP32 DevKit — ไม่ได้ใช้กับเฟิร์มแวร์ปัจจุบัน)
└─ Polygraph-Studio/              ── โปรแกรมฝั่งคอม ──
   ├─ *.bat                       ดับเบิลคลิกเพื่อใช้งาน (ดูข้อ 2)
   ├─ requirements.txt            ไลบรารี Python ที่ install.bat ติดตั้ง
   ├─ DATA_DICTIONARY.md          ความหมายทุกไฟล์/คอลัมน์/ตาราง
   ├─ backend/                    เซิร์ฟเวอร์ Studio (Python) (ดูข้อ 3.1)
   ├─ frontend/                   หน้าเว็บ Studio (HTML/CSS/JS) (ดูข้อ 3.2)
   ├─ ml/                         เก็บข้อมูล + เทรน AI (ใช้แต่ไลบรารีมาตรฐาน) (ดูข้อ 3.3)
   ├─ tools/virtual_watch.py      นาฬิกาจำลอง (ไม่มีบอร์ดก็ซ้อมได้)
   ├─ tests/                      ทดสอบว่า LieEngine ฉบับ Python = ฉบับ C++
   ├─ data/                       ข้อมูลทั้งหมด (ดูข้อ 3.4)
   └─ data_backup_20261008_180401/  สำเนาโฟลเดอร์ data ก่อนแปลงไฟล์เป็นรูปแบบใหม่ (ลบได้เมื่อแน่ใจ)
```

---

## 2. ไฟล์ที่ดับเบิลคลิก (`Polygraph-Studio/*.bat`)

ทุกไฟล์หา Python ให้เอง (`python` ก่อน แล้วค่อย `py -3`) และ `cd` ไปที่โฟลเดอร์ Polygraph-Studio ก่อนรัน

| ไฟล์ | รันอะไร | ใช้ตอนไหน | ต้องต่อ WiFi นาฬิกา | ได้อะไร |
|---|---|---|---|---|
| `install.bat` | `pip install -r requirements.txt` | ครั้งแรกครั้งเดียว (ต้องมีเน็ต) | ไม่ (ต้องต่อเน็ตปกติ) | FastAPI, uvicorn, websockets, httpx, python-multipart |
| `run_studio.bat` | `python -m backend` | ใช้งานจริง/นำเสนอ | ต้อง | หน้าเว็บ Studio ที่ 127.0.0.1:8000 |
| `run_demo_simulator.bat` | `tools/virtual_watch.py` + `python -m backend --sim` | ซ้อม/ทดสอบโดยไม่มีบอร์ด | ไม่ | Studio ต่อกับนาฬิกาจำลอง (หน้าเว็บนาฬิกาจำลองที่ 127.0.0.1:8081) |
| `collect_data.bat` | `ml/collect.py` | เก็บข้อมูลแบบพิมพ์คำสั่ง (ไม่เปิด Studio) | ต้อง | `data/result_<รอบ>.csv` + `data/signals/signals_<รอบ>.csv` |
| `train_ai.bat` | `ml/train.py` | เทรน AI (ไม่เปิด Studio) | ไม่จำเป็น (ต่อ = ส่งโมเดลให้เลย) | `data/model.json` + `data/models/model_<ชื่อ>.json` |
| `train_ai_upload.bat` | `ml/train.py --upload-only` | ส่ง model.json ที่มีอยู่เข้านาฬิกา โดยไม่เทรนใหม่ | ต้อง | นาฬิกาใช้โมเดลนั้น (Studio ทำให้อัตโนมัติอยู่แล้ว) |

---

## 3. Polygraph-Studio (ฝั่งคอม)

### 3.1 `backend/` — เซิร์ฟเวอร์ (Python FastAPI)

| path | ทำอะไร | ทำไมต้องมี | เชื่อมกับ | ใช้ตอนไหน |
|---|---|---|---|---|
| `backend/__main__.py` | จุดเริ่ม: ตรวจพอร์ตว่าง, พิมพ์ที่อยู่, เปิดเบราว์เซอร์, รัน uvicorn | ให้ `python -m backend` ทำงาน | `config.py`, `main.py` | ทุกครั้งที่เปิด Studio |
| `backend/config.py` | ค่าตั้ง (IP นาฬิกา, พอร์ต, path ฐานข้อมูล) + argument `--sim --lan --port` | รวมค่าตั้งไว้ที่เดียว | ทุกไฟล์ใน backend | ตอนเปิด |
| `backend/main.py` | สร้าง FastAPI app: ประกอบทุกส่วน + endpoint หลัก (สถานะ, คำสั่งนาฬิกา, เซสชัน, เก็บข้อมูล, รายงาน) | ศูนย์กลางของ backend | ทุกไฟล์ใน backend, frontend เรียก `/api/...` | ตลอดเวลาที่เปิด Studio |
| `backend/watch_link.py` | คุยกับนาฬิกา: ส่ง hello ทุก 2 วินาที, รับ UDP (ค่าสด/คลื่น/เหตุการณ์), เรียก REST ของนาฬิกา, วัด ping | ช่องทางเดียวที่คุยกับนาฬิกา | นาฬิกา (`net/telemetry.cpp`, `net/web_server.cpp`), `sessions.py` | ตลอดเวลา |
| `backend/sessions.py` | `Interrogation` ตัวกลาง: รับทุกอย่างจาก watch_link แล้วกระจาย (เซสชันทดสอบ → SQLite, รอบเก็บข้อมูล → collector, ทุกอย่าง → หน้าเว็บ) + ชุดคำถามสำเร็จรูป | แยก "รับข้อมูล" กับ "ใช้ข้อมูล" | `watch_link.py`, `store.py`, `collector.py`, `hub.py` | ตลอดเวลา |
| `backend/collector.py` | ควบคุมรอบการถามของหน้า "เก็บข้อมูลเทรน AI" (fix/manual) และ "ใช้งานจริง" (live) + กันผลค้างด้วยเลข seq | ให้ 2 หน้าใช้ตัวบันทึกเดียวกับ collect_data.bat | `ml/recorder.py`, `store.py`, `hub.py` | ตอนเก็บข้อมูล / ใช้งานจริง |
| `backend/data_api.py` | API ไฟล์ข้อมูล: รายการ/อ่าน/ดาวน์โหลด/รวม/นำเข้า `result_*.csv`, ดึงข้อมูลจากนาฬิกา, ปุ่มเทรน, ส่งโมเดล | หน้า "ข้อมูล & เทรน AI" + ประวัติทุกหน้า | `ml/datafiles.py`, `ml/train.py`, `model_sync.py` | ตอนดูข้อมูล/เทรน |
| `backend/model_sync.py` | ทุก 4 วินาที เทียบ `data/model.json` กับโมเดลในนาฬิกา ไม่ตรง = ส่งให้เอง (ปิดได้) | ไม่ต้องกดอัปโหลดเอง | นาฬิกา `/api/ml`, `/api/ml/model`; `data/studio_settings.json` | ตลอดเวลาที่ต่อนาฬิกา |
| `backend/store.py` | ฐานข้อมูล SQLite `data/studio.db`: ตาราง/VIEW, บันทึกเซสชัน คำถาม ผล ค่าสด เหตุการณ์, export CSV | เก็บประวัติเซสชันทดสอบ + เปิดดูใน DBeaver | `sessions.py`, `collector.py`, `main.py` | ตลอดเวลา |
| `backend/hub.py` | กระจายข้อความ WebSocket ไปทุกแท็บที่เปิดหน้าเว็บ | ข้อมูลสดแบบเรียลไทม์ | ทุกส่วนที่เรียก `hub.publish()`, frontend `ws.js` | ตลอดเวลา |
| `backend/report.py` | สร้างหน้า HTML รายงานเซสชัน (สว่าง/มืด, พิมพ์เป็น PDF ได้) + แปลง markdown เป็นหน้าเว็บ | รายงานผลการทดสอบ, หน้า Data Dictionary | `main.py` (`/report/<id>`, `/report-data-dictionary`) | ตอนดูรายงาน |
| `backend/lie_engine.py` | LieEngine ฉบับ Python (ตรงกับ `src/lie/lie_engine.cpp` ทุกขั้น) | ให้นาฬิกาจำลองตัดสินเหมือนของจริง + ทดสอบเทียบ C++ | `tools/virtual_watch.py`, `tests/test_parity.py` | ตอนใช้นาฬิกาจำลอง/ทดสอบ |

### 3.2 `frontend/` — หน้าเว็บ Studio (ไม่ต้องใช้เน็ต: ไม่มี CDN)

| path | ทำอะไร | เชื่อมกับ |
|---|---|---|
| `frontend/index.html` | โครงหน้า: เมนูซ้าย, แถบบน, section ของแต่ละหน้า, ไอคอน SVG | `assets/css/app.css`, `assets/js/main.js`, `assets/vendor/chart.umd.min.js` |
| `assets/css/app.css` | สไตล์ทั้งหมด (ธีมสว่าง/มืด) | ทุกหน้า |
| `assets/vendor/chart.umd.min.js` | Chart.js (ไฟล์ในเครื่อง) วาดกราฟเส้น | `charts.js`, `widgets.js` |
| `assets/js/main.js` | จุดเริ่มของหน้าเว็บ: เปลี่ยนหน้า, แถบสถานะบน (เชื่อมต่อ/แบต/WiFi), แจ้งเตือนเหตุการณ์, ผู้จัดทำ | ทุก view, `ws.js`, `store.js` |
| `assets/js/ws.js` | ต่อ WebSocket `/ws` แยกข้อความตามชนิด (vitals, collect, ai, data, train ...) | `backend/hub.py`, `store.js` |
| `assets/js/store.js` | สถานะกลาง `S` + ระบบ on/emit + ข้อมูลกราฟย้อนหลัง 2 นาที | ทุก view |
| `assets/js/api.js` | เรียก REST API + แสดง error ภาษาไทย | `backend/main.py`, `data_api.py` |
| `assets/js/ui.js` | ตัวช่วย: escape, format ตัวเลข/เวลา, toast, กล่องยืนยัน, สี/ชื่อคำตัดสิน | ทุก view |
| `assets/js/charts.js` | กราฟเส้นเรียลไทม์, คลื่นชีพจร (canvas), เกจโอกาสโกหก | `widgets.js`, views |
| `assets/js/widgets.js` | ช่องค่าสด 6 ช่อง + กราฟ 4 กราฟ (ใช้ซ้ำหลายหน้า) | home, collect, use |
| `assets/js/history.js` | การ์ด "ประวัติทั้งหมด" อ่านทุก `result_*.csv` + ดาวน์โหลดไฟล์เดียว/รวม | `GET /api/data/rows`, `/api/data/merged.csv` |
| `assets/js/views/home.js` | หน้า "Wireless Polygraph": ค่าสด + กราฟ, ทดสอบแบบเซสชัน, ควบคุมด่วน, พลังงาน | `session.js`, `/api/watch/command` |
| `assets/js/views/session.js` | ส่วน "ทดสอบแบบเซสชัน" (ชุดคำถาม, ถามทีละข้อ, ผล, ตรวจความพร้อม) ฝังในหน้าหลัก | `/api/sessions...` |
| `assets/js/views/use.js` | หน้า **"ใช้งานจริง"**: ถาม → ผล → กด ถูก/ผิด/ไม่ทราบ, AI ที่ใช้, ความแม่นยำรอบนี้ | `/api/collect/*` (mode live), `/api/ai` |
| `assets/js/views/collect.js` | หน้า **"เก็บข้อมูลเทรน AI"** (fix/manual, นับถอยหลัง, รายละเอียด qid/ไฟล์) + ประวัติทั้งหมด | `/api/collect/*` |
| `assets/js/views/data.js` | หน้า **"ข้อมูล & เทรน AI"**: ไฟล์, ติ๊กใช้เทรน, รวม/นำเข้า/ดึงจากนาฬิกา, ปุ่มเทรน, โมเดลคอม↔นาฬิกา | `backend/data_api.py` |
| `assets/js/views/results.js` | หน้า "ผลลัพธ์ & รายงาน": ประวัติเซสชัน, ดาวน์โหลด CSV/JSON, เปิดรายงาน | `/api/sessions`, `/report/<id>` |
| `assets/js/views/system.js` | หน้า "ระบบ & อุปกรณ์": บูต/watchdog, หน่วยความจำ/แฟลช, task, พลังงาน, OTA, ค่าตั้ง LieEngine, ปุ่มนาฬิกาจำลอง | `/api/watch/system`, `/api/watch/config`, `/api/watch/ota` |
| `assets/js/views/guide.js` | หน้า "คู่มือ & หลักการ" | — |

### 3.3 `ml/` — เก็บข้อมูล + เทรน AI (Python ล้วน ไม่ต้องติดตั้งอะไร)

| path | ทำอะไร | ทำไมต้องมี | ใครเรียก |
|---|---|---|---|
| `ml/datafiles.py` | **กำหนดรูปแบบไฟล์ `result_*.csv`** (หัวตาราง), อ่าน/เขียน, แปลงไฟล์รูปแบบเก่า (migrate), นำเข้าไฟล์/ข้อมูลจากนาฬิกา (ตัดข้อซ้ำ), รวมไฟล์, รายชื่อไฟล์ที่ใช้เทรน | ให้ทุกโปรแกรมใช้รูปแบบเดียวกันเป๊ะ | recorder, train, collect, backend/data_api, backend/main (migrate ตอนเปิด) |
| `ml/recorder.py` | บันทึก 1 รอบ: ค่าสด → `signals/`, ผลรายข้อ → `result_<รอบ>.csv`, โหมด fix/manual/live, กันผลค้าง | ตัวบันทึกเดียวของทุกช่องทาง | backend/collector.py, ml/collect.py |
| `ml/collect.py` | โปรแกรมเก็บข้อมูลแบบพิมพ์คำสั่ง (UDP + HTTP ตรงกับนาฬิกา) | เก็บข้อมูลได้แม้ไม่เปิด Studio | collect_data.bat |
| `ml/train.py` | เทรน Logistic Regression จาก `result_*.csv`, ประเมิน (LOSO), บันทึก model.json, ส่งเข้านาฬิกา | สร้างโมเดล | train_ai.bat, train_ai_upload.bat, ปุ่ม "เทรน AI" |
| `ml/polyml.py` | คณิตศาสตร์ของ AI (standardize, IRLS, L2, CV, metrics, Wilson CI) + feature 12 ตัว + คุยกับนาฬิกาด้วย HTTP | แกนกลางที่ใช้ร่วมกัน (สูตรเดียวกับ `ml_model.cpp`) | train, datafiles, collect, virtual_watch |

### 3.4 `data/` — ข้อมูลทั้งหมด (รายละเอียดคอลัมน์: DATA_DICTIONARY.md ข้อ 0)

| path | สร้างโดย | อ่านโดย | ใช้ทำอะไร |
|---|---|---|---|
| `data/result_<YYYYMMDD_HHMMSS>.csv` | recorder (หน้าเก็บข้อมูล, ใช้งานจริง, collect_data.bat), import (ดึงจากนาฬิกา/นำเข้าไฟล์), migrate | train.py, หน้า "ข้อมูล & เทรน AI", ประวัติ | **ข้อมูลเทรน AI รูปแบบเดียว** 1 แถว = 1 ข้อ |
| `data/signals/signals_<รอบ>.csv` | recorder | คนวิเคราะห์ (Excel/Python), ดาวน์โหลดผ่าน `/api/data/signals/<ชื่อ>` | ค่าสด 5 ครั้ง/วินาทีของรอบเดียวกัน (ไม่ใช้เทรน) |
| `data/model.json` | train.py | model_sync.py (ส่งเข้านาฬิกา), train_ai_upload.bat | โมเดลล่าสุด |
| `data/models/model_<ชื่อ>.json` | train.py | หน้า "ข้อมูล & เทรน AI" (เลือกส่งตัวเก่า) | ประวัติโมเดล |
| `data/train_exclude.txt` | ช่องติ๊ก "ใช้เทรน" | train.py, data_api | ไฟล์ที่ไม่ใช้เทรน |
| `data/studio_settings.json` | model_sync.py | model_sync.py | ส่งโมเดลอัตโนมัติ เปิด/ปิด |
| `data/studio.db` | store.py | Studio, DBeaver | เซสชัน, ผล, ค่าสด, เหตุการณ์, สำเนารอบเก็บข้อมูล |
| `data/old_format/` | migrate | ไม่มีใครอ่าน | ต้นฉบับไฟล์รูปแบบเก่า (ไม่ถูกแก้) |
| `data/migration_log.txt` | migrate | คน | บันทึกการแปลงไฟล์ |
| `data/trash/` | ปุ่มลบ (data_api → datafiles.trash_file / delete_rows / set_used) | หน้า "ข้อมูล & เทรน AI" การ์ดถังขยะ (กู้คืน) | ไฟล์ที่ลบ + สำเนาก่อนแก้ `result_x.before_<เวลา>.csv` — train.py ไม่อ่าน |

### 3.5 เครื่องมือ/ทดสอบ

| path | ทำอะไร | ใช้ตอนไหน |
|---|---|---|
| `tools/virtual_watch.py` | นาฬิกาจำลองทั้งเครื่อง: UDP + REST API ชุดเดียวกับเฟิร์มแวร์ + หน้าเว็บนาฬิกา + ผู้ตอบจำลอง (ควบคุมโกหก/จริงได้) | `run_demo_simulator.bat`, ซ้อมนำเสนอ, ทดสอบ |
| `tests/test_parity.py` + `scenario.txt` + `golden_cpp.txt` | ป้อนสถานการณ์เดียวกันเข้า LieEngine Python เทียบผลกับ C++ ทีละข้อ | หลังแก้ LieEngine (`python tests/test_parity.py`) |

---

## 4. Wireless Polygraph (เฟิร์มแวร์นาฬิกา)

### 4.1 ไฟล์ตั้งค่า

| path | ทำอะไร |
|---|---|
| `platformio.ini` | env `esp32c3` (USB), `esp32c3_ota` (อัปโหลดผ่าน WiFi), `native` (unit test บนคอม), ใช้ partition ของเรา, USB CDC |
| `partitions_polygraph.csv` | แผนที่แฟลช: nvs 20 KB, otadata 8 KB, app0/app1 1.75 MB (OTA + rollback), LittleFS 384 KB, coredump 64 KB |
| `src/config.h` | ค่าคงที่ทั้งหมด: ขา GPIO, ชื่อ/รหัส WiFi, ความถี่ timer, stack/priority ของ task, เวลา watchdog, ค่าเริ่มต้น LieEngine |
| `test/test_native/test_main.cpp` | unit test ของ DSP + LieEngine + ml_model ด้วยสัญญาณจำลอง (`pio test -e native`) |

### 4.2 `src/` — ลำดับการทำงานและไฟล์

| path | ทำอะไร | เชื่อมกับ / เรียกจาก |
|---|---|---|
| `main.cpp` | `setup()`: Serial → watchdog → NVS/EEPROM/LittleFS → เซนเซอร์ → WiFi → โหลดโมเดล AI → สร้าง task; `loop()` = Serial console | ทุกโมดูล |
| `app.h / app.cpp` | ข้อมูลกลาง (ค่าสด, สรุปสถานะ engine), mutex/queue/event group, `logEvent()`, `pushEvent()` | ทุก task |
| `tasks.h / tasks.cpp` | สร้าง 6 task + hardware timer 100 Hz: sensorTask(5) engineTask(4) telemetryTask(3) httpTask(2) uiTask(2) supervisorTask(1) | `main.cpp` |
| `sensors.h / sensors.cpp` | sensorTask: อ่าน MAX30102/MPU6050/ADC → DSP → รวมเป็นเฟรม 5 Hz → frameQueue, ตรวจสุขภาพเซนเซอร์/กู้บัส I2C | drivers/, dsp/, `tasks.cpp` |
| `cli.h / cli.cpp` | Serial console: help, status, live, tasks, flash, boots, ask, mode, model, sleep, wifi ... | `main.cpp loop()` |
| `drivers/i2c_bus.*` | เปิดบัส I2C (SDA 6, SCL 7), สแกน, กู้บัสค้าง | sensors |
| `drivers/max30102.*` | driver เซนเซอร์ชีพจร (เขียนเอง) อ่าน FIFO แสง IR/แดง 100 Hz | sensors |
| `drivers/mpu6050.*` | driver เซนเซอร์ความเร่ง (รองรับ MPU6050/6500/รุ่นเลียนแบบ) | sensors |
| `drivers/analog_in.*` | อ่าน ADC เป็น mV (NTC GPIO0, GSR GPIO1, แบต GPIO4) | sensors |
| `drivers/sensor_math.*` | แปลงแรงดัน → °C (NTC), µS (GSR), % แบต | sensors, unit test |
| `dsp/filters.h` | ตัวกรองพื้นฐาน: Ema (ค่าเฉลี่ยเคลื่อนที่), Biquad (low/high-pass), RunningStats (เฉลี่ย/ส่วนเบี่ยงเบน) | dsp/* |
| `dsp/ppg.*` | แสง IR → ชีพจร, HRV, แรงชีพจร, การแตะผิว | sensors |
| `dsp/eda.*` | GSR → tonic/phasic, จำนวนยอด SCR | sensors |
| `dsp/motion.*` | ความเร่ง → มือสั่น (3–15 Hz) / การขยับตัว | sensors |
| `lie/lie_engine.*` | "สมอง": baseline 30 วินาที → วัด 12 วินาทีต่อข้อ → feature d/z → คะแนน → จริง/โกหก/ไม่แน่ชัด; ปรับเกณฑ์จากข้อควบคุม | `tasks.cpp engineTask`, web_server, cli |
| `lie/ml_model.*` | โมเดล AI บนนาฬิกา: `extract()` feature 12 ตัว, `predict()` = sigmoid(b + Σ w(x−mean)/scale), CRC32 | `sys/ml_runtime.cpp` |
| `sys/ml_runtime.*` | ผูกโมเดลเข้ากับ LieEngine, โหมด train/detect, ติดตั้ง/ลบโมเดล, สร้างแถว /train.csv | web_server (`/api/ml/*`), engineTask, ui (กด 2 ครั้ง) |
| `sys/storage.*` | หน่วยความจำ: NVS (ค่าตั้ง, โมเดล), EEPROM emulation (สถิติ), RTC (กล่องดำ), LittleFS (events.log, results.csv, train.csv) | ทุกโมดูลที่ต้องจำค่า |
| `sys/watchdog.*` | Watchdog 3 ชั้น (Task WDT 8 s, Interrupt WDT, Hardware timer 12 s) + กล่องดำสาเหตุรีเซ็ต + สาธิต | supervisor, main |
| `sys/power.*` | ECO 80 MHz, light/deep sleep, หลับอัตโนมัติเมื่อไม่มีใครใช้, แบตต่ำ | supervisor, ui (กดค้าง), web/cli |
| `sys/ui.*` | LED (PWM) บอกสถานะ/จังหวะหัวใจ/ผล + ปุ่ม BOOT (กด 1/2/3 ครั้ง, ค้าง 2/5/10 วินาที) | uiTask |
| `sys/ota.*` | อัปเดตเฟิร์มแวร์ผ่าน WiFi (หน้า /update, ArduinoOTA) + rollback ถ้าไม่ยืนยันใน 20 วินาที | web_server, supervisor |
| `sys/sysinfo.*` | สร้าง JSON ข้อมูลระบบ (/api/info, /api/system) | web_server |
| `net/wifi_ap.*` | ปล่อย WiFi เอง (SoftAP 192.168.4.1), ระดับกำลังส่ง, รหัสเครื่อง PW-xxxx | main, power |
| `net/telemetry.*` | telemetryTask: ส่งค่าสด 5 Hz + คลื่นชีพจร + เหตุการณ์ ทาง UDP 4210 ให้ผู้ที่ส่ง hello (สูงสุด 3 ราย) | Studio `watch_link.py`, collect.py |
| `net/web_server.*` | httpTask: หน้าเว็บในนาฬิกา + REST API ทั้งหมด (`/api/live`, `/api/lie/*`, `/api/ml/*`, `/api/config` ...) | Studio, มือถือ |
| `net/web_page.h` | HTML/JS ของหน้าเว็บในนาฬิกา (ฝังในแฟลช) | web_server |
| `net/json_writer.h` | ตัวสร้าง JSON แบบเบา (ไม่ใช้ heap มาก) | web_server, sysinfo, ml_runtime |

---

## 5. เส้นทางข้อมูล (path โยงกันตอนใช้งานจริง)

### 5.1 ค่าเซนเซอร์ → กราฟบนหน้าเว็บ Studio
```
hardware timer 100 Hz ──notify──► sensors.cpp tick()
  ├─ drivers/max30102 → dsp/ppg     (ชีพจร, HRV, แรงชีพจร)
  ├─ drivers/mpu6050  → dsp/motion  (มือสั่น, ขยับ)
  └─ drivers/analog_in + sensor_math → dsp/eda (GSR), NTC (°C), แบต
        └─ publishFrame() ทุก 200 ms ──► app::setVitals() + frameQueue
net/telemetry.cpp sendVitals() ──UDP 4210──► backend/watch_link.py datagram_received()
  └─► backend/sessions.py on_vitals() ──► backend/hub.py publish("vitals")
        └─WebSocket──► frontend/assets/js/ws.js ──► store.js pushVitals() ──► widgets.js / charts.js (กราฟ)
                                      └─► collector.on_vitals() ──► ml/recorder.py ──► data/signals/signals_<รอบ>.csv
```

### 5.2 กดถาม 1 ข้อในหน้า "เก็บข้อมูลเทรน AI" → ไฟล์ result
```
views/collect.js กด "ถาม..." ──POST /api/collect/ask──► backend/main.py ──► collector.ask()
  ├─ GET /api/lie?n=1 (จำ seq ล่าสุด กันผลค้าง)
  └─ POST /api/lie/question?qid=600+n ──► นาฬิกา net/web_server.cpp hQuestion() ──► lie_engine startQuestion()
engineTask: 12 วินาทีครบ ──► LieEngine คำนวณผล (+ ml_runtime scorer → ml_model predict() ถ้ามีโมเดล)
  ├─ resultEvent() ──UDP──► watch_link ──► sessions._ingest() ──► collector.on_result() ──► recorder.on_result()
  └─ (โหมด train ของนาฬิกา + ข้อควบคุม) makeTrainRow() ──► /train.csv ในแฟลช
recorder: fix = บันทึกทันที / manual = รอ POST /api/collect/label
  └─► ml/datafiles.py row_from_result() ──► data/result_<รอบ>.csv  (+ store.add_training_question → studio.db)
      └─► data_api publish("data") ──► history.js / data.js รีเฟรชเอง
```

### 5.3 เทรน → model.json → นาฬิกา
```
views/data.js "เทรน AI" ──POST /api/ai/train──► backend/data_api.py ──► ml/train.py run_training()
  ├─ ml/datafiles.py training_files(): data/result_*.csv − train_exclude.txt
  ├─ build_dataset(): แถว used_for_training=1 + label truth/lie, ตัดซ้ำ run_id+question_no
  ├─ ml/polyml.py: cross_validate (LOSO) → choose_lambda → Model.fit
  └─ save_model(): data/model.json + data/models/model_<ชื่อ>.json
backend/model_sync.py tick() (ทุก 4 วินาที หรือทันทีหลังเทรน)
  ├─ GET /api/ml (นาฬิกามีโมเดลชื่ออะไร เทรนเมื่อไร)
  └─ ไม่ตรง ──POST /api/ml/model (form)──► web_server hMlModel() ──► ml_runtime install()
        ──► ml_model seal() + valid() ──► storage saveModel() (NVS) ──► ใช้ตัดสินข้อถัดไปทันที
(ทางเลือก: train_ai.bat = ml/train.py → upload ถ้าต่อนาฬิกาอยู่; train_ai_upload.bat = ml/train.py --upload-only)
```

### 5.4 หน้า "ใช้งานจริง"
```
views/use.js เริ่ม (mode live) ──► collector.start() ──► recorder (mode live)
ถาม ──► เหมือน 5.2 (kind = test) ──► ผลมาถึง ──► รอ feedback
กด ถูก/ผิด/ไม่ทราบ ──POST /api/collect/feedback──► recorder.set_feedback()
  └─ datafiles.label_from_feedback(): ทาย "จริง"+ถูก=truth, "จริง"+ผิด=lie, "โกหก"+ถูก=lie, "โกหก"+ผิด=truth
      ──► data/result_<รอบ>.csv (mode=live, feedback, question_text) ──► เทรนรอบต่อไปได้
```

### 5.5 หน้าเว็บในนาฬิกา (มือถือ ไม่ผ่านคอม)
```
มือถือ ──HTTP 192.168.4.1──► net/web_server.cpp ──► net/web_page.h (HTML) + /api/live, /api/lie/*, /api/ml/*
ดาวน์โหลด CSV ──GET /api/ml/data.csv──► polygraph_train_<วันเวลา>.csv ──(นำเข้าใน Studio)──► data/result_*.csv (source = mobile)
Studio "ดึงข้อมูลที่นาฬิกาบันทึกเอง" ──GET /api/ml/data.csv──► data/result_*.csv (source = esp_backup)
ใช้งานจริงบนมือถือ กด ถูก/ผิด ──POST /api/ml/feedback?seq=&label=──► 1 แถวใน /train.csv (supervisorTask เขียน)
ลบรายแถวบนมือถือ ──POST /api/ml/data/delete?row=&t=&qid=──► storage::deleteTrainRow() (คัดลอกไป /train.tmp แล้ว rename)
ชุดคำถาม/ถูก-ผิด/ข้อความคำถาม ──► localStorage ของมือถือ (key "pg2") — นาฬิการู้แค่ qid 1000-59999
```

### 5.6 นาฬิกาค้าง / รีบูต
```
ทุก task ส่ง heartbeat ──► supervisorTask ตรวจครบทุกตัว ──► ป้อน Task WDT + hardware timer WDT (sys/watchdog.cpp)
task ค้าง ──► WDT รีเซ็ต ──► บูตใหม่: watchdog.cpp อ่านสาเหตุ + กล่องดำ (RTC_NOINIT) + coredump
  ──► main.cpp countBootReason() (สถิติใน EEPROM) ──► telemetry sendHi (reason) ──► Studio แจ้งเตือน "นาฬิการีบูต"
```

---

## 6. ใช้อะไร ตอนไหน (สรุป)

| สถานการณ์ | ทำอะไร |
|---|---|
| ครั้งแรกบนคอมเครื่องใหม่ | `install.bat` (ต่อเน็ต) |
| ซ้อม/ไม่มีบอร์ด | `run_demo_simulator.bat` |
| นำเสนอ/ใช้งานจริง | ต่อ WiFi นาฬิกา → `run_studio.bat` → เมนู "ใช้งานจริง" หรือหน้าหลัก |
| เก็บข้อมูลเทรน | Studio เมนู "เก็บข้อมูลเทรน AI" (หรือ `collect_data.bat`) |
| เทรน | Studio เมนู "ข้อมูล & เทรน AI" → เทรน AI (หรือ `train_ai.bat`) |
| ส่งโมเดล | อัตโนมัติใน Studio (หรือ `train_ai_upload.bat`) |
| แก้เฟิร์มแวร์ | VS Code + PlatformIO: Build ✓ → Upload → (ไร้สาย: env `esp32c3_ota`) |
| ดูสาเหตุนาฬิการีบูต | Studio เมนู "ระบบ & อุปกรณ์" หรือ Serial `boots` |
| ดูฐานข้อมูล | เปิด `data/studio.db` ใน DBeaver → VIEW `v_*` |
