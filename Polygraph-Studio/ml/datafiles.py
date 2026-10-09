"""
datafiles.py — รูปแบบไฟล์ข้อมูล "result_<วันเวลา>.csv" และเครื่องมือจัดการไฟล์ในโฟลเดอร์ data/

ทำไมต้องมีไฟล์นี้
  เดิมข้อมูลกระจายอยู่หลายรูปแบบ (results_*.csv, training_samples.csv, watch_train_*.csv,
  polygraph_train.csv) และ train.py หยิบทุกไฟล์ที่ "หน้าตาเหมือนข้อมูลเทรน" มาใช้ ทำให้งงว่าเทรนจากไฟล์ไหน
  ตอนนี้จึงกำหนดให้มี "รูปแบบเดียว" คือ result_<YYYYMMDD_HHMMSS>.csv
    * 1 ไฟล์ = 1 รอบการเก็บ/การใช้งาน, 1 แถว = 1 คำถาม
    * มีทั้งเฉลย (label), คำตัดสินของนาฬิกา (verdict) และ feature 12 ตัวที่ AI ใช้ ในแถวเดียวกัน
    * train.py อ่าน "เฉพาะ" ไฟล์ชื่อ result_<ตัวเลข>.csv ในโฟลเดอร์ data/ เท่านั้น

ใครใช้ไฟล์นี้
  ml/recorder.py      เขียนแถวใหม่ (เก็บข้อมูล fix/manual และใช้งานจริง live)
  ml/train.py         อ่านแถวที่ใช้เทรนได้
  ml/collect.py       ดาวน์โหลดข้อมูลจากนาฬิกาแล้วแปลงเป็น result_*.csv
  backend/data_api.py หน้า "ข้อมูล & เทรน AI" และประวัติในหน้าเก็บข้อมูล (ดู/รวม/นำเข้าไฟล์)
  tools/migrate_data.py  แปลงไฟล์รูปแบบเก่าทีละครั้ง (Studio และ train.py ก็เรียก migrate() เองตอนเริ่ม)

หลักความปลอดภัยของข้อมูล (สำคัญ)
  * ไม่แก้ไขเนื้อหาไฟล์เดิมเลย: แปลงโดย "อ่านแล้วเขียนไฟล์ใหม่" จากนั้นย้ายไฟล์เดิมไปเก็บที่ data/old_format/
    (ย้ายเฉย ๆ ไบต์เหมือนเดิมทุกตัว เปิดดู/เอากลับมาได้เสมอ)
  * ไม่ลบแถวใดทิ้ง: แถวที่น่าสงสัยยังอยู่ในไฟล์ แต่ตั้ง used_for_training = 0 และเขียนเหตุผลในคอลัมน์ note
ใช้แต่ไลบรารีมาตรฐานของ Python
"""
from __future__ import annotations

import csv
import io
import json
import os
import re
import shutil
import time
from typing import Any, Callable, Dict, Iterable, List, Optional, Sequence, Tuple

import polyml as pm

# ============================================================ รูปแบบไฟล์ result_*.csv
RESULT_RE = re.compile(r"^result_(\d{8}_\d{6})\.csv$")      # ชื่อที่ train.py ยอมรับ (ตรงตัวเท่านั้น)
D_COLS = ["d_" + s for s in pm.SIGNALS]                     # ค่าที่เปลี่ยนไปจาก baseline (หน่วยจริง)
Z_COLS = ["z_" + s for s in pm.SIGNALS]                     # เปลี่ยนไปกี่เท่าของความแกว่งปกติ
RESULT_HEADER = [
    "time_iso",          # เวลาที่บันทึกข้อนี้ (เวลาคอม)
    "run_id",            # รหัสรอบ = วันเวลาเริ่มรอบ (ตรงกับชื่อไฟล์)
    "source",            # มาจากไหน: desktop / desktop_cli / desktop_sim / mobile / esp_backup / legacy
                         # (ไฟล์ก่อน 9 ต.ค. 2026 อาจเป็น studio / cli / watch — ดู origin_of())
    "subject",           # ผู้ตอบ (ผู้ถูกทดสอบ)
    "operator",          # ผู้ถาม
    "question_no",       # ข้อที่เท่าไรในรอบ (1, 2, 3 ...)
    "watch_qid",         # เลข qid ที่ส่งให้นาฬิกา (ใช้จับคู่ผลกับคำถาม)
    "mode",              # fix / manual / live / watch
    "question_text",     # ข้อความคำถาม (ถ้าพิมพ์ไว้)
    "label",             # เฉลย: truth / lie / unknown / aborted
    "feedback",          # โหมด live: นาฬิกาตอบ correct / wrong / unknown / inconclusive (ผู้ใช้บอกหลังได้ผล)
    "verdict",           # คำตัดสินของนาฬิกา: truth / lie / inconclusive / invalid
    "p_lie",             # โอกาสโกหกที่นาฬิกาคำนวณ (0-1)
    "decided_by",        # rules / rules_calibrated / ai_model
    "quality",           # คุณภาพสัญญาณ 0-100
    "reasons",           # bit เหตุผลที่ผลเชื่อถือได้น้อย (ดู DATA_DICTIONARY.md)
    "ok_mask",           # bit สัญญาณที่ใช้ได้: 1 GSR, 2 ชีพจร, 4 แรงชีพจร, 8 การสั่น, 16 อุณหภูมิ
    "gsr_ok", "ppg_ok",  # feature 2 ตัวสุดท้ายของ AI (มีสัญญาณ GSR / ชีพจร หรือไม่)
    "used_for_training",  # 1 = train.py ใช้แถวนี้ (แก้เป็น 0 เองได้ถ้าไม่อยากให้ใช้)
] + D_COLS + Z_COLS + [
    "note",              # หมายเหตุ เช่น เหตุผลที่ไม่ใช้เทรน
]
FEATURE_COLS = Z_COLS + D_COLS + ["gsr_ok", "ppg_ok"]     # ลำดับเดียวกับ pm.FEATURES (ตรงกับเฟิร์มแวร์)
assert FEATURE_COLS == pm.FEATURES

DECIDED_BY = {0: "rules", 1: "rules_calibrated", 2: "ai_model"}
LABELS = ("truth", "lie")

# ============================================================ แหล่งที่มาของข้อมูล (คอลัมน์ source + note)
# ข้อมูลเข้ามาได้ 3 ทางหลัก ตามรูปที่ใช้อธิบายระบบ:
#   desktop    : Studio บนคอม (127.0.0.1:8000) สั่งนาฬิกาถามแล้วบันทึกเอง  -> แม่นที่สุด มีค่าสดครบ
#   mobile     : เก็บผ่านหน้าเว็บนาฬิกาบนมือถือ (192.168.4.1) แล้วกด "ดาวน์โหลด CSV" ได้ polygraph_train.csv
#                แล้วนำไฟล์นั้นมา "นำเข้าไฟล์ CSV" ที่หน้า ข้อมูล & เทรน AI
#   esp_backup : ข้อมูลชุดเดียวกับ mobile แต่ Studio ดึงตรงจากหน่วยความจำนาฬิกา (/api/ml/data.csv)
#                = สำรองที่บอร์ดเก็บไว้เอง (ใช้เมื่อไม่ได้ดาวน์โหลดจากมือถือ)
ORIGIN_TH = {
    "desktop": "desktop (Studio บนคอม)",
    "desktop_cli": "desktop (collect_data.bat)",
    "desktop_sim": "desktop (นาฬิกาจำลอง — ไม่ใช่ข้อมูลคนจริง)",
    "mobile": "mobile (หน้าเว็บนาฬิกาบนมือถือ)",
    "esp_backup": "esp_backup (ดึงจากหน่วยความจำนาฬิกา)",
    "legacy": "desktop (Studio รุ่นเก่า)",
    "import": "นำเข้าจากไฟล์อื่น",
}


