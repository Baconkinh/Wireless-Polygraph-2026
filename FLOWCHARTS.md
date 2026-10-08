# Wireless Polygraph — Flowchart ทั้งระบบ

ไฟล์นี้อธิบายการทำงานของระบบเป็นภาพ (Mermaid) เปิดดูได้ใน VS Code (Markdown Preview) หรือ GitHub
แต่ละภาพบอกด้วยว่าเป็นโค้ดไฟล์ไหน เผื่ออาจารย์ถามว่า "ส่วนนี้อยู่ตรงไหนในโค้ด"

สารบัญ

1. ภาพรวมระบบ (ฮาร์ดแวร์ → เฟิร์มแวร์ → หน้าเว็บ/คอม)
2. ลำดับการบูต `setup()`
3. FreeRTOS task และการส่งข้อมูลระหว่าง task
4. เส้นทางสัญญาณ: เซนเซอร์ → DSP → LieEngine
5. State machine ของ LieEngine
6. การตัดสิน 1 ข้อ (จริง / โกหก)
7. การเก็บข้อมูลเทรน AI (fix / manual)
8. Pipeline ของ AI: เก็บข้อมูล → เทรน → ใช้งานบนนาฬิกา
9. Polygraph Studio: backend / frontend
10. Watchdog 3 ชั้น + กล่องดำ
11. พลังงานและ sleep
12. แผนที่หน่วยความจำ (Flash map) และที่เก็บข้อมูลแต่ละชนิด
13. OTA + rollback
14. Serial console (คำสั่งพิมพ์)

---

## 1. ภาพรวมระบบ

```mermaid
flowchart LR
  subgraph HW["ฮาร์ดแวร์ (สายรัดข้อมือ)"]
    BAT["Li-Po 3.7V 450mAh<br/>+ TP4056 ชาร์จ"] --> LDO["HT7333<br/>3.3V"]
    LDO --> MCU["ESP32-C3 SuperMini"]
    MAX["MAX30102<br/>ชีพจร (PPG)"] -- "I2C 0x57" --> MCU
    MPU["MPU6050<br/>การสั่น/ขยับ"] -- "I2C 0x68" --> MCU
    NTC["NTC 10k<br/>อุณหภูมิผิว"] -- "ADC GPIO0" --> MCU
    GSR["แผ่น GSR<br/>ความชื้นผิว"] -- "ADC GPIO1" --> MCU
    DIV["ตัวแบ่งแรงดันแบต"] -- "ADC GPIO4" --> MCU
  end
  subgraph FW["เฟิร์มแวร์ (FreeRTOS)"]
    MCU --> DSP["DSP: หาจังหวะหัวใจ,<br/>แยก GSR, กรองการสั่น"]
    DSP --> ENG["LieEngine<br/>+ โมเดล AI"]
    ENG --> NET["WiFi SoftAP<br/>Polygraph-Watch"]
  end
  NET -- "HTTP 80: หน้าเว็บ + REST API" --> PHONE["มือถือ<br/>192.168.4.1"]
  NET -- "UDP 4210: ค่าสด 5 Hz" --> PC["โน้ตบุ๊ก:<br/>Polygraph Studio"]
  NET -- "HTTP: สั่งงาน/อ่านผล" --> PC
  PC --> CSV["CSV + SQLite<br/>ในคอม"] --> TRAIN["train.py<br/>เทรน AI"]
  TRAIN -- "model.json" --> NET
```

---

## 2. ลำดับการบูต `setup()` — `src/main.cpp`

