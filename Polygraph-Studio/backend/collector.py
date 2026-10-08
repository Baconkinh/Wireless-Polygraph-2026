"""
collector.py — ตัวควบคุม "รอบการถาม" ฝั่ง backend ของ 2 หน้า
  * หน้า "เก็บข้อมูลเทรน AI"  โหมด fix / manual  (รู้เฉลยจากผู้ถาม/ผู้ตอบ)
  * หน้า "ใช้งานจริง"         โหมด live          (นาฬิกาตัดสินก่อน แล้วผู้ใช้บอกว่าตอบถูก/ผิด/ไม่ทราบ)

หน้าที่
  * ใช้ ml/recorder.py ตัวเดียวกับโปรแกรม collect_data.bat -> ได้ไฟล์ data/result_<รอบ>.csv รูปแบบเดียวกันเป๊ะ
  * รับค่าสด/ผลจากนาฬิกาผ่าน sessions.Interrogation (ซึ่งรับมาจาก watch_link.WatchLink อีกที)
  * แจ้งสถานะไปหน้าเว็บทาง WebSocket (hub.publish("collect", ...)) — ทั้ง 2 หน้าฟังข้อความเดียวกัน
  * มีได้ทีละ 1 รอบ (นาฬิกามีเครื่องเดียว ถามได้ทีละข้อ)

ลำดับ 1 ข้อ
  หน้าเว็บกด "ถาม" -> ask() -> สั่งนาฬิกา /api/lie/question (qid 600+) -> นาฬิกาวัด 12 วินาที
  -> นาฬิกาส่งผล (UDP event "result" หรือ /api/lie) -> on_result()
  -> fix: บันทึกทันที / manual: รอผู้ตอบบอกเฉลย -> label() / live: รอผู้ใช้บอกถูก-ผิด -> feedback()

กันผลค้าง (บั๊กที่เคยเกิดใน Studio 2.2)
  qid 600, 601 ... ถูกใช้ซ้ำทุกรอบ และ Studio ดึงประวัติผล 24 ข้อล่าสุดจากนาฬิกาเป็นระยะ
  -> ผลของ "ข้อ 1 รอบก่อน" เคยถูกจับคู่กับ "ข้อ 1 รอบใหม่" ทันทีที่กดถาม
  แก้: จำเลข seq ของผลล่าสุดที่นาฬิกามี ณ ตอนกดถาม แล้วรับเฉพาะผลที่ seq ใหม่กว่า + ต้องผ่านเวลาวัดก่อน
"""
from __future__ import annotations

import os
import sys
from typing import Any, Dict, Optional

from . import config as cfgmod

sys.path.insert(0, os.path.join(cfgmod.ROOT, "ml"))
from recorder import RunRecorder  # noqa: E402


