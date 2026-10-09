"""
store.py — ฐานข้อมูล SQLite ของ Studio (ไฟล์เดียว data/studio.db ไม่ต้องติดตั้ง database server)

ทำไมใช้ SQLite: เป็นไฟล์เดียว พกไปกับโปรเจกต์ได้ เปิดดูด้วย DBeaver / DB Browser for SQLite ได้ทันที
และเป็น database จริง (มี SQL, index, transaction) เหมาะกับข้อมูลเซสชันที่ต้องค้น/สรุปทีหลัง

ตาราง (ชื่อตาราง/คอลัมน์เป็นภาษาอังกฤษทั้งหมด — ความหมายละเอียดอยู่ใน DATA_DICTIONARY.md)
  sessions            การทดสอบแต่ละครั้ง (ผู้ถูกทดสอบ, ผู้ถาม, เวลาเริ่ม/จบ, นาฬิกาที่ใช้)
  questions           คำถามในเซสชัน (ข้อความ, ชนิด, เวลาที่ถาม/ตอบ, qid ที่ส่งให้นาฬิกา)
  results             ผลที่นาฬิกาคำนวณ (คำตัดสิน, p_lie, feature, z-score) — 1 ผลต่อ 1 คำถาม
  samples             ค่าสัญญาณ 5 ครั้ง/วินาทีระหว่างเซสชัน (+ ข้อที่กำลังถาม + ช่วง baseline/คำถาม)
  events              เหตุการณ์ (เริ่ม baseline, นาฬิการีบูต, แบตต่ำ, กดปุ่ม ฯลฯ)
  training_runs       รอบการเก็บข้อมูลเทรน AI จากหน้า "เก็บข้อมูลเทรน AI"
  training_questions  1 แถว/ข้อในรอบเก็บข้อมูล (เฉลย, คำตัดสิน, feature)
VIEW (ตารางสำเร็จรูปที่อ่านง่าย — เปิดใน DBeaver แล้วดูได้เลย เวลาแปลงเป็นวันที่แล้ว)
  v_sessions_overview     สรุปทุกเซสชัน: จำนวนข้อ, โกหก/จริง
  v_question_results      คำถาม + คำตอบ + ผลการตัดสิน เรียงตามลำดับข้อ
  v_training_questions    ข้อมูลเทรนจากหน้าเก็บข้อมูล พร้อมชื่อผู้ถูกทดสอบ
"""
from __future__ import annotations

import csv
import io
import json
import sqlite3
import threading
import time
from typing import Any, Dict, List, Optional

SCHEMA_VERSION = 2

