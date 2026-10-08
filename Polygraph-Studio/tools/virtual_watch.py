"""
virtual_watch.py — "นาฬิกาจำลอง" ทำตัวเหมือนเฟิร์มแวร์จริงทุกอย่าง (โปรโตคอล UDP + REST API เดียวกัน)

ใช้เมื่อ:
  * ฮาร์ดแวร์มีปัญหาในวันสอบ -> ยังสาธิตซอฟต์แวร์ได้ครบ
  * พัฒนา/ทดสอบ Polygraph Studio โดยไม่ต้องเสียบนาฬิกา

รัน:   python tools/virtual_watch.py          (UDP 4211, HTTP 8081)
แล้ว:  python -m backend --sim                 (Polygraph Studio)
หรือ:  เปิด http://127.0.0.1:8081/            (หน้าเว็บเดียวกับในนาฬิกา 192.168.4.1 — อ่านจาก web_page.h)
หรือ:  python ml/collect.py --sim              (เก็บข้อมูลเทรน AI ลง CSV)

ร่างกายจำลอง: ชีพจร (มีการหายใจแทรก), เหงื่อ, อุณหภูมิผิว, มือสั่น — เมื่อถูกถาม "ข้อที่โกหก"
จะตอบสนองแรง (เหงื่อพุ่ง ชีพจรเร่ง หลอดเลือดหด) แบบเดียวกับคน ส่วน LieEngine ใช้ตัวเดียวกับ
เฟิร์มแวร์ (พอร์ต Python ที่ทดสอบแล้วว่าให้ผลตรงกัน)
ปุ่มควบคุมผู้ถูกทดสอบจำลองอยู่ในหน้า "ระบบ" ของ Studio (การ์ด "นาฬิกาจำลอง")
"""
from __future__ import annotations

import argparse
import asyncio
import json
import math
import os
import random
import re
import sys
import time
import urllib.parse
from typing import Any, Dict, List, Optional

STUDIO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, STUDIO)
sys.path.insert(0, os.path.join(STUDIO, "ml"))

import uvicorn  # noqa: E402
from fastapi import FastAPI, Request  # noqa: E402
from fastapi.responses import HTMLResponse, JSONResponse, PlainTextResponse, Response  # noqa: E402

from backend import lie_engine as le  # noqa: E402
import polyml as pm  # noqa: E402

FW_VERSION = "2.1.0-sim"
DEVICE_ID = "SIM-0001"
# หน้าเว็บของนาฬิกาจริง (ฝังในเฟิร์มแวร์) — โฟลเดอร์ "Wireless Polygraph" อยู่ข้าง ๆ Polygraph-Studio
WEB_PAGE_H = os.path.join(os.path.dirname(STUDIO), "Wireless Polygraph", "src", "net", "web_page.h")
TRAIN_MAX = 180 * 1024        # เท่ากับ storage::TRAIN_MAX ในเฟิร์มแวร์


def load_watch_pages() -> Dict[str, str]:
    """ดึง INDEX_HTML และ UPDATE_HTML จาก web_page.h (non-greedy: ไฟล์มีหลายหน้า)"""
    try:
        with open(WEB_PAGE_H, encoding="utf-8") as fh:
            src = fh.read()
        pages = dict(re.findall(r'static const char (\w+)\[\] PROGMEM = R"HTML\((.*?)\)HTML"', src, re.S))
        if "INDEX_HTML" in pages:
            return pages
    except OSError:
        pass
    msg = ("<!doctype html><meta charset=utf-8><p>ไม่พบไฟล์ web_page.h ของเฟิร์มแวร์ที่ "
           + WEB_PAGE_H + " — หน้าเว็บนาฬิกาจำลองใช้ไม่ได้ แต่ API ยังใช้ได้ตามปกติ</p>")
    return {"INDEX_HTML": msg, "UPDATE_HTML": msg}


def r2(x, d=3):
    """ปัดทศนิยมสำหรับส่ง JSON (None/NaN -> null)"""
    return None if x is None or (isinstance(x, float) and math.isnan(x)) else round(x, d)


class Subject:
    """สรีรวิทยาจำลองของผู้ถูกทดสอบ 1 คน"""

    def __init__(self):
        """ผู้ถูกทดสอบจำลอง: สุ่มค่าปกติของชีพจร/GSR/อุณหภูมิ ให้แต่ละคนไม่เหมือนกัน"""
        self.hr_base = random.uniform(66, 80)
        self.gsr_base = random.uniform(1.4, 2.8)
        self.temp_base = random.uniform(32.8, 34.0)
        self.responses: List[List[float]] = []   # [เวลาเริ่ม, ความแรง]
        self.ppg_contact = True
        self.gsr_contact = True
        self.motion = 0.01
        self.phase = 0.0
        self.last_beat = -10.0
        self.ibis: List[float] = []
        self.drift = 0.0

    def respond(self, t: float, gain: float):
        """จำลองการตอบสนองของร่างกาย (เช่น ตอนโกหก) เริ่มที่เวลา t ความแรง gain"""
        self.responses.append([t, gain])

    def shape(self, t: float) -> float:
        """รวมการตอบสนองทั้งหมด ณ เวลา t (แฝง ~1 วินาที ขึ้นแล้วค่อย ๆ ลด) — 0 = ปกติ"""
        s = 0.0
        for start, gain in self.responses:
            x = t - start - 1.0                      # แฝง ~1 วินาที แล้วค่อยตอบสนอง
            if x > 0:
                s += gain * (1 - math.exp(-x / 1.0)) * math.exp(-x / 8.0)
        self.responses = [r for r in self.responses if t - r[0] < 60]
        return s

    def hr(self, t: float, sh: float) -> float:
        """ชีพจรจำลอง = ค่าปกติ + การแกว่งตามการหายใจ + ส่วนที่เพิ่มตอนตื่นเต้น"""
        return self.hr_base + 3.0 * math.sin(2 * math.pi * 0.25 * t) + 10.0 * sh