```mermaid
flowchart TD
  A(["จ่ายไฟ / รีเซ็ต"]) --> B["Serial.begin + setTxTimeoutMs 50 ms"]
  B --> C{"power::handleWakeEarly()<br/>ตื่นจาก standby?"}
  C -- "ยังไม่มีใครใส่นาฬิกา" --> Z(["กลับไป deep sleep"])
  C -- "บูตต่อ" --> D["wdt::captureBootInfo()<br/>อ่านกล่องดำ RTC_NOINIT + coredump"]
  D --> E["storage::begin()<br/>NVS, EEPROM, LittleFS"]
  E --> F["countBootReason()<br/>นับรีเซ็ต + ประวัติ 8 ครั้ง"]
  F --> G["ไฟ LED บอกสาเหตุ:<br/>2 = ปกติ, 4 = WDT/panic, 6 = ไฟตก"]
  G --> H["sensors::begin()<br/>I2C 400 kHz, ตั้งค่า MAX30102/MPU6050"]
  H --> I["LieEngine.configure + mlrt::begin()<br/>โหลดโมเดล AI จาก NVS"]
  I --> J{"รีเซ็ตครั้งก่อน<br/>เพราะไฟตก?"}
  J -- "ใช่" --> K["เปิด WiFi แบบประหยัดกระแส:<br/>2 dBm, CPU 80 MHz, รอ 800 ms"]
  J -- "ไม่" --> L["เปิด WiFi ตามค่าตั้ง (8.5 dBm)"]
  K --> M
  L --> M["web::begin() — REST API + OTA"]
  M --> N["wdt::beginTaskWdt() 8 s"]
  N --> O["tasks::startAll()<br/>สร้าง 6 task + timer 100 Hz"]
  O --> P["wdt::beginHwWdt() 12 s"]
  P --> Q(["loop(): Serial console"])
```

---

## 3. FreeRTOS task และการส่งข้อมูล — `src/tasks.cpp`, `src/app.h`

```mermaid
flowchart LR
  T["Hardware timer<br/>100 Hz (ISR onTick)"] -- "task notification" --> S["sensorTask<br/>prio 5"]
  S -- "frameQueue (5 Hz)" --> E["engineTask<br/>prio 4 · LieEngine"]
  S -- "waveQueue (คลื่น PPG)" --> TL["telemetryTask<br/>prio 3 · UDP"]
  S -- "notify: หัวใจเต้น" --> U["uiTask<br/>prio 2 · LED/ปุ่ม"]
  E -- "eventQueue (ผล/เหตุการณ์)" --> TL
  E -- "logQueue (CSV/TRAIN)" --> SV["supervisorTask<br/>prio 1"]
  H["httpTask prio 2<br/>REST/หน้าเว็บ/OTA"] -- "engineMutex" --> E
  SV -- "fsMutex" --> FS[("LittleFS")]
  SV -- "ป้อน HW WDT +<br/>ตรวจ heartbeat ทุก task" --> WD{{"Watchdog"}}
  L["loopTask prio 1<br/>Serial console"] -- "engineMutex" --> E
```

ทำไมแบ่งแบบนี้: งานที่ต้องตรงเวลา (สุ่มสัญญาณ 100 Hz) ได้ priority สูงสุด ส่วนงานช้า เช่น เขียนแฟลช ส่งไปทำใน task priority ต่ำผ่านคิว
งานใน ISR สั้นที่สุด คือแค่ "ปลุก" sensorTask (deferred interrupt processing)

---

## 4. เส้นทางสัญญาณ — `src/sensors.cpp`, `src/dsp/*`

```mermaid
flowchart TD
  P["MAX30102 FIFO<br/>IR 100 sps"] --> P1["ppg.cpp: กรอง DC/high-pass<br/>หาจุดยอด (beat detection)"]
  P1 --> P2["HR, HRV, แอมพลิจูดชีพจร<br/>+ ตรวจการแตะผิว"]
  A["MPU6050<br/>ความเร่ง 3 แกน"] --> A1["motion.cpp: band-pass 3–15 Hz = มือสั่น<br/>0.3–3 Hz = การขยับตัว"]
  G["ADC GPIO1<br/>oversample 16 ครั้ง"] --> G1["eda.cpp: µS = 1/R ผิว<br/>แยก tonic / phasic, นับ SCR"]
  N["ADC GPIO0"] --> N1["sensor_math: สมการ Beta → °C"]
  V["ADC GPIO4"] --> V1["แรงดันแบต → %"]
  P2 --> F["Frame ทุก 200 ms (5 Hz)"]
  A1 --> F
  G1 --> F
  N1 --> F
  V1 --> F
  F --> Q["frameQueue → engineTask"]
  F --> UDP["UDP 'v' ไป Studio / collect.py"]
```

---

## 5. State machine ของ LieEngine — `src/lie/lie_engine.cpp`