def origin_of(row: Dict[str, Any], file_note: str = "") -> str:
    """แหล่งที่มาของ 1 แถว (desktop / desktop_cli / desktop_sim / mobile / esp_backup / legacy)
    ไฟล์รุ่นก่อนใช้ค่า studio / cli / watch -> แปลงให้ตอนแสดงผล "โดยไม่แก้ไฟล์เดิม"
    watch แยกไม่ได้จากคอลัมน์ source -> ดูหมายเหตุของไฟล์ (file_note = note ของแถวแรก) ว่านำเข้าจากไหน
      polygraph_train*.csv = ไฟล์ที่มือถือดาวน์โหลด -> mobile
      /api/ml/data.csv, watch_train_* = ดึงจากนาฬิกาด้วยคอม -> esp_backup"""
    s = str(row.get("source") or "")
    if s in ORIGIN_TH:
        return s
    if s == "studio":
        return "desktop"
    if s == "cli":
        return "desktop_cli"
    if s == "watch":
        n = (str(row.get("note") or "") + " " + (file_note or "")).lower()
        return "mobile" if "polygraph_train" in n else "esp_backup"
    return s or "-"


def with_origin(note: str, source: str) -> str:
    """เติม "ที่มา: ..." ไว้หน้าหมายเหตุ (ผู้ใช้ขอให้เห็นแหล่งที่มาในคอลัมน์ note ด้วย ไม่ต้องเปิดคอลัมน์ source)"""
    tag = "ที่มา: " + ORIGIN_TH.get(source, source)
    if not note:
        return tag
    return note if note.startswith("ที่มา:") else f"{tag} | {note}"

# หัวไฟล์รูปแบบเก่า (ไว้ตรวจว่าไฟล์เป็นแบบไหน)
V1_RESULT_KEYS = {"time_iso", "run_id", "question_no", "watch_qid", "label", "verdict", "used_for_training"}
WATCH_KEYS = {"time", "qid", "label", "label_name", "gsr_ok", "ppg_ok", "p_model"}


def iso(t: Optional[float] = None) -> str:
    """เวลาแบบอ่านง่ายที่ Excel ก็อ่านได้ เช่น 2026-10-08 16:48:58.012"""
    t = time.time() if t is None else t
    return time.strftime("%Y-%m-%d %H:%M:%S", time.localtime(t)) + f".{int((t % 1) * 1000):03d}"


def result_path(data_dir: str, run_id: str) -> str:
    """path ของไฟล์ผลของรอบ: data/result_<run_id>.csv"""
    return os.path.join(data_dir, f"result_{run_id}.csv")


def new_run_id(data_dir: str, t: Optional[float] = None) -> str:
    """รหัสรอบจากวันเวลา — ถ้าไฟล์ชื่อนี้มีอยู่แล้ว (เริ่ม 2 รอบในวินาทีเดียวกัน) ขยับไปวินาทีถัดไป"""
    t = int(time.time() if t is None else t)
    while True:
        rid = time.strftime("%Y%m%d_%H%M%S", time.localtime(t))
        if not os.path.exists(result_path(data_dir, rid)):
            return rid
        t += 1


def is_result_name(name: str) -> bool:
    """ชื่อไฟล์ตรงรูปแบบ result_<YYYYMMDD_HHMMSS>.csv ไหม (train.py อ่านเฉพาะไฟล์ที่ตรง)"""
    return bool(RESULT_RE.match(os.path.basename(name)))


def list_result_files(data_dir: str) -> List[str]:
    """ไฟล์ result_<ตัวเลข>.csv ทั้งหมดในโฟลเดอร์ data/ (ไม่รวมโฟลเดอร์ย่อย) เรียงตามเวลา"""
    try:
        names = os.listdir(data_dir)
    except OSError:
        return []
    return [os.path.join(data_dir, n) for n in sorted(names) if is_result_name(n)]


# ============================================================ อ่าน/เขียน CSV แบบทนทาน
def read_csv(path: str) -> Tuple[List[str], List[Dict[str, str]]]:
    """อ่าน CSV เป็น (หัวตาราง, แถว) — ลอง UTF-8 ก่อน แล้วค่อย cp874 (ไฟล์ที่ Excel ภาษาไทยเซฟทับ)"""
    for enc in ("utf-8-sig", "cp874", "latin-1"):
        try:
            with open(path, encoding=enc, newline="") as fh:
                text = fh.read()
            break
        except UnicodeDecodeError:
            continue
    rd = csv.DictReader(io.StringIO(text))
    header = [h.strip() for h in (rd.fieldnames or [])]
    rd.fieldnames = header
    rows = [{k: (v or "").strip() if isinstance(v, str) else "" for k, v in r.items() if k} for r in rd]
    return header, rows


def detect_format(header: Sequence[str]) -> str:
    """บอกว่าไฟล์เป็นแบบไหน: result (ใหม่) / results_v1 (Studio รุ่นก่อน) / watch_train (จากนาฬิกา)
    / signals (ค่าสด) / unknown"""
    h = set(header)
    if {"run_id", "label", "source", "gsr_ok"} <= h and set(D_COLS) <= h:
        return "result"
    if V1_RESULT_KEYS <= h:
        return "results_v1"
    if WATCH_KEYS <= h:
        return "watch_train"
    if "t_sec" in h or "pc_time" in h:
        return "signals"
    return "unknown"


def _num(v: Any) -> Optional[float]:
    """แปลงเป็นตัวเลข — ว่าง/อ่านไม่ได้/NaN/อนันต์ = None"""
    try:
        x = float(v)
    except (TypeError, ValueError):
        return None
    return x if x == x and abs(x) != float("inf") else None


def _fmt(v: Any, nd: int = 4) -> str:
    """ตัวเลข -> ข้อความสั้น ๆ (ตัดศูนย์ท้าย) / ค่าว่างคงว่าง"""
    x = _num(v)
    if x is None:
        return ""
    s = f"{x:.{nd}f}".rstrip("0").rstrip(".")
    return "0" if s in ("-0", "") else s


def write_rows(path: str, rows: Iterable[Dict[str, Any]], append: bool = False):
    """เขียนแถว result ลงไฟล์ — ไฟล์ใหม่ใส่ BOM ให้ Excel อ่านภาษาไทยถูก"""
    new = not append or not os.path.exists(path) or os.path.getsize(path) == 0
    os.makedirs(os.path.dirname(os.path.abspath(path)), exist_ok=True)
    with open(path, "w" if new else "a", encoding="utf-8-sig" if new else "utf-8", newline="") as fh:
        w = csv.writer(fh)
        if new:
            w.writerow(RESULT_HEADER)
        for r in rows:
            w.writerow([r.get(k, "") for k in RESULT_HEADER])


def rows_to_csv_text(rows: Iterable[Dict[str, Any]]) -> str:
    """แถว result -> ข้อความ CSV (มี BOM) สำหรับดาวน์โหลด"""
    out = io.StringIO()
    w = csv.writer(out)
    w.writerow(RESULT_HEADER)
    for r in rows:
        w.writerow([r.get(k, "") for k in RESULT_HEADER])
    return "﻿" + out.getvalue()


# ============================================================ สร้าง 1 แถวจากผลของนาฬิกา
def decide_used(row: Dict[str, Any]) -> int:
    """แถวนี้ใช้เทรนได้ไหม: ต้องรู้เฉลย (truth/lie), สัญญาณไม่ invalid และมี GSR หรือชีพจรอย่างน้อย 1 อย่าง"""
    if row.get("label") not in LABELS:
        return 0
    if row.get("verdict") == "invalid":
        return 0
    return 1 if (str(row.get("gsr_ok")) == "1" or str(row.get("ppg_ok")) == "1") else 0


