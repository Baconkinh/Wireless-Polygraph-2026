"""
sessions.py — ตัวจัดการ "การสอบสวน" (session) และการแปลผลให้คนอ่านเข้าใจง่าย

หน้าที่
  * เก็บรายการคำถาม (ข้อความภาษาไทย) — นาฬิการู้แค่เลข qid กับชนิดคำถาม
  * สั่งนาฬิกาเริ่มคำถาม/บันทึกคำตอบ แล้วจับคู่ "ผล" ที่นาฬิกาส่งกลับมากับคำถาม
  * เขียนคำอธิบายผลเป็นภาษาคน: สัญญาณไหนเปลี่ยนเท่าไร มีผลต่อคำตัดสินแค่ไหน
  * วิเคราะห์ "เกมทายเลขลับ" (Concealed Information Test): ข้อที่ร่างกายตอบสนองแรงสุด = เลขลับ
"""
from __future__ import annotations

import time
from typing import Any, Dict, List, Optional

FEATURES = [
    # key, ชื่อไทย, หน่วยของค่าที่เปลี่ยน, รูปแบบตัวเลข, คำอธิบายสั้น
    ("gsr", "เหงื่อ / ความนำไฟฟ้าผิว (GSR)", "µS", "{:+.3f}", "ต่อมเหงื่อถูกกระตุ้นตอนตื่นเต้น"),
    ("hr", "อัตราการเต้นหัวใจ", "bpm", "{:+.1f}", "หัวใจเต้นเร็วขึ้นตอนเครียด"),
    ("amp", "แรงชีพจรปลายนิ้วลดลง", "%", "{:+.0f}", "หลอดเลือดปลายมือหดตัวตอนเครียด"),
    ("trm", "มือสั่น", "m/s²", "{:+.3f}", "กล้ามเนื้อเกร็ง/สั่น"),
    ("tmp", "ผิวเย็นลง", "°C", "{:+.2f}", "เลือดไปเลี้ยงผิวน้อยลง"),
]
VERDICT_TH = {"lie": "โกหก", "truth": "พูดจริง", "inconclusive": "ไม่แน่ชัด", "invalid": "วัดไม่ได้", "none": "-"}
KIND_TH = {"test": "คำถามจริง", "truth": "ควบคุม (ตอบจริง)", "lie": "ควบคุม (สั่งให้โกหก)", "warmup": "อุ่นเครื่อง"}
REASONS = [(1, "ไม่มีสัญญาณ GSR (ใช้สัญญาณอื่นแทน)"), (2, "เซนเซอร์ชีพจรไม่แตะผิว (ใช้สัญญาณอื่นแทน)"),
           (4, "ขยับตัวมากเกินไป"), (8, "ข้อมูลก่อนถามไม่พอ (ถามติดกันเร็วเกินไป)")]