class VirtualWatch:
    """นาฬิกาจำลองทั้งเครื่อง: UDP (ค่าสด/คลื่น/เหตุการณ์) + REST API ชุดเดียวกับเฟิร์มแวร์ + LieEngine ฉบับ Python
    ใช้ซ้อม/ทดสอบ Studio และ collect_data.bat ได้โดยไม่ต้องมีบอร์ด (run_demo_simulator.bat)
    """
    def __init__(self, udp_port: int, http_port: int, speed: float = 1.0):
        """สร้างสถานะเริ่มต้นของนาฬิกาจำลอง (เหมือนเพิ่งเปิดเครื่อง)"""
        self.udp_port, self.http_port = udp_port, http_port
        # เร่งเวลา (เช่น 5 = baseline 30 วินาทีเสร็จใน 6 วินาที) ไว้ทดสอบ/เก็บข้อมูลตัวอย่างเร็ว ๆ
        self.speed = max(0.1, min(20.0, speed))
        self.boot = 1
        self.reason, self.planned, self.planned_task, self.prev_uptime = "POWERON", "none", "", 0
        self.coredump: Optional[Dict[str, Any]] = None
        # ---- AI (เหมือน ml_runtime ในเฟิร์มแวร์) — อยู่ใน "แฟลช" จึงไม่หายตอนรีบูตจำลอง
        self.ml_subject = "-"
        self.auto_qid = 1
        self.train_lines: List[str] = []          # แทนไฟล์ /train.csv ใน LittleFS
        self.model: Optional[pm.Model] = None
        self.model_info: Dict[str, Any] = {}
        self.reset_state()
        self.clients: Dict[tuple, Dict[str, Any]] = {}
        self.transport = None
        self.paused_until = 0.0          # จำลอง sleep/รีบูต: หยุดส่งข้อมูลชั่วคราว
        self.pending_reboot: Optional[tuple] = None
        self.eid = 0
        self.mode = "random"             # คำถามจริงข้อถัดไป: random / lie / truth / nervous
        self.next_override: Optional[str] = None
        self.secret = random.randint(1, 5)
        self.truth_log: List[Dict[str, Any]] = []
        self.settings = self.default_settings()
        self.stats = {"bootCount": self.boot, "sessions": 0, "questions": 0, "lies": 0, "truths": 0,
                      "inconclusive": 0, "invalid": 0, "wdtResets": 0, "panicResets": 0, "deepSleeps": 0,
                      "otaUpdates": 0}
        self.events_log: List[str] = []
        self.log("BOOT", f"fw {FW_VERSION} boot#{self.boot} reset={self.reason}")

    # ------------------------------------------------------------------ state
    def reset_state(self):
        self.t0 = time.time()
        self.subject = Subject() if not hasattr(self, "subject") else self.subject
        self.engine = le.Engine()
        self.engine.scorer = self.scorer          # เหมือน app::engine.setScorer() ใน mlrt::begin()
        self.last_csv = 0.0
        self.n100 = 0
        self.seq_v = self.seq_w = 0
        self.wave: List[int] = []
        self.wave_beats = 0
        self.wave_n0 = 0
        self.last_seq_seen = 0
        self.eco = False
        self.gsr_smooth = None
        self.scr_times: List[float] = []

    @staticmethod
    def default_settings():
        """ค่าตั้งเริ่มต้น (ตรงกับเฟิร์มแวร์)"""
        # w เท่ากับเฟิร์มแวร์ v2.1 (ลดน้ำหนัก GSR — คนไม่ได้มีเหงื่อตลอดเวลา)
        return {"baselineSec": 30, "windowSec": 12, "preSec": 3, "s0": 2.0, "k": 1.5, "lieP": 0.65,
                "truthP": 0.35, "w": [0.20, 0.35, 0.25, 0.12, 0.08], "contactThr": 50000, "irLed": 31,
                "vccMv": 3300, "eco": False, "standbyMin": 0, "wakeCheckS": 20, "waveform": True,
                "mode": 0, "wifiPower": 1}

    # ------------------------------------------------------------------ AI
    def scorer(self, r: le.Result):
        """ใช้โมเดลถ้าติดตั้งไว้ (ทุกโหมด เหมือนเฟิร์มแวร์) ไม่มีโมเดล -> None = ใช้สูตรเดิม"""
        if self.model is None:
            return None
        return self.model.prob(pm.features_from(r.okMask, r.z, r.feat))

    def train_counts(self):
        """จำนวนข้อมูลเทรนที่นาฬิกาจำลองบันทึกเอง (ตอบจริง, โกหก)"""
        n1 = sum(1 for ln in self.train_lines if ln.split(",")[3] == "1")
        return len(self.train_lines) - n1, n1

    def train_bytes(self) -> int:
        """ขนาดไฟล์ /train.csv จำลอง (ไบต์)"""
        if not self.train_lines:
            return 0
        return len((",".join(pm.TRAIN_HEADER) + "\n").encode()) + sum(len(ln.encode()) + 1 for ln in self.train_lines)

    def apply_settings(self):
        """นำค่าตั้งไปใช้กับ LieEngine จำลอง"""
        c = le.Config()
        s = self.settings
        c.baselineSec, c.windowSec, c.preSec = s["baselineSec"], s["windowSec"], s["preSec"]
        c.s0, c.k, c.lieP, c.truthP = s["s0"], s["k"], s["lieP"], s["truthP"]
        c.weight = list(s["w"])
        self.engine.configure(c)
        self.eco = bool(s["eco"])

    def uptime(self) -> float:
        """เวลาทำงานจำลอง (วินาที) — คูณ speed เมื่อเร่งเวลา"""
        return (time.time() - self.t0) * self.speed

    def log(self, typ: str, text: str):
        """จด log เหตุการณ์ (เก็บ 400 บรรทัดล่าสุด เหมือน events.log ในนาฬิกา) และพิมพ์ออกจอ"""
        self.events_log.append(f"{int(self.uptime())},{int(time.time())},{typ},{text}")
        self.events_log = self.events_log[-400:]
        print(f"[{self.uptime():7.1f}s] {typ:<9} {text}")

    # ------------------------------------------------------------------ UDP
    def connection_made(self, transport):
        self.transport = transport

    def datagram_received(self, data: bytes, addr):
        """รับ UDP จากคอม: "hello" (ลงทะเบียนผู้รับข้อมูล + ตั้งเวลา), ping (วัดความหน่วง)"""
        if time.time() < self.paused_until:
            return                                # "หลับ/รีบูต" อยู่ -> ไม่ตอบ
        msg = data.decode(errors="replace").strip()
        if msg.startswith("hello"):
            wave = self.settings["waveform"]
            toks = msg.split()[1:]
            # "hello" เฉย ๆ (สคริปต์ของเพื่อน receive_data.py) หรือ csv=1 -> ส่ง CSV แบบเก่า 1 ครั้ง/วินาที
            legacy = not toks or "csv=1" in toks
            for tok in toks:
                if tok.startswith("w="):
                    wave = tok[2:] == "1"
            new = addr not in self.clients
            self.clients[addr] = {"last": time.time(), "wave": wave and not legacy, "csv": legacy}
            if new:
                self.log("UDP", f"client {addr[0]}:{addr[1]} joined ({'legacy CSV' if legacy else 'JSON'}, wave={int(wave)})")
                if not legacy:
                    self.send(addr, {"t": "hi", "id": DEVICE_ID, "fw": FW_VERSION, "build": "simulated",
                                     "boot": self.boot, "reason": self.reason, "planned": self.planned,
                                     "uptime": int(self.uptime()), "ip": "127.0.0.1", "http": self.http_port})
        elif msg.startswith("ping"):
            self.send(addr, {"t": "pong", "n": msg[5:25], "ms": int(self.uptime() * 1000)})

    def error_received(self, exc):
        """asyncio เรียกเมื่อ UDP error — ไม่ต้องทำอะไร"""
        pass

    def connection_lost(self, exc):
        """asyncio เรียกตอนปิด socket — ไม่ต้องทำอะไร"""
        pass

    def send(self, addr, obj):
        """ส่ง JSON 1 ก้อนทาง UDP ไปที่ addr"""
        if self.transport:
            self.transport.sendto(json.dumps(obj, separators=(",", ":")).encode(), addr)

    def send_all(self, obj, wave_only=False):
        """ส่งให้ทุกคอมที่ทักทายมาภายใน 10 วินาที (ผู้รับที่เงียบนานถูกลบ)"""
        now = time.time()
        for addr, c in list(self.clients.items()):
            if now - c["last"] > 10:
                del self.clients[addr]
                continue
            if c.get("csv") or (wave_only and not c["wave"]):
                continue
            self.send(addr, obj)

    def send_legacy_csv(self):
        """รูปแบบเดียวกับ telemetry.cpp: ms,bpm,finger,tremor,temp,gsr_uS,gsr_mV,vbat_mV"""
        v = getattr(self, "live", None)
        if not v or not self.transport:
            return
        line = "%d,%.0f,%d,%.3f,%.1f,%.2f,%.0f,%.0f\n" % (
            v["ms"], v["hr"] or 0, v["con"], v["trm"] or 0, v["tmp"] or 0, v["gsr"] or 0, v["gmv"], v["vb"])
        for addr, c in list(self.clients.items()):
            if c.get("csv") and time.time() - c["last"] <= 10:
                self.transport.sendto(line.encode(), addr)

    def event(self, ev: str, **data):
        """ส่งเหตุการณ์ (เลข eid เพิ่มทีละ 1 ให้ผู้รับตัดซ้ำได้)"""
        self.eid += 1
        msg = dict({"t": "e", "eid": self.eid, "ev": ev}, **data)
        self.send_all(msg)
        asyncio.get_running_loop().call_later(0.15, lambda: self.send_all(msg))   # ส่งซ้ำแบบเฟิร์มแวร์

    # ------------------------------------------------------------------ simulation loop (100 Hz)
    async def run(self):
        dt = 0.01
        next_t = time.perf_counter()
        while True:
            next_t += dt / self.speed
            await asyncio.sleep(max(0.0, next_t - time.perf_counter()))
            if self.pending_reboot and time.time() >= self.pending_reboot[0]:
                self.do_reboot(*self.pending_reboot[1:])
            if time.time() < self.paused_until:
                continue
            self.step(dt)

    def step(self, dt: float):
        """1 ก้าวเวลา (10 ms): สร้างคลื่นชีพจร ทุก 20 ก้าว (0.2 วินาที) สร้างค่าสด 1 เฟรม"""
        s = self.subject
        self.n100 += 1
        t = self.uptime()
        sh = s.shape(t)
        hr = s.hr(t, sh)
        # ---- PPG 100 Hz ----
        s.phase += hr / 60.0 * dt
        beat = False
        if s.phase >= 1.0:
            s.phase -= 1.0 + random.gauss(0, 0.02)
            if s.last_beat > 0:
                s.ibis.append((t - s.last_beat) * 1000)
                s.ibis = s.ibis[-12:]
            s.last_beat = t
            beat = s.ppg_contact
        x = t - s.last_beat
        amp = 1000.0 * (1 - 0.3 * min(sh, 1.5))
        pulse = math.exp(-((x - 0.12) / 0.06) ** 2) + 0.4 * math.exp(-((x - 0.36) / 0.08) ** 2)
        w = (amp * (pulse - 0.25) + random.gauss(0, 12)) if s.ppg_contact else 0.0
        if not self.wave:
            self.wave_n0 = self.n100
        if beat:
            self.wave_beats |= 1 << len(self.wave)
        self.wave.append(int(w))
        if len(self.wave) >= 10:
            if not self.eco:
                self.send_all({"t": "w", "seq": self.seq_w + 1, "n0": self.wave_n0, "fs": 100,
                               "b": self.wave_beats, "d": self.wave}, wave_only=True)
                self.seq_w += 1
            self.wave, self.wave_beats = [], 0
        # ---- frame 5 Hz ----
        if self.n100 % 20 == 0:
            self.frame(t, sh, hr, amp)

    def frame(self, t: float, sh: float, hr: float, amp: float):
        """สร้างค่าสด 1 เฟรม (GSR, อุณหภูมิ, การสั่น, ...) ป้อน LieEngine แล้วส่งให้คอม"""
        s = self.subject
        s.drift += random.gauss(0, 0.0008)
        gsr = s.gsr_base + 0.0015 * t / 60 + s.drift + 0.5 * sh + random.gauss(0, 0.006)
        if self.gsr_smooth is None:
            self.gsr_smooth = gsr
        tonic = self.gsr_smooth = self.gsr_smooth + 0.013 * (gsr - self.gsr_smooth)
        if sh > 0.25 and (not self.scr_times or t - self.scr_times[-1] > 4):
            self.scr_times.append(t)
        self.scr_times = [x for x in self.scr_times if t - x < 60]
        temp = s.temp_base - 0.05 * sh + random.gauss(0, 0.015)
        trm = max(0.005, 0.04 + 0.03 * sh + random.gauss(0, 0.004)) + (0.6 * s.motion if s.motion > 0.5 else 0)
        mot = s.motion + abs(random.gauss(0, 0.004))
        hr_ok = s.ppg_contact and len(s.ibis) >= 2
        rmssd = None
        if len(s.ibis) >= 5:
            d = [s.ibis[i] - s.ibis[i - 1] for i in range(1, len(s.ibis))]
            rmssd = math.sqrt(sum(x * x for x in d) / len(d))
        hr_meas = hr + random.gauss(0, 0.6)
        f = le.Frame(gsr=gsr, hr=hr_meas if hr_ok else 0.0, amp=amp if s.ppg_contact else 0.0, tremor=trm,
                     motion=mot, temp=temp, gsrOk=s.gsr_contact, ppgOk=hr_ok)
        before = self.engine.state
        self.engine.push(f)
        st = self.engine.status()
        if before == le.STATE_BASELINE and self.engine.state == le.STATE_READY:
            b = self.engine.base
            self.stats["sessions"] += 1
            self.event("baseline_done", gsr=r2(b.mean[0]), hr=r2(b.mean[1], 1), nullN=b.nullN,
                       gsrFrac=r2(b.gsrFrac, 2), ppgFrac=r2(b.ppgFrac, 2))
            self.log("ENGINE", f"baseline done GSR {b.mean[0]:.3f} HR {b.mean[1]:.1f}")
        elif before == le.STATE_BASELINE and self.engine.state == le.STATE_IDLE:
            self.event("baseline_fail", error=self.engine.err)
        while self.last_seq_seen < self.engine.resultSeq:
            self.last_seq_seen += 1
            r = next((x for x in self.engine.res if x.seq == self.last_seq_seen), None)
            if r:
                self.result_event(r)
        vb = 3950 - self.uptime() * 0.02
        self.seq_v += 1
        self.live = {
            "t": "v", "id": DEVICE_ID, "seq": self.seq_v, "ms": int(self.uptime() * 1000),
            "hr": r2(hr_meas, 1) if hr_ok else None, "hrv": r2(rmssd, 1) if (hr_ok and rmssd) else None,
            "ibi": round(s.ibis[-1]) if s.ibis else 0, "con": int(s.ppg_contact),
            "pi": r2(amp / 140000 * 100, 2) if s.ppg_contact else 0, "amp": round(amp) if s.ppg_contact else 0,
            "ir": 140000 if s.ppg_contact else 3000, "beats": int(t * hr / 60),
            "gsr": r2(gsr) if s.gsr_contact else None, "gt": r2(tonic) if s.gsr_contact else None,
            "gp": r2(gsr - tonic) if s.gsr_contact else None, "gc": int(s.gsr_contact),
            "gmv": round(3300 * 100 / (200 + 1000 / max(gsr, 0.05))) if s.gsr_contact else 0,
            "scr": len(self.scr_times), "tmp": r2(temp, 2), "trm": r2(trm), "mot": r2(mot),
            "vb": round(vb), "bp": max(0, min(100, int((vb - 3300) / 9))), "bat": 1,
            "si": st["stress"], "es": st["state"], "ep": round(st["progress"], 2), "eel": round(st["elapsed"], 1),
            "eq": st["qid"], "ek": st["kind"], "rv": st["revision"], "rs": st["lastSeq"],
            "set": int(st["settled"]), "bl": int(st["baselineValid"]), "cal": int(st["calibrated"]),
            "fl": (512 if mot > 1.5 else 0) | (1 << 11), "cpu": 80 if self.eco else 160, "eco": int(self.eco),
            "md": int(self.settings["mode"]), "ml": int(self.model is not None),
            "sby": int(self.settings["standbyMin"]), "wp": int(self.settings["wifiPower"]),
        }
        if self.n100 % (50 if self.eco else 20) == 0:
            self.send_all(self.live)
        if time.time() - self.last_csv >= 1.0:
            self.last_csv = time.time()
            self.send_legacy_csv()

    def result_event(self, r: le.Result):
        """ได้ผลคำถาม 1 ข้อ: นับสถิติ, ส่งเหตุการณ์ result, บันทึกข้อมูลเทรน (ถ้าอยู่โหมด train)"""
        j = r.to_json()
        self.stats["questions"] += 1
        key = {"lie": "lies", "truth": "truths", "inconclusive": "inconclusive"}.get(j["verdict"], "invalid")
        self.stats[key] += 1
        md = int(self.settings["mode"])
        self.event("result", seq=j["seq"], qid=j["qid"], kind=j["kind"], verdict=j["verdict"], exp=j["expected"],
                   cor=int(j["correct"]), p=j["p"], score=j["score"], q=j["quality"], rs=j["reasons"], ok=j["ok"],
                   f=j["feat"], z=j["z"], lat=j["latency"], ans=j["answerAt"], ay=j["answerYes"], src=j["src"], md=md)
        # โหมดเก็บข้อมูล: บันทึกข้อที่รู้เฉลยลง "/train.csv" (เหมือน mlrt::makeTrainRow)
        if md == 1 and j["kind"] in ("truth", "lie") and j["verdict"] != "invalid":
            label = 1 if j["kind"] == "lie" else 0
            row = pm.train_row(pm.features_from(r.okMask, r.z, r.feat), epoch=int(time.time()),
                               subject=self.ml_subject, qid=r.qid, label=label, quality=r.quality,
                               p_model=r.pLie, source=r.source)
            line = ",".join(row)
            if self.train_bytes() + len(line) + 1 <= TRAIN_MAX:
                self.train_lines.append(line)
            else:
                self.log("ML", "train.csv full — not saved")
        truth = next((x for x in self.truth_log if x["qid"] == j["qid"] and "verdict" not in x), None)
        if truth:
            truth["verdict"] = j["verdict"]
        self.log("RESULTQ", f"q{j['qid']} {j['kind']} -> {j['verdict']} p={j['p']}")

    # ------------------------------------------------------------------ reboot simulation
    def schedule_reboot(self, delay: float, reason: str, planned: str, task: str = "", coredump=None):
        self.pending_reboot = (time.time() + delay, reason, planned, task, coredump)

    def do_reboot(self, reason: str, planned: str, task: str, coredump):
        """จำลองการรีบูต (นับบูต, จำสาเหตุ/coredump) เหมือนเฟิร์มแวร์"""
        self.prev_uptime = int(self.uptime())
        self.pending_reboot = None
        self.boot += 1
        self.stats["bootCount"] = self.boot
        if reason in ("TASK_WDT", "INT_WDT") or planned.startswith("demo_hw"):
            self.stats["wdtResets"] += 1
        elif reason == "PANIC":
            self.stats["panicResets"] += 1
        self.reason, self.planned, self.planned_task, self.coredump = reason, planned, task, coredump
        self.clients.clear()                     # RAM หาย = ลืมเครื่องรับทั้งหมด (เหมือนของจริง)
        self.reset_state()
        self.apply_settings()
        self.paused_until = time.time() + 2.0    # บูตใหม่ ~2 วินาที
        self.log("BOOT", f"fw {FW_VERSION} boot#{self.boot} reset={reason} planned={planned}")