# หมายเหตุ: comment หลัง -- จะถูกเก็บไว้ในคำสั่ง CREATE ของ SQLite ด้วย -> เปิดแท็บ DDL ใน DBeaver จะเห็นคำอธิบาย
SCHEMA = """
PRAGMA journal_mode=WAL;
CREATE TABLE IF NOT EXISTS sessions(        -- การทดสอบ 1 ครั้ง
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  created REAL NOT NULL,                    -- เวลาเริ่ม (Unix epoch วินาที)
  ended REAL,                               -- เวลาจบ (NULL = ยังเปิดอยู่)
  subject TEXT,                             -- ชื่อผู้ถูกทดสอบ
  operator TEXT,                            -- ชื่อผู้ถาม
  notes TEXT, template TEXT,                -- หมายเหตุ / ชุดคำถามที่ใช้
  device TEXT, fw TEXT                      -- รหัสนาฬิกา / เวอร์ชันเฟิร์มแวร์
);
CREATE TABLE IF NOT EXISTS questions(       -- คำถามในเซสชัน
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  session_id INTEGER NOT NULL REFERENCES sessions(id) ON DELETE CASCADE,
  ord INTEGER NOT NULL,                     -- ลำดับข้อในเซสชัน (1, 2, 3 ...)
  qid INTEGER NOT NULL,                     -- เลขข้อที่ส่งให้นาฬิกา (ผลที่ได้กลับมาจะมี qid นี้)
  kind TEXT NOT NULL,                       -- test=คำถามจริง, truth/lie=คำถามควบคุม, warmup=อุ่นเครื่อง
  text TEXT NOT NULL, hint TEXT,            -- ข้อความคำถาม / คำแนะนำผู้ถาม
  grp TEXT, label TEXT,                     -- กลุ่มคำถาม (เช่น card = เกมทายเลขลับ) / ป้ายของข้อ
  asked_at REAL,                            -- เวลาที่ถาม (epoch)
  answer TEXT,                              -- คำตอบของผู้ถูกทดสอบ yes/no
  answered_at REAL
);
CREATE TABLE IF NOT EXISTS results(         -- ผลการตัดสินจากนาฬิกา
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  session_id INTEGER REFERENCES sessions(id) ON DELETE CASCADE,
  question_id INTEGER REFERENCES questions(id) ON DELETE SET NULL,
  device_boot TEXT, seq INTEGER,            -- "<นาฬิกา>#<รอบบูต>" + ลำดับผลในนาฬิกา (กันบันทึกซ้ำ)
  qid INTEGER, kind TEXT,
  verdict TEXT,                             -- lie / truth / inconclusive / invalid
  p REAL,                                   -- โอกาสโกหก 0..1
  score REAL, quality INTEGER,              -- คะแนนรวม / คุณภาพสัญญาณ 0..100
  reasons INTEGER, ok INTEGER,              -- bit เหตุผล / bit สัญญาณที่ใช้ได้ (ดู DATA_DICTIONARY.md)
  data TEXT,                                -- ผลเต็มรูปแบบ (JSON)
  received REAL,
  UNIQUE(session_id, device_boot, seq)
);
CREATE TABLE IF NOT EXISTS samples(         -- ค่าสัญญาณ 5 ครั้ง/วินาที
  session_id INTEGER NOT NULL,
  t REAL NOT NULL,                          -- เวลา (epoch วินาที)
  question_id INTEGER,                      -- คำถามที่กำลังถามอยู่ (NULL = ไม่ได้ถาม)
  phase TEXT,                               -- baseline / rest / question
  heart_rate_bpm REAL, hrv_ms REAL,
  gsr_us REAL, gsr_phasic_us REAL,
  skin_temp_c REAL, tremor_ms2 REAL, motion_ms2 REAL,
  stress_index INTEGER,                     -- ดัชนีความตื่นตัว 0..100
  engine_state INTEGER,                     -- 0 idle, 1 baseline, 2 ready, 3 question
  ppg_contact INTEGER, gsr_contact INTEGER, -- 1 = เซนเซอร์แตะผิว
  battery_mv REAL
);
CREATE INDEX IF NOT EXISTS idx_samples ON samples(session_id, t);
CREATE TABLE IF NOT EXISTS events(          -- เหตุการณ์ในระบบ
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  session_id INTEGER, t REAL NOT NULL,
  type TEXT NOT NULL,                       -- เช่น ask, cmd_baseline, device_reboot, low_batt
  data TEXT                                 -- รายละเอียด (JSON)
);
CREATE TABLE IF NOT EXISTS training_runs(   -- รอบการเก็บข้อมูลเทรน AI
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  run_id TEXT UNIQUE,                       -- ตรงกับชื่อไฟล์ result_<run_id>.csv
  started REAL, ended REAL,
  subject TEXT, operator TEXT, mode TEXT,   -- mode: fix / manual / live
  signals_file TEXT, results_file TEXT
);
CREATE TABLE IF NOT EXISTS training_questions(  -- 1 แถว/ข้อ ในรอบเก็บข้อมูล
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  run_id TEXT, question_no INTEGER, watch_qid INTEGER,
  mode TEXT,                                -- fix / manual / live
  label TEXT,                               -- เฉลย: truth / lie / unknown / aborted
  verdict TEXT, p_lie REAL, decided_by INTEGER, quality INTEGER,
  used_for_training INTEGER,                -- 1 = แถวนี้ใน result_<run_id>.csv ใช้เทรนได้
  data TEXT, created REAL                   -- data = ผลจากนาฬิกา (JSON) + feedback + question_text
);
"""

# ชื่อคอลัมน์ในฐานข้อมูล (อังกฤษเต็ม) <-> ชื่อสั้นที่หน้าเว็บ/รายงานใช้ (เหมือนชื่อใน JSON จากนาฬิกา)
SAMPLE_MAP = [("t", "t"), ("heart_rate_bpm", "hr"), ("hrv_ms", "hrv"), ("gsr_us", "gsr"), ("gsr_phasic_us", "gp"),
              ("skin_temp_c", "tmp"), ("tremor_ms2", "trm"), ("motion_ms2", "mot"), ("stress_index", "si"),
              ("engine_state", "es"), ("ppg_contact", "con"), ("gsr_contact", "gc"), ("battery_mv", "vb")]
