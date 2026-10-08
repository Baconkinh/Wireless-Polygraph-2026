"""
recorder.py — ตัวบันทึก 1 รอบ (run) ของการถามคำถาม ลงไฟล์ CSV ในคอม

ใช้ร่วมกัน 2 ที่ (รูปแบบไฟล์จึงเหมือนกันเสมอ):
  * ml/collect.py            — โปรแกรมเก็บข้อมูลแบบพิมพ์คำสั่ง (collect_data.bat)
  * backend/collector.py     — หน้า "เก็บข้อมูลเทรน AI" และหน้า "ใช้งานจริง" ใน Polygraph Studio

แนวคิดหลัก
  1) ทุกข้อมีเลขลำดับ (question_no = 1, 2, 3 ...) และเลข qid ที่ส่งให้นาฬิกา (watch_qid = 600 + ลำดับ)
     -> ทุกแถวในไฟล์บอกได้ว่ามาจาก "ข้อไหน" และ "เฉลยคืออะไร" (label)
  2) โหมดของรอบ 3 แบบ
       fix    : ผู้ถามบอกผู้ตอบ "ก่อนถาม" ว่าข้อนี้ให้ตอบจริง/ให้โกหก -> รู้เฉลยตั้งแต่เริ่มข้อ
       manual : ถามไปเลย ผู้ตอบเลือกเองว่าจะจริงหรือโกหก หมดเวลาแล้วค่อย "สารภาพ" ว่าข้อนี้โกหกไหม
       live   : ใช้งานจริง — นาฬิกาตัดสินก่อน แล้วผู้ใช้บอกว่านาฬิกาตอบ "ถูก / ผิด / ไม่ทราบ"
                (ถูก/ผิด แปลงเป็นเฉลยได้ จึงใช้เทรนต่อได้ด้วย)
     ระหว่างรอเฉลย แถวสัญญาณของข้อนั้นถูกพักไว้ในหน่วยความจำ แล้วค่อยเขียนพร้อมเฉลย
  3) ไม่สร้างไฟล์เปล่า: ไฟล์ถูกสร้างตอน "เริ่มข้อแรก" เท่านั้น
     ช่วงก่อนหน้า (วัด baseline / นั่งพัก) เก็บไว้ในหน่วยความจำก่อน แล้วค่อยเขียนตามลงไปตอนสร้างไฟล์
  4) กันผลค้าง: ผลที่มาถึงเร็วกว่าเวลาวัด หรือเป็นผลเก่าของนาฬิกา (เลข seq ไม่ใหม่กว่าตอนเริ่มข้อ)
     จะไม่ถูกจับคู่กับข้อปัจจุบัน (Studio 2.2 เคยจับคู่ผลของรอบก่อนที่มี qid ซ้ำกัน)

ไฟล์ที่ได้ — ความหมายของทุกคอลัมน์อยู่ใน DATA_DICTIONARY.md
  data/result_<run_id>.csv           1 แถว/ข้อ: เฉลย, คำตัดสินของนาฬิกา, feature 12 ตัว  <- train.py ใช้ไฟล์นี้
  data/signals/signals_<run_id>.csv  ค่าเซนเซอร์ทุกแถว (5 ครั้ง/วินาที) + phase + question_no + label
                                     (ไว้ดูกราฟ/วิเคราะห์ย้อนหลัง ไม่ได้ใช้เทรน)
ใช้แต่ไลบรารีมาตรฐานของ Python
"""
from __future__ import annotations

import collections
import csv
import os
import time
from typing import Any, Callable, Dict, List, Optional

import datafiles as df
import polyml as pm

QID_BASE = 600            # qid ที่ส่งให้นาฬิกา = 600 + ลำดับ (Studio ใช้ 1-599, Serial 800+, ปุ่ม 900+ -> ไม่ชนกัน)
QID_SPAN = 200            # วนกลับหลังข้อที่ 200 (นาฬิการับ qid ได้ถึง 65535 แต่อยากให้อ่านง่าย)
PRE_BUFFER_ROWS = 5 * 60 * 5   # ช่วงก่อนข้อแรกเก็บได้ 5 นาที (5 แถว/วินาที) — เก่ากว่านั้นทิ้ง
MODES = ("fix", "manual", "live")

