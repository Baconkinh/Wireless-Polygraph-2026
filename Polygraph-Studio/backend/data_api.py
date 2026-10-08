"""
data_api.py — REST API ของไฟล์ข้อมูล (data/result_*.csv) และ AI สำหรับหน้า
  * "เก็บข้อมูลเทรน AI"  ส่วน "ประวัติทั้งหมด" (ดูทุกข้อที่เคยบันทึก + ดาวน์โหลด)
  * "ใช้งานจริง"          ประวัติรอบใช้งานจริง (mode = live)
  * "ข้อมูล & เทรน AI"    รายการไฟล์, เลือกไฟล์ที่ใช้เทรน, รวมไฟล์, นำเข้าไฟล์, ดึงข้อมูลจากนาฬิกา,
                          เทรน AI, ดู/ส่งโมเดลเข้านาฬิกา (อัตโนมัติหรือเอง)

ทุกการอ่าน/เขียนไฟล์ใช้ ml/datafiles.py ตัวเดียวกับ train.py และ collect_data.bat -> ผลตรงกันทุกทาง
ชื่อไฟล์ที่รับจากหน้าเว็บถูกตรวจด้วย regex ก่อนเสมอ (กันการขอไฟล์นอกโฟลเดอร์ data/)
"""
from __future__ import annotations

import asyncio
import os
import re
import sys
import tempfile
import time
from typing import Any, Dict, List, Optional

from fastapi import FastAPI, File, UploadFile
from fastapi.responses import FileResponse, JSONResponse, Response
from pydantic import BaseModel

from . import config as cfgmod
from .watch_link import WatchError

sys.path.insert(0, os.path.join(cfgmod.ROOT, "ml"))
import datafiles as df  # noqa: E402
import train as trainmod  # noqa: E402

DATA = cfgmod.DATA_DIR
SIGNAL_RE = re.compile(r"^signals_\d{8}_\d{6}\.csv$")
MODEL_RE = re.compile(r"^model_[\w.\-]+\.json$")


class ExcludeIn(BaseModel):
    """body ของ POST /api/data/exclude: ชื่อไฟล์ + จะไม่ใช้เทรน (true) หรือกลับมาใช้ (false)"""
    name: str
    exclude: bool


class TrainIn(BaseModel):
    """body ของ POST /api/ai/train: ไฟล์ที่ใช้ (ไม่ระบุ = ทุกไฟล์ที่ติ๊ก) + จำนวนข้อขั้นต่ำต่อคลาส"""
    files: Optional[List[str]] = None    # None = ทุกไฟล์ที่ติ๊ก "ใช้เทรน"
    min_samples: int = 10


class AutoIn(BaseModel):
    """body ของ POST /api/ai/auto: เปิด/ปิดการส่งโมเดลเข้านาฬิกาอัตโนมัติ"""
    on: bool


class UploadIn(BaseModel):
    """body ของ POST /api/ai/upload: ไฟล์โมเดลที่จะส่ง"""
    file: str = "model.json"             # "model.json" หรือ "models/model_xxx.json"


def _bad(msg: str, code: int = 400) -> JSONResponse:
    """ตอบ error เป็น JSON {ok: false, msg} — หน้าเว็บแสดง msg เป็นข้อความแจ้งเตือน"""
    return JSONResponse({"ok": False, "msg": msg}, status_code=code)


def _result_path(name: str) -> Optional[str]:
    """ชื่อไฟล์จากหน้าเว็บ -> path จริง (เฉพาะ result_<ตัวเลข>.csv ใน data/ เท่านั้น)"""
    name = os.path.basename(name or "")
    if not df.is_result_name(name):
        return None
    p = os.path.join(DATA, name)
    return p if os.path.isfile(p) else None


