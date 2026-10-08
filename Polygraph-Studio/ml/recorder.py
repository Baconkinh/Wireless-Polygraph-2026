"""
recorder.py — ตัวบันทึก "การเก็บข้อมูลเทรน AI" 1 รอบ (run) ลงไฟล์ CSV ในคอม
ใช้ร่วมกัน 2 ที่ (รูปแบบไฟล์จึงเหมือนกันเสมอ):
  * ml/collect.py            — โปรแกรมเก็บข้อมูลแบบพิมพ์คำสั่ง (collect_data.bat)
  * backend/collector.py     — หน้า "เก็บข้อมูลเทรน AI" ใน Polygraph Studio

แนวคิดหลัก
  1) ทุกข้อมีเลขลำดับ (question_no = 1, 2, 3 ...) และเลข qid ที่ส่งให้นาฬิกา (watch_qid = 600 + ลำดับ)
     -> ทุกแถวในไฟล์บอกได้ว่ามาจาก "ข้อไหน" และ "เฉลยคืออะไร" (label)
  2) โหมดการให้เฉลย 2 แบบ
       fix    : ผู้ถามบอกผู้ตอบ "ก่อนถาม" ว่าข้อนี้ให้ตอบจริง/ให้โกหก -> รู้เฉลยตั้งแต่เริ่มข้อ
       manual : ถามไปเลย ผู้ตอบเลือกเองว่าจะจริงหรือโกหก หมดเวลาแล้วค่อย "สารภาพ" ว่าข้อนี้โกหกไหม
                -> ระหว่างรอเฉลย แถวสัญญาณของข้อนั้นถูกพักไว้ในหน่วยความจำ แล้วค่อยเขียนพร้อมเฉลย
  3) ไม่สร้างไฟล์เปล่า: ไฟล์ถูกสร้างตอน "เริ่มข้อแรก" เท่านั้น
     ช่วงก่อนหน้า (วัด baseline / นั่งพัก) เก็บไว้ในหน่วยความจำก่อน แล้วค่อยเขียนตามลงไปตอนสร้างไฟล์
     กดเริ่มแล้วออกเลยโดยไม่ได้ถามสักข้อ = ไม่มีไฟล์ใหม่เกิดขึ้น

ไฟล์ที่ได้ (โฟลเดอร์ data/)  — ความหมายของทุกคอลัมน์อยู่ใน DATA_DICTIONARY.md
  signals_<run_id>.csv     ค่าเซนเซอร์ทุกแถว (5 ครั้ง/วินาที) + phase + question_no + label
  results_<run_id>.csv     1 แถว/ข้อ: เฉลย, คำตัดสินของนาฬิกา, feature ที่ใช้ตัดสิน
  training_samples.csv     1 แถว/ข้อที่รู้เฉลยและสัญญาณใช้ได้ -> ไฟล์ที่ ml/train.py ใช้เทรน (ต่อท้ายไฟล์เดิม)
ใช้แต่ไลบรารีมาตรฐานของ Python
"""
from __future__ import annotations

import collections
import csv
import os
import time
from typing import Any, Callable, Dict, List, Optional

import polyml as pm

QID_BASE = 600            # qid ที่ส่งให้นาฬิกา = 600 + ลำดับ (Studio ใช้ 1-599, Serial 800+, ปุ่ม 900+ -> ไม่ชนกัน)
QID_SPAN = 200            # วนกลับหลังข้อที่ 200 (นาฬิการับ qid ได้ถึง 65535 แต่อยากให้อ่านง่าย)
PRE_BUFFER_ROWS = 5 * 60 * 5   # ช่วงก่อนข้อแรกเก็บได้ 5 นาที (5 แถว/วินาที) — เก่ากว่านั้นทิ้ง

ENGINE_STATE = {0: "idle", 1: "baseline", 2: "ready", 3: "question"}
SIGNAL_HEADER = ["time_iso", "t_sec", "run_id", "subject", "phase", "question_no", "watch_qid", "mode",
                 "label", "hr_bpm", "hrv_ms", "ppg_contact", "gsr_us", "gsr_tonic_us", "gsr_phasic_us",
                 "gsr_contact", "skin_temp_c", "tremor_ms2", "motion_ms2", "stress_index", "engine_state",
                 "battery_mv"]
RESULT_HEADER = ["time_iso", "run_id", "subject", "operator", "question_no", "watch_qid", "mode", "label",
                 "verdict", "p_lie", "decided_by", "quality", "reasons", "used_for_training"] + \
                ["d_" + s for s in pm.SIGNALS] + ["z_" + s for s in pm.SIGNALS]