# ====================================================================== HTTP API (เหมือนเฟิร์มแวร์)
def make_app(vw: VirtualWatch) -> FastAPI:
    app = FastAPI(title="Virtual Polygraph Watch", docs_url="/docs")

    def ok(msg=None, **extra):
        """ตอบ JSON {ok: true, msg}"""
        d = {"ok": True}
        if msg:
            d["msg"] = msg
        d.update(extra)
        return d

    def fail(code, err, msg):
        """ตอบ JSON error {ok: false, error, msg} พร้อม HTTP status"""
        return JSONResponse({"ok": False, "error": err, "msg": msg}, status_code=code)

    ERR_TH = {"NO_BASELINE": "ต้องวัด baseline ก่อน", "QUESTION_ACTIVE": "กำลังวัดคำถามอยู่ รอให้ครบเวลาก่อน",
              "BASELINE_RUNNING": "กำลังวัด baseline อยู่", "NO_QUESTION": "ยังไม่ได้เริ่มคำถาม",
              "BASELINE_NO_GSR": "แผ่น GSR ไม่แตะผิว baseline ใช้ไม่ได้",
              "BASELINE_NO_SIGNAL": "ไม่มีสัญญาณชีพจรและ GSR ตอน baseline — ใส่นาฬิกาให้แนบผิวแล้ววัดใหม่"}
    pages = load_watch_pages()

    @app.get("/", response_class=HTMLResponse)
    async def root():
        """หน้าเว็บในนาฬิกา (HTML เดียวกับใน web_page.h ของเฟิร์มแวร์)"""
        return HTMLResponse(pages["INDEX_HTML"])

    @app.get("/update", response_class=HTMLResponse)
    async def update_page():
        """หน้าอัปโหลดเฟิร์มแวร์ (OTA)"""
        return HTMLResponse(pages.get("UPDATE_HTML", ""))

    @app.middleware("http")
    async def asleep(request: Request, call_next):
        """ระหว่าง "หลับ/รีบูต" จำลอง ตอบทุก request ว่าไม่ว่าง (เหมือนนาฬิกาจริงที่ติดต่อไม่ได้)"""
        if time.time() < vw.paused_until:
            return JSONResponse({"ok": False, "error": "ASLEEP", "msg": "นาฬิกาจำลองกำลังรีบูต/หลับ"}, 503)
        return await call_next(request)

    @app.get("/api/info")
    async def info():
        """GET /api/info: ข้อมูลเครื่อง"""
        cd = vw.coredump or {}
        return {"name": "Wireless Polygraph Watch", "fw": FW_VERSION, "build": "simulated", "id": DEVICE_ID,
                "mac": "SI:MU:LA:TE:D0:01", "chip": "ESP32-C3 (simulated)", "chipRev": 4, "cores": 1,
                "cpuMHz": 80 if vw.eco else 160, "idf": "v4.4.7 (sim)", "arduino": "2.0.17",
                "uptime": int(vw.uptime()), "time": int(time.time()), "bootCount": vw.boot,
                "boot": {"reason": vw.reason, "reasonTh": {"POWERON": "เพิ่งจ่ายไฟ", "SOFTWARE": "ซอฟต์แวร์สั่งรีสตาร์ท",
                                                            "TASK_WDT": "Task Watchdog: มี task ค้าง",
                                                            "INT_WDT": "Interrupt Watchdog: interrupt ค้าง",
                                                            "PANIC": "โปรแกรมล่ม (exception/abort)",
                                                            "DEEPSLEEP": "ตื่นจาก Deep Sleep"}.get(vw.reason, vw.reason),
                         "wake": "timer" if vw.reason == "DEEPSLEEP" else "none", "planned": vw.planned,
                         "plannedTask": vw.planned_task, "prevUptime": vw.prev_uptime,
                         "coredump": bool(cd), "cdTask": cd.get("task", ""), "cdPc": cd.get("pc", "0x00000000")}}

    @app.get("/api/live")
    async def live():
        """GET /api/live: ค่าสดล่าสุด"""
        v = dict(getattr(vw, "live", {}) or {})
        v.pop("t", None)
        v.update({"fw": FW_VERSION, "uptime": int(vw.uptime()), "clients": len(vw.clients)})
        return v

    @app.get("/api/lie")
    async def lie(n: int = 24):
        """GET /api/lie: สถานะ LieEngine + ผล n ข้อล่าสุด"""
        return vw.engine.lie_json(max(0, min(24, n)))

    @app.post("/api/lie/baseline")
    async def baseline(sec: float = 0):
        """POST /api/lie/baseline: เริ่มวัดค่าปกติ"""
        if not vw.engine.start_baseline(sec):
            return fail(409, vw.engine.err, ERR_TH.get(vw.engine.err, vw.engine.err))
        vw.event("baseline_start", sec=sec or vw.engine.cfg.baselineSec)
        vw.log("ENGINE", "baseline start")
        return ok("baseline started")

    @app.post("/api/lie/question")
    async def question(qid: Optional[int] = None, kind: str = "test", label: str = ""):
        """POST /api/lie/question: เริ่มวัด 1 ข้อ (qid, kind)"""
        if kind not in le.KIND_NAMES:
            return fail(400, "BAD_KIND", "kind ต้องเป็น test, truth, lie หรือ warmup")
        if qid is None:                       # ไม่ระบุ qid -> นับต่อเอง (เหมือน s_autoQid ในเฟิร์มแวร์)
            qid = vw.auto_qid
        if not 1 <= qid <= 65535:
            return fail(400, "BAD_QID", "qid ต้องอยู่ระหว่าง 1-65535")
        k = le.KIND_NAMES.index(kind)
        if not vw.engine.start_question(qid, k):
            return fail(409, vw.engine.err, ERR_TH.get(vw.engine.err, vw.engine.err))
        vw.auto_qid = qid + 1
        # ตัดสินใจว่า "ผู้ถูกทดสอบจำลอง" จะตอบสนองแรงแค่ไหน (= โกหกหรือไม่)
        truth = "truth"
        if kind == "truth":
            gain = random.uniform(0.0, 0.15)
        elif kind == "lie":
            gain, truth = random.uniform(0.8, 1.2), "lie"
        elif kind == "warmup":
            gain = random.uniform(0.15, 0.35)
        elif label.startswith("card:"):
            hit = label == f"card:{vw.secret}"
            gain, truth = (random.uniform(0.8, 1.1), "lie") if hit else (random.uniform(0.0, 0.25), "truth")
        else:
            mode = vw.next_override or vw.mode
            vw.next_override = None
            if mode == "random":
                mode = random.choice(["lie", "truth"])
            if mode == "lie":
                gain, truth = random.uniform(0.7, 1.1), "lie"
            elif mode == "nervous":
                gain, truth = random.uniform(0.35, 0.6), "truth (ตื่นเต้น)"
            else:
                gain = random.uniform(0.0, 0.2)
        vw.subject.respond(vw.uptime(), gain)
        vw.truth_log.append({"qid": qid, "kind": kind, "label": label, "truth": truth, "gain": round(gain, 2),
                             "at": time.time()})
        vw.truth_log = vw.truth_log[-50:]
        vw.event("q_start", qid=qid, kind=kind)
        return ok(qid=qid)

    @app.post("/api/lie/answer")
    async def answer(ans: str = "yes"):
        """POST /api/lie/answer: บันทึกคำตอบ ใช่/ไม่ใช่"""
        if not vw.engine.mark_answer(ans != "no"):
            return fail(409, vw.engine.err, ERR_TH.get(vw.engine.err, vw.engine.err))
        return ok("answer " + ans)

    @app.post("/api/lie/abort")
    async def abort():
        """POST /api/lie/abort: ยกเลิกสิ่งที่กำลังวัด"""
        vw.engine.abort()
        vw.event("abort")
        return ok("aborted")

    @app.post("/api/lie/reset")
    async def reset():
        """POST /api/lie/reset: เริ่มผู้ตอบคนใหม่ (ล้าง baseline/ข้อควบคุม)"""
        vw.engine.reset_session()
        vw.last_seq_seen = vw.engine.resultSeq
        vw.event("session_reset")
        return ok("session reset")

    @app.get("/api/config")
    async def config_get():
        """GET /api/config: ค่าตั้ง"""
        return vw.settings

    @app.post("/api/config")
    async def config_set(request: Request):
        """POST /api/config: เปลี่ยนค่าตั้ง (ตรวจช่วงค่าเหมือนเฟิร์มแวร์)"""
        q = dict(request.query_params)
        s = dict(vw.settings)
        ranges = {"baselineSec": (20, 90), "windowSec": (6, 20), "preSec": (1, 5), "s0": (-5, 10), "k": (0.1, 10),
                  "lieP": (0.5, 0.99), "truthP": (0.01, 0.5), "contactThr": (5000, 250000), "irLed": (1, 255),
                  "vccMv": (3000, 3600), "eco": (0, 1), "standbyMin": (0, 240), "wakeCheckS": (5, 600),
                  "waveform": (0, 1)}
        for key, val in q.items():
            if key.startswith("w") and len(key) == 2 and key[1].isdigit():
                v = float(val)
                if not 0 <= v <= 1:
                    return fail(400, "BAD_VALUE", f"{key} ต้องอยู่ระหว่าง 0 ถึง 1")
                s["w"] = list(s["w"])
                s["w"][int(key[1])] = v
            elif key in ranges:
                v = float(val)
                lo, hi = ranges[key]
                if not lo <= v <= hi:
                    return fail(400, "BAD_VALUE", f"{key} ต้องอยู่ระหว่าง {lo} ถึง {hi}")
                s[key] = bool(v) if key in ("eco", "waveform") else (int(v) if key not in ("s0", "k", "lieP", "truthP") else v)
        if s["truthP"] >= s["lieP"]:
            return fail(400, "BAD_VALUE", "truthP ต้องน้อยกว่า lieP")
        vw.settings = s
        vw.apply_settings()
        vw.log("CONFIG", "settings updated")
        return s

    @app.post("/api/config/reset")
    async def config_reset():
        """POST /api/config/reset: คืนค่าเริ่มต้น"""
        vw.settings = vw.default_settings()
        vw.apply_settings()
        return vw.settings

    @app.get("/api/system")
    async def system():
        """GET /api/system: ข้อมูล FreeRTOS task, watchdog, แฟลช, พลังงาน (จำลอง)"""
        up = vw.uptime()
        cpu = lambda base: round(base * (0.5 if vw.eco else 1.0) + random.uniform(-0.05, 0.05), 2)  # noqa: E731
        tasks = [
            {"name": "sensor", "ours": True, "prio": 5, "state": "blocked", "stackFree": 2312, "stack": 4096, "cpu": cpu(3.1), "beats": int(up * 100), "lastBeatMs": 4},
            {"name": "engine", "ours": True, "prio": 4, "state": "blocked", "stackFree": 3920, "stack": 6144, "cpu": cpu(0.6), "beats": int(up * 2), "lastBeatMs": 120},
            {"name": "telemetry", "ours": True, "prio": 3, "state": "blocked", "stackFree": 1996, "stack": 4096, "cpu": cpu(1.4), "beats": int(up * 100), "lastBeatMs": 6},
            {"name": "http", "ours": True, "prio": 2, "state": "blocked", "stackFree": 4880, "stack": 8192, "cpu": cpu(0.9), "beats": int(up * 300), "lastBeatMs": 2},
            {"name": "ui", "ours": True, "prio": 2, "state": "blocked", "stackFree": 1752, "stack": 3072, "cpu": cpu(0.2), "beats": int(up * 50), "lastBeatMs": 15},
            {"name": "supervisor", "ours": True, "prio": 1, "state": "blocked", "stackFree": 3540, "stack": 6144, "cpu": cpu(0.3), "beats": int(up), "lastBeatMs": 400},
            {"name": "loopTask", "ours": False, "prio": 1, "state": "blocked", "stackFree": 6900},
            {"name": "IDLE", "ours": False, "prio": 0, "state": "ready", "stackFree": 1180},
            {"name": "wifi", "ours": False, "prio": 23, "state": "blocked", "stackFree": 3460},
            {"name": "tiT", "ours": False, "prio": 18, "state": "blocked", "stackFree": 1900},
            {"name": "sys_evt", "ours": False, "prio": 20, "state": "blocked", "stackFree": 860},
            {"name": "esp_timer", "ours": False, "prio": 22, "state": "blocked", "stackFree": 3200},
            {"name": "Tmr Svc", "ours": False, "prio": 1, "state": "blocked", "stackFree": 1560},
            {"name": "arduino_events", "ours": False, "prio": 19, "state": "blocked", "stackFree": 3100},
        ]
        parts = [
            {"label": "nvs", "type": "data", "subtype": 2, "addr": 0x9000, "size": 0x5000, "running": False},
            {"label": "otadata", "type": "data", "subtype": 0, "addr": 0xE000, "size": 0x2000, "running": False},
            {"label": "app0", "type": "app", "subtype": 16, "addr": 0x10000, "size": 0x1C0000,
             "running": vw.stats["otaUpdates"] % 2 == 0, "state": "valid", "firmware": FW_VERSION + "|simulated"},
            {"label": "app1", "type": "app", "subtype": 17, "addr": 0x1D0000, "size": 0x1C0000,
             "running": vw.stats["otaUpdates"] % 2 == 1, "state": "valid" if vw.stats["otaUpdates"] else "undefined",
             "firmware": (FW_VERSION + "|simulated") if vw.stats["otaUpdates"] else ""},
            {"label": "spiffs", "type": "data", "subtype": 130, "addr": 0x390000, "size": 0x60000, "running": False},
            {"label": "coredump", "type": "data", "subtype": 3, "addr": 0x3F0000, "size": 0x10000, "running": False},
        ]
        live = getattr(vw, "live", {}) or {}
        return {
            "uptime": int(up),
            "chip": {"model": "ESP32-C3 (sim)", "rev": 4, "cpuMHz": 80 if vw.eco else 160,
                     "tempC": round(38 + random.uniform(-0.5, 0.5), 1), "idf": "v4.4.7"},
            "memory": {"heapTotal": 280000, "heapFree": 151000 + random.randint(-800, 800), "heapMin": 142300,
                       "heapMaxBlock": 106000, "internalFree": 151000, "engineBytes": 23264},
            "flash": {"size": 4194304, "speedMHz": 80, "sketch": 1001234, "sketchFree": 1835008},
            "ota": {"running": "app1" if vw.stats["otaUpdates"] % 2 else "app0", "boot": "app0",
                    "pendingVerify": False, "rollbackPossible": vw.stats["otaUpdates"] > 0, "invalidExists": False,
                    "updates": vw.stats["otaUpdates"]},
            "partitions": parts,
            "nvs": {"used": 41, "free": 589},
            "fs": {"ok": True, "used": 16384 + len(vw.events_log) * 60, "total": 393216,
                   "files": [{"name": "events.log", "size": len(vw.events_log) * 60},
                             {"name": "results.csv", "size": vw.stats["questions"] * 150}]},
            "taskCount": 14, "tasks": tasks,
            "wdt": {"twdtS": 8, "iwdtMs": 300, "hwTimeoutMs": 12000, "hwRunning": True, "hwFeeds": int(up),
                    "hwLastFeedMs": int((up % 1) * 1000), "demo": "none", "wdtResets": vw.stats["wdtResets"],
                    "panicResets": vw.stats["panicResets"], "brownouts": 0},
            "power": {"eco": vw.eco, "usb": False, "vbatMv": live.get("vb", 3950), "battPct": live.get("bp", 72),
                      "battPresent": True, "lowBatt": False, "lightSleeps": 0, "lastLightSleepMs": 0,
                      "deepSleeps": vw.stats["deepSleeps"], "rtcWakeCount": 0, "rtcStandbyChecks": 0,
                      "rtcSleepSeconds": 0, "standbyMin": vw.settings["standbyMin"], "idleS": 0},
            "net": {"ssid": "Polygraph-Watch (sim)", "ip": "127.0.0.1", "mac": "SI:MU:LA:TE:D0:01", "channel": 1,
                    "stations": len(vw.clients), "rssi": -42, "udpClients": len(vw.clients),
                    "udpSent": vw.seq_v + vw.seq_w, "udpErrors": 0},
            "sensors": {"max30102": True, "maxPart": 21, "maxRev": 3, "fifoOverflows": 0, "mpu": True, "mpuWho": 112,
                        "mpuChip": "MPU6500", "mpuAccelCfg2": True, "i2cErrors": 0, "i2cRecoveries": 0,
                        "ticks": vw.n100, "button": 0},
            "stats": {"boots": vw.boot, "sessions": vw.stats["sessions"], "questions": vw.stats["questions"],
                      "lies": vw.stats["lies"], "truths": vw.stats["truths"], "inconclusive": vw.stats["inconclusive"],
                      "invalid": vw.stats["invalid"], "uptimeMin": int(up / 60), "otaUpdates": vw.stats["otaUpdates"]},
        }

    @app.get("/api/logs")
    async def logs(file: str = "events"):
        """GET /api/logs: ไฟล์ log เหตุการณ์/ผลคำถาม"""
        if file.startswith("results"):
            rows = ["epoch,uptime_s,seq,qid,kind,verdict,p_lie,score,quality,reasons"]
            for r in vw.engine.res:
                rows.append(f"{int(time.time())},{int(vw.uptime())},{r.seq},{r.qid},{le.KIND_NAMES[r.kind]},"
                            f"{le.VERDICT_NAMES[r.verdict]},{r.pLie:.3f},{r.score:.2f},{r.quality},{r.reasons}")
            return PlainTextResponse("\n".join(rows) + "\n")
        return PlainTextResponse("\n".join(vw.events_log) + "\n")

    @app.post("/api/logs/clear")
    async def logs_clear():
        """POST /api/logs/clear: ล้าง log"""
        vw.events_log = []
        return ok("logs cleared")

    @app.post("/api/stats/reset")
    async def stats_reset():
        """POST /api/stats/reset: ล้างสถิติสะสม (ยกเว้นจำนวนบูต)"""
        for k in vw.stats:
            if k != "bootCount":
                vw.stats[k] = 0
        return ok("stats reset")

    @app.post("/api/power")
    async def power(mode: str = "", sec: int = 0):
        """POST /api/power: โหมดประหยัดไฟ / หลับ"""
        if mode in ("eco", "normal"):
            vw.settings["eco"] = mode == "eco"
            vw.eco = mode == "eco"
            return ok("eco on (CPU 80 MHz)" if vw.eco else "normal (CPU 160 MHz)")
        if mode == "light":
            vw.paused_until = time.time() + (sec or 8)
            vw.log("SLEEP", f"light sleep {sec or 8}s (simulated)")
            return ok("light sleep: กดปุ่ม BOOT เพื่อปลุก (WiFi จะหลุดชั่วคราว)")
        if mode == "deep":
            vw.stats["deepSleeps"] += 1
            vw.schedule_reboot(0.3, "DEEPSLEEP", "none")
            vw.paused_until = time.time() + (sec or 10)
            return ok("deep sleep: จะตื่นเองตามเวลา" if sec else "standby: ใส่นาฬิกา/แตะเซนเซอร์เพื่อปลุก")
        return fail(400, "BAD_MODE", "mode ต้องเป็น normal, eco, light หรือ deep")

    @app.post("/api/restart")
    async def restart():
        """POST /api/restart: รีสตาร์ท"""
        vw.schedule_reboot(0.4, "SOFTWARE", "user_restart", "user")
        return ok("restarting")

    @app.post("/api/demo")
    async def demo(type: str = "", confirm: str = ""):
        """POST /api/demo: สาธิต watchdog แต่ละชั้น (ทำให้ค้างแล้วรีเซ็ต)"""
        plans = {"twdt": (8.0, "TASK_WDT", "demo_task_wdt", "sensor", None,
                          "sensor task จะค้าง -> Task WDT รีเซ็ตใน ~8 s"),
                 "hwwdt": (12.0, "PANIC", "demo_hw_timer_wdt", "supervisor", None,
                           "supervisor หยุดป้อน timer WDT -> รีเซ็ตใน ~12 s"),
                 "panic": (0.3, "PANIC", "demo_panic", "http", {"task": "http", "pc": "0x42012A3C"},
                           "เขียนลงแอดเดรส 0 -> panic + core dump -> รีเซ็ตทันที"),
                 "intwdt": (0.6, "INT_WDT", "demo_int_wdt", "http", None,
                            "ปิด interrupt แล้ววนค้าง -> Interrupt WDT รีเซ็ตใน ~0.3 s")}
        if type not in plans:
            return fail(400, "BAD_TYPE", "type ต้องเป็น twdt, hwwdt, panic หรือ intwdt")
        if confirm != "yes":
            return fail(400, "NEED_CONFIRM", "เครื่องจะรีเซ็ต! เพิ่ม confirm=yes เพื่อยืนยัน")
        delay, reason, planned, task, cd, msg = plans[type]
        vw.log("DEMO", f"watchdog demo requested: {type}")
        vw.schedule_reboot(delay, reason, planned, task, cd)
        if type in ("twdt", "hwwdt"):
            # ระหว่างรอ watchdog ยิง task ที่ "ค้าง" จะไม่ส่งข้อมูล (เหมือนของจริงที่ sensor ค้าง)
            vw.paused_until = time.time() + delay
        return ok(msg)

    @app.post("/update")
    async def update(request: Request):
        """POST /update: รับไฟล์เฟิร์มแวร์ (OTA) แล้วรีบูต"""
        body = await request.body()
        vw.stats["otaUpdates"] += 1
        vw.log("OTA", f"web upload ok ({len(body)} bytes) -> reboot")
        vw.schedule_reboot(0.6, "SOFTWARE", "ota_update", "http")
        return ok("rebooting")

    # ---------------------------------------------------------- AI / การหลับ / WiFi (v2.1)
    async def form(request: Request) -> Dict[str, str]:
        """รวม query string + body แบบ form-urlencoded (WebServer ของ ESP32 ก็รวมแบบนี้)
        แปลงเองด้วย urllib -> ไม่ต้องพึ่ง python-multipart"""
        d = dict(request.query_params)
        body = (await request.body()).decode("utf-8", "replace")
        if body:
            d.update({k: v[-1] for k, v in urllib.parse.parse_qs(body, keep_blank_values=True).items()})
        return d

    @app.get("/api/ml")
    async def ml_get():
        """GET /api/ml: โหมด train/detect, ข้อมูลเทรนในเครื่อง, โมเดลที่ติดตั้ง"""
        n0, n1 = vw.train_counts()
        m = {"loaded": vw.model is not None}
        if vw.model is not None:
            m.update({"name": vw.model_info.get("name", "model"), "accuracy": vw.model_info.get("acc", 0.0),
                      "samples": vw.model_info.get("samples", 0), "trainedAt": vw.model_info.get("trained", 0),
                      "b": round(vw.model.b, 4), "features": vw.model.features, "w": [round(x, 4) for x in vw.model.w]})
        return {"ok": True, "mode": "train" if vw.settings["mode"] == 1 else "detect", "subject": vw.ml_subject,
                "data": {"truth": n0, "lie": n1, "bytes": vw.train_bytes(), "max": TRAIN_MAX}, "model": m}

    @app.post("/api/ml/mode")
    async def ml_mode(mode: str = ""):
        """POST /api/ml/mode: สลับโหมดเก็บข้อมูล (train) / ใช้งานจริง (detect)"""
        if mode not in ("train", "detect"):
            return fail(400, "BAD_MODE", "mode ต้องเป็น train หรือ detect")
        vw.settings["mode"] = 1 if mode == "train" else 0
        vw.log("ML", f"mode -> {mode}")
        vw.event("mode", mode=mode)
        return ok("โหมดเก็บข้อมูล (TRAIN)" if mode == "train" else "โหมดใช้งานจริง (DETECT)")

    @app.post("/api/ml/subject")
    async def ml_subject(name: str = ""):
        """POST /api/ml/subject: ตั้งชื่อผู้ตอบ (เขียนลงข้อมูลเทรนของนาฬิกา)"""
        vw.ml_subject = pm.clean_subject(name)
        return ok(vw.ml_subject)

    @app.post("/api/ml/model")
    async def ml_model(request: Request):
        """POST /api/ml/model: ติดตั้งโมเดล AI (ตรวจรูปแบบเหมือนเฟิร์มแวร์)"""
        f = await form(request)
        feats = [x.strip() for x in f.get("features", "").split(",") if x.strip()]
        if not feats or len(feats) > pm.FW_MAX_FEATURES or any(x not in pm.FEATURES for x in feats):
            return fail(400, "BAD_FEATURES", "รายชื่อ feature ไม่ถูกต้อง")
        try:
            nums = {k: [float(v) for v in f.get(k, "").split(",") if v.strip()] for k in ("mean", "scale", "w")}
            b = float(f["b"])
        except (KeyError, ValueError):
            return fail(400, "BAD_MODEL", "จำนวนค่า mean/scale/w ไม่เท่ากับจำนวน feature")
        if any(len(v) != len(feats) for v in nums.values()):
            return fail(400, "BAD_MODEL", "จำนวนค่า mean/scale/w ไม่เท่ากับจำนวน feature")
        if any(not s > 0 for s in nums["scale"]) or any(math.isnan(x) for x in nums["w"] + nums["mean"] + [b]):
            return fail(400, "BAD_MODEL", "โมเดลไม่ผ่านการตรวจ (ค่า scale ต้องมากกว่า 0)")
        vw.model = pm.Model(feats, nums["mean"], nums["scale"], nums["w"], b)
        vw.model_info = {"name": f.get("name", "model")[:23], "acc": float(f.get("acc") or 0),
                         "samples": int(float(f.get("samples") or 0)), "trained": int(float(f.get("trained") or 0))}
        vw.log("ML", f"model installed: {len(feats)} features, acc {vw.model_info['acc']:.2f}")
        vw.event("model", loaded=1)
        return ok("ติดตั้งโมเดล AI แล้ว")

    @app.post("/api/ml/model/clear")
    async def ml_model_clear():
        """POST /api/ml/model/clear: ลบโมเดล"""
        vw.model, vw.model_info = None, {}
        vw.log("ML", "model removed")
        return ok("ลบโมเดลแล้ว กลับไปใช้สูตรมาตรฐาน")

    @app.get("/api/ml/data.csv")
    async def ml_data():
        """GET /api/ml/data.csv: ดาวน์โหลดข้อมูลที่นาฬิกาบันทึกเอง"""
        if not vw.train_lines:
            return fail(404, "NO_DATA", "ยังไม่มีข้อมูลเทรน — เปลี่ยนเป็นโหมดเก็บข้อมูลแล้วถามคำถามที่รู้เฉลยก่อน")
        text = ",".join(pm.TRAIN_HEADER) + "\n" + "\n".join(vw.train_lines) + "\n"
        return Response(text, media_type="text/csv",
                        headers={"Content-Disposition": 'attachment; filename="polygraph_train.csv"'})

    @app.post("/api/ml/data/clear")
    async def ml_data_clear(confirm: str = ""):
        """POST /api/ml/data/clear: ล้างข้อมูลเทรนในนาฬิกา (ต้อง confirm=yes)"""
        if confirm != "yes":
            return fail(400, "NEED_CONFIRM", "ข้อมูลเทรนจะหายทั้งหมด! เพิ่ม confirm=yes")
        vw.train_lines = []
        return ok("ล้างข้อมูลเทรนแล้ว")

    @app.post("/api/sleep")
    async def sleep_(request: Request):
        """POST /api/sleep: ตั้งค่าการหลับอัตโนมัติ / สั่งหลับทันที"""
        f = await form(request)
        if "auto" in f:
            try:
                m = int(f["auto"])
            except ValueError:
                m = -1
            if not 0 <= m <= 240:
                return fail(400, "BAD_VALUE", "auto ต้องเป็น 0 (ปิด) ถึง 240 นาที")
            vw.settings["standbyMin"] = m
            vw.log("POWER", f"auto-standby = {m} min")
        now, sec = f.get("now", ""), int(float(f.get("sec") or 0))
        if now == "light":
            vw.paused_until = time.time() + (min(sec, 3600) or 8)
            vw.log("SLEEP", "light sleep (simulated)")
        elif now == "deep":
            vw.stats["deepSleeps"] += 1
            vw.schedule_reboot(0.3, "DEEPSLEEP", "none")
            vw.paused_until = time.time() + (min(sec, 86400) or 10)
        return {"ok": True, "autoMin": vw.settings["standbyMin"], "now": now}

    @app.post("/api/wifi")
    async def wifi(level: int = -1):
        """POST /api/wifi: ระดับกำลังส่ง WiFi 0-2"""
        if not 0 <= level <= 2:
            return fail(400, "BAD_VALUE", "level ต้องเป็น 0 (ต่ำ), 1 (กลาง) หรือ 2 (สูง)")
        vw.settings["wifiPower"] = level
        return ok(["low (5 dBm)", "medium (8.5 dBm)", "high (13 dBm)"][level])

    @app.post("/api/time")
    async def set_time(epoch: int = 0):
        """POST /api/time: ตั้งเวลาจริง (นาฬิกาจำลองใช้เวลาคอมอยู่แล้ว)"""
        if not 1600000000 <= epoch <= 2147483000:
            return fail(400, "BAD_VALUE", "epoch ไม่ถูกต้อง")
        return ok("time set")      # นาฬิกาจำลองใช้เวลาของคอมอยู่แล้ว

    # ---------------------------------------------------------- ปุ่มควบคุมผู้ถูกทดสอบจำลอง
    @app.post("/sim/mode")
    async def sim_mode(mode: str = "random"):
        """ปุ่มควบคุมนาฬิกาจำลอง: ผู้ตอบจำลองโกหก/พูดจริง/สุ่ม/ตื่นเต้นตลอด"""
        if mode not in ("random", "lie", "truth", "nervous"):
            return fail(400, "BAD_MODE", "mode: random / lie / truth / nervous")
        vw.mode = mode
        return ok(f"คำถามจริงทุกข้อ: {mode}")

    @app.post("/sim/next")
    async def sim_next(mode: str = "lie"):
        """กำหนดว่าคำถามจริงข้อถัดไปให้ผู้ตอบจำลองโกหกหรือพูดจริง"""
        vw.next_override = mode
        return ok(f"คำถามจริงข้อถัดไป: {mode}")

    @app.post("/sim/contact")
    async def sim_contact(ppg: int = 1, gsr: int = 1):
        """จำลองเซนเซอร์หลุด/แตะผิว (ทดสอบกรณีสัญญาณหาย)"""
        vw.subject.ppg_contact, vw.subject.gsr_contact = bool(ppg), bool(gsr)
        return ok(f"PPG={'แตะ' if ppg else 'หลุด'} GSR={'แตะ' if gsr else 'หลุด'}")

    @app.post("/sim/motion")
    async def sim_motion(level: float = 0.01):
        """จำลองการขยับตัว"""
        vw.subject.motion = max(0.0, min(5.0, level))
        return ok(f"motion={level}")

    @app.post("/sim/secret")
    async def sim_secret(n: int = 0):
        """สุ่มเลขลับใหม่ (เกมทายเลขสาธิต)"""
        vw.secret = n if 1 <= n <= 5 else random.randint(1, 5)
        return ok("ตั้งเลขลับใหม่แล้ว (ซ่อนไว้)")

    @app.post("/sim/new_subject")
    async def sim_new_subject():
        """เปลี่ยนเป็นผู้ตอบจำลองคนใหม่"""
        vw.subject = Subject()
        return ok("เปลี่ยนผู้ถูกทดสอบจำลองคนใหม่")

    @app.get("/sim/state")
    async def sim_state():
        """สถานะของตัวจำลอง (โหมด, การสัมผัส, คำตอบจริงล่าสุด)"""
        return {"mode": vw.mode, "next": vw.next_override, "secret": vw.secret,
                "ppg": vw.subject.ppg_contact, "gsr": vw.subject.gsr_contact, "motion": vw.subject.motion,
                "truth": list(reversed(vw.truth_log[-20:]))}

    return app


