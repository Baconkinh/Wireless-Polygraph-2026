# TECHNIQUES.md — เทคนิคที่ใช้ในโครงงาน อยู่ไฟล์ไหน บรรทัดไหน ทำงานยังไง ทำไปเพื่ออะไร

> เอกสารนี้ทำไว้ตอบคำถามอาจารย์ว่า "เทคนิค X ที่เรียน อยู่ตรงไหนในโค้ด ทำงานยังไง ถ้าไม่ทำจะเกิดอะไร"
> เรียงตามหัวข้อรายวิชา (GPIO → Interrupt → RTOS → UART → SPI → I2C → ADC → DAC → Memory → WiFi → Bluetooth → Sleep/WDT)
> แล้วต่อด้วยการประมวลผลสัญญาณ, การตัดสินโกหก/จริง, AI และฝั่งคอม (Polygraph Studio)

## วิธีหาในโค้ดเร็วที่สุด

- ทุกจุดในโค้ดที่ใช้เทคนิคมีคอมเมนต์ขึ้นต้นด้วย **`[เทคนิค: ...]`** อยู่เหนือบรรทัดนั้น
  ใน VS Code กด **Ctrl+Shift+F** แล้วพิมพ์ `[เทคนิค:` จะได้รายการทั้งหมด (85 จุด) กดแล้วกระโดดไปที่บรรทัดได้เลย