def row_from_result(r: Dict[str, Any], *, run_id: str, source: str, subject: str, operator: str,
                    question_no: int, watch_qid: int, mode: str, label: str, feedback: str = "",
                    question_text: str = "", note: str = "", t: Optional[float] = None) -> Dict[str, Any]:
    """r = ผล 1 ข้อจากนาฬิกา (รูปแบบเดียวกับ /api/lie: verdict, p, src, quality, reasons, ok, feat, z)
    คืน 1 แถวของ result_*.csv โดย d_*/z_* เป็น "ค่าที่ AI เห็นจริง" (สัญญาณที่ใช้ไม่ได้ = 0 แบบเดียวกับ
    ml::extract() ในเฟิร์มแวร์) — เทรนจากค่านี้ได้ตรงกับที่นาฬิกาคำนวณตอนใช้งาน"""
    r = r or {}
    ok_mask = int(_num(r.get("ok")) or 0)
    feat = r.get("feat") or r.get("f") or [None] * 5
    z = r.get("z") or [None] * 5
    has = r.get("verdict") not in (None, "") and ok_mask != 0
    x = pm.features_from(ok_mask, z, feat) if has else None
    src = r.get("src")
    row = {
        "time_iso": iso(t), "run_id": run_id, "source": source, "subject": pm.clean_subject(subject),
        "operator": pm.clean_subject(operator) if operator else "-", "question_no": question_no,
        "watch_qid": watch_qid, "mode": mode, "question_text": _clean_text(question_text), "label": label,
        "feedback": feedback, "verdict": r.get("verdict", ""), "p_lie": _fmt(r.get("p"), 3),
        "decided_by": DECIDED_BY.get(int(src), str(src)) if _num(src) is not None else "",
        "quality": r.get("quality", r.get("q", "")), "reasons": r.get("reasons", r.get("rs", "")),
        "ok_mask": ok_mask if has else "", "gsr_ok": int(x["gsr_ok"]) if x else "",
        "ppg_ok": int(x["ppg_ok"]) if x else "", "note": with_origin(note, source),
    }
    for c in D_COLS + Z_COLS:
        row[c] = _fmt(x[c]) if x else ""
    row["used_for_training"] = decide_used(row)
    if source == "desktop_sim" and row["used_for_training"]:
        # ข้อมูลจากนาฬิกาจำลองไม่ใช่สัญญาณคนจริง -> ไม่ให้ปนตอนเทรน (แก้เป็น 1 เองได้ถ้าต้องการทดลอง)
        row["used_for_training"] = 0
        row["note"] += " | ข้อมูลจำลอง ไม่ใช้เทรน"
    return row


def _clean_text(s: str) -> str:
    """ข้อความคำถาม: ตัดขึ้นบรรทัดใหม่ (CSV ยังถูกต้องเพราะ csv.writer ใส่ "" ให้เอง แต่อ่านใน Excel ง่ายกว่า)"""
    return " ".join((s or "").split())[:200]


# ============================================================ แปลงแถวรูปแบบเก่า -> result
def label_from_feedback(verdict: str, feedback: str) -> str:
    """โหมด live: ได้ผลแล้วผู้ใช้บอกว่านาฬิกาตอบถูก/ผิด -> แปลงเป็นเฉลย
    นาฬิกาบอก "จริง" + ถูก = truth, "จริง" + ผิด = lie, "โกหก" + ถูก = lie, "โกหก" + ผิด = truth"""
    if feedback == "correct" and verdict in LABELS:
        return verdict
    if feedback == "wrong" and verdict in LABELS:
        return "lie" if verdict == "truth" else "truth"
    return "unknown"


def _features_close(a: Dict[str, Any], b: Dict[str, Any], tol: float = 0.011, zero_wild: bool = False) -> bool:
    """สองแถวเป็นการวัดครั้งเดียวกันไหม (ทศนิยมจากคนละแหล่งต่างกันเล็กน้อย จึงเทียบแบบมีค่าเผื่อ)
    ข้อที่ต่างกันจริงจะไม่ใกล้กันขนาดนี้ครบทั้ง 10 ค่าพร้อมกัน
      zero_wild=True : ค่า 0 ฝั่งใดฝั่งหนึ่งถือว่าตรง (ใช้เทียบค่าดิบกับค่าที่ AI ใช้ ซึ่งสัญญาณเสียถูกตั้งเป็น 0)"""
    for c in D_COLS + Z_COLS:
        x, y = _num(a.get(c)), _num(b.get(c))
        if x is None or y is None:
            return False
        if abs(x - y) > tol and not (zero_wild and (x == 0 or y == 0)):
            return False
    return True


def _fix_excel_time(rows: List[Dict[str, str]], run_id: str) -> int:
    """Excel เปิดแล้วเซฟทับ จะเปลี่ยน "2026-10-08 16:48:58.012" เป็น "48:58.0" (หายทั้งวันที่และชั่วโมง)
    กู้คืนจาก run_id (วันเวลาเริ่มรอบ): ชั่วโมงเริ่มจากเวลาเริ่มรอบ แล้วบวก 1 ทุกครั้งที่นาทีวนกลับ
    คืนจำนวนแถวที่แก้"""
    m = re.match(r"^(\d{8})_(\d{2})(\d{2})(\d{2})$", run_id or "")
    if not m:
        return 0
    day, hour, prev_min, fixed = m.group(1), int(m.group(2)), int(m.group(3)), 0
    for r in rows:
        t = r.get("time_iso", "")
        mm = re.match(r"^(\d{1,2}):(\d{2})(\.\d+)?$", t)
        if not mm:
            continue
        minute, sec, frac = int(mm.group(1)), int(mm.group(2)), mm.group(3) or ".0"
        if minute < prev_min:
            hour += 1
        prev_min = minute
        r["time_iso"] = (f"{day[:4]}-{day[4:6]}-{day[6:]} {hour % 24:02d}:{minute:02d}:{sec:02d}"
                         f".{(frac[1:] + '000')[:3]}")
        fixed += 1
    return fixed


def _parse_iso(s: str) -> Optional[float]:
    """เวลาแบบ "2026-10-08 16:48:58.012" -> epoch (อ่านไม่ได้ = None)"""
    for f in ("%Y-%m-%d %H:%M:%S", "%Y-%m-%d %H:%M"):
        try:
            return time.mktime(time.strptime(s.split(".")[0], f))
        except (ValueError, TypeError):
            continue
    return None


