"""
collector.py — หน้า "เก็บข้อมูลเทรน AI" ฝั่ง backend

หน้าที่: ควบคุมการเก็บข้อมูลทีละข้อ (โหมด fix / manual) แล้วบันทึกลง CSV ในคอม + SQLite
  * ใช้ ml/recorder.py ตัวเดียวกับโปรแกรม collect_data.bat -> ไฟล์รูปแบบเดียวกันเป๊ะ
  * รับค่าสด/ผลจากนาฬิกาผ่าน sessions.Interrogation (ซึ่งรับมาจาก watch_link.WatchLink อีกที)
  * แจ้งสถานะไปหน้าเว็บทาง WebSocket (hub.publish("collect", ...))

ลำดับ 1 ข้อ
  หน้าเว็บกด "ถาม" -> ask() -> สั่งนาฬิกา /api/lie/question (qid 600+) -> นาฬิกาวัด 12 วินาที
  -> นาฬิกาส่งผล (UDP event "result") -> on_result() -> fix: บันทึกทันที / manual: รอผู้ตอบบอกเฉลย -> label()
"""
from __future__ import annotations

import os
import sys
from typing import Any, Dict, Optional

from . import config as cfgmod

sys.path.insert(0, os.path.join(cfgmod.ROOT, "ml"))
from recorder import RunRecorder  # noqa: E402


class Collector:
    def __init__(self, store, hub):
        self.store = store
        self.hub = hub
        self.link = None                       # ตั้งใน main.py
        self.rec: Optional[RunRecorder] = None
        self.log_lines = []

    # ------------------------------------------------------------------ สถานะ
    def _log(self, s: str):
        self.log_lines = (self.log_lines + [s])[-40:]

    def state(self) -> Dict[str, Any]:
        st = self.rec.state() if self.rec else None
        lie = (self.link.lie if self.link else {}) or {}
        return {"active": self.rec is not None, "run": st, "log": self.log_lines[-12:],
                "window_sec": (lie.get("config") or {}).get("windowSec", 12),
                "baseline": lie.get("baseline")}

    def publish(self):
        self.hub.publish("collect", self.state())

    def owns(self, qid: Any) -> bool:
        return self.rec is not None and self.rec.owns_qid(qid)

    # ------------------------------------------------------------------ คำสั่งจากหน้าเว็บ
    def start(self, subject: str, operator: str, mode: str) -> Dict[str, Any]:
        if self.rec:
            self.stop()
        self.log_lines = []
        self.rec = RunRecorder(cfgmod.DATA_DIR, subject or "-", operator or "-", mode,
                               log=self._log, on_done=self._on_done)
        self._log(f"เริ่มรอบเก็บข้อมูล {self.rec.run_id} — ไฟล์จะถูกสร้างเมื่อเริ่มข้อแรก")
        self.publish()
        return {"ok": True, **self.state()}

    async def ask(self, label: Optional[str]) -> Dict[str, Any]:
        if not self.rec:
            return {"ok": False, "msg": "ยังไม่ได้เริ่มรอบเก็บข้อมูล — กรอกชื่อแล้วกด 'เริ่มเก็บข้อมูล' ก่อน"}
        cur = self.rec.current
        if cur and cur["status"] == "asking":
            return {"ok": False, "msg": "กำลังบันทึกข้ออยู่ รอให้ครบเวลาก่อน"}
        if self.rec.mode == "fix" and label not in ("truth", "lie"):
            return {"ok": False, "msg": "โหมด fix ต้องเลือกว่าข้อนี้ให้ตอบจริงหรือโกหก"}
        n = self.rec.next_question()
        kind = label if self.rec.mode == "fix" else "test"
        res = await self.link.post("/api/lie/question", qid=n["watch_qid"], kind=kind)
        if res.get("ok"):
            if not self.rec.files_created:
                self.store.add_training_run(self.rec.run_id, self.rec.subject, self.rec.operator, self.rec.mode,
                                            os.path.basename(self.rec.paths["signals"]),
                                            os.path.basename(self.rec.paths["results"]))
            self.rec.start_question(self.rec.mode, label if self.rec.mode == "fix" else None)
            self.publish()
        return res

    def label(self, label: str) -> Dict[str, Any]:
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

    async def abort(self) -> Dict[str, Any]:
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
        if not self.rec:
            return {"ok": False, "msg": "ยังไม่ได้เริ่มรอบเก็บข้อมูล"}
        if self.rec.current:
            return {"ok": False, "msg": "เปลี่ยนโหมดระหว่างข้อไม่ได้ — จบข้อนี้ก่อน"}
        if mode not in ("fix", "manual"):
            return {"ok": False, "msg": "mode ต้องเป็น fix หรือ manual"}
        self.rec.mode = mode
        self.publish()
        return {"ok": True}

    def stop(self) -> Dict[str, Any]:
        if self.rec:
            created = self.rec.files_created
            run_id = self.rec.run_id
            self.rec.close()
            if created:
                self.store.end_training_run(run_id)
            self._log("จบรอบเก็บข้อมูล" + ("" if created else " — ไม่มีข้อมูล จึงไม่ได้สร้างไฟล์"))
            self.rec = None
            self.publish()
        return {"ok": True}

    # ------------------------------------------------------------------ จาก WatchLink (ผ่าน Interrogation)
    def on_vitals(self, v: Dict[str, Any]):
        if self.rec:
            self.rec.on_vitals(v)

    def on_result(self, r: Dict[str, Any]) -> bool:
        """คืน True ถ้าผลนี้เป็นของหน้าเก็บข้อมูล (เซสชันทดสอบจะได้ไม่เอาไปปน)"""
        if not self.rec or not self.rec.owns_qid(r.get("qid")):
            return False
        if self.rec.on_result(r) is not None:
            self.publish()
        return True

    def _on_done(self, entry: Dict[str, Any]):
        if self.rec:
            self.store.add_training_question(self.rec.run_id, entry)