TEMPLATES: Dict[str, Dict[str, Any]] = {
    "classroom": {
        "name": "สาธิตในห้องเรียน (แนะนำ)",
        "desc": "อุ่นเครื่อง 1 ข้อ → คำถามควบคุม จริง/โกหก อย่างละ 2 ข้อ (ให้ระบบเรียนรู้ผู้ถูกทดสอบ) → คำถามจริง 3 ข้อ",
        "questions": [
            {"kind": "warmup", "text": "ตอนนี้คุณนั่งอยู่บนเก้าอี้ใช่ไหม?", "hint": "อุ่นเครื่อง ไม่นำมาตัดสิน — ตอบตามจริง"},
            {"kind": "truth", "text": "คุณชื่อ {name} ใช่ไหม?", "hint": "ควบคุม (จริง): ให้ตอบ \"ใช่\" ตามความจริง"},
            {"kind": "lie", "text": "คุณอายุ 50 ปีใช่ไหม?", "hint": "ควบคุม (โกหก): สั่งให้ตอบ \"ใช่\" ทั้งที่ไม่จริง"},
            {"kind": "truth", "text": "วันนี้คุณมามหาวิทยาลัยใช่ไหม?", "hint": "ควบคุม (จริง) ข้อ 2: ตอบ \"ใช่\" ตามจริง"},
            {"kind": "lie", "text": "คุณเป็นนักบินอวกาศใช่ไหม?", "hint": "ควบคุม (โกหก) ข้อ 2: สั่งให้ตอบ \"ใช่\""},
            {"kind": "test", "text": "เมื่อคืนคุณนอนก่อนเที่ยงคืนใช่ไหม?", "hint": "ให้ผู้ถูกทดสอบตอบเอง (จะจริงหรือโกหกก็ได้)"},
            {"kind": "test", "text": "คุณเคยลอกการบ้านเพื่อนใช่ไหม?", "hint": "ให้ผู้ถูกทดสอบตอบเอง"},
            {"kind": "test", "text": "คุณชอบวิชา Embedded Systems ใช่ไหม?", "hint": "ให้ผู้ถูกทดสอบตอบเอง"},
        ],
    },
    "card": {
        "name": "เกมทายเลขลับ 1–5 (ไม่ต้องรู้คำตอบล่วงหน้า)",
        "desc": "ให้ผู้ถูกทดสอบแอบเขียนเลข 1–5 ซ่อนไว้ วัด baseline ครั้งเดียว → ข้อควบคุม จริง 1 + โกหก 1 "
                "(ปรับเกณฑ์ตามคน) → ถามเลข 1–5 ต่อกันได้เลย ตอบ \"ไม่ใช่\" ทุกข้อ — ข้อที่ร่างกายตอบสนองแรงสุดคือเลขลับ",
        "questions": [
            {"kind": "warmup", "text": "คุณเลือกเลขไว้ในใจแล้วใช่ไหม?", "hint": "อุ่นเครื่อง — ตอบ \"ใช่\""},
            {"kind": "truth", "text": "คุณชื่อ {name} ใช่ไหม?", "hint": "ควบคุม (จริง): ตอบ \"ใช่\" ตามจริง"},
            {"kind": "lie", "text": "คุณอายุ 50 ปีใช่ไหม?", "hint": "ควบคุม (โกหก): สั่งให้ตอบ \"ใช่\" ทั้งที่ไม่จริง"},
        ] + [
            {"kind": "test", "text": f"เลขที่คุณเลือกคือเลข {n} ใช่ไหม?", "hint": "ตอบ \"ไม่ใช่\" ทุกข้อ",
             "grp": "card", "label": f"card:{n}"} for n in range(1, 6)
        ],
    },
    "quick": {
        "name": "ทดสอบเร็ว 4 ข้อ",
        "desc": "ควบคุม จริง 1 + โกหก 1 แล้วถามจริง 2 ข้อ (ใช้เวลา ~3 นาทีรวม baseline)",
        "questions": [
            {"kind": "truth", "text": "คุณชื่อ {name} ใช่ไหม?", "hint": "ควบคุม (จริง): ตอบ \"ใช่\""},
            {"kind": "lie", "text": "คุณมีพี่น้อง 9 คนใช่ไหม?", "hint": "ควบคุม (โกหก): สั่งให้ตอบ \"ใช่\""},
            {"kind": "test", "text": "คุณเตรียมตัวสอบมาอย่างดีใช่ไหม?", "hint": "ตอบเอง"},
            {"kind": "test", "text": "คุณเคยแอบกินขนมของเพื่อนใช่ไหม?", "hint": "ตอบเอง"},
        ],
    },
    "custom": {"name": "กำหนดเองทั้งหมด", "desc": "เริ่มจากรายการว่าง แล้วเพิ่มคำถามเอง", "questions": []},
}


def normalize_result(r: Dict[str, Any]) -> Dict[str, Any]:
    """แปลงผลจาก UDP event (คีย์ย่อ) ให้อยู่รูปแบบเดียวกับ /api/lie"""
    if "feat" in r:
        return dict(r)
    return {
        "seq": r.get("seq"), "qid": r.get("qid"), "kind": r.get("kind"), "verdict": r.get("verdict"),
        "expected": r.get("exp", "none"), "correct": bool(r.get("cor")), "p": r.get("p"),
        "score": r.get("score"), "quality": r.get("q"), "reasons": r.get("rs", 0), "ok": r.get("ok", 0),
        "feat": r.get("f") or [None] * 5, "z": r.get("z") or [None] * 5, "pre": None,
        "latency": r.get("lat"), "answerAt": r.get("ans"), "answerYes": r.get("ay"), "tStart": None,
        "src": r.get("src"),
    }