SAMPLE_COLS = [k for _, k in SAMPLE_MAP]
OLD_SAMPLE_NAMES = {"hr": "heart_rate_bpm", "hrv": "hrv_ms", "gsr": "gsr_us", "gp": "gsr_phasic_us",
                    "tmp": "skin_temp_c", "trm": "tremor_ms2", "mot": "motion_ms2", "si": "stress_index",
                    "es": "engine_state", "con": "ppg_contact", "gc": "gsr_contact", "vb": "battery_mv"}

VIEWS = """
CREATE VIEW IF NOT EXISTS v_sessions_overview AS
SELECT s.id AS session_id, datetime(s.created,'unixepoch','localtime') AS started_at,
       datetime(s.ended,'unixepoch','localtime') AS ended_at, s.subject, s.operator, s.template AS question_set,
       s.device AS watch_id,
       (SELECT COUNT(*) FROM questions q WHERE q.session_id=s.id) AS questions_total,
       (SELECT COUNT(*) FROM results r WHERE r.session_id=s.id) AS results_total,
       (SELECT COUNT(*) FROM results r WHERE r.session_id=s.id AND r.verdict='lie') AS verdict_lie,
       (SELECT COUNT(*) FROM results r WHERE r.session_id=s.id AND r.verdict='truth') AS verdict_truth
FROM sessions s;
CREATE VIEW IF NOT EXISTS v_question_results AS
SELECT q.session_id, s.subject, q.ord AS question_order, q.qid AS watch_qid, q.kind AS question_kind,
       q.text AS question_text, datetime(q.asked_at,'unixepoch','localtime') AS asked_at,
       q.answer AS subject_answer, r.verdict, ROUND(r.p, 3) AS p_lie, r.quality AS signal_quality,
       r.reasons AS reason_bits
FROM questions q JOIN sessions s ON s.id = q.session_id
LEFT JOIN results r ON r.question_id = q.id
ORDER BY q.session_id, q.ord;
CREATE VIEW IF NOT EXISTS v_training_questions AS
SELECT t.run_id, r.subject, r.operator, t.question_no, t.watch_qid, t.mode, t.label AS answer_key,
       t.verdict AS watch_verdict, ROUND(t.p_lie, 3) AS p_lie, t.quality AS signal_quality,
       t.used_for_training, datetime(t.created,'unixepoch','localtime') AS recorded_at
FROM training_questions t LEFT JOIN training_runs r ON r.run_id = t.run_id
ORDER BY t.id;
"""