ENGINE_STATE = {0: "idle", 1: "baseline", 2: "ready", 3: "question"}
SIGNAL_HEADER = ["time_iso", "t_sec", "run_id", "subject", "phase", "question_no", "watch_qid", "mode",
                 "label", "hr_bpm", "hrv_ms", "ppg_contact", "gsr_us", "gsr_tonic_us", "gsr_phasic_us",
                 "gsr_contact", "skin_temp_c", "tremor_ms2", "motion_ms2", "stress_index", "engine_state",
                 "battery_mv"]


class RunRecorder:
    """บันทึก 1 รอบ: เรียก on_vitals() ทุกครั้งที่ได้ค่าสด, start_question() ตอนเริ่มข้อ,
    on_result() ตอนนาฬิกาส่งผล, set_label()/set_feedback() ตอนได้เฉลย, close() ตอนจบ"""

    def __init__(self, data_dir: str, subject: str, operator: str = "", mode: str = "fix",
                 log: Optional[Callable[[str], None]] = None,
                 on_done: Optional[Callable[[Dict[str, Any]], None]] = None, source: str = "desktop"):
        """เตรียมรอบใหม่ (ยังไม่สร้างไฟล์): ตั้งชื่อรอบจากวันเวลา, กำหนด path ไฟล์ result และ signals"""
        self.data_dir = data_dir
        self.subject = pm.clean_subject(subject)
        self.operator = pm.clean_subject(operator) if operator else "-"
        self.mode = mode if mode in MODES else "fix"
        self.source = source                          # desktop / desktop_cli / desktop_sim — เขียนลงคอลัมน์ source (+ "ที่มา:" ในหมายเหตุ)
        self.run_id = df.new_run_id(data_dir)         # วันเวลาเริ่มรอบ (ไม่ซ้ำกับไฟล์ที่มีอยู่)
        self.t0 = time.time()
        self.log = log or (lambda s: None)
        self.on_done = on_done or (lambda entry: None)   # เรียกทุกครั้งที่จบ 1 ข้อ (Studio ใช้บันทึกลง SQLite)
        self.paths = {
            "results": df.result_path(data_dir, self.run_id),
            "signals": os.path.join(data_dir, df.SIGNALS_DIR, f"signals_{self.run_id}.csv"),
        }
        self._sig_fh = None                    # ไฟล์ signals (เปิดค้างไว้ เขียนบ่อย)
        self._sig_w = None
        self.files_created = False
        self.question_no = 0                   # เลขข้อล่าสุดที่เริ่มไปแล้ว
        self.current: Optional[Dict[str, Any]] = None   # ข้อที่กำลังถามหรือรอเฉลย
        self.pre_buffer: collections.deque = collections.deque(maxlen=PRE_BUFFER_ROWS)
        self.pending_rows: List[Dict[str, Any]] = []    # แถวสัญญาณของข้อที่ยังไม่รู้เฉลย
        self.counts = {"truth": 0, "lie": 0, "invalid": 0, "aborted": 0, "unknown": 0,
                       "correct": 0, "wrong": 0}
        self.history: List[Dict[str, Any]] = []         # สรุปทุกข้อในรอบนี้ (ไว้แสดงบนจอ)

    # ------------------------------------------------------------------ ข้อมูลสำหรับแสดงผล
    def next_question(self) -> Dict[str, Any]:
        no = self.question_no + 1
        return {"question_no": no, "watch_qid": QID_BASE + (no - 1) % QID_SPAN}

    def owns_qid(self, qid: Any) -> bool:
        """qid นี้อยู่ในช่วงของหน้าเก็บข้อมูล/ใช้งานจริง (600-799) ไหม"""
        return isinstance(qid, int) and QID_BASE <= qid < QID_BASE + QID_SPAN

    def state(self) -> Dict[str, Any]:
        """สรุปสถานะรอบนี้สำหรับหน้าเว็บ (ไม่รวมผลดิบที่ใหญ่)"""
        cur = dict(self.current) if self.current else None
        if cur:
            cur.pop("result_raw", None)
        return {
            "run_id": self.run_id, "subject": self.subject, "operator": self.operator, "mode": self.mode,
            "next": self.next_question(), "current": cur, "counts": dict(self.counts),
            "files_created": self.files_created,
            "files": {"results": os.path.basename(self.paths["results"]),
                      "signals": f"{df.SIGNALS_DIR}/{os.path.basename(self.paths['signals'])}"},
            "history": self.history[-50:],
        }

    # ------------------------------------------------------------------ ไฟล์ (สร้างเมื่อมีข้อมูลจริงเท่านั้น)
    def _ensure_files(self):
        if self.files_created:
            return
        os.makedirs(os.path.dirname(self.paths["signals"]), exist_ok=True)
        self._sig_fh = open(self.paths["signals"], "a", encoding="utf-8-sig", newline="")
        self._sig_w = csv.writer(self._sig_fh)
        self._sig_w.writerow(SIGNAL_HEADER)
        self.files_created = True
        for row in self.pre_buffer:            # ช่วง baseline/พักก่อนข้อแรก
            self._write_signal(row)
        self.pre_buffer.clear()
        self.log(f"สร้างไฟล์ data/{os.path.basename(self.paths['results'])} "
                 f"และ data/{df.SIGNALS_DIR}/{os.path.basename(self.paths['signals'])}")

    def _write_signal(self, row: Dict[str, Any]):
        """เขียนค่าสด 1 แถวลงไฟล์ signals"""
        self._sig_w.writerow([row.get(k, "") for k in SIGNAL_HEADER])

    def _write_result(self, row: Dict[str, Any]):
        """ต่อท้ายแถวผล 1 ข้อลงไฟล์ result_<รอบ>.csv"""
        # เปิด-เขียน-ปิดทุกข้อ: ไฟล์ผลไม่ค้างเปิด -> เปิดดูใน Excel ระหว่างเก็บได้ และไฟดับก็ไม่เสียข้อที่บันทึกแล้ว
        df.write_rows(self.paths["results"], [row], append=True)

    def flush(self):
        """เขียนข้อมูลที่ค้างใน buffer ลงดิสก์ทันที"""
        if self._sig_fh:
            self._sig_fh.flush()

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
            "time_iso": df.iso(now), "t_sec": round(now - self.t0, 2), "run_id": self.run_id,
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
            self.pending_rows.append(row)            # รอเฉลยก่อนค่อยเขียน
        else:
            self._write_signal(row)

    # ------------------------------------------------------------------ เริ่มข้อ
    def start_question(self, mode: Optional[str] = None, label: Optional[str] = None, text: str = "",
                       seq_floor: Optional[int] = None, boot: Any = None, min_delay: float = 6.0
                       ) -> Dict[str, Any]:
        """label = "truth"/"lie" (โหมด fix) หรือ None (manual/live = รู้ทีหลัง)
        seq_floor/boot : เลข seq ของผลล่าสุดที่นาฬิกามี ณ ตอนเริ่มข้อ -> ผลที่ seq ไม่มากกว่านี้คือของเก่า
        min_delay      : ผลที่มาถึงเร็วกว่านี้ (วินาที) นับจากเริ่มข้อ เป็นไปไม่ได้ -> ถือเป็นผลค้าง"""
        if self.current and self.current["status"] == "waiting_label":
            # ยังไม่ได้ให้เฉลยข้อก่อน -> ถือว่าไม่รู้เฉลย (ไม่เอาไปเทรน) แล้วเริ่มข้อใหม่ได้เลย
            self._finish_unlabeled()
        mode = mode if mode in MODES else self.mode
        self.mode = mode
        self._ensure_files()
        self.question_no += 1
        nq = {"question_no": self.question_no, "watch_qid": QID_BASE + (self.question_no - 1) % QID_SPAN}
        self.current = dict(nq, mode=mode, label=label if mode == "fix" else None, status="asking",
                            started=time.time(), result=None, text=df._clean_text(text),
                            seq_floor=seq_floor, boot=boot, min_delay=min_delay)
        self.pending_rows = []
        self.log(f"เริ่มข้อที่ {self.question_no} (qid {nq['watch_qid']}) โหมด {mode}"
                 + (f" เฉลย = {label}" if label and mode == "fix" else " — รอเฉลยหลังได้ผล"))
        return self.current

    def cancel_question(self, reason: str = "aborted"):
        """ยกเลิกข้อปัจจุบัน (ขยับตัวแรง/ถามผิด) — เขียนแถว label = aborted ไว้ด้วย (ไม่ใช้เทรน)
        เพื่อให้เลขข้อในไฟล์ต่อเนื่อง ไม่มีข้อ "หายไป" เฉย ๆ"""
        cur = self.current
        if not cur:
            return
        for row in self.pending_rows:
            row["label"] = "aborted"
            self._write_signal(row)
        self.pending_rows = []
        self.counts["aborted"] += 1
        row = df.row_from_result(cur.get("result_raw") or {}, run_id=self.run_id, source=self.source,
                                 subject=self.subject, operator=self.operator, question_no=cur["question_no"],
                                 watch_qid=cur["watch_qid"], mode=cur["mode"], label="aborted",
                                 question_text=cur.get("text", ""), note=f"ยกเลิก: {reason}")
        self._write_result(row)
        self.history.append({"question_no": cur["question_no"], "watch_qid": cur["watch_qid"],
                             "mode": cur["mode"], "label": "aborted", "verdict": "-", "p": None,
                             "text": cur.get("text", ""), "used": False})
        self.on_done(dict(self.history[-1], result=None))
        self.current = None
        self.flush()
        self.log(f"ยกเลิกข้อที่ {cur['question_no']} ({reason})")

    # ------------------------------------------------------------------ ผลจากนาฬิกา
    def is_stale(self, r: Dict[str, Any], boot: Any = None) -> bool:
        """ผลนี้เป็นของเก่า (ไม่ใช่ผลของข้อที่เพิ่งถาม) หรือไม่"""
        cur = self.current
        if not cur:
            return True
        seq, floor = r.get("seq"), cur.get("seq_floor")
        if isinstance(seq, int) and isinstance(floor, int) and boot == cur.get("boot"):
            # วิธีหลัก: seq ไม่ใหม่กว่าตอนเริ่มข้อ = ผลเก่าในประวัติของนาฬิกา
            return seq <= floor
        # วิธีสำรอง (ไม่รู้ seq เช่น นาฬิกาเพิ่งรีบูต): ผลที่มาเร็วกว่าเวลาวัด 1 ข้อเป็นไปไม่ได้
        return time.time() - cur["started"] < cur.get("min_delay", 0)

    def on_result(self, r: Dict[str, Any], boot: Any = None) -> Optional[Dict[str, Any]]:
        """r = ผล 1 ข้อ (รูปแบบเดียวกับ /api/lie: seq, qid, verdict, p, src, quality, reasons, ok, feat, z)
        คืนค่าข้อที่จับคู่ได้ (หรือ None ถ้าไม่ใช่ผลของข้อปัจจุบัน)"""
        cur = self.current
        if not cur or r.get("qid") != cur["watch_qid"] or cur["status"] != "asking":
            return None
        if self.is_stale(r, boot):
            key = (r.get("seq"), r.get("qid"))
            if key not in cur.setdefault("stale_seen", []):     # แจ้งครั้งเดียวต่อผล (นาฬิกาส่งซ้ำหลายทาง)
                cur["stale_seen"].append(key)
                self.log(f"ไม่รับผล seq {r.get('seq')} ของ qid {r.get('qid')}: เป็นผลเก่าค้างในนาฬิกา")
            return None
        cur["result_raw"] = r
        cur["result"] = {"verdict": r.get("verdict"), "p": r.get("p"), "src": r.get("src"),
                         "quality": r.get("quality", r.get("q")), "reasons": r.get("reasons", r.get("rs"))}
        if cur.get("label"):
            self._finish(cur["label"])
        else:
            cur["status"] = "waiting_label"
            self.log(f"ข้อที่ {cur['question_no']} ได้ผลแล้ว — "
                     + ("นาฬิกาตอบถูกไหม?" if cur["mode"] == "live" else "รอเฉลย: ผู้ตอบพูดจริงหรือโกหก?"))
        return cur

    def set_label(self, label: str) -> bool:
        """โหมด manual: ผู้ตอบบอกเฉลยหลังตอบ ("truth" / "lie")"""
        cur = self.current
        if not cur or label not in df.LABELS:
            return False
        cur["label"] = label
        for row in self.pending_rows:            # แถวที่พักไว้ได้เฉลยแล้ว -> เขียนลงไฟล์
            row["label"] = label
            self._write_signal(row)
        self.pending_rows = []
        if cur["status"] == "waiting_label":
            self._finish(label)
        return True

    def set_feedback(self, feedback: str) -> bool:
        """โหมด live: ผู้ใช้บอกว่านาฬิกาตอบ correct / wrong / unknown -> แปลงเป็นเฉลยแล้วบันทึก
        ถ้านาฬิกาตอบ "ไม่แน่ชัด" (ถูก/ผิดไม่ได้) ส่งคำตอบจริงมาแทน: truth / lie -> feedback = inconclusive"""
        cur = self.current
        if not cur or cur["status"] != "waiting_label" or \
                feedback not in ("correct", "wrong", "unknown", "truth", "lie"):
            return False
        verdict = (cur.get("result") or {}).get("verdict", "")
        if feedback in df.LABELS:
            label = feedback
            feedback = ("correct" if verdict == label else "wrong") if verdict in df.LABELS else "inconclusive"
        else:
            label = df.label_from_feedback(verdict, feedback)
        if label == "unknown":
            self._finish_unlabeled(feedback=feedback)
            return True
        cur["label"] = label
        for row in self.pending_rows:
            row["label"] = label
            self._write_signal(row)
        self.pending_rows = []
        self._finish(label, feedback=feedback)
        return True

    def _finish(self, label: str, feedback: str = ""):
        """จบข้อแบบรู้เฉลย: สร้างแถว result (feature 12 ตัวแบบที่ AI ใช้), นับยอด, เขียนไฟล์, แจ้ง Studio"""
        cur = self.current
        r = cur.get("result_raw") or {}
        row = df.row_from_result(r, run_id=self.run_id, source=self.source, subject=self.subject,
                                 operator=self.operator, question_no=cur["question_no"],
                                 watch_qid=cur["watch_qid"], mode=cur["mode"], label=label, feedback=feedback,
                                 question_text=cur.get("text", ""))
        usable = row["used_for_training"] == 1
        self.counts[label if usable else "invalid"] += 1
        if feedback in ("correct", "wrong"):
            self.counts[feedback] += 1
        self._write_result(row)
        self.history.append({"question_no": cur["question_no"], "watch_qid": cur["watch_qid"], "mode": cur["mode"],
                             "label": label, "verdict": row["verdict"], "p": r.get("p"), "used": usable,
                             "feedback": feedback, "text": cur.get("text", ""), "decided_by": row["decided_by"]})
        self.on_done(dict(self.history[-1], result=r))
        self.log(f"บันทึกข้อที่ {cur['question_no']}: เฉลย {label}, นาฬิกาทาย {row['verdict']}"
                 + (f" ({feedback})" if feedback else "")
                 + ("" if usable else " (สัญญาณใช้ไม่ได้ — ไม่นำไปเทรน)"))
        self.current = None
        self.flush()

    def _finish_unlabeled(self, feedback: str = ""):
        """จบข้อแบบไม่รู้เฉลย (กดข้าม/ไม่ทราบ/เริ่มข้อใหม่โดยไม่ให้เฉลย) — บันทึกไว้แต่ไม่ใช้เทรน"""
        cur = self.current
        for row in self.pending_rows:
            row["label"] = "unknown"
            self._write_signal(row)
        self.pending_rows = []
        r = cur.get("result_raw") or {}
        row = df.row_from_result(r, run_id=self.run_id, source=self.source, subject=self.subject,
                                 operator=self.operator, question_no=cur["question_no"],
                                 watch_qid=cur["watch_qid"], mode=cur["mode"], label="unknown",
                                 feedback=feedback, question_text=cur.get("text", ""))
        self.counts["unknown"] += 1
        self._write_result(row)
        self.history.append({"question_no": cur["question_no"], "watch_qid": cur["watch_qid"], "mode": cur["mode"],
                             "label": "unknown", "verdict": row["verdict"] or "-", "p": r.get("p"), "used": False,
                             "feedback": feedback, "text": cur.get("text", ""), "decided_by": row["decided_by"]})
        self.on_done(dict(self.history[-1], result=r))
        self.current = None
        self.flush()

    # ------------------------------------------------------------------ จบรอบ
    def close(self):
        if self.current:
            if self.current["status"] == "waiting_label":
                self._finish_unlabeled()
            else:
                self.cancel_question("ปิดโปรแกรมระหว่างถาม")
        if self._sig_fh:
            self._sig_fh.close()
        self._sig_fh = self._sig_w = None
