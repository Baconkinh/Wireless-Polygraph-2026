"""
store.py — ฐานข้อมูล SQLite (ไฟล์เดียว data/studio.db ไม่ต้องติดตั้ง server)

ตาราง
  sessions  : การสอบสวนแต่ละครั้ง (ชื่อผู้ถูกทดสอบ, ผู้คุม, เวลา, นาฬิกาที่ใช้)
  questions : คำถามในเซสชัน (ข้อความ, ชนิด, เวลาที่ถาม/ตอบ, qid ที่ส่งให้นาฬิกา)
  results   : ผลที่นาฬิกาคำนวณ (คำตัดสิน, P(โกหก), feature, z-score) — 1 ผลต่อ 1 คำถาม
  samples   : ค่าสัญญาณ 5 ครั้ง/วินาทีระหว่างเซสชัน (ไว้วาดกราฟย้อนหลัง/รายงาน)
  events    : เหตุการณ์ (เริ่ม baseline, นาฬิการีบูต, แบตต่ำ, กดปุ่ม ฯลฯ)
"""
from __future__ import annotations

import csv
import io
import json
import sqlite3
import threading
import time
from typing import Any, Dict, List, Optional

SCHEMA = """
PRAGMA journal_mode=WAL;
CREATE TABLE IF NOT EXISTS sessions(
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  created REAL NOT NULL, ended REAL,
  subject TEXT, operator TEXT, notes TEXT, template TEXT,
  device TEXT, fw TEXT
);
CREATE TABLE IF NOT EXISTS questions(
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  session_id INTEGER NOT NULL REFERENCES sessions(id) ON DELETE CASCADE,
  ord INTEGER NOT NULL, qid INTEGER NOT NULL,
  kind TEXT NOT NULL, text TEXT NOT NULL, hint TEXT, grp TEXT, label TEXT,
  asked_at REAL, answer TEXT, answered_at REAL
);
CREATE TABLE IF NOT EXISTS results(
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  session_id INTEGER REFERENCES sessions(id) ON DELETE CASCADE,
  question_id INTEGER REFERENCES questions(id) ON DELETE SET NULL,
  device_boot TEXT, seq INTEGER, qid INTEGER, kind TEXT,
  verdict TEXT, p REAL, score REAL, quality INTEGER, reasons INTEGER, ok INTEGER,
  data TEXT, received REAL,
  UNIQUE(session_id, device_boot, seq)
);
CREATE TABLE IF NOT EXISTS samples(
  session_id INTEGER NOT NULL, t REAL NOT NULL,
  hr REAL, hrv REAL, gsr REAL, gp REAL, tmp REAL, trm REAL, mot REAL,
  si INTEGER, es INTEGER, con INTEGER, gc INTEGER, vb REAL
);
CREATE INDEX IF NOT EXISTS idx_samples ON samples(session_id, t);
CREATE TABLE IF NOT EXISTS events(
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  session_id INTEGER, t REAL NOT NULL, type TEXT NOT NULL, data TEXT
);
"""

SAMPLE_COLS = ["t", "hr", "hrv", "gsr", "gp", "tmp", "trm", "mot", "si", "es", "con", "gc", "vb"]