- เลขบรรทัดในเอกสารนี้นับ ณ วันที่ 9 ต.ค. 2026 — ถ้าแก้โค้ดแล้วเลขเลื่อน ให้ค้นด้วยชื่อฟังก์ชันหรือคำว่า `[เทคนิค:` แทน
- ตารางครบทุกจุด (สร้างอัตโนมัติจากโค้ด) อยู่ท้ายเอกสาร: [ภาคผนวก ก](#appendix)
- path ในเอกสารนี้: `fw/` = `Wireless Polygraph/src/` (เฟิร์มแวร์นาฬิกา), `studio/` = `Polygraph-Studio/`

---

## 0. ภาพรวม: โค้ดทำงานยังไงตั้งแต่เปิดเครื่อง

`fw/main.cpp` → `setup()` (บรรทัด 80) ทำตามลำดับนี้ — ลำดับมีเหตุผล ถ้าสลับจะพัง:

| ลำดับ | บรรทัด | ทำอะไร | ทำไมต้องลำดับนี้ |
|---|---|---|---|
| 1 | 83 | `Serial.begin(115200)` | เปิด console ก่อน จะได้เห็นข้อความตั้งแต่ต้น |
| 2 | 93 | `power::handleWakeEarly()` | ตื่นจาก deep sleep เพื่อเช็คว่ามีคนใส่ไหม — ถ้าไม่มีหลับต่อเลย ไม่เปิด WiFi (ประหยัดสุด) |
| 3 | 96 | `wdt::captureBootInfo()` | อ่านสาเหตุรีเซ็ต + กล่องดำใน RTC ก่อนโค้ดอื่นเขียนทับ |
| 4 | 98 | `storage::begin()` | เปิด NVS / EEPROM / LittleFS (ค่าตั้งต้องมีก่อนเริ่มเซนเซอร์/WiFi) |
| 5 | 102 | `app::createSyncObjects()` | สร้าง mutex/queue/event group ก่อนมี task ใดใช้ |
| 6 | 110–114 | `sensors::begin()`, ตั้งค่า LieEngine, `mlrt::begin()` | เซนเซอร์ + โหลดโมเดล AI จาก NVS |
| 7 | 120–128 | กรณีรีเซ็ตเพราะไฟตก + `net::beginAp()` | เปิด WiFi แบบลดกระแสพีคถ้าเคยไฟตก |
| 8 | 131–135 | `wdt::beginTaskWdt()` → `tasks::startAll()` → `wdt::beginHwWdt()` | เปิด watchdog ก่อน แล้วสร้าง task แล้วค่อยเปิด timer watchdog |

หลังจากนั้น `loop()` (บรรทัด 155) ทำแค่ Serial console — งานจริงทั้งหมดอยู่ใน 6 FreeRTOS task:

```
 hardware timer 100 Hz ──ISR onTick()──► sensorTask (P5) ──frameQueue 5 Hz──► engineTask (P4) ──► LieEngine / โมเดล AI
                                              │                                     │
                                              ├─waveQueue──► telemetryTask (P3) ◄───┴─eventQueue (ผลคำถาม)
                                              │                  │ UDP 4210 → Polygraph Studio บนคอม
 httpTask (P2): หน้าเว็บ + REST API + OTA     uiTask (P2): LED + ปุ่ม
 supervisorTask (P1): เขียนแฟลช (logQueue), ป้อน timer watchdog, เช็คแบต, sleep, ยืนยัน OTA
```

---

## 1. GPIO + PWM (Digital I/O)

### 1.1 ปุ่ม BOOT — input + pull-up + software debounce
- **ที่ไหน:** `fw/sys/ui.cpp` → `begin()` บรรทัด 214 `pinMode(PIN_BUTTON, INPUT_PULLUP)` และ `pollButton()` บรรทัด 174
- **ทำงานยังไง:** ปุ่มต่อลง GND + pull-up ภายใน → ปล่อย = HIGH, กด = LOW
  หน้าสัมผัสปุ่มเด้ง (bounce) หลายครั้งใน ~ms แรก จึงจำเวลาที่ค่าเปลี่ยนล่าสุด (`s_rawChange`)
  แล้วยอมรับค่าใหม่เมื่อนิ่งเกิน `DEBOUNCE_MS = 30 ms` (บรรทัด 20)
  จากนั้นแยกท่าทาง: กดสั้น 1/2/3 ครั้ง (ห่างกันไม่เกิน `MULTI_CLICK_MS = 400 ms`), กดค้าง 2 s / 5 s / 10 s (`handleHold()`)
- **ทำไม:** ไม่ debounce = กด 1 ครั้งนับเป็นหลายครั้ง สั่งงานผิด
- **ทำไมไม่ใช้ interrupt กับปุ่ม:** uiTask วนทุก 20 ms อยู่แล้ว อ่านแบบ polling ง่ายกว่าและไม่ต้องกัน bounce ใน ISR
  (interrupt ใช้กับงานที่ต้องตรงเวลาจริง คือ timer สุ่มสัญญาณ — ข้อ 2)
- **ใช้ปุ่มเป็นแหล่งปลุก:** `fw/sys/power.cpp` บรรทัด ~66 `gpio_wakeup_enable(PIN_BUTTON, GPIO_INTR_LOW_LEVEL)` (ข้อ 12)

### 1.2 LED — PWM ด้วย LEDC
- **ที่ไหน:** `fw/sys/ui.cpp` บรรทัด 216 `ledcAttach(PIN_LED, 5000, 8)` (core 3.x) / `ledcSetup + ledcAttachPin` (core 2.x), `ledWrite()` บรรทัด 39
- **ทำงานยังไง:** PWM 5 kHz ความละเอียด 8 บิต → duty 0–255 = ความสว่าง; `computeLed()` (บรรทัด 49) เลือกรูปแบบ:
  กระพริบตามจังหวะหัวใจ, ติดค้างตามผล (โกหก/จริง), หายใจช้า ๆ ตอนรอ, กระพริบ N ครั้งบอกสาเหตุการบูต
- **ทำไม PWM ไม่ใช่ digitalWrite:** ได้หลายระดับความสว่าง (หายใจ/หรี่) — และใช้แทน DAC ที่ ESP32-C3 ไม่มี (ข้อ 9)

### 1.3 GPIO แบบ open-drain (bit-banging) — กู้บัส I2C
- **ที่ไหน:** `fw/drivers/i2c_bus.cpp` → `recover()` บรรทัด 26 ดูข้อ 7.3

---

## 2. Interrupt + Hardware Timer

### 2.1 Timer interrupt 100 Hz = นาฬิกาของการสุ่มสัญญาณ
- **ที่ไหน:** `fw/tasks.cpp` → `startAll()` บรรทัด 312 (ตั้ง timer) และ ISR `onTick()` บรรทัด 41
- **ตั้งค่า (core 2.x):** `timerBegin(0, 80, true)` → APB 80 MHz ÷ prescaler 80 = 1 MHz (1 tick = 1 µs)
  `timerAlarmWrite(timer, 10000, true)` → ครบ 10,000 µs = 10 ms เกิด interrupt, `true` = auto-reload (วนเอง)
  (core 3.x ใช้ `timerBegin(1000000)` + `timerAlarm(...)` ผลเท่ากัน — มี `#if` รองรับทั้งสองเวอร์ชัน)
- **ใน ISR ทำอะไร:** แค่ `vTaskNotifyGiveFromISR()` ปลุก sensorTask แล้ว `portYIELD_FROM_ISR()` ให้สลับไป task นั้นทันที
- **ทำไม ISR ต้องสั้น:** ขณะอยู่ใน ISR interrupt อื่น (รวม WiFi) รอ; ห้ามใช้ Serial/I2C/mutex ใน ISR
  → **deferred interrupt processing**: ISR ส่งสัญญาณ แล้วงานหนัก (อ่าน I2C, กรองสัญญาณ) ไปทำใน task priority สูง
- **`IRAM_ATTR`:** วางโค้ด ISR ไว้ใน RAM — ตอนเขียนแฟลช (LittleFS/NVS) cache ของแฟลชถูกปิด
  ถ้า ISR อยู่ในแฟลชแล้ว interrupt เกิดตอนนั้น = crash
- **ทำไมไม่ใช้ `delay()`/`vTaskDelay()`:** คาบจะ = เวลาทำงาน + delay → เพี้ยนไปตามภาระ; timer ฮาร์ดแวร์ตรงเวลาเสมอ
  ซึ่งสำคัญกับการกรองสัญญาณ (สูตร filter ใช้ fs คงที่)
- **ตรวจว่าพลาดไหม:** `sensorTask` บรรทัด 53 `ulTaskNotifyTake(..., 50 ms)` ถ้าไม่มีสัญญาณใน 50 ms นับ `s_missed`
  (ดูได้ใน `/api/system`)

### 2.2 Timer interrupt ตัวที่ 2 = watchdog แบบฮาร์ดแวร์ (ข้อ 12.5)
- `fw/sys/watchdog.cpp` → `beginHwWdt()` ใช้ timer 1, alarm 12 s ไม่ auto-reload; ISR `hwWdtIsr()` บรรทัด 44

---

## 3. RTOS (FreeRTOS) — task, priority, queue, mutex, event group, notification

### 3.1 Task 6 ตัว + priority
- **ที่ไหน:** `fw/tasks.cpp` → `startAll()` บรรทัด 302–307, ค่าคงที่ใน `fw/config.h` บรรทัด 87–99

| task | priority | stack | ทำอะไร | ฟังก์ชัน |
|---|---|---|---|---|
| sensor | 5 (สูงสุด) | 4096 | อ่านเซนเซอร์ทุก 10 ms | `tasks.cpp` `sensorTask()` |
| engine | 4 | 6144 | LieEngine/AI ตัดสิน 5 Hz | `tasks.cpp` `engineTask()` |
| telemetry | 3 | 4096 | ส่ง UDP | `net/telemetry.cpp` `task()` บรรทัด 249 |
| http | 2 | 8192 | หน้าเว็บ, REST, OTA | `net/web_server.cpp` `task()` |
| ui | 2 | 3072 | LED + ปุ่ม | `sys/ui.cpp` `task()` บรรทัด 225 |
| supervisor | 1 (ต่ำสุด) | 6144 | เขียนแฟลช, watchdog, แบต, sleep | `tasks.cpp` `supervisorTask()` |

- **Preemptive scheduling:** task priority สูงที่พร้อมทำงานแย่ง CPU ได้ทันที
  → ระหว่างส่งหน้าเว็บ 57 KB (http, P2) การสุ่มสัญญาณ (P5) ยังตรงเวลา
- **ESP32-C3 มี core เดียว** จึงใช้ `xTaskCreate` (ไม่ pin core) — ทุกอย่างอาศัย priority

### 3.2 Queue — ส่งข้อมูลข้าม task (producer–consumer)
- **ที่ไหน:** สร้างที่ `fw/app.cpp` บรรทัด 45–48

| queue | ขนาด | ผู้ส่ง → ผู้รับ | ใช้ทำอะไร |
|---|---|---|---|
| frameQueue | 10 × Vitals | sensor → engine | เฟรม 5 Hz (เผื่อ engine ช้าได้ 2 s) |
| waveQueue | 8 × WaveChunk | sensor → telemetry | คลื่นชีพจร 100 Hz สำหรับกราฟ |
| eventQueue | 6 × EventMsg | ทุก task → telemetry | เหตุการณ์ (ผลคำถาม, baseline เสร็จ, แบตต่ำ) |
| logQueue | 16 × LogMsg | ทุก task → supervisor | ข้อความที่จะเขียนลงแฟลช |

- **ทำไมไม่ใช้ตัวแปร global:** queue คัดลอกค่า + บล็อกรอได้ (`xQueueReceive(..., timeout)` บรรทัด 121 ใน tasks.cpp)
  ผู้รับไม่ต้องวนเช็คเปลือง CPU และไม่มีปัญหาอ่านค่าระหว่างที่อีก task เขียนอยู่

### 3.3 Mutex — กัน race condition
- **ที่ไหน:** `fw/app.cpp` บรรทัด 40–42 สร้าง 3 ตัว

| mutex | ป้องกันอะไร | ตัวอย่างจุดใช้ |
|---|---|---|
| liveMutex | ค่าสดล่าสุด (struct Vitals) | `app.cpp` `setVitals()` บรรทัด 55 / `getVitals()` |
| engineMutex | LieEngine + โมเดล AI | `tasks.cpp` บรรทัด 139 (engineTask), `web_server.cpp` `hQuestion()` 318, `ml_runtime.cpp` `install()` / `setUseModel()` |
| fsMutex | ระบบไฟล์ LittleFS | `storage.cpp` `lockFs()`/`unlockFs()`, `web_server.cpp` `hMlData()` 565 |

- **หลักที่ใช้:** ถือ mutex สั้นที่สุด (คัดลอกค่าแล้วปล่อย) → task อื่นไม่ต้องรอนาน
  ตัวอย่างที่สำคัญ: `hMlData()` ส่งไฟล์ทีละ 1 KB และ **ไม่ถือ fsMutex ระหว่างส่ง WiFi**
  — เวอร์ชันก่อนถือไว้ตลอด ทำให้ supervisor รอ mutex จน Task WDT รีเซ็ตเครื่อง (บั๊กที่เคยเจอจริง)

### 3.4 Event group — ธงสถานะรวม
- **ที่ไหน:** `fw/app.cpp` บรรทัด 50 สร้าง, บิตนิยามใน `fw/app.h` (`EV_WIFI_UP`, `EV_USB`, `EV_ECO`, `EV_ENGINE_BUSY`,
  `EV_TIME_SYNCED`, `EV_OTA_ACTIVE`, `EV_LOW_BATT`, `EV_CLIENT` ...)
- **ทำไม:** หลาย task ต้องรู้สถานะเดียวกัน (เช่น telemetry ต้องรู้ว่า WiFi ขึ้นไหม, power ต้องรู้ว่าเสียบ USB ไหมก่อนหลับ)
  อ่าน/ตั้งบิตเป็น atomic ไม่ต้องใช้ mutex

### 3.5 Task notification — ISR ปลุก task
- `tasks.cpp` บรรทัด 41 (ส่ง) / 53 (รับ) — เบาและเร็วกว่า semaphore สำหรับการปลุก task เดียว

### 3.6 Single-writer pattern สำหรับแฟลช
- `tasks.cpp` บรรทัด 224: ทุก task แค่ `xQueueSend(logQueue, ...)` แต่ **supervisor เขียนแฟลชคนเดียว**
  เพราะการเขียนแฟลชบล็อกได้นับสิบ ms ถ้าให้ sensorTask เขียนเอง การสุ่มสัญญาณจะสะดุด

---

## 4. UART (Serial)

- **ที่ไหน:** `fw/main.cpp` บรรทัด 83 `Serial.begin(115200)`; console อยู่ `fw/cli.cpp` → `poll()` อ่านทีละตัวอักษร,
  `execute()` บรรทัด 386 แยกคำสั่ง (`help`, `status`, `boots`, `tasks`, `flash`, `wifi low`, `sleep`, `baseline`, `ask` ...)
- **ข้อเท็จจริงของบอร์ด:** ESP32-C3 SuperMini ไม่มีชิป USB-UART แยก ใช้ **USB-CDC ในชิป** (`ARDUINO_USB_CDC_ON_BOOT=1` ใน `platformio.ini`)
  แต่ฝั่งโปรแกรมใช้ API `Serial` เหมือน UART ปกติ
- **บั๊กที่แก้ (บรรทัด 88):** `Serial.setTxTimeoutMs(50)` — ถ้าตั้ง 0 ใน core 2.0.17 ข้อความตอบกลับหายหมด (เคยพิมพ์คำสั่งแล้วเงียบ)
- **ทำไมต้องมี console:** ดูสาเหตุรีเซ็ต (`boots`) / ตั้ง WiFi ต่ำ ได้แม้ WiFi ไม่ขึ้น (ใช้แก้ปัญหาแบตจริง)

## 5. SPI — ไม่ได้ใช้ (และเหตุผล)

- เซนเซอร์ทั้งสองตัว (MAX30102, MPU6050) มีแต่ขา **I2C** และ I2C ใช้สายแค่ 2 เส้นต่อได้หลายชิป เหมาะกับนาฬิกาที่ขาน้อย
- ความเร็วที่ต้องการ: PPG 100 sample/s × 6 ไบต์ ≈ 0.6 KB/s ซึ่ง I2C 400 kHz รองรับเหลือเฟือ จึงไม่จำเป็นต้องใช้ SPI

## 6. (เผื่อถูกถาม) ทำไมไม่ใช้ Bluetooth — ดูข้อ 11

---

## 7. I2C

### 7.1 ตั้งบัส
- `fw/drivers/i2c_bus.cpp` บรรทัด 19: `Wire.begin(SDA=GPIO6, SCL=GPIO7, 400 kHz)` + `Wire.setTimeOut()` กันค้าง
- 2 ชิปบนบัสเดียวกัน แยกด้วย address: MAX30102 = `0x57`, MPU6050 = `0x68` (`fw/config.h` บรรทัด 31–32)

### 7.2 อ่านรีจิสเตอร์ + burst read + FIFO ของเซนเซอร์
- **MAX30102** `fw/drivers/max30102.cpp` → `read()` บรรทัด 98
  1. อ่าน 3 รีจิสเตอร์ติดกันทีเดียว: `FIFO_WR_PTR`, `OVF_COUNTER`, `FIFO_RD_PTR`
  2. จำนวนที่รอ = (WR − RD) mod 32; ถ้า overflow แปลว่าอ่านไม่ทัน (นับไว้ใน `ovf_`)
  3. อ่าน `FIFO_DATA` แบบ burst ทีละหลาย sample (6 ไบต์ = RED 3 + IR 3, big-endian, ใช้ 18 บิตล่าง)
  - **ทำไม FIFO:** ชิปเก็บ sample เองได้ 32 ตัว MCU ไม่ต้องอ่านตรงเวลาทุก sample (ทนการสะดุดได้ ~0.3 s)
- **MPU6050/6500** `fw/drivers/mpu6050.cpp` → `readAccel()` บรรทัด 66: อ่าน 6 ไบต์จาก `0x3B` แล้วประกอบ `(b[0]<<8)|b[1]` เป็น int16
  - ตรวจ `WHO_AM_I` แยกรุ่น (บอร์ดจริงเป็น MPU6500 ตัวเลียนแบบ — โค้ดรองรับทั้งสองรุ่น)

### 7.3 กู้บัส I2C ที่ค้าง (fault tolerance)
- `fw/drivers/i2c_bus.cpp` → `recover()` บรรทัด 26; ถูกเรียกจาก `fw/sensors.cpp` → `checkHealth()` บรรทัด 158
- **อาการ:** สายหลวม/ไฟกระตุกกลางการอ่าน → ชิปค้างถือ SDA = LOW รอ clock ต่อ บัสใช้ไม่ได้ทั้งเส้น
- **วิธีแก้:** ปิด Wire → ตั้ง SCL เป็น open-drain แล้วสลับ SCL เอง (bit-banging) สูงสุด 9 ลูกจนชิปปล่อย SDA
  → สร้าง STOP condition (SDA LOW→HIGH ขณะ SCL HIGH) → เปิด Wire ใหม่ → init ชิปใหม่
- เว้นระยะ 10 s ระหว่างความพยายาม กันวนรัว

## 8. ADC

### 8.1 ตั้งค่า
- `fw/drivers/analog_in.cpp` บรรทัด 16: ความละเอียด 12 บิต, attenuation 11 dB ทั้ง 3 ขา (NTC=GPIO0, GSR=GPIO1, VBAT=GPIO4)
- ใช้ ADC1 เท่านั้น (ADC2 ใช้ร่วมกับ WiFi อ่านไม่ได้ตอนเปิด WiFi)

### 8.2 อ่านแบบปรับเทียบ + oversampling
- `readMv()` บรรทัด 25: `analogReadMilliVolts()` คืนค่า mV ที่ปรับด้วยค่าปรับเทียบในชิป (eFuse) แทนตัวเลขดิบ 0–4095
  และเฉลี่ย 16 ครั้ง (`ADC_OVERSAMPLE`) → noise แบบสุ่มลดลงประมาณ √16 = 4 เท่า
- อ่าน ADC ที่ 10 Hz (`ADC_EVERY_TICK = 10`) พอสำหรับ GSR/อุณหภูมิ/แบต ซึ่งเปลี่ยนช้า

### 8.3 วงจรแบ่งแรงดัน → หน่วยจริง (`fw/drivers/sensor_math.cpp`)
- **NTC** (บรรทัด 14): R<sub>ntc</sub> = R1·(Vcc − V)/V แล้วสมการ Beta: 1/T = 1/298.15 + ln(R/R25)/β → °C
  ค่า V ใกล้ 0 หรือใกล้ Vcc = สายขาด/ลัด → คืน NaN (ไม่เอาค่าหลอกไปใช้)
- **GSR** (บรรทัด 24): หาความต้านทานผิวจากแรงดันกลางวงจร แล้ว G = 1/R → µS; ไม่แตะผิว = 0, ลัดวงจร = NaN
- **แบต**: แรงดันที่ GPIO4 × 2 (R4 = R5 = 100 kΩ) → mV → % (`batteryPercent()`) — ต่ำกว่า 2.5 V ถือว่าไม่มีแบต
  (นี่คือเหตุผลที่ Studio ขึ้น "วัดแบตไม่ได้" ถ้าสายวัดแบตไม่ต่อ ดูคู่มือ 7.3.1)

## 9. DAC — ไม่ได้ใช้

- **ข้อเท็จจริง:** ESP32-C3 ไม่มี DAC ในชิป (ต่างจาก ESP32 รุ่นแรกที่มี 2 ช่อง)
- งานที่ต้องการ "ระดับแรงดัน" ในโครงงานมีแค่ความสว่าง LED จึงใช้ **PWM (LEDC)** แทน (ข้อ 1.2)

---

## 10. Memory (Flash, NVS, EEPROM, File system, RTC memory)

### 10.1 แผนที่แฟลช 4 MB (partition table)
- **ที่ไหน:** `Wireless Polygraph/partitions_polygraph.csv` (อ้างใน `platformio.ini`)

| ชื่อ | ขนาด | ใช้ทำอะไร |
|---|---|---|
| nvs | 20 KB | ค่าตั้ง (Preferences) + โมเดล AI + EEPROM emulation |
| otadata | 8 KB | บอกว่าบูตจาก app0 หรือ app1 + สถานะ rollback |
| app0 / app1 | 1.75 MB × 2 | เฟิร์มแวร์ 2 ช่อง สำหรับ OTA (เขียนช่องที่ไม่ได้ใช้) |
| spiffs | 384 KB | LittleFS: log, ผล, ข้อมูลเทรน |
| coredump | 64 KB | ภาพหน่วยความจำตอนล่ม (panic) |

### 10.2 NVS ผ่าน Preferences — ค่าตั้ง + โมเดล
- `fw/sys/storage.cpp` บรรทัด 107 `prefs.begin(NVS_NS)`, `loadSettings()` / `saveSettings()`
  เขียนเฉพาะ key ที่ค่าเปลี่ยนจริง (ถนอมแฟลช)
- โมเดล AI เก็บเป็น blob ใน namespace `"ml"` key `"model"` (`loadModel()`/`saveModel()`) + ตรวจ CRC32 ก่อนใช้
- ตัวเลือก "ตัดสินด้วย AI/สูตร" key `"use"` บรรทัด 461 (เพิ่ม 9 ต.ค.)
- **ทำไม NVS:** เป็น key-value ที่ทนไฟดับและกระจายการเขียน (wear leveling) ให้เอง

### 10.3 EEPROM emulation — สถิติสะสม
- `storage.cpp` บรรทัด 112: struct `Stats` ก้อนเดียว (จำนวนบูต, คำถาม, ประวัติสาเหตุรีเซ็ต 8 ครั้ง ...)
  ตรวจ magic + version + size + CRC ทุกครั้งที่อ่าน ไม่ผ่าน = เริ่มนับใหม่ (กันข้อมูลขยะ)
- commit ทุก 10 นาที (`tasks.cpp` บรรทัด 286) — ไม่ commit ทุกครั้งที่ค่าเปลี่ยน เพราะแฟลชเขียนได้จำกัดรอบ

### 10.4 LittleFS — ไฟล์บนแฟลช
- `storage.cpp` บรรทัด 125 `LittleFS.begin(true)` (format เองครั้งแรก)
- ไฟล์: `/events.log` (24 KB แล้วหมุน), `/results.csv` (48 KB แล้วหมุน), `/train.csv` (ข้อมูลเทรน สูงสุด 180 KB ไม่หมุนทิ้ง)
- **Log rotation** บรรทัด 267 `rotateIfBig()`: เกินขนาด → เปลี่ยนชื่อเป็น `.1` แล้วเริ่มใหม่
- **ลบรายแถว** `deleteTrainRow()`: คัดลอกไป `/train.tmp` ยกเว้นแถวนั้น แล้ว `rename` ทับ (ไม่เสียครึ่งทางถ้าไฟดับ)
- ป้องกันการเข้าถึงพร้อมกันด้วย `fsMutex` (ข้อ 3.3)

### 10.5 RTC memory — รอดจาก deep sleep / รีเซ็ต
- `storage.cpp` บรรทัด 20–28
  - `RTC_DATA_ATTR` (ตัวนับการตื่น, เหตุผลที่หลับ): อยู่รอดระหว่าง deep sleep แต่ถูกตั้งค่าใหม่เมื่อบูตปกติ
  - `RTC_NOINIT_ATTR rtcRecord` = **กล่องดำ**: ไม่ถูกแตะตอนบูต จึงรอดจาก watchdog/panic/software reset
    ISR ของ watchdog เขียนชื่อ task ที่ค้างลงที่นี่ก่อนรีเซ็ต แล้วบูตรอบหน้าอ่านมาแสดง

### 10.6 PROGMEM — หน้าเว็บอยู่ในแฟลช
- `fw/net/web_page.h`: หน้าเว็บทั้งหน้า (~57 KB) เป็น `static const char INDEX_HTML[] PROGMEM`
  ส่งด้วย `server.send_P()` (`web_server.cpp` บรรทัด 274) อ่านตรงจากแฟลชไม่ต้องคัดลอกเข้า RAM (RAM มีแค่ ~400 KB)

### 10.7 Core dump
- `fw/sys/watchdog.cpp` → `captureBootInfo()` บรรทัด 61: ถ้ามี core dump ในพาร์ทิชัน อ่านสรุป (ชื่อ task, PC) แล้วลบ
  → แสดงในหน้า "ระบบ & อุปกรณ์" ว่าล่มที่ task ไหน

---

## 11. WiFi (+ เหตุผลที่ไม่ใช้ Bluetooth)

### 11.1 SoftAP — นาฬิกาเป็น access point เอง
- `fw/net/wifi_ap.cpp` → `beginAp()` บรรทัด 51: `WiFi.softAP("Polygraph-Watch", "polygraph123", ch, 0, max clients)` → IP 192.168.4.1
- **ทำไม AP ไม่ใช่ STA:** ใช้ที่ไหนก็ได้ไม่ต้องมีเราเตอร์/เน็ต (ห้องสอบ) มือถือ/คอมต่อตรง
- **กำลังส่ง (TX power):** ตั้งก่อนและหลังเปิด AP (บางเวอร์ชันรีเซ็ตค่า) 3 ระดับ — ลดกระแสพีค
  ซึ่งเป็นสาเหตุหลักที่สงสัยว่า WiFi ไม่ขึ้นตอนใช้แบต (คู่มือส่วน 7)
- **Event callback** `onWifiEvent()` บรรทัด 36: มีเครื่องเข้า/ออก → log + ต่อเวลาก่อนหลับอัตโนมัติ

### 11.2 HTTP REST API + หน้าเว็บ
- `fw/net/web_server.cpp` → `begin()` บรรทัด 725 ลงทะเบียน endpoint ทั้งหมด, `task()` วน `server.handleClient()`
- ตัวอย่าง: `GET /api/live` (`hLive()` บรรทัด 285), `POST /api/lie/baseline` (`hBaseline()` 302),
  `POST /api/lie/question` (`hQuestion()` 318), `POST /api/ml/model` (`hMlModel()` 534),
  `POST /api/ml/feedback` (`hMlFeedback()` 614), `POST /api/ml/use` (`hMlUse()` 645)
- JSON สร้างด้วย `fw/net/json_writer.h` (เขียนเอง ไม่ใช้ ArduinoJson) ใช้ได้ทั้งหน้าเว็บมือถือและ Studio
- `enableCORS(true)` ให้หน้าเว็บจากเครื่องอื่นเรียกได้

### 11.3 UDP telemetry — ส่งค่าสดให้คอม
- `fw/net/telemetry.cpp`: คอมส่ง `"hello"` → `handleHello()` บรรทัด 108 จำ IP/พอร์ต (สูงสุดหลายเครื่อง)
  → `sendVitals()` บรรทัด 189 ส่ง JSON 5 ครั้ง/วินาที, คลื่นชีพจร 100 Hz เป็นก้อน, เหตุการณ์ส่ง 2 ครั้งกันหาย
  → `expireClients()` บรรทัด 234 ไม่ได้ hello เกิน 10 s = เลิกส่ง
- **ทำไม UDP:** ไม่ต้องสร้าง/รักษาการเชื่อมต่อ หน่วงต่ำ ค่าสดหายบางแพ็กเก็ตไม่เป็นไร
  ส่วน "ผลคำถาม" ที่ห้ามหาย Studio ดึงซ้ำผ่าน HTTP (TCP) ทุกครั้งที่เลข `rv` เปลี่ยน (`studio/backend/watch_link.py` บรรทัด 202)
- บัฟเฟอร์บน stack (`char b[600]`) ไม่ใช้ `String`/heap → ไม่เกิด heap fragmentation เมื่อรันนาน

### 11.4 OTA (อัปเดตเฟิร์มแวร์ไร้สาย)
- `fw/sys/ota.cpp`: 2 ทาง — (1) หน้าเว็บ `/update` (`handleUpload()` บรรทัด 114, มี Basic auth)
  (2) ArduinoOTA/espota จาก PlatformIO (บรรทัด 173)
- **Rollback:** `serviceVerify()` บรรทัด 215 — เฟิร์มแวร์ใหม่ต้องบูตผ่าน 20 s + WiFi ขึ้น + task หลักยังรายงานตัว
  จึงเรียก `esp_ota_mark_app_valid_cancel_rollback()` ถ้าล่มก่อนนั้น bootloader กลับไปใช้ app เดิม

### 11.5 Bluetooth — ไม่ได้ใช้
- ข้อมูลต้องไปทั้งมือถือ (หน้าเว็บ) และคอม (Studio) พร้อมกัน + ส่งคลื่น 100 Hz — WiFi ทำได้ด้วยเบราว์เซอร์อย่างเดียว
  ไม่ต้องลงแอป; การเปิดทั้ง WiFi + BLE บน C3 (วิทยุตัวเดียว) จะกินแรมและกระแสเพิ่ม

---

## 12. Sleep modes + Watchdog (ตรงบทสุดท้ายของวิชา)

### 12.1 Light sleep (ปลุกด้วยปุ่ม)
- `fw/sys/power.cpp` → `doLightSleep()` บรรทัด 52 (สั่งได้จากปุ่มกดค้าง 5 s / หน้าเว็บ)
- ลำดับ: เขียน log ค้างลงแฟลช → ปิด LED → รอปล่อยปุ่ม → **หยุด HW watchdog** → ปิด WiFi AP
  → `gpio_wakeup_enable(GPIO9, LOW)` + `esp_sleep_enable_gpio_wakeup()` (+ timer ถ้ากำหนด) → `esp_light_sleep_start()`
  → ตื่น: ปิดแหล่งปลุก, เปิด AP ใหม่, แจ้ง web server เปิด socket ใหม่
- **RAM อยู่ครบ** → baseline/ผลไม่หาย ต่างจาก deep sleep
- **ทำไมต้องหยุด HW watchdog:** ระหว่างหลับไม่มีใครป้อน ถ้าไม่หยุดจะรีเซ็ตตอนตื่น

### 12.2 Deep sleep + timer wake-up + RTC memory
- `doDeepSleep()` บรรทัด 99: เขียน log, สั่งเซนเซอร์โหมดประหยัด (MAX30102 shutdown ~0.7 µA, MPU sleep) ผ่าน I2C,
  จดเหตุผลใน `RTC_DATA_ATTR` → `esp_sleep_enable_timer_wakeup()` + `esp_deep_sleep_start()`
- ตื่น = บูตใหม่ → `handleWakeEarly()` บรรทัด 121 อ่านเหตุผล:
  - standby: เช็คแวบเดียวว่ามีผิวแตะเซนเซอร์ไหม (`sensors::quickContactCheck()` 350 ms) ไม่มี → หลับต่อทันที **ไม่เปิด WiFi**
  - แบตต่ำ: วัดแบต ยังไม่ชาร์จ → หลับต่อ 5 นาที

### 12.3 Power optimization อื่น
- **ECO / dynamic frequency scaling:** `setEco()` บรรทัด 152 CPU 160 → 80 MHz (`setCpuFrequencyMhz`)
- **Auto-standby:** `service()` บรรทัด 185 ไม่มีใครใช้เกินเวลาที่ตั้ง (และไม่ได้เสียบ USB/ไม่ได้ OTA) → deep sleep
- **ป้องกันแบต Li-Po:** ต่ำกว่า 3.45 V ต่อเนื่อง 10 s เตือน, ต่ำกว่า 3.3 V ต่อเนื่อง 10 s → deep sleep (hysteresis 50 mV กันกระพือ)
- **บูตหลังไฟตก:** `main.cpp` บรรทัด 120 ลด CPU + TX power ก่อนเปิด WiFi

### 12.4 Task Watchdog Timer (TWDT) — ชั้นที่ 1
- `fw/sys/watchdog.cpp` → `beginTaskWdt()` บรรทัด 150: timeout 8 s, `trigger_panic = true`
- ทุก task เรียก `wdt::subscribe()` ตอนเริ่ม และ `wdt::feed()` (= `esp_task_wdt_reset()`) ทุกรอบลูป
- task ใดไม่ feed ภายใน 8 s → panic → บันทึก core dump → รีเซ็ต

### 12.5 Timer-interrupt watchdog + heartbeat — ชั้นที่ 2
- **ป้อน (kick the dog):** `feedHw()` บรรทัด 183 = `timerWrite(timer, 0)` เริ่มนับใหม่
- **ใครป้อน:** supervisor (`tasks.cpp` บรรทัด 251) ป้อนก็ต่อเมื่อ **ทุก task** ยังส่ง heartbeat ภายใน 6.5 s
  ถ้ามี task เงียบ → ไม่ป้อน + จดชื่อ task ต้องสงสัย
- **ครบ 12 s ไม่มีใครป้อน:** ISR `hwWdtIsr()` บรรทัด 44 เขียนกล่องดำ (`RTC_NOINIT`) แล้ว `abort()`
  (ไม่ใช้ `esp_restart()` ใน ISR เพราะมันเรียก `esp_wifi_stop()` ซึ่งรอ mutex = ห้ามใน ISR)
- **ทำไมมี 2 ชั้น:** TWDT จับ task ที่ค้างในลูปตัวเอง; ชั้นที่ 2 จับกรณีที่ระบบยังเดินแต่ "ทำงานไม่ครบ"
  และบอกได้ว่า task ไหนเป็นต้นเหตุ
- **สาธิตได้จริง:** หน้า "ระบบ & อุปกรณ์" หรือ `POST /api/demo?type=twdt|hwwdt|panic|intwdt&confirm=yes`
  (`crashNow()` บรรทัด 238) แล้วบูตรอบหน้าจะแสดงสาเหตุ + ชื่อ task

### 12.6 Post-mortem: รู้ว่ารีบูตเพราะอะไร
- `captureBootInfo()` บรรทัด 61: `esp_reset_reason()` (POWERON / BROWNOUT / TASK_WDT / INT_WDT / PANIC / SW ...)
  + `esp_sleep_get_wakeup_cause()` + กล่องดำ RTC + สรุป core dump
- เก็บประวัติ 8 ครั้งล่าสุดใน EEPROM → Serial `boots` หรือหน้า "ระบบ"
- LED ตอนบูตบอกสาเหตุได้แม้ไม่มีสาย: 2 ครั้ง = ปกติ, 4 = watchdog/ล่ม, 6 ครั้งรัว = ไฟตก (`main.cpp` บรรทัด 108)

---

## 13. การประมวลผลสัญญาณ (DSP)

| เทคนิค | ที่ไหน | สูตร/วิธี | ทำไม |
|---|---|---|---|
| EMA (low-pass อันดับ 1) | `fw/dsp/filters.h` บรรทัด 23 | y += α(x − y), α = 1 − e^(−1/(fs·τ)) | เรียบค่าโดยใช้ RAM 1 ค่า (อุณหภูมิ, แบต, tonic ของ GSR) |
| Biquad IIR อันดับ 2 | `filters.h` บรรทัด 44 | สัมประสิทธิ์จาก RBJ cookbook, Q = 0.7071 (Butterworth) | low-pass/high-pass ที่ชันกว่า EMA ไม่มีริปเปิล |
| PPG → ชีพจร | `fw/dsp/ppg.cpp` `push()` บรรทัด 81 | ตรวจแตะผิวแบบ hysteresis → LPF 6 Hz → SSF (ผลรวมความชันขาขึ้น 120 ms) → เกณฑ์ปรับตัว → จุดยอด → IBI → bpm | หาจังหวะหัวใจทนต่อ DC ลอย/การหายใจ |
| HRV (RMSSD) | `ppg.cpp` `recompute()` บรรทัด 241 | √(mean((IBIᵢ − IBIᵢ₋₁)²)) + กรอง IBI ผิดปกติด้วย median | ตัวชี้วัดระบบประสาทอัตโนมัติ |
| EDA tonic/phasic | `fw/dsp/eda.cpp` `push()` บรรทัด 33 | tonic = EMA 15 s, phasic = ค่า − tonic, นับยอด SCR | แยก "ยอดตื่นเต้นชั่วคราว" ออกจากระดับพื้นที่ลอยช้า ๆ |
| แยกมือสั่น/ขยับตัว | `fw/dsp/motion.cpp` `push()` บรรทัด 28 | \|a\| → band-pass 3–15 Hz (สั่น) / 0.3–3 Hz (ขยับ) → EMA ของกำลังสอง | มือสั่น = สัญญาณเครียด, ขยับแรง = ข้อนั้นใช้ไม่ได้ |
| Multi-rate sampling | `fw/sensors.cpp` `tick()` บรรทัด 214 | 100 Hz (PPG, IMU), 10 Hz (ADC), 5 Hz (เฟรมตัดสิน) | ใช้ CPU เท่าที่สัญญาณแต่ละตัวต้องการ |

## 14. การตัดสินโกหก/จริง (2 แบบ — สูตรเต็มในคู่มือส่วน 5.0)

- **สรุปตอบสนอง 1 ข้อ:** `fw/lie/lie_engine.cpp` → `computeFeatures()` บรรทัด 187 (3 s ก่อนถาม vs 12 s หลังถาม)
- **z-score เทียบการแกว่งเอง:** `finishBaseline()` บรรทัด 280 (Welford mean/SD + null distribution จากหน้าต่างเลื่อนใน baseline)
  → `finishQuestion()` บรรทัด 353 คิด z ของ 5 สัญญาณ
- **แบบที่ 1 สูตรมาตรฐาน:** `scoreOf()` บรรทัด 265 + `r.pLie = sigmoid(k*(score − s0))` บรรทัด 385
  + ปรับเกณฑ์ตามคน `recalibrate()` บรรทัด 440
- **แบบที่ 2 โมเดล AI:** ถ้าเลือก AI และมีโมเดล `scorer_` (บรรทัด 388) เรียก `fw/sys/ml_runtime.cpp` `scorer()` บรรทัด 23
  → `fw/lie/ml_model.cpp` `predict()` บรรทัด 83 (Logistic Regression บนนาฬิกา)
  — ออกแบบเป็น function pointer (strategy) จึงสลับได้โดยไม่แก้ LieEngine
- **เลือกแบบ:** `POST /api/ml/use?on=1|0` (`web_server.cpp` บรรทัด 645) เก็บใน NVS key `use`

## 15. AI ฝั่งคอม (เทรน)

| เทคนิค | ที่ไหน | อธิบาย |
|---|---|---|
| Logistic Regression + L2 | `studio/ml/polyml.py` `fit_logreg()` บรรทัด 159 | Newton's method (IRLS) 60 รอบ, Python ล้วนไม่ใช้ numpy |
| Standardization | `polyml.py` `standardize_fit()` | (x − mean)/SD ของชุดเทรน — นาฬิกาใช้ mean/scale ชุดเดียวกัน |
| Nested CV เลือก λ | `polyml.py` `choose_lambda()` บรรทัด 296 | λ ∈ {0.1, 0.3, 1, 3, 10} เลือกจากชุดเทรนเท่านั้น |
| Leave-one-subject-out | `polyml.py` `cv_splits()` บรรทัด 252 | ทดสอบกับคนที่ไม่เคยเห็น (≥ 3 คน) |
| Wilson 95% CI | `polyml.py` `wilson()` บรรทัด 281 | บอกช่วงความไม่แน่นอนของความแม่นยำ |
| เทียบ baseline | `studio/ml/train.py` | รายงาน "เดาคลาสที่มากที่สุด" คู่กันเสมอ |
| ตรวจความตรงกันคอม ↔ นาฬิกา | `studio/tests/test_parity.py` | รันสูตรเดียวกันใน C++ และ Python แล้วเทียบผล (19/19 ตรง) |

## 16. ฝั่งคอม: Polygraph Studio

| เทคนิค | ที่ไหน | อธิบาย |
|---|---|---|
| UDP client (asyncio) | `studio/backend/watch_link.py` `WatchLink` บรรทัด 28 | รับค่าสด, ส่ง hello, นับแพ็กเก็ตหาย |
| ความน่าเชื่อถือบน UDP | `watch_link.py` `schedule_lie_sync()` บรรทัด 202 | ดึงผลซ้ำทาง HTTP เมื่อ `rv` เปลี่ยน |
| WebSocket pub/sub | `studio/backend/hub.py` `Hub` บรรทัด 37 | ดันข้อมูลสดไปทุกหน้าเว็บ |
| SQLite WAL + batch | `studio/backend/store.py` `Store` บรรทัด 145 | อ่านขณะเขียนได้, เขียนค่าสดเป็นชุด |
| Model sync | `studio/backend/model_sync.py` `ModelSync` บรรทัด 73 | ทุก 4 s ตรวจ/ส่งโมเดล, retry 30 s |
| Atomic file update | `studio/ml/datafiles.py` `_rewrite()` บรรทัด 873 | สำรอง → เขียน .tmp → `os.replace` |

---

## 17. คำถามที่อาจารย์น่าจะถาม + ตอบ + ชี้ที่โค้ด

| คำถาม | ตอบสั้น ๆ | เปิดที่ |
|---|---|---|
| ใช้ interrupt ตรงไหน | timer 100 Hz ปลุก sensorTask, timer watchdog 12 s, (ปุ่มใช้ GPIO wake ตอน light sleep) | `tasks.cpp` 41, `watchdog.cpp` 44, `power.cpp` 52 |
| ทำไม ISR สั้น / IRAM_ATTR คืออะไร | ข้อ 2.1 | `tasks.cpp` 41 |
| ใช้ RTOS อะไรบ้าง | 6 task + priority, 4 queue, 3 mutex, event group, task notification | `tasks.cpp` 302, `app.cpp` 40–50 |
| ถ้าไม่มี mutex จะเกิดอะไร | อ่านค่าสดครึ่งเก่าครึ่งใหม่ / engine ถูกสั่งถามระหว่างคำนวณ | `app.cpp` 55, `tasks.cpp` 141 |
| watchdog ทำงานยังไง สาธิตได้ไหม | 2 ชั้น (TWDT 8 s + timer 12 s แบบ heartbeat) สาธิตได้จากหน้าระบบ | ข้อ 12.4–12.5 |
| ข้อมูลเก็บที่ไหนบ้าง | NVS (ค่าตั้ง/โมเดล), EEPROM (สถิติ), LittleFS (log/ผล/ข้อมูลเทรน), RTC (กล่องดำ), แฟลช (หน้าเว็บ) | ข้อ 10 |
| ปิดเครื่องแล้วโมเดลหายไหม | ไม่หาย อยู่ NVS + ตรวจ CRC32 ตอนโหลด | `storage.cpp` `loadModel()`, `ml_model.cpp` 43 |
| ประหยัดไฟยังไง | light/deep sleep, standby เช็คการแตะโดยไม่เปิด WiFi, ECO 80 MHz, ลด TX power, ปิดเซนเซอร์ก่อนหลับ | ข้อ 12.1–12.3 |
| I2C ต่อ 2 ชิปยังไง / บัสค้างทำไง | address ต่างกัน 0x57/0x68, กู้บัสด้วย 9 clock + STOP | ข้อ 7 |
| ADC อ่านแม่นแค่ไหน ทำอะไรเพิ่ม | ใช้ค่าปรับเทียบ eFuse + เฉลี่ย 16 ครั้ง, ADC1 เท่านั้นเพราะ ADC2 ชนกับ WiFi | ข้อ 8 |
| ทำไมไม่ใช้ SPI / DAC / Bluetooth | ข้อ 5, 9, 11.5 | — |
| WiFi ส่งข้อมูลยังไง ทำไม UDP | ข้อ 11.3 | `telemetry.cpp` 108, 189 |
| อัปเดตเฟิร์มแวร์แล้วพังทำไง | OTA rollback app0/app1 | `ota.cpp` 215 |
| AI คืออะไร แม่นแค่ไหน | Logistic Regression 12 ค่า, ประเมินแบบ leave-one-subject-out; ตอนนี้ 45% ยังไม่ดีกว่าเดา → สาธิตด้วยสูตรมาตรฐาน | คู่มือ 5.0, ข้อ 14–15 |
| สูตรมาตรฐานคิดยังไง | weighted z-score → sigmoid → เกณฑ์ 0.65/0.35 + ปรับตามคนจากข้อควบคุม | คู่มือ 5.0, `lie_engine.cpp` 265, 440 |

---

<a id="appendix"></a>

## ภาคผนวก ก — ทุกจุด `[เทคนิค: ...]` ในโค้ด (สร้างอัตโนมัติจากไฟล์จริง)

"บรรทัด" = บรรทัดของโค้ดที่อยู่ใต้คอมเมนต์ `[เทคนิค]` (ณ 9 ต.ค. 2026)

| # | เทคนิค | ไฟล์ | บรรทัด | โค้ดบรรทัดนั้น |
|---|---|---|---|---|
| 1 | RTOS Mutex | `Wireless Polygraph/src/app.cpp` | 40 | `liveMutex = xSemaphoreCreateMutex();` |
| 2 | RTOS Queue (producer-consumer) | `Wireless Polygraph/src/app.cpp` | 45 | `frameQueue = xQueueCreate(10, sizeof(Vitals));      // 2 s เผื่อ en...` |
| 3 | RTOS Event Group | `Wireless Polygraph/src/app.cpp` | 50 | `events = xEventGroupCreate();` |
| 4 | Critical section ด้วย mutex | `Wireless Polygraph/src/app.cpp` | 55 | `void setVitals(const Vitals& v) {` |
| 5 | UART command-line interface | `Wireless Polygraph/src/cli.cpp` | 386 | `void execute(char* line) {` |
| 6 | UART / USB-CDC Serial | `Wireless Polygraph/src/main.cpp` | 83 | `Serial.begin(115200);` |
| 7 | Deep sleep wake-up handling + RTC memory | `Wireless Polygraph/src/main.cpp` | 93 | `power::handleWakeEarly();      // (1) อาจหลับต่อทันทีถ้าไม่มีใครใส่...` |
| 8 | Reset reason + RTC_NOINIT "กล่องดำ" + Core dump | `Wireless Polygraph/src/main.cpp` | 96 | `wdt::captureBootInfo();        // (2) ต้องอ่านก่อนอย่างอื่นเขียนทับ...` |
| 9 | Memory — NVS, EEPROM emulation, LittleFS | `Wireless Polygraph/src/main.cpp` | 98 | `storage::begin();              // (3)` |
| 10 | RTOS synchronization | `Wireless Polygraph/src/main.cpp` | 102 | `app::createSyncObjects();` |
| 11 | Brownout recovery / Power optimization | `Wireless Polygraph/src/main.cpp` | 120 | `if (brownout) {` |
| 12 | WiFi SoftAP | `Wireless Polygraph/src/main.cpp` | 126 | `const bool wifiOk = net::beginAp(wifiLevel);` |
| 13 | Task Watchdog Timer (TWDT) | `Wireless Polygraph/src/main.cpp` | 131 | `wdt::beginTaskWdt();` |
| 14 | FreeRTOS multitasking | `Wireless Polygraph/src/main.cpp` | 133 | `tasks::startAll();` |
| 15 | Timer-interrupt watchdog | `Wireless Polygraph/src/main.cpp` | 135 | `wdt::beginHwWdt();` |
| 16 | Arduino loop = FreeRTOS loopTask priority 1 | `Wireless Polygraph/src/main.cpp` | 155 | `void loop() {` |
| 17 | ADC + voltage divider | `Wireless Polygraph/src/sensors.cpp` | 86 | `void readAnalog() {` |
| 18 | Fault tolerance — I2C bus recovery | `Wireless Polygraph/src/sensors.cpp` | 158 | `void checkHealth(uint32_t now) {` |
| 19 | Multi-rate sampling | `Wireless Polygraph/src/sensors.cpp` | 214 | `void tick() {` |
| 20 | Hardware timer interrupt + ISR + IRAM_ATTR + Deferred interrupt processing | `Wireless Polygraph/src/tasks.cpp` | 41 | `void IRAM_ATTR onTick() {` |
| 21 | Task notification | `Wireless Polygraph/src/tasks.cpp` | 53 | `if (ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(50)) == 0) s_missed++;  ...` |
| 22 | Blocking queue receive + timeout | `Wireless Polygraph/src/tasks.cpp` | 121 | `const bool got = xQueueReceive(app::frameQueue, &v, pdMS_TO_TICKS(5...` |
| 23 | Mutex | `Wireless Polygraph/src/tasks.cpp` | 141 | `const lie::State before = app::engine.state();` |
| 24 | Single-writer flash logging | `Wireless Polygraph/src/tasks.cpp` | 224 | `if (xQueueReceive(app::logQueue, &m, pdMS_TO_TICKS(200)) == pdTRUE) {` |
| 25 | Watchdog แบบ heartbeat | `Wireless Polygraph/src/tasks.cpp` | 251 | `if (!stale) wdt::feedHw();` |
| 26 | CPU load profiling | `Wireless Polygraph/src/tasks.cpp` | 263 | `app::tasks[i].cpuPct = elapsedUs > 0 ? 100.0f * (float)(busy - app:...` |
| 27 | USB detection | `Wireless Polygraph/src/tasks.cpp` | 270 | `if (HWCDC::isPlugged()) app::setBits(EV_USB);` |
| 28 | ถนอมอายุแฟลช (write endurance) | `Wireless Polygraph/src/tasks.cpp` | 286 | `if (secCount % 600 == 0) storage::saveStats();   // EEPROM commit ท...` |
| 29 | Preemptive priority scheduling | `Wireless Polygraph/src/tasks.cpp` | 302 | `create(sensorTask, T_SENSOR, STACK_SENSOR, PRIO_SENSOR);` |
| 30 | Hardware timer | `Wireless Polygraph/src/tasks.cpp` | 312 | `s_tickTimer = timerBegin(1000000);` |
| 31 | ADC configuration | `Wireless Polygraph/src/drivers/analog_in.cpp` | 16 | `analogReadResolution(12);` |
| 32 | ADC oversampling + calibrated read | `Wireless Polygraph/src/drivers/analog_in.cpp` | 25 | `float readMv(uint8_t pin, uint8_t n) {` |
| 33 | I2C master | `Wireless Polygraph/src/drivers/i2c_bus.cpp` | 19 | `bool ok = Wire.begin((int)PIN_SDA, (int)PIN_SCL, I2C_FREQ_HZ);` |
| 34 | I2C bus recovery (bit-banging GPIO) | `Wireless Polygraph/src/drivers/i2c_bus.cpp` | 26 | `bool recover() {` |
| 35 | I2C burst read + FIFO ของเซนเซอร์ | `Wireless Polygraph/src/drivers/max30102.cpp` | 98 | `int Max30102::read(Sample* out, int maxN) {` |
| 36 | I2C register read | `Wireless Polygraph/src/drivers/mpu6050.cpp` | 66 | `bool Mpu6050::readAccel(float& ax, float& ay, float& az) {` |
| 37 | วงจรแบ่งแรงดัน + สมการ Beta ของ NTC | `Wireless Polygraph/src/drivers/sensor_math.cpp` | 14 | `float ntcCelsius(float vMv, float vccMv, float r1, float r25, float...` |
| 38 | วงจรแบ่งแรงดัน (กฎของโอห์ม) | `Wireless Polygraph/src/drivers/sensor_math.cpp` | 24 | `float gsrMicroSiemens(float vMv, float vccMv, float r2, float r3) {` |
| 39 | Signal processing — EDA tonic/phasic | `Wireless Polygraph/src/dsp/eda.cpp` | 33 | `void EdaProcessor::push(float uS, bool contact) {` |
| 40 | Digital filter — EMA (low-pass อันดับ 1) | `Wireless Polygraph/src/dsp/filters.h` | 23 | `class Ema {` |
| 41 | Digital filter — Biquad IIR (อันดับ 2, Butterworth Q = 0.707) | `Wireless Polygraph/src/dsp/filters.h` | 44 | `class Biquad {` |
| 42 | Signal processing — band-pass แยกการสั่นกับการขยับ | `Wireless Polygraph/src/dsp/motion.cpp` | 28 | `void MotionProcessor::push(float ax, float ay, float az) {` |
| 43 | Signal processing — PPG peak detection | `Wireless Polygraph/src/dsp/ppg.cpp` | 81 | `bool PpgProcessor::push(uint32_t irRaw) {` |
| 44 | สูตรมาตรฐาน — weighted z-score | `Wireless Polygraph/src/lie/lie_engine.cpp` | 265 | `float Engine::scoreOf(const float* z, uint8_t okMask, const float* ...` |
| 45 | Statistics — Welford mean/SD + null distribution | `Wireless Polygraph/src/lie/lie_engine.cpp` | 280 | `void Engine::finishBaseline() {` |
| 46 | Per-subject calibration | `Wireless Polygraph/src/lie/lie_engine.cpp` | 440 | `void Engine::recalibrate() {` |
| 47 | Data integrity — CRC32 | `Wireless Polygraph/src/lie/ml_model.cpp` | 43 | `static uint32_t crc32(const uint8_t* p, size_t n) {` |
| 48 | On-device ML inference — Logistic Regression | `Wireless Polygraph/src/lie/ml_model.cpp` | 83 | `float predict(const Model& m, const lie::Result& r) {` |
| 49 | Lightweight serializer | `Wireless Polygraph/src/net/json_writer.h` | 12 | `class Json {` |
| 50 | UDP + client registration | `Wireless Polygraph/src/net/telemetry.cpp` | 108 | `void handleHello(char* msg, IPAddress ip, uint16_t port) {` |
| 51 | JSON over UDP | `Wireless Polygraph/src/net/telemetry.cpp` | 189 | `void sendVitals() {` |
| 52 | Keep-alive timeout | `Wireless Polygraph/src/net/telemetry.cpp` | 234 | `void expireClients(uint32_t now) {` |
| 53 | Embedded web page ใน PROGMEM/แฟลช | `Wireless Polygraph/src/net/web_server.cpp` | 274 | `void hRoot() { server.send_P(200, "text/html", INDEX_HTML); }` |
| 54 | HTTP REST API (WebServer) | `Wireless Polygraph/src/net/web_server.cpp` | 285 | `void hLive() {` |
| 55 | Chunked streaming + mutex แบบสั้น | `Wireless Polygraph/src/net/web_server.cpp` | 565 | `void hMlData() {` |
| 56 | Event-driven WiFi callback | `Wireless Polygraph/src/net/wifi_ap.cpp` | 36 | `void onWifiEvent(arduino_event_id_t ev, arduino_event_info_t info) {` |
| 57 | WiFi AP mode + TX power control | `Wireless Polygraph/src/net/wifi_ap.cpp` | 51 | `bool beginAp(uint8_t level) {` |
| 58 | Strategy / function pointer | `Wireless Polygraph/src/sys/ml_runtime.cpp` | 23 | `bool scorer(const lie::Result& r, float& p, void*) {` |
| 59 | OTA ผ่าน HTTP + Basic auth | `Wireless Polygraph/src/sys/ota.cpp` | 114 | `void handleUpload() {` |
| 60 | OTA rollback (app0/app1 + otadata) | `Wireless Polygraph/src/sys/ota.cpp` | 215 | `void serviceVerify() {` |
| 61 | Light sleep + GPIO wake-up | `Wireless Polygraph/src/sys/power.cpp` | 52 | `void doLightSleep(uint32_t maxSec) {` |
| 62 | Deep sleep + timer wake-up + RTC memory | `Wireless Polygraph/src/sys/power.cpp` | 99 | `[[noreturn]] void doDeepSleep(uint32_t sec, uint8_t reason) {` |
| 63 | Dynamic frequency scaling | `Wireless Polygraph/src/sys/power.cpp` | 152 | `void setEco(bool on) {` |
| 64 | Battery protection + auto-standby | `Wireless Polygraph/src/sys/power.cpp` | 185 | `void service() {` |
| 65 | NVS (Non-Volatile Storage) ผ่าน Preferences | `Wireless Polygraph/src/sys/storage.cpp` | 107 | `s_prefsOk = prefs.begin(NVS_NS, false);` |
| 66 | EEPROM emulation + CRC | `Wireless Polygraph/src/sys/storage.cpp` | 112 | `EEPROM.begin(EEPROM_SIZE);` |
| 67 | File system บนแฟลช (LittleFS) | `Wireless Polygraph/src/sys/storage.cpp` | 125 | `s_fsOk = LittleFS.begin(true);` |
| 68 | Log rotation | `Wireless Polygraph/src/sys/storage.cpp` | 267 | `static void rotateIfBig(const char* path, const char* old, size_t m...` |
| 69 | NVS (Preferences) | `Wireless Polygraph/src/sys/storage.cpp` | 461 | `bool loadUseModel() { return mlPrefs.getBool("use", true); }` |
| 70 | PWM (LEDC) | `Wireless Polygraph/src/sys/ui.cpp` | 39 | `void ledWrite(uint8_t duty) {` |
| 71 | GPIO input + software debounce | `Wireless Polygraph/src/sys/ui.cpp` | 174 | `void pollButton(uint32_t now) {` |
| 72 | Watchdog ISR + RTC_NOINIT black box | `Wireless Polygraph/src/sys/watchdog.cpp` | 44 | `void IRAM_ATTR hwWdtIsr() {` |
| 73 | Post-mortem diagnostics | `Wireless Polygraph/src/sys/watchdog.cpp` | 61 | `void captureBootInfo() {` |
| 74 | Task Watchdog Timer (ESP-IDF) | `Wireless Polygraph/src/sys/watchdog.cpp` | 150 | `void beginTaskWdt() {` |
| 75 | Kicking the dog | `Wireless Polygraph/src/sys/watchdog.cpp` | 183 | `void feedHw() {` |
| 76 | WebSocket publish/subscribe | `Polygraph-Studio/backend/hub.py` | 37 | `class Hub:` |
| 77 | Periodic sync + retry backoff | `Polygraph-Studio/backend/model_sync.py` | 73 | `class ModelSync:` |
| 78 | SQLite (WAL) + batch insert | `Polygraph-Studio/backend/store.py` | 145 | `class Store:` |
| 79 | UDP client (asyncio DatagramProtocol) | `Polygraph-Studio/backend/watch_link.py` | 28 | `class WatchLink(asyncio.DatagramProtocol):` |
| 80 | Reliability บน UDP | `Polygraph-Studio/backend/watch_link.py` | 202 | `def schedule_lie_sync(self):` |
| 81 | Atomic file update + backup | `Polygraph-Studio/ml/datafiles.py` | 873 | `def _rewrite(data_dir: str, name: str, change: Callable[[List[Dict[...` |
| 82 | Machine learning — Logistic Regression + L2 regularization, Newton's method (IRLS) | `Polygraph-Studio/ml/polyml.py` | 159 | `def fit_logreg(Z: List[List[float]], y: List[int], lam: float = 1.0...` |
| 83 | Leave-one-subject-out CV | `Polygraph-Studio/ml/polyml.py` | 252 | `def cv_splits(ds: Dataset, k: int = 5) -> Tuple[str, List[List[int]]]:` |
| 84 | Wilson score interval | `Polygraph-Studio/ml/polyml.py` | 281 | `def wilson(k: int, n: int, z: float = 1.96) -> Tuple[float, float]:` |
| 85 | Nested cross-validation | `Polygraph-Studio/ml/polyml.py` | 296 | `def choose_lambda(X, y, features, idx: List[int], k: int = 4) -> fl...` |