```mermaid
stateDiagram-v2
  [*] --> Idle
  Idle --> Baseline: startBaseline() (ปุ่ม/เว็บ/Serial)
  Baseline --> Ready: ครบ 30 วินาที และมีสัญญาณพอ<br/>(ชีพจรหรือ GSR อย่างน้อย 70%)
  Baseline --> Idle: ไม่มีสัญญาณ → BASELINE_NO_SIGNAL
  Ready --> Question: startQuestion(qid, kind)
  Question --> Ready: ครบ 12 วินาที → คำนวณผล
  Question --> Ready: abort()
  Ready --> Idle: resetSession()
  Ready --> Baseline: วัดค่าปกติใหม่
```

---

## 6. การตัดสิน 1 ข้อ

```mermaid
flowchart TD
  A["เริ่มคำถาม: จำค่า 3 วินาทีก่อนถาม (pre)"] --> B["เก็บ 12 วินาทีหลังถาม"]
  B --> C["feature 5 ตัว = การเปลี่ยนแปลงเทียบ pre<br/>GSR↑, ชีพจร↑, แรงชีพจร↓, มือสั่น↑, ผิวเย็นลง"]
  C --> D["z = (feature − ค่าเฉลี่ยตอน baseline) ÷ ความแกว่งตอน baseline"]
  D --> E{"ใช้ได้ไหม?"}
  E -- "ไม่มีทั้งชีพจรและ GSR /<br/>ขยับตัวมาก / pre ไม่พอ" --> X["ใช้ไม่ได้ (invalid)"]
  E -- "ใช้ได้ (GSR หายก็ยังได้)" --> F{"มีโมเดล AI?"}
  F -- "มี" --> G["p = sigmoid(b + Σ w·(x−mean)/scale)<br/>Logistic Regression 12 ค่า"]
  F -- "ไม่มี" --> H["score = Σ น้ำหนัก × z<br/>ชีพจร 35%, แรงชีพจร 25%, GSR 20%,<br/>สั่น 12%, อุณหภูมิ 8%<br/>(ปรับน้ำหนักจากข้อควบคุมได้)"]
  H --> I["p = sigmoid(k·(score − s0))"]
  G --> J{"p ≥ 65%?"}
  I --> J
  J -- "ใช่" --> L["โกหก"]
  J -- "ไม่" --> K{"p ≤ 35%?"}
  K -- "ใช่" --> T["จริง"]
  K -- "ไม่" --> U["ไม่แน่ชัด"]
```

---

## 7. การเก็บข้อมูล / ใช้งานจริง — `Polygraph-Studio/ml/recorder.py`, `backend/collector.py`, `ml/collect.py`

```mermaid
flowchart TD
  S(["เริ่มรอบ: ชื่อผู้ตอบ, ผู้ถาม, โหมด fix / manual / live"]) --> B["วัดค่าปกติ (baseline)"]
  B --> N["แสดง: ข้อถัดไป = ข้อที่ N (qid 600+N)<br/>ไฟล์ที่จะบันทึก, ใครถาม → ใครตอบ"]
  N --> M{"โหมด"}
  M -- "fix" --> F1["ผู้ถามบอกผู้ตอบก่อน:<br/>ข้อนี้ให้ตอบจริง / ให้โกหก<br/>กดปุ่มให้ตรง → รู้เฉลยตั้งแต่เริ่ม"]
  M -- "manual / live" --> M1["กด 'ถาม' (พิมพ์คำถามได้)<br/>ผู้ตอบตอบตามใจ"]
  F1 --> SEQ["จำเลข seq ของผลล่าสุดในนาฬิกา<br/>(กันจับคู่ผลเก่าที่ qid ซ้ำ)"]
  M1 --> SEQ
  SEQ --> R["ข้อแรก? → สร้างไฟล์ตอนนี้<br/>(ก่อนหน้านี้เก็บในหน่วยความจำ ไม่มีไฟล์เปล่า)"]
  R --> W["บันทึก 12 วินาที<br/>เส้นนับถอยหลังวิ่ง"]
  W --> RES["นาฬิกาส่งผล (seq ใหม่กว่า) + feature"]
  RES --> L{"รู้เฉลยแล้ว?"}
  L -- "fix: รู้แล้ว" --> SAVE
  L -- "manual" --> ASK["ผู้ตอบบอก: พูดจริง / โกหก / ไม่รู้"]
  L -- "live" --> FB["ผู้ใช้กด: นาฬิกาตอบ ถูก / ผิด / ไม่ทราบ<br/>→ แปลงเป็นเฉลย"]
  ASK --> SAVE["เขียน data/result_รอบ.csv (1 แถว/ข้อ: เฉลย + คำตัดสิน + 12 feature)<br/>+ data/signals/signals_รอบ.csv + SQLite"]
  FB --> SAVE
  SAVE --> N
```