class Store:
    def __init__(self, path: str):
        self.path = path
        # check_same_thread=False + lock: ใช้จาก event loop และ thread ของ export ได้ปลอดภัย
        self.db = sqlite3.connect(path, check_same_thread=False)
        self.db.row_factory = sqlite3.Row
        self.lock = threading.RLock()
        with self.lock:
            self.db.executescript(SCHEMA)
            self.db.execute("PRAGMA foreign_keys=ON")
            self.db.commit()
        self._pending_samples: List[tuple] = []

    # ------------------------------------------------------------ helpers
    def _q(self, sql: str, args: tuple = ()) -> List[sqlite3.Row]:
        with self.lock:
            return self.db.execute(sql, args).fetchall()

    def _x(self, sql: str, args: tuple = ()) -> int:
        with self.lock:
            cur = self.db.execute(sql, args)
            self.db.commit()
            return cur.lastrowid

    # ------------------------------------------------------------ sessions
    def create_session(self, subject: str, operator: str, notes: str, template: str,
                       device: str, fw: str) -> int:
        return self._x("INSERT INTO sessions(created,subject,operator,notes,template,device,fw) VALUES(?,?,?,?,?,?,?)",
                       (time.time(), subject, operator, notes, template, device, fw))

    def end_session(self, sid: int):
        self.flush_samples()
        self._x("UPDATE sessions SET ended=? WHERE id=? AND ended IS NULL", (time.time(), sid))

    def delete_session(self, sid: int):
        with self.lock:
            self.db.execute("DELETE FROM samples WHERE session_id=?", (sid,))
            self.db.execute("DELETE FROM events WHERE session_id=?", (sid,))
            self.db.execute("DELETE FROM results WHERE session_id=?", (sid,))
            self.db.execute("DELETE FROM questions WHERE session_id=?", (sid,))
            self.db.execute("DELETE FROM sessions WHERE id=?", (sid,))
            self.db.commit()

    def list_sessions(self) -> List[Dict[str, Any]]:
        rows = self._q("""
          SELECT s.*,
            (SELECT COUNT(*) FROM questions q WHERE q.session_id=s.id) AS n_questions,
            (SELECT COUNT(*) FROM results r WHERE r.session_id=s.id) AS n_results,
            (SELECT COUNT(*) FROM results r WHERE r.session_id=s.id AND r.verdict='lie') AS n_lie,
            (SELECT COUNT(*) FROM results r WHERE r.session_id=s.id AND r.verdict='truth') AS n_truth
          FROM sessions s ORDER BY s.id DESC""")
        return [dict(r) for r in rows]

    def get_session(self, sid: int) -> Optional[Dict[str, Any]]:
        rows = self._q("SELECT * FROM sessions WHERE id=?", (sid,))
        if not rows:
            return None
        s = dict(rows[0])
        s["questions"] = [dict(r) for r in self._q("SELECT * FROM questions WHERE session_id=? ORDER BY ord", (sid,))]
        res = []
        for r in self._q("SELECT * FROM results WHERE session_id=? ORDER BY id", (sid,)):
            d = dict(r)
            d["data"] = json.loads(d["data"]) if d["data"] else {}
            res.append(d)
        s["results"] = res
        s["events"] = [dict(r) for r in self._q("SELECT * FROM events WHERE session_id=? ORDER BY id", (sid,))]
        return s

    # ------------------------------------------------------------ questions
    def add_question(self, sid: int, kind: str, text: str, hint: str = "", grp: str = "",
                     label: str = "") -> int:
        with self.lock:
            # qid 800+ สงวนไว้ให้คำถามจาก Serial console/ปุ่มบนนาฬิกา -> นับเฉพาะ qid < 800
            row = self.db.execute("SELECT COALESCE(MAX(ord),0)+1, COALESCE(MAX(CASE WHEN qid < 800 THEN qid END),0)+1 "
                                  "FROM questions WHERE session_id=?",
                                  (sid,)).fetchone()
            cur = self.db.execute(
                "INSERT INTO questions(session_id,ord,qid,kind,text,hint,grp,label) VALUES(?,?,?,?,?,?,?,?)",
                (sid, row[0], row[1], kind, text, hint, grp, label))
            self.db.commit()
            return cur.lastrowid

    def update_question(self, question_id: int, **fields):
        allowed = {k: v for k, v in fields.items() if k in ("kind", "text", "hint", "grp", "label") and v is not None}
        if not allowed:
            return
        sets = ",".join(f"{k}=?" for k in allowed)
        self._x(f"UPDATE questions SET {sets} WHERE id=?", tuple(allowed.values()) + (question_id,))

    def delete_question(self, question_id: int):
        self._x("DELETE FROM questions WHERE id=? AND asked_at IS NULL", (question_id,))

    def get_question(self, question_id: int) -> Optional[Dict[str, Any]]:
        rows = self._q("SELECT * FROM questions WHERE id=?", (question_id,))
        return dict(rows[0]) if rows else None

    def mark_asked(self, question_id: int, t: float):
        with self.lock:
            # ถามข้อเดิมซ้ำ (เช่นครั้งก่อนวัดไม่ได้) -> ผลเก่ายังเก็บไว้แต่ไม่ผูกกับคำถามแล้ว
            self.db.execute("UPDATE results SET question_id=NULL WHERE question_id=?", (question_id,))
            self.db.execute("UPDATE questions SET asked_at=?, answer=NULL, answered_at=NULL WHERE id=?",
                            (t, question_id))
            self.db.commit()

    def set_question_qid(self, question_id: int, qid: int):
        self._x("UPDATE questions SET qid=? WHERE id=?", (qid, question_id))

    def mark_answer(self, question_id: int, answer: str, t: float):
        self._x("UPDATE questions SET answer=?, answered_at=? WHERE id=?", (answer, t, question_id))

    def find_question_for_result(self, sid: int, qid: int) -> Optional[Dict[str, Any]]:
        """คำถามล่าสุดในเซสชันที่ถามด้วย qid นี้และยังไม่มีผล"""
        rows = self._q("""SELECT q.* FROM questions q
                          WHERE q.session_id=? AND q.qid=? AND q.asked_at IS NOT NULL
                            AND NOT EXISTS (SELECT 1 FROM results r WHERE r.question_id=q.id)
                          ORDER BY q.asked_at DESC LIMIT 1""", (sid, qid))
        # หมายเหตุ: qid ไม่ซ้ำกันภายในเซสชัน (กำหนดตอนเพิ่มคำถาม) ยกเว้นคำถามจากปุ่มบนนาฬิกา
        return dict(rows[0]) if rows else None

    # ------------------------------------------------------------ results
    def add_result(self, sid: Optional[int], question_id: Optional[int], device_boot: str,
                   r: Dict[str, Any]) -> Optional[int]:
        try:
            return self._x("""INSERT INTO results(session_id,question_id,device_boot,seq,qid,kind,verdict,p,score,
                              quality,reasons,ok,data,received) VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?)""",
                           (sid, question_id, device_boot, r.get("seq"), r.get("qid"), r.get("kind"),
                            r.get("verdict"), r.get("p"), r.get("score"), r.get("quality"),
                            r.get("reasons"), r.get("ok"), json.dumps(r, ensure_ascii=False), time.time()))
        except sqlite3.IntegrityError:
            return None   # ได้ผลนี้มาแล้ว (UDP event + HTTP sync ซ้ำกัน)

    def update_result_data(self, sid: Optional[int], device_boot: str, seq: int, r: Dict[str, Any]):
        self._x("""UPDATE results SET data=?, verdict=?, p=?, score=?, quality=?, reasons=?, ok=?
                   WHERE session_id IS ? AND device_boot=? AND seq=?""",
                (json.dumps(r, ensure_ascii=False), r.get("verdict"), r.get("p"), r.get("score"),
                 r.get("quality"), r.get("reasons"), r.get("ok"), sid, device_boot, seq))

    def has_result(self, sid: Optional[int], device_boot: str, seq: int) -> bool:
        rows = self._q("SELECT 1 FROM results WHERE session_id IS ? AND device_boot=? AND seq=?", (sid, device_boot, seq))
        return bool(rows)

    # ------------------------------------------------------------ samples
    def queue_sample(self, sid: int, v: Dict[str, Any]):
        self._pending_samples.append((sid, time.time(), v.get("hr"), v.get("hrv"), v.get("gsr"), v.get("gp"),
                                      v.get("tmp"), v.get("trm"), v.get("mot"), v.get("si"), v.get("es"),
                                      v.get("con"), v.get("gc"), v.get("vb")))

    def flush_samples(self):
        if not self._pending_samples:
            return
        rows, self._pending_samples = self._pending_samples, []
        with self.lock:
            self.db.executemany("INSERT INTO samples VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?)", rows)
            self.db.commit()

    def get_samples(self, sid: int, max_points: int = 2000) -> Dict[str, List]:
        rows = self._q("SELECT " + ",".join(SAMPLE_COLS) + " FROM samples WHERE session_id=? ORDER BY t", (sid,))
        step = max(1, len(rows) // max_points)       # ลดจำนวนจุดให้กราฟเบา
        rows = rows[::step]
        return {c: [r[i] for r in rows] for i, c in enumerate(SAMPLE_COLS)}

    # ------------------------------------------------------------ events
    def add_event(self, sid: Optional[int], typ: str, data: Dict[str, Any]):
        self._x("INSERT INTO events(session_id,t,type,data) VALUES(?,?,?,?)",
                (sid, time.time(), typ, json.dumps(data, ensure_ascii=False)))

    # ------------------------------------------------------------ export
    def export_results_csv(self, sid: int) -> str:
        s = self.get_session(sid)
        out = io.StringIO()
        w = csv.writer(out)
        w.writerow(["order", "question", "kind", "answer", "verdict", "p_lie", "score", "quality", "reasons",
                    "d_gsr_uS", "d_hr_bpm", "d_amp_frac", "d_tremor", "d_temp_C",
                    "z_gsr", "z_hr", "z_amp", "z_trm", "z_tmp", "gsr_peak_latency_s"])
        by_q = {r["question_id"]: r for r in s["results"]}
        for q in s["questions"]:
            r = by_q.get(q["id"])
            d = r["data"] if r else {}
            feat, z = d.get("feat") or [None] * 5, d.get("z") or [None] * 5
            w.writerow([q["ord"], q["text"], q["kind"], q["answer"] or "", d.get("verdict", ""), d.get("p", ""),
                        d.get("score", ""), d.get("quality", ""), d.get("reasons", "")] + list(feat) + list(z) +
                       [d.get("latency", "")])
        return "﻿" + out.getvalue()   # BOM ให้ Excel อ่านภาษาไทยถูก

    def export_samples_csv(self, sid: int) -> str:
        data = self.get_samples(sid, max_points=10 ** 9)
        out = io.StringIO()
        w = csv.writer(out)
        w.writerow(SAMPLE_COLS)
        for i in range(len(data["t"])):
            w.writerow([data[c][i] for c in SAMPLE_COLS])
        return out.getvalue()