# [เทคนิค: SQLite (WAL) + batch insert] เก็บเซสชัน/ผล/ค่าสด; ค่าสดรวมเป็นชุดเขียนวินาทีละครั้ง (flush_samples) ลดภาระดิสก์
class Store:
    """ฐานข้อมูล SQLite ของ Studio (data/studio.db) — โครงสร้างตารางอยู่ใน SCHEMA ด้านบน"""
    def __init__(self, path: str):
        """เปิด/สร้างฐานข้อมูล, เปิดโหมด WAL (อ่านขณะเขียนได้), สร้างตาราง/view และอัปเกรดโครงสร้างเก่า"""
        self.path = path
        # check_same_thread=False + lock: ใช้จาก event loop และ thread ของ export ได้ปลอดภัย
        self.db = sqlite3.connect(path, check_same_thread=False)
        self.db.row_factory = sqlite3.Row
        self.lock = threading.RLock()
        with self.lock:
            self._migrate()
            self.db.executescript(SCHEMA)
            self.db.executescript(VIEWS)
            self.db.execute(f"PRAGMA user_version={SCHEMA_VERSION}")
            self.db.execute("PRAGMA foreign_keys=ON")
            self.db.commit()
        self._pending_samples: List[tuple] = []

    def _migrate(self):
        """ฐานข้อมูลจาก Studio v2.0 ใช้ชื่อคอลัมน์ย่อ (hr, gsr, tmp ...) -> เปลี่ยนชื่อเป็นอังกฤษเต็ม
        ข้อมูลเดิมอยู่ครบ (ALTER TABLE ... RENAME COLUMN ไม่ลบข้อมูล)"""
        cols = [r[1] for r in self.db.execute("PRAGMA table_info(samples)").fetchall()]
        if not cols:
            return                                   # ฐานข้อมูลใหม่ -> SCHEMA สร้างให้
        for old, new in OLD_SAMPLE_NAMES.items():
            if old in cols and new not in cols:
                self.db.execute(f"ALTER TABLE samples RENAME COLUMN {old} TO {new}")
        cols = [r[1] for r in self.db.execute("PRAGMA table_info(samples)").fetchall()]
        if "question_id" not in cols:
            self.db.execute("ALTER TABLE samples ADD COLUMN question_id INTEGER")
        if "phase" not in cols:
            self.db.execute("ALTER TABLE samples ADD COLUMN phase TEXT")
        self.db.commit()

    # ------------------------------------------------------------ helpers
    def _q(self, sql: str, args: tuple = ()) -> List[sqlite3.Row]:
        with self.lock:
            return self.db.execute(sql, args).fetchall()

    def _x(self, sql: str, args: tuple = ()) -> int:
        """รัน SQL ที่เขียนข้อมูล 1 คำสั่ง + commit (ถือ lock กันชนกันระหว่าง thread) คืน id แถวล่าสุด"""
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
        """บันทึกเวลาจบเซสชัน (เขียนค่าสดที่ค้างในหน่วยความจำลงก่อน)"""
        self.flush_samples()
        self._x("UPDATE sessions SET ended=? WHERE id=? AND ended IS NULL", (time.time(), sid))

    def delete_session(self, sid: int):
        """ลบเซสชันและทุกอย่างที่ผูกอยู่ (ค่าสด เหตุการณ์ ผล คำถาม)"""
        with self.lock:
            self.db.execute("DELETE FROM samples WHERE session_id=?", (sid,))
            self.db.execute("DELETE FROM events WHERE session_id=?", (sid,))
            self.db.execute("DELETE FROM results WHERE session_id=?", (sid,))
            self.db.execute("DELETE FROM questions WHERE session_id=?", (sid,))
            self.db.execute("DELETE FROM sessions WHERE id=?", (sid,))
            self.db.commit()

    def list_sessions(self) -> List[Dict[str, Any]]:
        """รายการเซสชันทั้งหมด พร้อมจำนวนคำถาม/ผล (หน้า "ผลลัพธ์ & รายงาน")"""
        rows = self._q("""
          SELECT s.*,
            (SELECT COUNT(*) FROM questions q WHERE q.session_id=s.id) AS n_questions,
            (SELECT COUNT(*) FROM results r WHERE r.session_id=s.id) AS n_results,
            (SELECT COUNT(*) FROM results r WHERE r.session_id=s.id AND r.verdict='lie') AS n_lie,
            (SELECT COUNT(*) FROM results r WHERE r.session_id=s.id AND r.verdict='truth') AS n_truth
          FROM sessions s ORDER BY s.id DESC""")
        return [dict(r) for r in rows]

    def get_session(self, sid: int) -> Optional[Dict[str, Any]]:
        """ข้อมูลเซสชัน 1 อัน + คำถาม + ผล (ใช้สร้างหน้าเว็บและรายงาน)"""
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
            # แบ่งช่วง qid กันชนกัน: 1-499 เซสชัน, 500-599 ควบคุมด่วนหน้าหลัก, 600-799 หน้าเก็บข้อมูล,
            # 800-899 Serial console, 900+ ปุ่มบนนาฬิกา
            row = self.db.execute("SELECT COALESCE(MAX(ord),0)+1, COALESCE(MAX(CASE WHEN qid < 500 THEN qid END),0)+1 "
                                  "FROM questions WHERE session_id=?",
                                  (sid,)).fetchone()
            cur = self.db.execute(
                "INSERT INTO questions(session_id,ord,qid,kind,text,hint,grp,label) VALUES(?,?,?,?,?,?,?,?)",
                (sid, row[0], row[1], kind, text, hint, grp, label))
            self.db.commit()
            return cur.lastrowid

    def update_question(self, question_id: int, **fields):
        """แก้ข้อความ/ชนิดของคำถาม (แก้ได้เฉพาะคอลัมน์ที่อนุญาต)"""
        allowed = {k: v for k, v in fields.items() if k in ("kind", "text", "hint", "grp", "label") and v is not None}
        if not allowed:
            return
        sets = ",".join(f"{k}=?" for k in allowed)
        self._x(f"UPDATE questions SET {sets} WHERE id=?", tuple(allowed.values()) + (question_id,))

    def delete_question(self, question_id: int):
        """ลบคำถาม (เฉพาะข้อที่ยังไม่ได้ถาม)"""
        self._x("DELETE FROM questions WHERE id=? AND asked_at IS NULL", (question_id,))

    def get_question(self, question_id: int) -> Optional[Dict[str, Any]]:
        """คำถาม 1 ข้อตาม id"""
        rows = self._q("SELECT * FROM questions WHERE id=?", (question_id,))
        return dict(rows[0]) if rows else None

    def mark_asked(self, question_id: int, t: float):
        """บันทึกว่าเริ่มถามข้อนี้แล้ว (ถามซ้ำได้: ผลเก่าถูกแยกออกจากคำถาม)"""
        with self.lock:
            # ถามข้อเดิมซ้ำ (เช่นครั้งก่อนวัดไม่ได้) -> ผลเก่ายังเก็บไว้แต่ไม่ผูกกับคำถามแล้ว
            self.db.execute("UPDATE results SET question_id=NULL WHERE question_id=?", (question_id,))
            self.db.execute("UPDATE questions SET asked_at=?, answer=NULL, answered_at=NULL WHERE id=?",
                            (t, question_id))
            self.db.commit()

    def set_question_qid(self, question_id: int, qid: int):
        """ผูกคำถามกับเลข qid ที่ส่งให้นาฬิกา (ใช้จับคู่ผลที่ส่งกลับมา)"""
        self._x("UPDATE questions SET qid=? WHERE id=?", (qid, question_id))

    def mark_answer(self, question_id: int, answer: str, t: float):
        """บันทึกคำตอบ ใช่/ไม่ใช่ และเวลาที่ตอบ"""
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
        """ผลข้อเดิมได้ข้อมูลครบกว่า (จาก /api/lie) -> อัปเดตแทนข้อมูลจาก UDP event"""
        self._x("""UPDATE results SET data=?, verdict=?, p=?, score=?, quality=?, reasons=?, ok=?
                   WHERE session_id IS ? AND device_boot=? AND seq=?""",
                (json.dumps(r, ensure_ascii=False), r.get("verdict"), r.get("p"), r.get("score"),
                 r.get("quality"), r.get("reasons"), r.get("ok"), sid, device_boot, seq))

    def has_result(self, sid: Optional[int], device_boot: str, seq: int) -> bool:
        """เคยบันทึกผลนี้แล้วหรือยัง (ดูจาก boot + seq) — กันบันทึกซ้ำ"""
        rows = self._q("SELECT 1 FROM results WHERE session_id IS ? AND device_boot=? AND seq=?", (sid, device_boot, seq))
        return bool(rows)

    # ------------------------------------------------------------ samples
    def queue_sample(self, sid: int, v: Dict[str, Any], question_id: Optional[int] = None):
        """พักค่าไว้ในหน่วยความจำ แล้ว flush_samples() เขียนทีละชุดทุก 1 วินาที (เขียนดิสก์ทีละแถวช้า)"""
        es = v.get("es")
        phase = {1: "baseline", 3: "question"}.get(es, "rest")
        self._pending_samples.append((sid, time.time(), question_id if es == 3 else None, phase,
                                      v.get("hr"), v.get("hrv"), v.get("gsr"), v.get("gp"), v.get("tmp"),
                                      v.get("trm"), v.get("mot"), v.get("si"), es, v.get("con"), v.get("gc"),
                                      v.get("vb")))

    def flush_samples(self):
        """เขียนค่าสดที่พักไว้ในหน่วยความจำลงฐานข้อมูลทีเดียว (เร็วกว่าเขียนทีละแถว 5 ครั้ง/วินาที)"""
        if not self._pending_samples:
            return
        rows, self._pending_samples = self._pending_samples, []
        cols = "session_id,t,question_id,phase," + ",".join(c for c, _ in SAMPLE_MAP[1:])
        with self.lock:
            self.db.executemany(f"INSERT INTO samples({cols}) VALUES({','.join('?' * 16)})", rows)
            self.db.commit()

    def get_samples(self, sid: int, max_points: int = 2000) -> Dict[str, List]:
        """คืนค่าเป็นชื่อสั้น (hr, gsr ...) เพราะหน้าเว็บและรายงานใช้ชื่อเดียวกับ JSON จากนาฬิกา"""
        rows = self._q("SELECT " + ",".join(c for c, _ in SAMPLE_MAP) + " FROM samples WHERE session_id=? ORDER BY t",
                       (sid,))
        step = max(1, len(rows) // max_points)       # ลดจำนวนจุดให้กราฟเบา
        rows = rows[::step]
        return {k: [r[i] for r in rows] for i, (_, k) in enumerate(SAMPLE_MAP)}

    # ------------------------------------------------------------ events
    def add_event(self, sid: Optional[int], typ: str, data: Dict[str, Any]):
        self._x("INSERT INTO events(session_id,t,type,data) VALUES(?,?,?,?)",
                (sid, time.time(), typ, json.dumps(data, ensure_ascii=False)))

    # ------------------------------------------------------------ training (หน้าเก็บข้อมูลเทรน AI)
    def add_training_run(self, run_id: str, subject: str, operator: str, mode: str, signals: str, results: str):
        self._x("INSERT OR IGNORE INTO training_runs(run_id,started,subject,operator,mode,signals_file,results_file) "
                "VALUES(?,?,?,?,?,?,?)", (run_id, time.time(), subject, operator, mode, signals, results))

    def end_training_run(self, run_id: str):
        """บันทึกเวลาจบรอบเก็บข้อมูล/ใช้งานจริง"""
        self._x("UPDATE training_runs SET ended=? WHERE run_id=?", (time.time(), run_id))

    def add_training_question(self, run_id: str, e: Dict[str, Any]):
        """บันทึกผล 1 ข้อของรอบเก็บข้อมูล/ใช้งานจริงลงตาราง training_questions (สำเนาของแถวใน result_*.csv)"""
        r = e.get("result") or {}
        self._x("""INSERT INTO training_questions(run_id,question_no,watch_qid,mode,label,verdict,p_lie,decided_by,
                   quality,used_for_training,data,created) VALUES(?,?,?,?,?,?,?,?,?,?,?,?)""",
                (run_id, e.get("question_no"), e.get("watch_qid"), e.get("mode"), e.get("label"), e.get("verdict"),
                 e.get("p"), r.get("src"), r.get("quality"), 1 if e.get("used") else 0,
                 json.dumps(dict(r, feedback=e.get("feedback", ""), question_text=e.get("text", "")),
                            ensure_ascii=False), time.time()))

    # ------------------------------------------------------------ export
    def export_results_csv(self, sid: int) -> str:
        s = self.get_session(sid)
        out = io.StringIO()
        w = csv.writer(out)
        w.writerow(["order", "watch_qid", "question", "kind", "answer", "verdict", "p_lie", "score", "quality",
                    "reasons", "d_gsr_uS", "d_hr_bpm", "d_amp_frac", "d_tremor", "d_temp_C",
                    "z_gsr", "z_hr", "z_amp", "z_trm", "z_tmp", "gsr_peak_latency_s"])
        by_q = {r["question_id"]: r for r in s["results"]}
        for q in s["questions"]:
            r = by_q.get(q["id"])
            d = r["data"] if r else {}
            feat, z = d.get("feat") or [None] * 5, d.get("z") or [None] * 5
            w.writerow([q["ord"], q["qid"], q["text"], q["kind"], q["answer"] or "", d.get("verdict", ""),
                        d.get("p", ""), d.get("score", ""), d.get("quality", ""), d.get("reasons", "")]
                       + list(feat) + list(z) + [d.get("latency", "")])
        return "﻿" + out.getvalue()   # BOM ให้ Excel อ่านภาษาไทยถูก

    def export_samples_csv(self, sid: int) -> str:
        """ไฟล์สัญญาณของเซสชัน: ทุกแถวบอกว่าอยู่ช่วงไหน (phase) และกำลังถามข้อไหน (question_order/watch_qid)"""
        cols = ["t"] + [c for c, _ in SAMPLE_MAP[1:]]
        rows = self._q(f"""SELECT datetime(sm.t,'unixepoch','localtime') AS time_local, sm.phase,
                                  q.ord AS question_order, q.qid AS watch_qid, q.kind AS question_kind,
                                  {",".join("sm." + c for c in cols)}
                           FROM samples sm LEFT JOIN questions q ON q.id = sm.question_id
                           WHERE sm.session_id=? ORDER BY sm.t""", (sid,))
        out = io.StringIO()
        w = csv.writer(out)
        w.writerow(["time_local", "phase", "question_order", "watch_qid", "question_kind"] + cols)
        for r in rows:
            w.writerow(list(r))
        return out.getvalue()