---

## 8. Pipeline ของ AI

```mermaid
flowchart LR
  A["เก็บข้อมูล / ใช้งานจริง<br/>(ข้อ 7)"] --> B[("data/result_*.csv<br/>รูปแบบเดียว 1 แถว/ข้อ")]
  W["นาฬิกาบันทึกเอง /train.csv<br/>(โหมด train บนมือถือ)"] -- "ดึงข้อมูล / นำเข้าไฟล์<br/>(ตัดข้อซ้ำ)" --> B
  B --> X["เลือกไฟล์ (ติ๊ก 'ใช้เทรน')<br/>แถว used_for_training = 1"]
  X --> C["train.py: standardize<br/>Logistic Regression + L2<br/>(Newton/IRLS)"]
  C --> D["ประเมินแบบไม่โกง:<br/>leave-one-subject-out<br/>หรือ stratified k-fold"]
  D --> E[("data/model.json<br/>+ data/models/")]
  E -- "model_sync.py ส่งอัตโนมัติ<br/>(หรือ train_ai_upload.bat)" --> F["นาฬิกาเก็บใน NVS<br/>ตรวจ CRC32"]
  F --> G["LieEngine เรียก scorer()<br/>ml_model predict() ทุกข้อ<br/>→ 'ตัดสินด้วยโมเดล AI'"]
```

---

## 9. Polygraph Studio — `Polygraph-Studio/backend`, `frontend`

```mermaid
flowchart LR
  subgraph Browser["frontend (เบราว์เซอร์)"]
    UI["index.html + main.js<br/>หน้าหลัก / ใช้งานจริง / เก็บข้อมูล /<br/>ข้อมูล & เทรน AI / ผลลัพธ์ / ระบบ / คู่มือ"]
  end
  subgraph Backend["backend (Python FastAPI)"]
    API["main.py REST /api/..."]
    DAPI["data_api.py ไฟล์ + เทรน"]
    HUB["hub.py WebSocket /ws"]
    INT["sessions.py เซสชันทดสอบ"]
    COL["collector.py เก็บข้อมูล / ใช้งานจริง"]
    SYNC["model_sync.py ส่งโมเดลอัตโนมัติ"]
    LINK["watch_link.py UDP + HTTP"]
    DB[("store.py SQLite<br/>data/studio.db")]
  end
  W["นาฬิกา ESP32-C3"]
  UI -- "fetch /api (คำสั่ง)" --> API
  UI -- "/api/data, /api/ai" --> DAPI
  HUB -- "ค่าสด/ผล/สถานะ (push)" --> UI
  API --> INT
  API --> COL
  INT --> LINK
  COL --> LINK
  SYNC --> LINK
  LINK -- "hello / HTTP" --> W
  W -- "UDP v/w/e" --> LINK
  LINK --> INT
  INT --> COL
  INT --> HUB
  INT --> DB
  COL --> DB
  COL --> CSV[("data/result_*.csv<br/>data/signals/")]
  DAPI --> CSV
  DAPI --> M[("data/model.json")]
  M --> SYNC
```

---

## 10. Watchdog 3 ชั้น + กล่องดำ — `src/sys/watchdog.cpp`

```mermaid
flowchart TD
  T1["ชั้น 1: Task WDT (8 s)<br/>ทุก task ต้องเรียก feed()"] --> R
  T2["ชั้น 2: Interrupt WDT (~0.3 s)<br/>interrupt ค้าง"] --> R
  T3["ชั้น 3: Hardware timer WDT (12 s)<br/>supervisor ตรวจ heartbeat ทุก task แล้วป้อน"] --> R
  R["เขียนกล่องดำ RTC_NOINIT:<br/>เหตุผล + task ที่ค้าง"] --> RS(["รีเซ็ต"])
  RS --> BOOT["บูตใหม่: อ่านกล่องดำ + core dump<br/>ไฟ LED กระพริบ 4 ครั้ง<br/>Studio/Serial แสดงสาเหตุ"]
```