async def main_async(udp_port: int, http_port: int, speed: float = 1.0):
    """เปิด UDP + เว็บเซิร์ฟเวอร์ของนาฬิกาจำลอง แล้ววนสร้างสัญญาณ 100 ครั้ง/วินาที"""
    vw = VirtualWatch(udp_port, http_port, speed)
    loop = asyncio.get_running_loop()
    await loop.create_datagram_endpoint(lambda: vw, local_addr=("127.0.0.1", udp_port))
    app = make_app(vw)
    server = uvicorn.Server(uvicorn.Config(app, host="127.0.0.1", port=http_port, log_level="warning"))
    print("=" * 62)
    print(f" นาฬิกาจำลอง {DEVICE_ID} พร้อมแล้ว  UDP 127.0.0.1:{udp_port}  HTTP http://127.0.0.1:{http_port}")
    print(f" หน้าเว็บนาฬิกา : http://127.0.0.1:{http_port}/" + (f"   (เร่งเวลา x{vw.speed:g})" if vw.speed != 1 else ""))
    print(" Studio        : python -m backend --sim")
    print(" เก็บข้อมูล AI  : python ml/collect.py --sim        หยุด: Ctrl + C")
    print("=" * 62)
    await asyncio.gather(vw.run(), server.serve())


def main():
    """จุดเริ่ม: อ่าน argument (--udp, --http, --speed) แล้วรัน main_async"""
    try:
        sys.stdout.reconfigure(errors="replace")
    except (AttributeError, ValueError):
        pass
    p = argparse.ArgumentParser(description="Virtual Polygraph Watch (simulator)")
    p.add_argument("--udp", type=int, default=4211)
    p.add_argument("--http", type=int, default=8081)
    p.add_argument("--speed", type=float, default=1.0, help="เร่งเวลาจำลอง (1 = เวลาจริง, สูงสุด 20)")
    a = p.parse_args()
    try:
        asyncio.run(main_async(a.udp, a.http, a.speed))
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