def convert_v1_rows(rows: List[Dict[str, str]], train_pool: Optional[List[Dict[str, str]]] = None
                    ) -> Tuple[List[Dict[str, Any]], Dict[str, int]]:
    """แปลงแถวของ results_<run>.csv (Studio รุ่น 2.2) เป็นรูปแบบ result

    รุ่นเก่าเก็บ d_/z_ แบบ "ค่าดิบ" และไม่มี gsr_ok/ppg_ok -> หาแถวคู่กันใน training_samples.csv
    (train_pool) ซึ่งเก็บค่าที่ AI ใช้จริงไว้ ถ้าไม่เจอค่อยเดาจากค่าที่เป็น 0
    แถวที่เจอคู่จะถูก "ใช้ไป" (ลบออกจาก pool) เพื่อหาแถวกำพร้าที่เหลือได้"""
    stat = {"rows": 0, "matched": 0, "time_fixed": 0}
    if rows:
        stat["time_fixed"] = _fix_excel_time(rows, rows[0].get("run_id", ""))
    out = []
    for r in rows:
        stat["rows"] += 1
        row = {k: r.get(k, "") for k in RESULT_HEADER}
        row["source"] = "legacy"
        row["feedback"] = ""
        row["question_text"] = ""
        row["note"] = ""
        label = r.get("label", "")
        match = None
        if train_pool is not None and label in LABELS:
            lab = "1" if label == "lie" else "0"
            for i, tr in enumerate(train_pool):
                if (tr.get("qid") == r.get("watch_qid") and tr.get("label") == lab
                        and pm.clean_subject(tr.get("subject", "")) == pm.clean_subject(r.get("subject", ""))
                        and _features_close(tr, r, zero_wild=True)):
                    match = train_pool.pop(i)
                    break
        if match:
            stat["matched"] += 1
            for c in D_COLS + Z_COLS + ["gsr_ok", "ppg_ok"]:
                row[c] = match.get(c, "")
        else:
            # ไม่มีคู่: สัญญาณที่ค่า d และ z เป็น 0 ทั้งคู่ = น่าจะใช้ไม่ได้ในข้อนั้น
            def dead(s):
                return all((_num(r.get(p + s)) or 0.0) == 0.0 for p in ("d_", "z_"))
            if r.get("d_gsr", "") == "":
                row["gsr_ok"] = row["ppg_ok"] = ""
            else:
                row["gsr_ok"] = 0 if dead("gsr") else 1
                row["ppg_ok"] = 0 if dead("hr") and dead("amp") else 1
        row["ok_mask"] = ""
        used = decide_used(row)
        # เคารพการตัดสินเดิม: ถ้าเดิมไม่ใช้เทรน (เช่น invalid) ก็ไม่ใช้ต่อ
        if str(r.get("used_for_training", "1")) == "0":
            used = 0
        row["used_for_training"] = used
        out.append(row)
    return out, stat


def mark_stale(rows: List[Dict[str, Any]], earlier: List[Dict[str, Any]], window_sec: float = 12.0) -> int:
    """หาข้อที่เป็น "ผลเก่าค้าง" (บั๊กของ Studio 2.2: เริ่มรอบใหม่แล้วผลของข้อ qid เดียวกันจากรอบก่อน
    ถูกจับคู่ซ้ำ) แล้วตั้ง used_for_training = 0 + เขียนเหตุผลใน note — ไม่ลบแถว
      1) ได้ผลเร็วกว่าเวลาวัดของข้อเดียว (window_sec) นับจากเวลาเริ่มรอบ -> เป็นไปไม่ได้สำหรับข้อแรก
      2) ค่าทั้ง 10 ตัวเท่ากับข้อในรอบก่อนหน้าเป๊ะ -> เป็นผลเดียวกันที่ถูกนับซ้ำ"""
    n = 0
    for r in rows:
        if not r.get("d_gsr"):
            continue
        why = ""
        t = _parse_iso(r.get("time_iso", ""))
        try:
            t0 = time.mktime(time.strptime(r.get("run_id", ""), "%Y%m%d_%H%M%S"))
        except ValueError:
            t0 = None
        if t is not None and t0 is not None and str(r.get("question_no")) == "1" and t - t0 < window_sec:
            why = f"stale: ได้ผล {t - t0:.0f} วินาทีหลังเริ่มรอบ (เร็วกว่าเวลาวัด {window_sec:.0f} วินาที) = ผลค้างจากรอบก่อน"
        else:
            for e in earlier:
                if e.get("run_id") != r.get("run_id") and all(
                        _num(e.get(c)) is not None and abs(_num(e.get(c)) - _num(r.get(c))) < 1e-9
                        for c in D_COLS + Z_COLS):
                    why = f"stale: ค่าเท่ากับรอบ {e.get('run_id')} ข้อ {e.get('question_no')} ทุกตัว = ผลเดียวกันถูกนับซ้ำ"
                    break
        if why:
            r["used_for_training"] = 0
            r["note"] = why
            n += 1
    return n


def convert_watch_rows(rows: List[Dict[str, str]], run_id: str, source: str = "watch") -> List[Dict[str, Any]]:
    """แปลงแถวข้อมูลเทรนจากนาฬิกา (/train.csv, polygraph_train.csv, watch_train_*.csv) เป็นรูปแบบ result
    source = mobile (ไฟล์ที่มือถือดาวน์โหลดแล้วนำเข้า) / esp_backup (คอมดึงจากนาฬิกาเอง)
             / watch (ไฟล์เก่าที่ไม่รู้ว่ามาทางไหน — ใช้ตอนแปลงไฟล์รุ่นเก่าเท่านั้น)"""
    out = []
    for i, r in enumerate(rows, 1):
        lab = {"0": "truth", "1": "lie"}.get(r.get("label", "").strip(), r.get("label_name", "unknown"))
        ep = _num(r.get("time"))
        src = _num(r.get("source"))
        row = {
            "time_iso": iso(ep) if ep and ep > 1.6e9 else "", "run_id": run_id, "source": source,
            "subject": pm.clean_subject(r.get("subject", "")), "operator": "-", "question_no": i,
            "watch_qid": r.get("qid", ""), "mode": "watch", "question_text": "", "label": lab, "feedback": "",
            "verdict": "", "p_lie": r.get("p_model", ""),
            "decided_by": DECIDED_BY.get(int(src), "") if src is not None else "",
            "quality": r.get("quality", ""), "reasons": "", "ok_mask": "",
            "gsr_ok": r.get("gsr_ok", ""), "ppg_ok": r.get("ppg_ok", ""),
            "note": with_origin("", source) if source in ORIGIN_TH else "",
        }
        for c in D_COLS + Z_COLS:
            row[c] = r.get(c, "")
        row["used_for_training"] = decide_used(row)
        out.append(row)
    return out


def read_result_file(path: str, watch_source: str = "watch") -> List[Dict[str, Any]]:
    """อ่านไฟล์ใดก็ได้ที่รู้จัก แล้วคืนเป็นแถวรูปแบบ result (แปลงให้อัตโนมัติถ้าเป็นแบบเก่า)
    watch_source = ค่า source ที่จะใส่ถ้าไฟล์เป็นข้อมูลจากนาฬิกา (mobile / esp_backup)"""
    header, rows = read_csv(path)
    kind = detect_format(header)
    if kind == "result":
        return [{k: r.get(k, "") for k in RESULT_HEADER} for r in rows]
    if kind == "results_v1":
        return convert_v1_rows(rows)[0]
    if kind == "watch_train":
        m = re.search(r"(\d{8}_\d{6})", os.path.basename(path))
        return convert_watch_rows(rows, m.group(1) if m else "watch", watch_source)
    raise ValueError(f"{os.path.basename(path)} ไม่ใช่ไฟล์ผลรายข้อ (รูปแบบ: {kind})")


# ============================================================ ข้อมูลสำหรับเทรน
def training_rows(rows: Iterable[Dict[str, Any]]) -> Iterable[Dict[str, Any]]:
    """เฉพาะแถวที่ใช้เทรนได้: used_for_training = 1, เฉลย truth/lie และ feature ครบ 12 ตัว"""
    for r in rows:
        if str(r.get("used_for_training")) != "1" or r.get("label") not in LABELS:
            continue
        if any(_num(r.get(c)) is None for c in FEATURE_COLS):
            continue
        yield r


def row_key(r: Dict[str, Any]) -> Tuple[str, str]:
    """กุญแจของ 1 ข้อ = (รหัสรอบ, เลขข้อ) — ใช้ตัดข้อซ้ำเมื่อไฟล์รวมกับไฟล์ต้นฉบับถูกส่งมาเทรนพร้อมกัน"""
    return str(r.get("run_id", "")), str(r.get("question_no", ""))