สาธิตได้จริง: Serial `wdt twdt confirm` / หน้า "ระบบ & อุปกรณ์" ปุ่มสาธิต watchdog

---

## 11. พลังงานและ sleep — `src/sys/power.cpp`

```mermaid
stateDiagram-v2
  [*] --> Normal: CPU 160 MHz
  Normal --> ECO: eco on (CPU 80 MHz)
  ECO --> Normal: eco off
  Normal --> LightSleep: "พักเครื่อง" / กดปุ่มค้าง 5 s
  LightSleep --> Normal: กดปุ่ม BOOT (RAM อยู่ครบ)
  Normal --> DeepSleep: "หลับลึก" / ค้าง 10 s / ไม่มีใครใช้ (ถ้าเปิด auto)
  Normal --> DeepSleep: แบต < 3.3 V นาน 10 s (ป้องกันแบต)
  DeepSleep --> Normal: ตื่นด้วย timer (บูตใหม่)
  DeepSleep --> DeepSleep: standby: ตื่นเช็คทุก 20 s ยังไม่ใส่นาฬิกา
```

ค่าเริ่มต้น: หลับอัตโนมัติ = ปิด, ไม่หลับเองขณะเสียบ USB

---

## 12. แผนที่หน่วยความจำ (Flash 4 MB) และที่เก็บข้อมูล — `partitions_polygraph.csv`, `src/sys/storage.h`

```mermaid
flowchart LR
  subgraph FLASH["Flash 4 MB"]
    direction LR
    BL["bootloader<br/>+ partition table"] --- NVS["nvs 20 KB<br/>0x9000"] --- OTAD["otadata 8 KB<br/>0xE000"] --- A0["app0 1.75 MB<br/>0x10000"] --- A1["app1 1.75 MB<br/>0x1D0000"] --- FS["LittleFS 384 KB<br/>0x390000"] --- CD["coredump 64 KB<br/>0x3F0000"]
  end
  NVS --> N1["Preferences: ค่าตั้ง, โมเดล AI"]
  NVS --> N2["EEPROM emulation: สถิติสะสม + CRC32"]
  FS --> F1["events.log, results.csv, train.csv"]
  subgraph RTC["RTC memory (ในชิป)"]
    R1["RTC_DATA_ATTR: ตัวนับการตื่น<br/>(รอด deep sleep)"]
    R2["RTC_NOINIT_ATTR: กล่องดำ<br/>(รอด WDT/panic)"]
  end
```

---

## 13. OTA + rollback — `src/sys/ota.cpp`

```mermaid
flowchart TD
  U["อัปโหลด firmware.bin<br/>(หน้า /update หรือ Studio หรือ espota)"] --> W["เขียนลงช่องที่ไม่ได้ใช้ (app0 ↔ app1)"]
  W --> S["สลับ otadata → รีบูตเข้าเฟิร์มแวร์ใหม่"]
  S --> V{"ทำงานปกติครบ 20 วินาที?"}
  V -- "ใช่" --> OK["mark valid — ใช้ต่อ"]
  V -- "ล่ม/ค้างก่อน" --> RB["bootloader ย้อนกลับเวอร์ชันเดิม (rollback)"]
```

---

## 14. Serial console — `src/cli.cpp`

```mermaid
flowchart TD
  K["ตัวอักษรจาก USB"] --> E["echo กลับให้เห็น<br/>Backspace ลบได้"]
  E --> L{"Enter หรือ<br/>ส่งมาทั้งคำแล้วเงียบ 300 ms?"}
  L -- "ยัง" --> K
  L -- "ครบบรรทัด" --> X["execute(): แยกคำ"]
  X --> C["help · status · live · i2c · mode · ask · data · model ·<br/>sleep · wifi · boots · tasks · mem · flash · wdt ..."]
  C --> P["พิมพ์ผล + prompt PW>"]
```