class Collector:
    """ตัวควบคุมรอบการถาม (1 รอบต่อครั้ง) ที่หน้า "เก็บข้อมูลเทรน AI" และ "ใช้งานจริง" ใช้ร่วมกัน"""
    def __init__(self, store, hub):
        """store = ฐานข้อมูล SQLite, hub = ส่งสถานะไปหน้าเว็บ; ยังไม่มีรอบ (rec = None) จนกว่าจะกดเริ่ม"""
        self.store = store
        self.hub = hub
        self.link = None                       # ตั้งใน main.py
        self.rec: Optional[RunRecorder] = None
        self.log_lines = []
        self._seq_boot = None                  # boot ของนาฬิกาที่นับ seq อยู่
        self._seq_max = 0                      # seq สูงสุดที่เคยเห็น (ผลทุกข้อ ไม่ว่าจะมาจากหน้าไหน)
        self.on_change = None                  # callback เมื่อไฟล์ result มีข้อใหม่ (หน้า "ข้อมูล" รีเฟรช)

    # ------------------------------------------------------------------ สถานะ
    def _log(self, s: str):
        self.log_lines = (self.log_lines + [s])[-40:]

    def state(self) -> Dict[str, Any]:
        """สถานะทั้งหมดที่หน้าเว็บต้องใช้: รอบปัจจุบัน, ข้อที่กำลังถาม/รอเฉลย, log, ความยาวช่วงวัด, baseline"""
        st = self.rec.state() if self.rec else None
        lie = (self.link.lie if self.link else {}) or {}
        return {"active": self.rec is not None, "run": st, "log": self.log_lines[-12:],
                "window_sec": (lie.get("config") or {}).get("windowSec", 12),
                "baseline": lie.get("baseline")}

    def publish(self):
        """ส่งสถานะล่าสุดไปทุกหน้าเว็บ (ข้อความชนิด "collect")"""
        self.hub.publish("collect", self.state())

    def owns(self, qid: Any) -> bool:
        """qid นี้เป็นของรอบที่กำลังเก็บอยู่ไหม (ช่วง 600-799)"""
        return self.rec is not None and self.rec.owns_qid(qid)

    def _boot(self):
        """รหัสการบูตของนาฬิกา ("<id>#<boot>") — seq ของผลเริ่มนับใหม่ทุกครั้งที่บูต"""
        return self.link.boot_key if self.link else None

    def _window(self) -> float:
        """ความยาวช่วงวัด 1 ข้อ (วินาที) ตามค่าตั้งของนาฬิกา ค่าเริ่มต้น 12"""
        try:
            return float(self.state()["window_sec"] or 12)
        except (TypeError, ValueError):
            return 12.0

    async def _latest_seq(self):
        """ถามนาฬิกาว่าผลล่าสุดมีเลข seq เท่าไร (ก่อนกดถาม) -> ใช้แยกผลใหม่ออกจากผลเก่าในประวัติ
        ถามไม่ได้ -> ใช้ค่าที่เคยเห็น (ถ้า boot เดียวกัน) ไม่งั้น None (recorder จะใช้วิธีสำรองตามเวลาแทน)"""
        boot = self._boot()
        try:
            lie = await self.link.get("/api/lie", n=1)
            seqs = [r.get("seq") for r in (lie.get("results") or []) if isinstance(r.get("seq"), int)]
            floor = max(seqs + [self._seq_max if self._seq_boot == boot else 0])
            return floor, boot
        except Exception:  # noqa: BLE001 - นาฬิกาไม่ตอบตอนนี้ ไม่เป็นไร ใช้วิธีสำรอง
            return (self._seq_max if self._seq_boot == boot else None), boot

    # ------------------------------------------------------------------ คำสั่งจากหน้าเว็บ
    def start(self, subject: str, operator: str, mode: str) -> Dict[str, Any]:
        """เริ่มรอบใหม่ (ถ้ามีรอบค้างอยู่ จบรอบนั้นก่อน) — ไฟล์ยังไม่ถูกสร้างจนกว่าจะเริ่มข้อแรก"""
        if self.rec:
            self.stop()
        self.log_lines = []
        self.rec = RunRecorder(cfgmod.DATA_DIR, subject or "-", operator or "-", mode,
                               log=self._log, on_done=self._on_done, source="studio")
        kind = "ใช้งานจริง" if self.rec.mode == "live" else "เก็บข้อมูล"
        self._log(f"เริ่มรอบ{kind} {self.rec.run_id} — ไฟล์จะถูกสร้างเมื่อเริ่มข้อแรก")
        self.publish()
        return {"ok": True, **self.state()}

    async def ask(self, label: Optional[str], text: str = "") -> Dict[str, Any]:
        """เริ่มถาม 1 ข้อ: สั่งนาฬิกาเริ่มวัด แล้วจำข้อนี้ไว้รอผล"""
        if not self.rec:
            return {"ok": False, "msg": "ยังไม่ได้เริ่มรอบ — กรอกชื่อแล้วกดเริ่มก่อน"}
        cur = self.rec.current
        if cur and cur["status"] == "asking":
            return {"ok": False, "msg": "กำลังบันทึกข้ออยู่ รอให้ครบเวลาก่อน"}
        if self.rec.mode == "fix" and label not in ("truth", "lie"):
            return {"ok": False, "msg": "โหมด fix ต้องเลือกว่าข้อนี้ให้ตอบจริงหรือโกหก"}
        n = self.rec.next_question()
        # fix: บอกนาฬิกาด้วยว่าเป็นข้อควบคุม (ใช้ปรับเกณฑ์ได้) / manual, live: ข้อทดสอบธรรมดา
        kind = label if self.rec.mode == "fix" else "test"
        floor, boot = await self._latest_seq()          # ผลที่ seq <= floor คือผลเก่าก่อนกดถาม
        res = await self.link.post("/api/lie/question", qid=n["watch_qid"], kind=kind)
        if res.get("ok"):
            if not self.rec.files_created:
                self.store.add_training_run(self.rec.run_id, self.rec.subject, self.rec.operator, self.rec.mode,
                                            self.rec.state()["files"]["signals"],
                                            os.path.basename(self.rec.paths["results"]))
            self.rec.start_question(self.rec.mode, label if self.rec.mode == "fix" else None, text=text,
                                    seq_floor=floor, boot=boot,
                                    min_delay=min(8.0, self._window() * 0.6))
            self.publish()
        return res

    def label(self, label: str) -> Dict[str, Any]:
        """โหมด manual: ผู้ตอบบอกเฉลยหลังได้ผล (truth / lie / unknown)"""
        if not self.rec or not self.rec.current:
            return {"ok": False, "msg": "ไม่มีข้อที่รอเฉลย"}
        if label == "unknown":
            if self.rec.current["status"] != "waiting_label":
                return {"ok": False, "msg": "ข้อนี้ยังไม่ได้ผล"}
            self.rec._finish_unlabeled()
        elif not self.rec.set_label(label):
            return {"ok": False, "msg": "เฉลยต้องเป็น truth หรือ lie"}
        self.publish()
        return {"ok": True}

    def feedback(self, fb: str) -> Dict[str, Any]:
        """โหมด live: นาฬิกาตอบถูก (correct) / ผิด (wrong) / ไม่ทราบ (unknown)
        ถ้านาฬิกาตอบไม่แน่ชัด ส่งคำตอบจริงมาแทน (truth / lie)"""
        if not self.rec or not self.rec.current or self.rec.current["status"] != "waiting_label":
            return {"ok": False, "msg": "ยังไม่มีผลที่รอให้บอกว่าถูกหรือผิด"}
        if not self.rec.set_feedback(fb):
            return {"ok": False, "msg": "ค่าต้องเป็น correct / wrong / unknown (หรือ truth / lie เมื่อนาฬิกาตอบไม่แน่ชัด)"}
        self.publish()
        return {"ok": True}

    async def abort(self) -> Dict[str, Any]:
        """ยกเลิกข้อปัจจุบัน: บอกนาฬิกาให้หยุดวัด (ถ้ากำลังวัด) แล้วบันทึกข้อนี้เป็น aborted (ไม่ใช้เทรน)"""
        if self.rec and self.rec.current:
            if self.rec.current["status"] == "asking":
                try:
                    await self.link.post("/api/lie/abort")
                except Exception:  # noqa: BLE001 - ยกเลิกฝั่งคอมได้แม้นาฬิกาไม่ตอบ
                    pass
            self.rec.cancel_question("ผู้ใช้กดยกเลิก")
            self.publish()
        return {"ok": True}

    def set_mode(self, mode: str) -> Dict[str, Any]:
        """สลับ fix <-> manual ระหว่างรอบเก็บข้อมูล (โหมด live สลับไม่ได้ เพราะเป็นอีกหน้า)"""
        if not self.rec:
            return {"ok": False, "msg": "ยังไม่ได้เริ่มรอบเก็บข้อมูล"}
        if self.rec.current:
            return {"ok": False, "msg": "เปลี่ยนโหมดระหว่างข้อไม่ได้ — จบข้อนี้ก่อน"}
        if mode not in ("fix", "manual") or self.rec.mode == "live":
            return {"ok": False, "msg": "สลับได้เฉพาะ fix / manual ในหน้าเก็บข้อมูล"}
        self.rec.mode = mode
        self.publish()
        return {"ok": True}

    def stop(self) -> Dict[str, Any]:
        """จบรอบ: ปิดไฟล์, บันทึกเวลาจบลง SQLite, แจ้งหน้าเว็บ — ถ้ายังไม่ได้ถามสักข้อจะไม่มีไฟล์เกิดขึ้น"""
        if self.rec:
            created = self.rec.files_created
            run_id = self.rec.run_id
            self.rec.close()
            if created:
                self.store.end_training_run(run_id)
            self._log("จบรอบ" + ("" if created else " — ไม่มีข้อมูล จึงไม่ได้สร้างไฟล์"))
            self.rec = None
            self.publish()
            if created and self.on_change:
                self.on_change()
        return {"ok": True}

    # ------------------------------------------------------------------ จาก WatchLink (ผ่าน Interrogation)
    def on_vitals(self, v: Dict[str, Any]):
        if self.rec:
            self.rec.on_vitals(v)

    def on_result(self, r: Dict[str, Any]) -> bool:
        """คืน True ถ้าผลนี้เป็นของหน้าเก็บข้อมูล/ใช้งานจริง (เซสชันทดสอบจะได้ไม่เอาไปปน)"""
        boot = self._boot()
        if boot != self._seq_boot:                # นาฬิการีบูต -> seq เริ่มนับใหม่
            self._seq_boot, self._seq_max = boot, 0
        seq = r.get("seq")
        if isinstance(seq, int):
            self._seq_max = max(self._seq_max, seq)
        if not self.rec or not self.rec.owns_qid(r.get("qid")):
            return False
        if self.rec.on_result(r, boot=boot) is not None:
            self.publish()
        return True

    def _on_done(self, entry: Dict[str, Any]):
        """recorder เรียกทุกครั้งที่จบ 1 ข้อ: บันทึกข้อนั้นลง SQLite (ตาราง training_questions) และแจ้งหน้าข้อมูลให้รีเฟรช"""
        if self.rec:
            self.store.add_training_question(self.rec.run_id, entry)
        if self.on_change:
            self.on_change()