def merge_rows(paths: Sequence[str]) -> Tuple[List[Dict[str, Any]], int]:
    """รวมหลายไฟล์เป็นรายการเดียว (ตัดข้อซ้ำที่ run_id + question_no ตรงกัน) คืน (แถว, จำนวนที่ซ้ำ)"""
    seen, out, dup = set(), [], 0
    for p in paths:
        for r in read_result_file(p):
            k = row_key(r)
            if k in seen and k != ("", ""):
                dup += 1
                continue
            seen.add(k)
            out.append(r)
    return out, dup


# ============================================================ สรุปไฟล์ (หน้า "ข้อมูล & เทรน AI")
def summarize(path: str) -> Dict[str, Any]:
    st = os.stat(path)
    info: Dict[str, Any] = {"name": os.path.basename(path), "bytes": st.st_size, "mtime": st.st_mtime}
    try:
        rows = read_result_file(path)
    except (OSError, ValueError) as e:
        info.update(error=str(e), rows=0)
        return info
    cnt: Dict[str, int] = {}
    for r in rows:
        cnt[r.get("label") or "-"] = cnt.get(r.get("label") or "-", 0) + 1
    fb = [r.get("feedback") for r in rows if r.get("feedback")]
    info.update(
        rows=len(rows), labels=cnt, used=sum(1 for _ in training_rows(rows)),
        subjects=sorted({r.get("subject", "-") for r in rows}), modes=sorted({r.get("mode", "") for r in rows} - {""}),
        sources=sorted({r.get("source", "") for r in rows} - {""}),
        origins=sorted({origin_of(r, rows[0].get("note", "")) for r in rows}),
        first=rows[0].get("time_iso", "") if rows else "", last=rows[-1].get("time_iso", "") if rows else "",
        correct=fb.count("correct"), wrong=fb.count("wrong"), fb_unknown=fb.count("unknown"),
        stale=sum(1 for r in rows if str(r.get("note", "")).startswith("stale")),
    )
    return info


# ============================================================ ไฟล์ที่ผู้ใช้ "ไม่เอาไปเทรน"
EXCLUDE_FILE = "train_exclude.txt"


def load_exclude(data_dir: str) -> List[str]:
    """รายชื่อไฟล์ที่ไม่ใช้เทรน (1 ชื่อต่อบรรทัด) — แก้ได้จากหน้าเว็บหรือเปิดไฟล์แก้เอง"""
    try:
        with open(os.path.join(data_dir, EXCLUDE_FILE), encoding="utf-8") as fh:
            return [ln.strip() for ln in fh if ln.strip() and not ln.startswith("#")]
    except OSError:
        return []


def save_exclude(data_dir: str, names: Sequence[str]):
    """บันทึกรายชื่อไฟล์ที่ไม่ใช้เทรนลง data/train_exclude.txt"""
    with open(os.path.join(data_dir, EXCLUDE_FILE), "w", encoding="utf-8") as fh:
        fh.write("# ไฟล์ result_*.csv ที่ไม่ต้องการให้ใช้เทรน AI (1 ชื่อต่อบรรทัด) — หน้า 'ข้อมูล & เทรน AI' แก้ให้อัตโนมัติ\n")
        for n in sorted(set(names)):
            fh.write(n + "\n")


def training_files(data_dir: str) -> List[str]:
    """ไฟล์ที่ train.py ใช้ตามค่าเริ่มต้น = result_*.csv ทั้งหมด ยกเว้นที่อยู่ใน train_exclude.txt"""
    ex = set(load_exclude(data_dir))
    return [p for p in list_result_files(data_dir) if os.path.basename(p) not in ex]


# ============================================================ นำเข้าไฟล์ / ข้อมูลจากนาฬิกา
def import_rows(data_dir: str, rows: List[Dict[str, Any]], origin: str, t: Optional[float] = None
                ) -> Dict[str, Any]:
    """เพิ่มแถว (รูปแบบ result แล้ว) เป็นไฟล์ result ใหม่ — ตัดข้อที่มีอยู่แล้วในไฟล์อื่นออก
    (เช่น ดาวน์โหลดจากนาฬิกาซ้ำหลายครั้ง ข้อเดิมจะไม่ถูกนับซ้ำ) ไม่มีข้อใหม่ = ไม่สร้างไฟล์"""
    existing = []
    for p in list_result_files(data_dir):
        try:
            existing.extend(read_result_file(p))
        except (OSError, ValueError):
            continue
    keys = {row_key(r) for r in existing}
    new, dup = [], 0
    for r in rows:
        if row_key(r) in keys or any(
                str(e.get("watch_qid")) == str(r.get("watch_qid")) and e.get("label") == r.get("label")
                and _features_close(e, r) for e in existing):
            dup += 1
            continue
        new.append(r)
        existing.append(r)
    if not new:
        return {"ok": True, "added": 0, "duplicates": dup, "file": None,
                "msg": f"ไม่มีข้อใหม่ (ซ้ำกับที่มีอยู่แล้ว {dup} ข้อ) — ไม่ได้สร้างไฟล์"}
    rid = new_run_id(data_dir, t)
    for r in new:
        if r.get("source") in ("watch", "mobile", "esp_backup") or not r.get("run_id"):
            r["run_id"] = rid
    # หมายเหตุแถวแรก = ไฟล์นี้มาจากไหน (origin_of() ใช้ข้อความนี้แยก mobile / esp_backup ของไฟล์รุ่นเก่า)
    tag = f"นำเข้าจาก {origin}"
    if tag not in str(new[0].get("note") or ""):
        new[0]["note"] = (str(new[0].get("note")) + " | " if new[0].get("note") else "") + tag
    path = result_path(data_dir, rid)
    write_rows(path, new)
    return {"ok": True, "added": len(new), "duplicates": dup, "file": os.path.basename(path),
            "msg": f"เพิ่ม {len(new)} ข้อ ลงไฟล์ {os.path.basename(path)}" + (f" (ข้ามข้อซ้ำ {dup})" if dup else "")}


def import_file(data_dir: str, path: str, origin: Optional[str] = None,
                watch_source: str = "mobile") -> Dict[str, Any]:
    """นำเข้าไฟล์ CSV ที่ได้มาจากที่อื่น (ดาวน์โหลดจากหน้าเว็บนาฬิกา / results_ รุ่นเก่า / ไฟล์ของเพื่อน)
    watch_source: ไฟล์ข้อมูลจากนาฬิกาที่ผู้ใช้อัปโหลดเอง = mobile (มือถือดาวน์โหลดมา),
                  ที่ Studio ดึงจากนาฬิกาเอง = esp_backup
    ไฟล์ result_*.csv จากเครื่องอื่นคงค่า source เดิมไว้ (แหล่งที่มาจริงของข้อนั้น)"""
    rows = read_result_file(path, watch_source)
    return import_rows(data_dir, rows, origin or os.path.basename(path))


# ============================================================ ย้ายไฟล์รูปแบบเก่า (ทำครั้งเดียว ปลอดภัย)
OLD_DIR = "old_format"
SIGNALS_DIR = "signals"
MODELS_DIR = "models"

OLD_README = """ไฟล์ในโฟลเดอร์นี้คือไฟล์รูปแบบเก่า (ก่อน Studio 2.3) ที่ถูกย้ายมาเก็บไว้ "ทั้งไฟล์" ไม่ได้แก้เนื้อหา
ข้อมูลทุกข้อถูกแปลงไปอยู่ในไฟล์ data/result_<วันเวลา>.csv แล้ว (รูปแบบเดียวที่ใช้เทรน AI)
  results_<รอบ>.csv      -> result_<รอบ>.csv (เลขรอบเดียวกัน)
  watch_train_*.csv,
  polygraph_train.csv    -> result_<วันเวลาที่ดาวน์โหลด>.csv (เฉพาะข้อที่ยังไม่มี ตัดข้อซ้ำออก)
  training_samples.csv   -> ไม่ต้องใช้แล้ว (ทุกแถวมีอยู่ใน result_*.csv; แถวที่ไม่มีคู่จะถูกแปลงเป็นไฟล์แยก)
จะลบโฟลเดอร์นี้ทิ้งก็ได้เมื่อแน่ใจแล้ว — หรือเก็บไว้เป็นสำรองก็ได้ train.py ไม่อ่านโฟลเดอร์นี้
"""