def level_of(z: Optional[float]) -> str:
    """แปลงค่า z (เปลี่ยนไปกี่เท่าของความแกว่งปกติ) เป็นคำอ่านง่าย: ปกติ / เพิ่มเล็กน้อย / เพิ่มชัดเจน ..."""
    if z is None:
        return "ไม่มีข้อมูล"
    if z < 1.0:
        return "ปกติ"
    if z < 2.0:
        return "เล็กน้อย"
    if z < 4.0:
        return "ชัดเจน"
    return "สูงมาก"


def explain(r: Dict[str, Any], weights: Optional[List[float]]) -> Dict[str, Any]:
    """อธิบายผล 1 ข้อเป็นภาษาไทย + สัดส่วนที่แต่ละสัญญาณมีต่อคะแนน"""
    w = weights or [0.20, 0.35, 0.25, 0.12, 0.08]     # ค่าเริ่มต้นเท่าเฟิร์มแวร์ v2.1 (ลดน้ำหนัก GSR)
    ok = int(r.get("ok") or 0)
    z = r.get("z") or [None] * 5
    feat = r.get("feat") or [None] * 5
    total_w = sum(w[i] for i in range(5) if ok & (1 << i)) or 1.0
    signals = []
    for i, (key, name, unit, fmt, why) in enumerate(FEATURES):
        has = bool(ok & (1 << i)) and z[i] is not None and feat[i] is not None
        zi = z[i] if has else None
        val = feat[i] * 100 if (has and key == "amp") else feat[i]
        contrib = (w[i] * max(-2.0, min(6.0, zi)) / total_w) if has else 0.0
        signals.append({
            "key": key, "name": name, "why": why, "available": has,
            "value": (fmt.format(val) + " " + unit) if has else "—",
            "z": round(zi, 2) if has else None, "level": level_of(zi) if has else "ไม่มีข้อมูล",
            "contrib": round(contrib, 3), "weight": round(w[i], 3),
        })
    verdict = r.get("verdict", "none")
    p = r.get("p") or 0.0
    strong = [s for s in sorted(signals, key=lambda s: -(s["z"] or -99)) if s["available"] and (s["z"] or 0) >= 2.0]
    if verdict == "invalid":
        summary = "วัดไม่ได้ในข้อนี้ — สัญญาณไม่ดีพอที่จะตัดสิน กรุณาตรวจการแตะเซนเซอร์แล้วถามใหม่"
    elif strong:
        parts = [f"{s['name']} {s['value']}" for s in strong[:3]]
        summary = "ร่างกายตอบสนองผิดปกติหลังถูกถาม: " + ", ".join(parts)
    else:
        summary = "ร่างกายตอบสนองใกล้เคียงช่วงพักปกติ ไม่พบสัญญาณความเครียดที่ชัดเจน"
    if verdict in ("lie", "truth", "inconclusive"):
        summary += f" → ระบบประเมินว่า \"{VERDICT_TH[verdict]}\" (โอกาสโกหก {round(p * 100)}%)"
    warnings = [txt for bit, txt in REASONS if int(r.get("reasons") or 0) & bit]
    q = r.get("quality")
    if isinstance(q, (int, float)) and q < 70 and verdict != "invalid":
        warnings.append(f"คุณภาพสัญญาณ {int(q)}% — ผลอาจคลาดเคลื่อน")
    return {"headline": VERDICT_TH.get(verdict, verdict), "summary": summary, "signals": signals,
            "warnings": warnings, "p": p}