DECIDED_BY = {0: "rules", 1: "rules_calibrated", 2: "ai_model"}


def _iso(t: float) -> str:
    return time.strftime("%Y-%m-%d %H:%M:%S", time.localtime(t)) + f".{int((t % 1) * 1000):03d}"


class RunRecorder:
    """บันทึกการเก็บข้อมูล 1 รอบ: เรียก on_vitals() ทุกครั้งที่ได้ค่าสด, start_question() ตอนเริ่มข้อ,
    on_result() ตอนนาฬิกาส่งผล, set_label() ตอนได้เฉลย (โหมด manual), close() ตอนจบ"""

    def __init__(self, data_dir: str, subject: str, operator: str = "", mode: str = "fix",
                 log: Optional[Callable[[str], None]] = None,
                 on_done: Optional[Callable[[Dict[str, Any]], None]] = None):
        self.data_dir = data_dir
        self.subject = pm.clean_subject(subject)
        self.operator = pm.clean_subject(operator) if operator else "-"
        self.mode = mode if mode in ("fix", "manual") else "fix"
        self.run_id = time.strftime("%Y%m%d_%H%M%S")
        self.t0 = time.time()
        self.log = log or (lambda s: None)
        self.on_done = on_done or (lambda entry: None)   # เรียกทุกครั้งที่จบ 1 ข้อ (Studio ใช้บันทึกลง SQLite)
        self.paths = {
            "signals": os.path.join(data_dir, f"signals_{self.run_id}.csv"),
            "results": os.path.join(data_dir, f"results_{self.run_id}.csv"),
            "training": os.path.join(data_dir, "training_samples.csv"),
        }
        self._fh: Dict[str, Any] = {}
        self._w: Dict[str, Any] = {}
        self.files_created = False
        self.question_no = 0                   # เลขข้อล่าสุดที่เริ่มไปแล้ว
        self.current: Optional[Dict[str, Any]] = None   # ข้อที่กำลังถามหรือรอเฉลย
        self.pre_buffer: collections.deque = collections.deque(maxlen=PRE_BUFFER_ROWS)
        self.pending_rows: List[Dict[str, Any]] = []    # แถวของข้อที่ยังไม่รู้เฉลย (manual)
        self.counts = {"truth": 0, "lie": 0, "invalid": 0, "aborted": 0}
        self.history: List[Dict[str, Any]] = []         # สรุปทุกข้อในรอบนี้ (ไว้แสดงบนจอ)

    # ------------------------------------------------------------------ ข้อมูลสำหรับแสดงผล
    def next_question(self) -> Dict[str, Any]:
        no = self.question_no + 1
        return {"question_no": no, "watch_qid": QID_BASE + (no - 1) % QID_SPAN}

    def owns_qid(self, qid: Any) -> bool:
        return isinstance(qid, int) and QID_BASE <= qid < QID_BASE + QID_SPAN

    def state(self) -> Dict[str, Any]:
        cur = dict(self.current) if self.current else None
        if cur:
            cur.pop("result", None)
        return {
            "run_id": self.run_id, "subject": self.subject, "operator": self.operator, "mode": self.mode,
            "next": self.next_question(), "current": cur, "counts": dict(self.counts),
            "files_created": self.files_created,
            "files": {k: os.path.basename(v) for k, v in self.paths.items()},
            "history": self.history[-30:],
        }

    # ------------------------------------------------------------------ ไฟล์ (สร้างเมื่อมีข้อมูลจริงเท่านั้น)
    def _open(self, key: str, header: List[str]):
        path = self.paths[key]
        os.makedirs(self.data_dir, exist_ok=True)
        new = not os.path.exists(path) or os.path.getsize(path) == 0
        fh = open(path, "a", encoding="utf-8", newline="")
        w = csv.writer(fh)
        if new:
            w.writerow(header)
        self._fh[key], self._w[key] = fh, w

    def _ensure_files(self):
        if self.files_created:
            return
        self._open("signals", SIGNAL_HEADER)
        self._open("results", RESULT_HEADER)
        self.files_created = True
        for row in self.pre_buffer:            # ช่วง baseline/พักก่อนข้อแรก
            self._w["signals"].writerow([row[k] for k in SIGNAL_HEADER])
        self.pre_buffer.clear()
        self.log(f"สร้างไฟล์ {os.path.basename(self.paths['signals'])} และ {os.path.basename(self.paths['results'])}")

    def _write_signal(self, row: Dict[str, Any]):
        self._w["signals"].writerow([row.get(k, "") for k in SIGNAL_HEADER])

    def flush(self):
        for fh in self._fh.values():
            fh.flush()

    # ------------------------------------------------------------------ ค่าสด 5 ครั้ง/วินาที
    def on_vitals(self, v: Dict[str, Any]):
        now = time.time()
        es = v.get("es")
        cur = self.current
        if cur and cur["status"] in ("asking", "waiting_label"):
            phase = "question" if es == 3 else "recovery"
        else:
            phase = {1: "baseline", 3: "question"}.get(es, "rest")
        row = {
            "time_iso": _iso(now), "t_sec": round(now - self.t0, 2), "run_id": self.run_id,
            "subject": self.subject, "phase": phase,
            "question_no": cur["question_no"] if cur else "", "watch_qid": cur["watch_qid"] if cur else "",
            "mode": cur["mode"] if cur else "", "label": (cur.get("label") or "") if cur else "",
            "hr_bpm": v.get("hr") if v.get("con") else "", "hrv_ms": v.get("hrv") if v.get("con") else "",
            "ppg_contact": v.get("con"), "gsr_us": v.get("gsr") if v.get("gc") else "",
            "gsr_tonic_us": v.get("gt") if v.get("gc") else "", "gsr_phasic_us": v.get("gp") if v.get("gc") else "",
            "gsr_contact": v.get("gc"), "skin_temp_c": v.get("tmp"), "tremor_ms2": v.get("trm"),
            "motion_ms2": v.get("mot"), "stress_index": v.get("si") if (v.get("si") or -1) >= 0 else "",
            "engine_state": ENGINE_STATE.get(es, es), "battery_mv": v.get("vb"),
        }
        if not self.files_created:
            self.pre_buffer.append(row)              # ยังไม่มีข้อแรก -> พักไว้ในหน่วยความจำ
        elif cur and not cur.get("label"):
            self.pending_rows.append(row)            # manual: รอเฉลยก่อนค่อยเขียน
        else:
            self._write_signal(row)

    # ------------------------------------------------------------------ เริ่มข้อ
    def start_question(self, mode: Optional[str] = None, label: Optional[str] = None) -> Dict[str, Any]:
        """label = "truth" / "lie" (โหมด fix) หรือ None (โหมด manual = รู้ทีหลัง)"""
        if self.current and self.current["status"] == "waiting_label":
            # ยังไม่ได้ให้เฉลยข้อก่อน -> ถือว่าไม่รู้เฉลย (ไม่เอาไปเทรน) แล้วเริ่มข้อใหม่ได้เลย
            self._finish_unlabeled()
        mode = mode if mode in ("fix", "manual") else self.mode
        self.mode = mode
        self._ensure_files()
        self.question_no += 1
        nq = {"question_no": self.question_no, "watch_qid": QID_BASE + (self.question_no - 1) % QID_SPAN}
        self.current = dict(nq, mode=mode, label=label if mode == "fix" else None, status="asking",
                            started=time.time(), result=None)
        self.pending_rows = []
        self.log(f"เริ่มข้อที่ {self.question_no} (qid {nq['watch_qid']}) โหมด {mode}"
                 + (f" เฉลย = {label}" if label else " — รอเฉลยหลังตอบ"))
        return self.current

    def cancel_question(self, reason: str = "aborted"):
        """ยกเลิกข้อปัจจุบัน (ขยับตัวแรง/ถามผิด) — แถวสัญญาณยังเขียนลงไฟล์แต่ label = aborted"""
        cur = self.current
        if not cur:
            return
        for row in self.pending_rows:
            row["label"] = "aborted"
            self._write_signal(row)
        self.pending_rows = []
        self.counts["aborted"] += 1
        self.history.append({"question_no": cur["question_no"], "watch_qid": cur["watch_qid"],
                             "mode": cur["mode"], "label": "aborted", "verdict": "-", "p": None})
        self.on_done(dict(self.history[-1], result=None))
        self.current = None
        self.flush()
        self.log(f"ยกเลิกข้อที่ {cur['question_no']} ({reason})")

    # ------------------------------------------------------------------ ผลจากนาฬิกา
    def on_result(self, r: Dict[str, Any]) -> Optional[Dict[str, Any]]:
        """r = ผล 1 ข้อ (รูปแบบเดียวกับ /api/lie: qid, verdict, p, src, quality, reasons, ok, feat, z)
        คืนค่าข้อที่จับคู่ได้ (หรือ None ถ้าไม่ใช่ข้อของรอบนี้)"""
        cur = self.current
        if not cur or r.get("qid") != cur["watch_qid"] or cur["status"] != "asking":
            return None
        cur["result"] = r
        if cur.get("label"):
            self._finish(cur["label"])
        else:
            cur["status"] = "waiting_label"
            self.log(f"ข้อที่ {cur['question_no']} ได้ผลแล้ว — รอเฉลย: ข้อนี้ผู้ตอบพูดจริงหรือโกหก?")
        return cur

    def set_label(self, label: str) -> bool:
        """โหมด manual: ผู้ตอบบอกเฉลยหลังตอบ ("truth" / "lie")"""
        cur = self.current
        if not cur or label not in ("truth", "lie"):
            return False
        cur["label"] = label
        for row in self.pending_rows:            # แถวที่พักไว้ได้เฉลยแล้ว -> เขียนลงไฟล์
            row["label"] = label
            self._write_signal(row)
        self.pending_rows = []
        if cur["status"] == "waiting_label":
            self._finish(label)
        return True

    def _finish(self, label: str):
        cur = self.current
        r = cur["result"] or {}
        verdict = r.get("verdict", "")
        ok_mask = int(r.get("ok") or 0)
        feat = r.get("feat") or r.get("f") or [None] * 5
        z = r.get("z") or [None] * 5
        usable = verdict != "invalid" and ok_mask != 0
        if usable:
            x = pm.features_from(ok_mask, z, feat)
            self._open_training()
            self._w["training"].writerow(pm.train_row(
                x, epoch=int(time.time()), subject=self.subject, qid=cur["watch_qid"],
                label=1 if label == "lie" else 0, quality=int(r.get("quality") or r.get("q") or 0),
                p_model=float(r.get("p") or 0), source=int(r.get("src") or 0)))
            self.counts[label] += 1
        else:
            self.counts["invalid"] += 1
        self._w["results"].writerow(
            [_iso(time.time()), self.run_id, self.subject, self.operator, cur["question_no"], cur["watch_qid"],
             cur["mode"], label, verdict, r.get("p"), DECIDED_BY.get(int(r.get("src") or 0), r.get("src")),
             r.get("quality", r.get("q")), r.get("reasons", r.get("rs")), int(usable)]
            + [feat[i] if i < len(feat) else "" for i in range(5)] + [z[i] if i < len(z) else "" for i in range(5)])
        self.history.append({"question_no": cur["question_no"], "watch_qid": cur["watch_qid"], "mode": cur["mode"],
                             "label": label, "verdict": verdict, "p": r.get("p"), "used": usable})
        self.on_done(dict(self.history[-1], result=r))
        self.log(f"บันทึกข้อที่ {cur['question_no']}: เฉลย {label}, นาฬิกาทาย {verdict}"
                 + ("" if usable else " (สัญญาณใช้ไม่ได้ — ไม่นำไปเทรน)"))
        self.current = None
        self.flush()

    def _finish_unlabeled(self):
        cur = self.current
        for row in self.pending_rows:
            row["label"] = "unknown"
            self._write_signal(row)
        self.pending_rows = []
        r = cur.get("result") or {}
        self._w["results"].writerow(
            [_iso(time.time()), self.run_id, self.subject, self.operator, cur["question_no"], cur["watch_qid"],
             cur["mode"], "unknown", r.get("verdict", ""), r.get("p"), DECIDED_BY.get(int(r.get("src") or 0), ""),
             r.get("quality", ""), r.get("reasons", ""), 0] + [""] * 10)
        self.history.append({"question_no": cur["question_no"], "watch_qid": cur["watch_qid"], "mode": cur["mode"],
                             "label": "unknown", "verdict": r.get("verdict", "-"), "p": r.get("p"), "used": False})
        self.on_done(dict(self.history[-1], result=r))
        self.current = None

    def _open_training(self):
        if "training" not in self._w:
            self._open("training", pm.TRAIN_HEADER)

    # ------------------------------------------------------------------ จบรอบ
    def close(self):
        if self.current:
            if self.current["status"] == "waiting_label":
                self._finish_unlabeled()
            else:
                self.cancel_question("ปิดโปรแกรมระหว่างถาม")
        for fh in self._fh.values():
            fh.close()
        self._fh, self._w = {}, {}