def _move(src: str, dst_dir: str) -> str:
    """ย้ายไฟล์ (ไม่แก้เนื้อหา) ถ้าปลายทางมีชื่อซ้ำ เติม _1, _2 ..."""
    os.makedirs(dst_dir, exist_ok=True)
    base, ext = os.path.splitext(os.path.basename(src))
    dst = os.path.join(dst_dir, base + ext)
    i = 1
    while os.path.exists(dst):
        dst = os.path.join(dst_dir, f"{base}_{i}{ext}")
        i += 1
    shutil.move(src, dst)
    return dst


def _note_watch_rows(rows: List[Dict[str, Any]], v1_names: Sequence[str]):
    """ข้อจากนาฬิกาที่ qid 600+ (ถามจากหน้าเก็บข้อมูล) — บอกว่าเป็นข้อไหนของรอบไหน
    ช่วยอธิบายกรณีบั๊กผลค้าง: ไฟล์ของรอบนั้นได้ผลเก่าไป ส่วนผลจริงอยู่ในไฟล์ที่ดาวน์โหลดจากนาฬิกา"""
    starts = []
    for n in v1_names:
        rid = n[len("results_"):-4]
        try:
            starts.append((time.mktime(time.strptime(rid, "%Y%m%d_%H%M%S")), rid))
        except ValueError:
            continue
    starts.sort()
    for r in rows:
        q = _num(r.get("watch_qid"))
        t = _parse_iso(r.get("time_iso", ""))
        if q is None or t is None or not 600 <= q < 800:
            continue
        run = [rid for t0, rid in starts if t0 <= t]
        if run:
            extra = f"ข้อ {int(q) - 599} ของรอบ {run[-1]} (บันทึกโดยนาฬิกาเอง)"
            r["note"] = (r.get("note") + " | " if r.get("note") else "") + extra


def migrate(data_dir: str, log: Callable[[str], None] = print, dry_run: bool = False) -> Dict[str, Any]:
    """แปลงไฟล์รูปแบบเก่าใน data/ ให้เป็น result_*.csv แล้วย้ายไฟล์เดิมไป data/old_format/
    เรียกซ้ำกี่ครั้งก็ได้ (ไม่มีไฟล์เก่าเหลือ = ไม่ทำอะไร)
      dry_run=True : แค่บอกว่าจะทำอะไร ไม่เขียน/ย้ายไฟล์"""
    rep: Dict[str, Any] = {"created": [], "moved": [], "notes": []}
    if not os.path.isdir(data_dir):
        return rep
    names = sorted(os.listdir(data_dir))
    old_dir = os.path.join(data_dir, OLD_DIR)

    def note(s: str):
        """จดข้อความรายงานการแปลง (เก็บไว้ + พิมพ์ทันที)"""
        rep["notes"].append(s)
        log(s)

    def move(name: str, sub: str):
        """ย้ายไฟล์เดิมไปโฟลเดอร์ย่อย (dry_run = แค่จดไว้ ไม่ย้ายจริง)"""
        if dry_run:
            rep["moved"].append(f"{name} -> {sub}/")
            return
        dst = _move(os.path.join(data_dir, name), os.path.join(data_dir, sub))
        rep["moved"].append(f"{name} -> {sub}/{os.path.basename(dst)}")

    # ---- 1) results_<run>.csv (Studio 2.2) -> result_<run>.csv
    v1 = [n for n in names if re.match(r"^results_\d{8}_\d{6}\.csv$", n)]
    train_pool: List[Dict[str, str]] = []
    has_ts = "training_samples.csv" in names
    if has_ts:
        _, train_pool = read_csv(os.path.join(data_dir, "training_samples.csv"))
    earlier: List[Dict[str, Any]] = []
    for p in list_result_files(data_dir):
        try:
            earlier.extend(read_result_file(p))
        except (OSError, ValueError):
            pass
    for n in v1:
        run_id = n[len("results_"):-4]
        dst = result_path(data_dir, run_id)
        header, rows = read_csv(os.path.join(data_dir, n))
        if detect_format(header) != "results_v1":
            note(f"ข้าม {n}: หัวตารางไม่ใช่รูปแบบ results รุ่นเก่า")
            continue
        conv, st = convert_v1_rows(rows, train_pool)
        stale = mark_stale(conv, earlier)
        earlier.extend(conv)
        if os.path.exists(dst):
            note(f"{n}: มี {os.path.basename(dst)} อยู่แล้ว — ไม่เขียนทับ แค่ย้ายไฟล์เดิมไปเก็บ")
        elif not dry_run:
            write_rows(dst, conv)
            rep["created"].append(os.path.basename(dst))
        else:
            rep["created"].append(os.path.basename(dst))
        note(f"{n} -> {os.path.basename(dst)}: {st['rows']} ข้อ"
             + (f", กู้เวลาที่ Excel ตัดทิ้ง {st['time_fixed']} แถว" if st["time_fixed"] else "")
             + (f", ผลค้างจากรอบก่อน {stale} ข้อ (ไม่ใช้เทรน ดูคอลัมน์ note)" if stale else ""))
        move(n, OLD_DIR)

    # ---- 2) ไฟล์ข้อมูลเทรนจากนาฬิกา -> result_<เวลาดาวน์โหลด>.csv (เฉพาะข้อใหม่)
    watch = [n for n in names if n.lower().endswith(".csv") and n != "training_samples.csv"
             and (n.startswith("watch_train_") or n.startswith("polygraph_train"))]
    for n in watch:
        path = os.path.join(data_dir, n)
        header, rows = read_csv(path)
        if detect_format(header) != "watch_train":
            note(f"ข้าม {n}: ไม่ใช่ไฟล์ข้อมูลเทรนจากนาฬิกา")
            continue
        m = re.search(r"(\d{8}_\d{6})", n)
        first = _num(rows[0].get("time")) if rows else None
        # ตั้งชื่อไฟล์ตามเวลาดาวน์โหลด (อยู่ในชื่อไฟล์) ถ้าไม่มีใช้เวลาของข้อแรก แล้วค่อยเวลาแก้ไขไฟล์
        t = (time.mktime(time.strptime(m.group(1), "%Y%m%d_%H%M%S")) if m
             else first if first and first > 1.6e9 else os.path.getmtime(path))
        conv = convert_watch_rows(rows, "", "mobile" if n.startswith("polygraph_train") else "esp_backup")
        _note_watch_rows(conv, v1)
        if dry_run:
            note(f"{n}: {len(conv)} ข้อจากนาฬิกา (จะตัดข้อซ้ำตอนแปลงจริง)")
        else:
            r = import_rows(data_dir, conv, n, t)
            if r["file"]:
                rep["created"].append(r["file"])
            note(f"{n}: {r['msg']}")
        move(n, OLD_DIR)

    # ---- 3) training_samples.csv: แถวที่ไม่มีคู่ใน results_ (กำพร้า) -> ไฟล์ result แยก
    if has_ts:
        if train_pool:
            conv = convert_watch_rows(train_pool, "")
            for r in conv:
                # แถวที่อยู่ใน training_samples.csv แต่ไม่มีในไฟล์ results_ ของรอบนั้น — อาจถูกลบออกเอง
                # (เช่น แก้ไฟล์ใน Excel) จึง "ไม่ใช้เทรน" ไว้ก่อน ถ้าต้องการใช้ ให้แก้ used_for_training เป็น 1
                r["source"], r["mode"], r["used_for_training"] = "legacy", "legacy", 0
                r["note"] = ("อยู่ใน training_samples.csv แต่ไม่มีในไฟล์ results_ ของรอบนั้น (อาจถูกลบออกตอนแก้ไฟล์)"
                             " — ไม่ใช้เทรนจนกว่าจะแก้ used_for_training เป็น 1")
            _note_watch_rows(conv, v1)
            if dry_run:
                note(f"training_samples.csv: แถวที่ไม่มีคู่ {len(conv)} ข้อ จะถูกแปลงเป็นไฟล์แยก")
            else:
                t0 = _num(train_pool[0].get("time")) or time.time()
                r = import_rows(data_dir, conv, "training_samples.csv (แถวที่ไม่มีคู่ใน results_)", t0)
                if r["file"]:
                    rep["created"].append(r["file"])
                note(f"training_samples.csv: แถวที่ไม่มีคู่ {len(conv)} ข้อ — {r['msg']}")
        else:
            note("training_samples.csv: ทุกแถวมีอยู่ใน result_*.csv แล้ว")
        move("training_samples.csv", OLD_DIR)

    # ---- 4) จัดโฟลเดอร์: ค่าสดไป signals/, ประวัติโมเดลไป models/
    for n in names:
        if re.match(r"^signals_\d{8}_\d{6}\.csv$", n) and os.path.exists(os.path.join(data_dir, n)):
            move(n, SIGNALS_DIR)
        elif re.match(r"^model_.+\.json$", n) and os.path.exists(os.path.join(data_dir, n)):
            move(n, MODELS_DIR)

    if rep["moved"] and not dry_run and os.path.isdir(old_dir):
        readme = os.path.join(old_dir, "README.txt")
        if not os.path.exists(readme):
            with open(readme, "w", encoding="utf-8") as fh:
                fh.write(OLD_README)
    if rep["created"] or rep["moved"]:
        if not dry_run:
            with open(os.path.join(data_dir, "migration_log.txt"), "a", encoding="utf-8") as fh:
                fh.write(f"=== {iso()} ===\n" + "\n".join(rep["notes"] + rep["moved"]) + "\n")
    return rep