def register(app: FastAPI, *, link, hub, sync, collector):
    """ผูก endpoint ทั้งหมดเข้ากับ FastAPI app (เรียกจาก main.create_app)"""
    train_state: Dict[str, Any] = {"running": False, "log": [], "result": None, "started": None}

    def files_payload() -> Dict[str, Any]:
        """สรุปทุกไฟล์ result_*.csv + สถานะติ๊ก "ใช้เทรน" + ยอดรวม (ใช้ทั้งตอบ API และส่งทาง WebSocket)"""
        ex = set(df.load_exclude(DATA))
        files = []
        for p in df.list_result_files(DATA):
            s = df.summarize(p)
            s["excluded"] = s["name"] in ex
            files.append(s)
        tot = {"files": len(files), "rows": sum(f.get("rows", 0) for f in files),
               "used": sum(f.get("used", 0) for f in files if not f["excluded"])}
        return {"files": files, "total": tot, "exclude": sorted(ex)}

    def publish_files():
        """ส่งรายการไฟล์ล่าสุดไปทุกหน้าเว็บ (ข้อความชนิด "data") — ตารางประวัติ/ไฟล์จะรีเฟรชเอง"""
        hub.publish("data", files_payload())

    collector.on_change = publish_files          # ข้อใหม่ถูกบันทึก -> หน้าข้อมูล/ประวัติรีเฟรชเอง

    # ------------------------------------------------------------ ไฟล์ข้อมูล
    @app.get("/api/data/files", tags=["data"])
    async def data_files():
        """รายการไฟล์ result_*.csv พร้อมสรุป (จำนวนข้อ, เฉลย, ใช้เทรนได้กี่ข้อ, ถูก/ผิดของโหมด live)"""
        return files_payload()

    @app.get("/api/data/rows", tags=["data"])
    async def data_rows(file: str = "all", mode: str = "", limit: int = 3000):
        """แถวทั้งหมด (ทุกไฟล์ หรือไฟล์เดียว) สำหรับตารางประวัติ — mode กรองได้ เช่น live / fix,manual"""
        if file == "all":
            paths = df.list_result_files(DATA)
        else:
            p = _result_path(file)
            if not p:
                return _bad("ไม่พบไฟล์นี้ (ต้องเป็น result_<วันเวลา>.csv ในโฟลเดอร์ data)", 404)
            paths = [p]
        rows: List[Dict[str, Any]] = []
        for p in paths:
            try:
                for r in df.read_result_file(p):
                    r["_file"] = os.path.basename(p)
                    rows.append(r)
            except (OSError, ValueError):
                continue
        if mode:
            keep = set(mode.split(","))
            rows = [r for r in rows if r.get("mode") in keep]
        rows.sort(key=lambda r: (r.get("time_iso") or "", str(r.get("question_no"))), reverse=True)
        return {"ok": True, "count": len(rows), "rows": rows[:max(1, min(limit, 20000))],
                "columns": df.RESULT_HEADER}

    @app.get("/api/data/file/{name}", tags=["data"])
    async def data_file(name: str):
        """ดาวน์โหลดไฟล์ result_*.csv ตามที่อยู่ในเครื่อง (ไบต์เดียวกับไฟล์จริง)"""
        p = _result_path(name)
        if not p:
            return _bad("ไม่พบไฟล์นี้", 404)
        return FileResponse(p, media_type="text/csv", filename=os.path.basename(p))

    @app.get("/api/data/signals/{name}", tags=["data"])
    async def data_signals(name: str):
        """ดาวน์โหลดไฟล์ค่าสดของรอบ (data/signals/signals_<รอบ>.csv)"""
        name = os.path.basename(name)
        p = os.path.join(DATA, df.SIGNALS_DIR, name)
        if not SIGNAL_RE.match(name) or not os.path.isfile(p):
            return _bad("ไม่พบไฟล์ค่าสดของรอบนี้", 404)
        return FileResponse(p, media_type="text/csv", filename=name)

    @app.get("/api/data/merged.csv", tags=["data"])
    async def data_merged(files: str = "", mode: str = ""):
        """รวมหลายไฟล์เป็นไฟล์เดียว (ตัดข้อซ้ำ) แล้วดาวน์โหลด — ไม่ได้เก็บไฟล์รวมไว้ใน data/
        (ถ้าเก็บไว้ train.py จะเห็นข้อเดิมซ้ำ 2 ที่) files ว่าง = ทุกไฟล์ที่ติ๊กใช้เทรน, "all" = ทุกไฟล์"""
        if files == "all":
            paths = df.list_result_files(DATA)
        elif files:
            paths = [p for p in (_result_path(n) for n in files.split(",")) if p]
        else:
            paths = df.training_files(DATA)
        rows, _dup = df.merge_rows(paths)
        if mode:
            keep = set(mode.split(","))
            rows = [r for r in rows if r.get("mode") in keep]
        name = time.strftime("result_all_%Y%m%d_%H%M%S.csv")
        return Response(df.rows_to_csv_text(rows), media_type="text/csv; charset=utf-8",
                        headers={"Content-Disposition": f'attachment; filename="{name}"'})

    @app.post("/api/data/exclude", tags=["data"])
    async def data_exclude(body: ExcludeIn):
        """ติ๊ก/ไม่ติ๊ก "ใช้เทรน" ของไฟล์ (เก็บใน data/train_exclude.txt — train_ai.bat ก็อ่านไฟล์นี้)"""
        if not _result_path(body.name):
            return _bad("ไม่พบไฟล์นี้", 404)
        ex = set(df.load_exclude(DATA))
        (ex.add if body.exclude else ex.discard)(os.path.basename(body.name))
        df.save_exclude(DATA, sorted(ex))
        publish_files()
        return {"ok": True}

    @app.post("/api/data/import", tags=["data"])
    async def data_import(file: UploadFile = File(...)):
        """นำเข้าไฟล์ CSV (เช่น polygraph_train.csv ที่ดาวน์โหลดจากหน้าเว็บนาฬิกา หรือ results_ รุ่นเก่า)
        -> แปลงเป็น result_<เวลา>.csv ใหม่ (เฉพาะข้อที่ยังไม่มี)"""
        data = await file.read()
        if len(data) > 20_000_000:
            return _bad("ไฟล์ใหญ่เกินไป")
        tmpdir = tempfile.mkdtemp()
        tmp = os.path.join(tmpdir, "import.csv")
        try:
            with open(tmp, "wb") as fh:
                fh.write(data)
            rep = df.import_file(DATA, tmp, origin=file.filename or "ไฟล์ที่นำเข้า")
        except ValueError as e:
            rep = {"ok": False, "msg": str(e)}
        finally:
            try:
                os.remove(tmp)
                os.rmdir(tmpdir)
            except OSError:
                pass
        publish_files()
        return rep if rep.get("ok") else JSONResponse(rep, status_code=400)

    @app.post("/api/data/pull_watch", tags=["data"])
    async def data_pull_watch():
        """ดึงข้อมูลที่นาฬิกาบันทึกเอง (โหมด train บนหน้าเว็บนาฬิกา, /api/ml/data.csv) มาเป็น result_*.csv"""
        try:
            text = await link.get_text("/api/ml/data.csv")
        except WatchError as e:
            if "404" in str(e):               # นาฬิกาตอบ NO_DATA = ยังไม่เคยบันทึกเอง ไม่ใช่ความผิดพลาด
                return {"ok": True, "added": 0, "file": None,
                        "msg": "นาฬิกายังไม่มีข้อมูลที่บันทึกเอง (ต้องถามข้อควบคุมในโหมด train บนหน้าเว็บนาฬิกาก่อน)"}
            return _bad(f"ดึงข้อมูลไม่ได้: {e}", 503)
        tmpdir = tempfile.mkdtemp()
        tmp = os.path.join(tmpdir, "watch_train.csv")
        try:
            with open(tmp, "w", encoding="utf-8", newline="") as fh:
                fh.write(text)
            rep = df.import_file(DATA, tmp, origin="นาฬิกา (/api/ml/data.csv)")
        except ValueError as e:
            rep = {"ok": False, "msg": str(e)}
        finally:
            try:
                os.remove(tmp)
                os.rmdir(tmpdir)
            except OSError:
                pass
        publish_files()
        return rep

    # ------------------------------------------------------------ เทรน AI (ใช้ ml/train.py ตัวเดียวกับ train_ai.bat)
    @app.post("/api/ai/train", tags=["ai"])
    async def ai_train(body: TrainIn):
        """กดปุ่ม "เทรน AI": เรียก ml/train.run_training() ใน thread แยก -> ได้ model.json -> ส่งเข้านาฬิกาอัตโนมัติ (ถ้าเปิดไว้)"""
        if train_state["running"]:
            return _bad("กำลังเทรนอยู่ รอสักครู่")
        if body.files:
            paths = [p for p in (_result_path(n) for n in body.files) if p]
        else:
            paths = df.training_files(DATA)
        train_state.update(running=True, log=[], result=None, started=time.time())
        hub.publish("train", {"running": True, "log": []})

        def log(s: str):
            """รับข้อความจาก train.py ทีละบรรทัด เก็บไว้แสดงในกล่อง log ของหน้าเว็บ"""
            train_state["log"].append(s)

        try:
            # เทรนใน thread แยก: คำนวณหลายวินาที ไม่ให้หน้าเว็บ/ข้อมูลสดค้าง
            res = await asyncio.to_thread(trainmod.run_training, paths, None, max(2, body.min_samples),
                                          trainmod.MODEL_PATH, False, log)
        except Exception as e:  # noqa: BLE001 - แสดงสาเหตุบนหน้าเว็บ
            res = {"ok": False, "msg": f"เทรนไม่สำเร็จ: {e}"}
        train_state.update(running=False, result={k: v for k, v in res.items() if k != "model"})
        hub.publish("train", {"running": False, "log": train_state["log"], "result": train_state["result"]})
        if res.get("ok"):
            await sync.tick()                 # ส่งเข้านาฬิกาเลยถ้าเปิดอัตโนมัติ (ไม่ต้องรอรอบถัดไป)
            sync.publish(force=True)
        return {"ok": res.get("ok", False), "msg": res.get("msg", ""), "log": train_state["log"],
                "metrics": res.get("metrics")}

    @app.get("/api/ai/train", tags=["ai"])
    async def ai_train_state():
        """สถานะการเทรนล่าสุด (กำลังเทรนอยู่ไหม + ข้อความ log + ผล)"""
        return train_state

    # ------------------------------------------------------------ โมเดล
    @app.get("/api/ai", tags=["ai"])
    async def ai_state():
        """โมเดลในคอม (model.json + ประวัติ), โมเดลในนาฬิกา, สถานะส่งอัตโนมัติ"""
        hist = []
        mdir = os.path.join(DATA, df.MODELS_DIR)
        if os.path.isdir(mdir):
            for n in sorted(os.listdir(mdir), reverse=True):
                if MODEL_RE.match(n):
                    info = df.model_info(os.path.join(mdir, n))
                    if info:
                        hist.append(info)
        return dict(sync.state(), history=hist[:30])

    @app.post("/api/ai/auto", tags=["ai"])
    async def ai_auto(body: AutoIn):
        """เปิด/ปิดการส่งโมเดลอัตโนมัติ (เปิดแล้วตรวจ/ส่งทันที)"""
        sync.set_auto(body.on)
        if body.on:
            await sync.tick()
        return {"ok": True, **sync.state()}

    @app.post("/api/ai/upload", tags=["ai"])
    async def ai_upload(body: UploadIn):
        """ส่งโมเดลที่เลือกเข้านาฬิกาเอง — ถ้าไม่ใช่ model.json ตัวล่าสุด จะปิดโหมดอัตโนมัติให้
        (ไม่งั้นระบบอัตโนมัติจะส่ง model.json กลับไปทับทันที)"""
        name = body.file.replace("\\", "/")
        if name == "model.json":
            path = trainmod.MODEL_PATH
        else:
            base = os.path.basename(name)
            path = os.path.join(DATA, df.MODELS_DIR, base)
            if not MODEL_RE.match(base) or not os.path.isfile(path):
                return _bad("ไม่พบไฟล์โมเดลนี้", 404)
            if sync.auto:
                sync.set_auto(False)
        if not os.path.isfile(path):
            return _bad("ยังไม่มี data/model.json — กดเทรนก่อน", 404)
        return await sync.upload_file(path)

    @app.post("/api/ai/clear_watch", tags=["ai"])
    async def ai_clear_watch():
        """ลบโมเดลในนาฬิกา (กลับไปใช้สูตร) — ปิดอัตโนมัติด้วย ไม่งั้นจะถูกส่งกลับเข้าไปใหม่"""
        if sync.auto:
            sync.set_auto(False)
        return await sync.clear_watch()