class Interrogation:
    """ตัวกลางของ Studio: รับทุกอย่างจาก WatchLink แล้วกระจายต่อ
      - เซสชันทดสอบ (ชุดคำถาม + รายงาน) เก็บใน SQLite
      - หน้าเก็บข้อมูล/ใช้งานจริง (collector) เขียน result_*.csv
      - หน้าเว็บ (hub) ได้ค่าสด/เหตุการณ์/ผล
    """
    def __init__(self, store, hub):
        """ยังไม่มีเซสชันที่เปิดอยู่ (active = None) — link/collector ถูกผูกทีหลังใน main.py"""
        self.store = store
        self.hub = hub
        self.link = None                      # ตั้งค่าใน main.py หลังสร้าง WatchLink
        self.collector = None                 # หน้าเก็บข้อมูลเทรน AI (collector.py) — ตั้งใน main.py
        self.active: Optional[int] = None
        self.current_q: Optional[int] = None  # id ของคำถามที่กำลังถาม
        self.start_boot = ""
        self.start_seq: Optional[int] = None
        rows = [s for s in store.list_sessions() if not s["ended"]]
        if rows:                               # เปิด Studio ใหม่ระหว่างเซสชัน -> ทำต่อได้
            self.active = rows[0]["id"]

    # ------------------------------------------------------------------ helpers
    def weights(self) -> Optional[List[float]]:
        lie = self.link.lie if self.link else {}
        cal = lie.get("calibration") or {}
        if cal.get("active") and cal.get("w"):
            return cal["w"]
        return (lie.get("config") or {}).get("w")

    def session_view(self, sid: Optional[int] = None) -> Optional[Dict[str, Any]]:
        """ข้อมูลเซสชันครบชุดสำหรับหน้าเว็บ/รายงาน: คำถาม, ผลแต่ละข้อ, คำอธิบายผล, สรุป"""
        sid = sid or self.active
        if not sid:
            return None
        s = self.store.get_session(sid)
        if not s:
            return None
        latest: Dict[int, Dict[str, Any]] = {}
        orphans = []
        for r in s["results"]:
            if r["question_id"]:
                latest[r["question_id"]] = r          # เรียงตาม id -> ตัวหลังคือผลล่าสุด
            else:
                orphans.append(r)
        w = self.weights()
        counts = {"lie": 0, "truth": 0, "inconclusive": 0, "invalid": 0}
        ctl_total = ctl_ok = 0
        for q in s["questions"]:
            r = latest.get(q["id"])
            q["result"] = r["data"] if r else None
            q["explain"] = explain(r["data"], w) if r else None
            q["kind_th"] = KIND_TH.get(q["kind"], q["kind"])
            if r:
                v = r["data"].get("verdict")
                if v in counts:
                    counts[v] += 1
                if q["kind"] in ("truth", "lie") and v != "invalid":
                    ctl_total += 1
                    ctl_ok += 1 if r["data"].get("correct") else 0
        s["summary"] = {"counts": counts, "controls": {"total": ctl_total, "correct": ctl_ok},
                        "asked": sum(1 for q in s["questions"] if q["asked_at"]),
                        "total": len(s["questions"])}
        s["cit"] = self.cit_analysis(s["questions"])
        s["active"] = (sid == self.active)
        s["current_question"] = self.current_q if sid == self.active else None
        s["orphans"] = len(orphans)
        del s["results"]
        return s

    @staticmethod
    def cit_analysis(questions: List[Dict[str, Any]]) -> List[Dict[str, Any]]:
        """เกมทายเลขลับ: เทียบคะแนนตอบสนองของข้อในกลุ่มเดียวกัน (ไม่ต้องมีคำตอบที่ถูกล่วงหน้า)"""
        groups: Dict[str, List[Dict[str, Any]]] = {}
        for q in questions:
            if q.get("grp") and q.get("result") and q["result"].get("verdict") != "invalid":
                groups.setdefault(q["grp"], []).append(q)
        out = []
        for g, qs in groups.items():
            ranked = sorted(qs, key=lambda q: -(q["result"].get("score") or 0))
            total = sum(1 for q in questions if q.get("grp") == g)
            best = ranked[0]
            margin = (best["result"]["score"] - ranked[1]["result"]["score"]) if len(ranked) > 1 else 0.0
            conf = "สูง" if margin >= 1.0 else ("ปานกลาง" if margin >= 0.5 else "ต่ำ")
            out.append({
                "group": g, "answered": len(ranked), "total": total, "complete": len(ranked) == total,
                "best_question": best["text"], "best_label": best.get("label"), "margin": round(margin, 2),
                "confidence": conf,
                "ranking": [{"text": q["text"], "label": q.get("label"), "score": q["result"].get("score"),
                             "p": q["result"].get("p")} for q in ranked],
            })
        return out

    def publish_session(self):
        """ส่งข้อมูลเซสชันล่าสุดไปทุกหน้าเว็บ (ข้อความชนิด "session")"""
        self.hub.publish("session", self.session_view())

    # ------------------------------------------------------------------ session lifecycle
    def create(self, subject: str, operator: str, notes: str, template: str,
               questions: List[Dict[str, Any]]) -> Dict[str, Any]:
        if self.active:
            self.store.end_session(self.active)
        dev = (self.link.device if self.link else {}) or {}
        sid = self.store.create_session(subject, operator, notes, template, dev.get("id", ""), dev.get("fw", ""))
        for q in questions:
            text = (q.get("text") or "").replace("{name}", subject or "...")
            if text.strip():
                self.store.add_question(sid, q.get("kind", "test"), text.strip(), q.get("hint", ""),
                                        q.get("grp", ""), q.get("label", ""))
        self.active = sid
        self.current_q = None
        # ผลที่อยู่ในนาฬิกาก่อนเริ่มเซสชันนี้ไม่นับ
        self.start_boot = self.link.boot_key if self.link else ""
        if self.link and self.link.connected and self.link.live:
            self.start_seq = int(self.link.live.get("rs") or 0)
        else:
            self.start_seq = None    # ยังไม่รู้ -> ซิงก์ครั้งแรกจะถือว่าผลที่มีอยู่เป็นของเก่า
        self.store.add_event(sid, "session_start", {"subject": subject, "template": template})
        self.publish_session()
        return self.session_view()

    def end(self):
        """ปิดเซสชันที่เปิดอยู่ (บันทึกเวลาจบ)"""
        if self.active:
            self.store.add_event(self.active, "session_end", {})
            self.store.end_session(self.active)
        self.active = None
        self.current_q = None
        self.hub.publish("session", None)

    # ------------------------------------------------------------------ คำสั่งไปนาฬิกา
    async def ask(self, question_id: int) -> Dict[str, Any]:
        q = self.store.get_question(question_id)
        if not q or q["session_id"] != self.active:
            return {"ok": False, "error": "NO_QUESTION", "msg": "ไม่พบคำถามนี้ในเซสชันที่เปิดอยู่"}
        res = await self.link.post("/api/lie/question", qid=q["qid"], kind=q["kind"], label=q.get("label") or None)
        if res.get("ok"):
            now = time.time()
            self.store.mark_asked(q["id"], now)
            self.current_q = q["id"]
            self.store.add_event(self.active, "ask", {"question_id": q["id"], "qid": q["qid"], "text": q["text"]})
            self.publish_session()
        return res

    async def answer(self, yes: bool) -> Dict[str, Any]:
        """ผู้ตอบตอบ ใช่/ไม่ใช่ (กดจากหน้าเว็บ) -> บอกนาฬิกา (ใช้คำนวณเวลาตอบ) + บันทึกลงเซสชัน"""
        res = await self.link.post("/api/lie/answer", ans="yes" if yes else "no")
        if res.get("ok") and self.current_q:
            self.store.mark_answer(self.current_q, "yes" if yes else "no", time.time())
            self.publish_session()
        return res

    # ------------------------------------------------------------------ จาก WatchLink
    def on_status(self, st: Dict[str, Any]):
        self.store.flush_samples()            # เขียน sample ที่ค้างลงดิสก์วินาทีละครั้ง
        self.hub.publish("status", st)

    def on_vitals(self, v: Dict[str, Any]):
        """ค่าสดทุก 0.2 วินาที: เก็บลง SQLite (ถ้ามีเซสชัน), ส่งให้ collector (ถ้ามีรอบเก็บข้อมูล), ส่งไปหน้าเว็บ"""
        if self.active:
            # ผูกแต่ละแถวกับคำถามที่กำลังถาม -> ไฟล์สัญญาณบอกได้ว่าค่านี้มาจากข้อไหน
            self.store.queue_sample(self.active, v, self.current_q)
        if self.collector:
            self.collector.on_vitals(v)
        self.hub.publish("vitals", v)

    def on_wave(self, w: Dict[str, Any]):
        """คลื่นชีพจร (PPG) 100 Hz -> ส่งไปหน้าเว็บวาดกราฟอย่างเดียว ไม่บันทึก"""
        self.hub.publish("wave", w)

    def on_event(self, e: Dict[str, Any]):
        """เหตุการณ์จากนาฬิกา (baseline เสร็จ, แบตอ่อน, ปุ่ม, ผลคำถาม ...) -> หน้าเว็บ + บันทึก"""
        self.hub.publish("event", e)
        if e.get("ev") != "result":
            self.store.add_event(self.active, e.get("ev", "?"), e)
        else:
            self._ingest(normalize_result(e))

    def on_device(self, hi: Dict[str, Any], info: Dict[str, Any], rebooted: bool):
        """ข้อมูลเครื่อง/การรีบูตของนาฬิกา -> แจ้งหน้าเว็บ และบันทึกถ้ารีบูต"""
        data = {"hi": hi, "info": info, "rebooted": rebooted}
        if rebooted:
            self.store.add_event(self.active, "device_reboot", data)
        self.hub.publish("device", data)

    async def on_lie(self, lie: Dict[str, Any]):
        """สถานะ LieEngine + ผล 24 ข้อล่าสุดจาก /api/lie -> ส่งผลที่ยังไม่เคยเห็นเข้า _ingest (กันผลหายถ้า UDP หล่น)"""
        self.hub.publish("lie", lie)
        results = lie.get("results") or []
        if self.active and self.start_seq is None:
            # เซสชันเริ่มตอนยังไม่ได้ต่อนาฬิกา: ผลที่มีอยู่ ณ ตอนนี้ถือเป็นของเก่าทั้งหมด
            self.start_seq = max([r.get("seq") or 0 for r in results] + [0])
            self.start_boot = self.link.boot_key
            return
        for r in reversed(results):           # เก่า -> ใหม่
            self._ingest(r)

    def _ingest(self, r: Dict[str, Any]):
        """จัดการผล 1 ข้อ: ของหน้าเก็บข้อมูล/ใช้งานจริง -> collector, ของควบคุมด่วน -> ข้าม,
        ของเซสชัน -> จับคู่กับคำถาม บันทึก และส่งไปหน้าเว็บ (ตัดผลซ้ำด้วย boot + seq)
        """
        # ผลของหน้าเก็บข้อมูลเทรน AI (qid 600-799) ไม่ปนกับเซสชันทดสอบ
        if self.collector and self.collector.on_result(r):
            return
        if 500 <= (r.get("qid") or 0) < 600:   # คำถามจาก "ควบคุมด่วน" หน้าหลัก (ถามทีละข้อ ไม่อยู่ในเซสชัน)
            return
        if not self.active or not self.link:
            return
        boot = self.link.boot_key
        seq = r.get("seq") or 0
        if boot == self.start_boot and self.start_seq is not None and seq <= self.start_seq:
            return
        if self.store.has_result(self.active, boot, seq):
            if r.get("pre") is not None:      # ข้อมูลจาก /api/lie ครบกว่า event -> อัปเดต
                self.store.update_result_data(self.active, boot, seq, r)
            return
        q = self.store.find_question_for_result(self.active, r.get("qid") or 0)
        if not q:
            qn = r.get("qid") or 0
            src = "ปุ่มบนนาฬิกา" if qn >= 900 else ("Serial console" if qn >= 800 else
                                                   ("หน้าเก็บข้อมูล" if qn >= 600 else "นอก Studio"))
            qid = self.store.add_question(self.active, r.get("kind") or "test",
                                          f"(คำถามที่เริ่มจาก{src} #{r.get('qid')})", "", "", "")
            self.store.mark_asked(qid, time.time() - 12)
            # ใช้ qid ของนาฬิกาเพื่อให้จับคู่ได้
            self.store.set_question_qid(qid, r.get("qid") or 0)
            q = self.store.get_question(qid)
        self.store.add_result(self.active, q["id"], boot, r)
        if self.current_q == q["id"]:
            self.current_q = None
        self.hub.publish("result", {"result": r, "question": q, "explain": explain(r, self.weights())})
        self.publish_session()