# ============================================================ ลบข้อมูลแบบกู้คืนได้ (ถังขยะ data/trash/)
# หลัก: ปุ่ม "ลบ" บนหน้าเว็บ "ไม่ลบถาวร" — ย้ายไฟล์ไปไว้ data/trash/ (train.py ไม่อ่านโฟลเดอร์ย่อย)
# และก่อนแก้เนื้อหาไฟล์ (ลบรายข้อ / เปลี่ยนใช้เทรน) จะสำรองไฟล์ทั้งไฟล์ไว้ในถังขยะก่อนทุกครั้ง
# อยากลบถาวรจริง ๆ ให้ลบโฟลเดอร์ data/trash/ เองใน File Explorer
TRASH_DIR = "trash"


def _stamp() -> str:
    """เวลาปัจจุบันแบบใส่ในชื่อไฟล์ได้ เช่น 20261009_013000"""
    return time.strftime("%Y%m%d_%H%M%S")


def _backup_path(tdir: str, name: str) -> str:
    """ชื่อสำเนาในถังขยะที่ "ไม่ชนของเดิม" เช่น result_x.before_20261009_013000.csv
    (แก้ 2 ครั้งในวินาทีเดียว -> เติม _1, _2 ... ไม่งั้นสำเนาแรกจะถูกเขียนทับ)"""
    base = os.path.join(tdir, name[:-4] + f".before_{_stamp()}")
    path, i = base + ".csv", 1
    while os.path.exists(path):
        path, i = f"{base}_{i}.csv", i + 1
    return path


def trash_file(data_dir: str, name: str) -> Dict[str, Any]:
    """ย้าย result_<รอบ>.csv (และ signals_<รอบ>.csv ของรอบเดียวกัน) ไปถังขยะ — กู้คืนได้ด้วย restore_file()"""
    name = os.path.basename(name)
    src = os.path.join(data_dir, name)
    if not is_result_name(name) or not os.path.isfile(src):
        return {"ok": False, "msg": "ไม่พบไฟล์นี้"}
    tdir = os.path.join(data_dir, TRASH_DIR)
    dst = _move(src, tdir)
    moved = [os.path.basename(dst)]
    sig = os.path.join(data_dir, SIGNALS_DIR, "signals_" + name[len("result_"):])
    if os.path.isfile(sig):
        moved.append("signals/" + os.path.basename(_move(sig, os.path.join(tdir, SIGNALS_DIR))))
    ex = load_exclude(data_dir)
    if name in ex:
        save_exclude(data_dir, [n for n in ex if n != name])
    return {"ok": True, "moved": moved,
            "msg": f"ย้าย {name} ไปถังขยะแล้ว (data/{TRASH_DIR}/) — กู้คืนได้จากรายการถังขยะ"}


def list_trash(data_dir: str) -> List[Dict[str, Any]]:
    """ไฟล์ในถังขยะ: ไฟล์ที่ถูกลบทั้งไฟล์ (result_*.csv) และสำเนาก่อนแก้ (result_*.before_*.csv)"""
    tdir = os.path.join(data_dir, TRASH_DIR)
    out = []
    try:
        names = sorted(os.listdir(tdir), reverse=True)
    except OSError:
        return out
    for n in names:
        p = os.path.join(tdir, n)
        if not (os.path.isfile(p) and n.startswith("result_") and n.endswith(".csv")):
            continue
        m = re.match(r"^(result_\d{8}_\d{6})(?:_\d+)?(?:\.before_(\d{8}_\d{6})(?:_\d+)?)?\.csv$", n)
        if not m:
            continue
        try:
            rows = len(read_result_file(p))
        except (OSError, ValueError):
            rows = 0
        out.append({"name": n, "original": m.group(1) + ".csv", "backup_of_edit": bool(m.group(2)),
                    "edited_at": m.group(2) or "", "rows": rows, "bytes": os.path.getsize(p),
                    "mtime": os.path.getmtime(p),
                    # ไฟล์ชื่อเดิมยังใช้อยู่ใน data/ ไหม (ช่วยให้หน้าเว็บบอกได้ว่ากู้คืนแล้วจะ "สลับ" หรือ "เอากลับมา")
                    "original_exists": os.path.isfile(os.path.join(data_dir, m.group(1) + ".csv"))})
    return out


def purge_trash(data_dir: str, name: str) -> Dict[str, Any]:
    """ลบถาวรจากถังขยะ (ผู้ใช้กดยืนยันบนหน้าเว็บแล้ว) name = ชื่อไฟล์ในถังขยะ หรือ "*" = ล้างทั้งถัง
    ลบเฉพาะไฟล์ result_*.csv ในถังขยะ (และ signals ของรอบที่ไม่มี result เหลือในถังแล้ว) ไม่แตะ data/ หลัก"""
    tdir = os.path.join(data_dir, TRASH_DIR)
    items = list_trash(data_dir)
    targets = items if name == "*" else [x for x in items if x["name"] == os.path.basename(name)]
    if not targets:
        return {"ok": False, "msg": "ไม่พบไฟล์นี้ในถังขยะ"}
    for x in targets:
        os.remove(os.path.join(tdir, x["name"]))
    # signals ในถังขยะที่ไม่มี result ของรอบนั้นเหลือในถังแล้ว = ไม่มีใครกู้คืนไปใช้ได้อีก -> ลบด้วย
    left = {x["original"] for x in list_trash(data_dir)}
    sdir = os.path.join(tdir, SIGNALS_DIR)
    if os.path.isdir(sdir):
        for n in os.listdir(sdir):
            if n.startswith("signals_") and "result_" + n[len("signals_"):] not in left:
                os.remove(os.path.join(sdir, n))
    return {"ok": True, "msg": f"ลบถาวร {len(targets)} ไฟล์แล้ว"}


def restore_file(data_dir: str, trash_name: str) -> Dict[str, Any]:
    """กู้ไฟล์จากถังขยะกลับไปที่ data/ ด้วยชื่อเดิม
    ถ้าชื่อเดิมยังมีอยู่ (เช่น กู้สำเนาก่อนแก้) -> ไฟล์ปัจจุบันถูกย้ายเข้าถังขยะแทน (สลับกัน ไม่มีอะไรหาย)"""
    trash_name = os.path.basename(trash_name)
    item = next((x for x in list_trash(data_dir) if x["name"] == trash_name), None)
    if not item:
        return {"ok": False, "msg": "ไม่พบไฟล์นี้ในถังขยะ"}
    tdir = os.path.join(data_dir, TRASH_DIR)
    dst = os.path.join(data_dir, item["original"])
    swapped = ""
    if os.path.exists(dst):
        cur = _backup_path(tdir, item["original"])
        shutil.move(dst, cur)
        swapped = f" (ไฟล์ที่ใช้อยู่ถูกเก็บเป็น {os.path.basename(cur)} ในถังขยะ)"
    shutil.move(os.path.join(tdir, trash_name), dst)
    sig_name = "signals_" + item["original"][len("result_"):]
    sig = os.path.join(tdir, SIGNALS_DIR, sig_name)
    if not item["backup_of_edit"] and os.path.isfile(sig) and not os.path.exists(
            os.path.join(data_dir, SIGNALS_DIR, sig_name)):
        os.makedirs(os.path.join(data_dir, SIGNALS_DIR), exist_ok=True)
        shutil.move(sig, os.path.join(data_dir, SIGNALS_DIR, sig_name))
    return {"ok": True, "msg": f"กู้คืน {item['original']} แล้ว" + swapped}


# [เทคนิค: Atomic file update + backup] สำรองเข้าถังขยะ -> เขียนไฟล์ .tmp -> os.replace() สลับชื่อ (ไม่มีไฟล์เสียครึ่งเดียว)
def _rewrite(data_dir: str, name: str, change: Callable[[List[Dict[str, Any]]], int]) -> Dict[str, Any]:
    """แก้เนื้อหาไฟล์ result อย่างปลอดภัย: สำเนาไฟล์เดิมเข้าถังขยะก่อน -> แก้ -> เขียนไฟล์ชั่วคราว -> สลับชื่อ
    (ไฟล์ไม่มีวันเสียครึ่ง ๆ กลาง ๆ ถ้าไฟดับระหว่างเขียน) change() คืนจำนวนแถวที่เปลี่ยน"""
    name = os.path.basename(name)
    path = os.path.join(data_dir, name)
    if not is_result_name(name) or not os.path.isfile(path):
        return {"ok": False, "msg": "ไม่พบไฟล์นี้"}
    header, _ = read_csv(path)
    if detect_format(header) != "result":
        return {"ok": False, "msg": "ไฟล์นี้ไม่ใช่รูปแบบ result มาตรฐาน — แก้จากหน้าเว็บไม่ได้"}
    rows = read_result_file(path)
    n = change(rows)
    if n == 0:
        return {"ok": True, "changed": 0, "msg": "ไม่มีอะไรเปลี่ยน"}
    tdir = os.path.join(data_dir, TRASH_DIR)
    os.makedirs(tdir, exist_ok=True)
    backup = _backup_path(tdir, name)
    shutil.copy2(path, backup)
    tmp = path + ".tmp"
    write_rows(tmp, rows)
    os.replace(tmp, path)
    return {"ok": True, "changed": n, "backup": os.path.basename(backup), "rows_left": len(rows)}


def _match(r: Dict[str, Any], key: Dict[str, Any]) -> bool:
    """แถวนี้คือข้อที่หน้าเว็บเลือกไหม (เทียบเลขข้อ + เวลา เพราะเลขข้ออาจซ้ำในไฟล์ที่นำเข้า)"""
    return (str(r.get("question_no")) == str(key.get("question_no"))
            and str(r.get("time_iso") or "") == str(key.get("time_iso") or ""))


def delete_rows(data_dir: str, name: str, keys: Sequence[Dict[str, Any]]) -> Dict[str, Any]:
    """ลบบางข้อออกจากไฟล์ (สำรองไฟล์เดิมไว้ในถังขยะก่อน) — ลบจนหมดไฟล์ = ย้ายทั้งไฟล์ไปถังขยะ"""
    def change(rows):
        before = len(rows)
        rows[:] = [r for r in rows if not any(_match(r, k) for k in keys)]
        return before - len(rows)
    res = _rewrite(data_dir, name, change)
    if res.get("ok") and res.get("changed") and res.get("rows_left") == 0:
        trash_file(data_dir, name)
        res["msg"] = f"ลบ {res['changed']} ข้อ — ไฟล์ว่างแล้วจึงย้ายไปถังขยะ"
    elif res.get("ok") and res.get("changed"):
        res["msg"] = f"ลบ {res['changed']} ข้อแล้ว (สำเนาก่อนลบ: trash/{res['backup']})"
    return res


def set_used(data_dir: str, name: str, key: Dict[str, Any], used: bool) -> Dict[str, Any]:
    """เปิด/ปิด "ใช้เทรน" รายข้อ (แทนการเปิดไฟล์แก้ used_for_training เอง)
    เปิดได้เฉพาะข้อที่รู้เฉลย truth/lie และ feature ครบ (ไม่งั้น train.py ก็ข้ามอยู่ดี)"""
    def change(rows):
        n = 0
        for r in rows:
            if not _match(r, key):
                continue
            if used and (r.get("label") not in LABELS or any(_num(r.get(c)) is None for c in FEATURE_COLS)):
                raise ValueError("ข้อนี้ใช้เทรนไม่ได้ (ไม่รู้เฉลย หรือไม่มีค่าสัญญาณครบ)")
            want = "1" if used else "0"
            if str(r.get("used_for_training")) != want:
                r["used_for_training"] = want
                tag = "ผู้ใช้ตั้งไม่ใช้เทรน" if not used else "ผู้ใช้ตั้งให้ใช้เทรน"
                r["note"] = (str(r.get("note")) + " | " if r.get("note") else "") + tag
                n += 1
        return n
    try:
        res = _rewrite(data_dir, name, change)
    except ValueError as e:
        return {"ok": False, "msg": str(e)}
    if res.get("ok") and res.get("changed"):
        res["msg"] = "ใช้ข้อนี้เทรน" if used else "ไม่ใช้ข้อนี้เทรนแล้ว (ข้อมูลยังอยู่ในไฟล์)"
    return res


def model_info(path: str) -> Optional[Dict[str, Any]]:
    """อ่านข้อมูลสรุปของไฟล์โมเดล (ชื่อ ความแม่นยำ จำนวนข้อ เวลาเทรน) — อ่านไม่ได้ = None"""
    try:
        with open(path, encoding="utf-8") as fh:
            d = json.load(fh)
        pm.Model.from_json(d)
    except (OSError, ValueError, KeyError, TypeError):
        return None
    return {"file": os.path.basename(path), "name": d.get("name", "model"), "accuracy": d.get("cv_accuracy"),
            "ci95": d.get("cv_ci95"), "samples": d.get("samples"), "n_truth": d.get("n_truth"),
            "n_lie": d.get("n_lie"), "trained_at": d.get("trained_at"), "subjects": d.get("subjects"),
            "source_files": d.get("source_files"), "features": d.get("features"),
            "majority": d.get("majority_accuracy"), "balanced": d.get("balanced_accuracy")}
