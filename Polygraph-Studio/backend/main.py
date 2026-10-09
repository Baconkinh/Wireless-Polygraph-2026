"""
main.py — Polygraph Studio backend (FastAPI)

  เบราว์เซอร์  <--WebSocket /ws (ข้อมูลสด)-->  Studio  <--UDP 4210 + HTTP-->  นาฬิกา (ESP32-C3)
              <--REST /api/... (สั่งงาน)----->         --SQLite data/studio.db (ประวัติ)

เอกสาร API อัตโนมัติ (Swagger UI): http://127.0.0.1:8000/docs
"""
from __future__ import annotations

import os
import time
from contextlib import asynccontextmanager
from typing import Any, Dict, List, Optional

from fastapi import FastAPI, File, UploadFile, WebSocket
from fastapi.responses import FileResponse, HTMLResponse, JSONResponse, PlainTextResponse, Response
from fastapi.staticfiles import StaticFiles
from pydantic import BaseModel

from . import config as cfgmod
from . import data_api, report
from .collector import Collector
from .model_sync import ModelSync
from .hub import Hub
from .sessions import FEATURES, KIND_TH, TEMPLATES, VERDICT_TH, Interrogation
from .store import Store
from .watch_link import WatchError, WatchLink

STUDIO_VERSION = "2.3.0"

# ผู้จัดทำ (แสดงในหน้าเว็บ/รายงาน)
CREDITS = {
    "project": "Wireless Polygraph — เครื่องจับเท็จไร้สาย",
    "course": "03603323 Introduction to Embedded Systems (และ 03603324 Embedded System Laboratory)",
    "org": "ภาควิชาวิศวกรรมคอมพิวเตอร์ คณะวิศวกรรมศาสตร์ ศรีราชา มหาวิทยาลัยเกษตรศาสตร์",
    "year": "ภาคต้น ปีการศึกษา 2569",
    "members": [{"name": "อัจฉรา ดังดี", "id": "6730300655"}, {"name": "ปภากร จันทร์ดี", "id": "6730300809"}],
}


# ---------------------------------------------------------------- request bodies
class SessionCreate(BaseModel):
    subject: str = ""
    operator: str = ""
    notes: str = ""
    template: str = "classroom"
    questions: Optional[List[Dict[str, Any]]] = None


class QuestionIn(BaseModel):
    """body ของการเพิ่มคำถามในเซสชัน: ชนิด (test/truth/lie/warmup), ข้อความ, คำใบ้"""
    kind: str = "test"
    text: str
    hint: str = ""
    grp: str = ""
    label: str = ""


class QuestionPatch(BaseModel):
    """body ของการแก้คำถาม (ส่งเฉพาะช่องที่จะแก้)"""
    kind: Optional[str] = None
    text: Optional[str] = None
    hint: Optional[str] = None


class AnswerIn(BaseModel):
    """body ของการกดคำตอบ ใช่ (true) / ไม่ใช่ (false)"""
    yes: bool


class CommandIn(BaseModel):
    """body ของคำสั่งไปนาฬิกา: action + พารามิเตอร์ (ดู routes ใน watch_command)"""
    action: str
    params: Dict[str, Any] = {}


class ConfigIn(BaseModel):
    """body ของการตั้งค่า LieEngine ในนาฬิกา (ชื่อค่า: ค่าใหม่)"""
    values: Dict[str, Any]


class CollectStart(BaseModel):
    """body ของการเริ่มรอบเก็บข้อมูล/ใช้งานจริง"""
    subject: str = ""
    operator: str = ""
    mode: str = "fix"            # fix = บอกเฉลยก่อนถาม, manual = ถามก่อนแล้วผู้ตอบบอกเฉลยทีหลัง, live = ใช้งานจริง


class CollectAsk(BaseModel):
    """body ของการเริ่มถาม 1 ข้อ"""
    label: Optional[str] = None  # โหมด fix: "truth" / "lie"
    text: str = ""               # ข้อความคำถาม (ไม่บังคับ) — บันทึกลงคอลัมน์ question_text


class CollectFeedback(BaseModel):
    """body ของการบอกว่านาฬิกาตอบถูก/ผิด (หน้าใช้งานจริง)"""
    feedback: str                # โหมด live: "correct" / "wrong" / "unknown"


class CollectLabel(BaseModel):
    """body ของการบอกเฉลยหลังตอบ (โหมด manual)"""
    label: str                   # "truth" / "lie" / "unknown"


class CollectMode(BaseModel):
    """body ของการสลับโหมด fix/manual ระหว่างรอบ"""
    mode: str


def create_app(settings: Optional[cfgmod.Settings] = None) -> FastAPI:
    """ประกอบทุกส่วนของ backend เข้าด้วยกันแล้วคืน FastAPI app
      Store (SQLite) + Hub (WebSocket) + Interrogation (ตัวกลาง) + WatchLink (คุยกับนาฬิกา)
      + Collector (รอบเก็บข้อมูล/ใช้งานจริง) + ModelSync (ส่งโมเดลอัตโนมัติ) + data_api (ไฟล์/เทรน)
    """
    s = settings or cfgmod.load([])
    # แปลงไฟล์ข้อมูลรูปแบบเก่า (results_*.csv, training_samples.csv, watch_train_*.csv) เป็น result_*.csv
    # ครั้งเดียว — ไฟล์เดิมถูกย้ายไป data/old_format/ ไม่ได้แก้ไข (ดู ml/datafiles.py)
    try:
        data_api.df.migrate(cfgmod.DATA_DIR, log=lambda m: print("[แปลงไฟล์เก่า] " + m))
    except Exception as e:  # noqa: BLE001 - แปลงไม่ได้ก็ยังเปิด Studio ได้ (ไฟล์เดิมไม่ถูกแตะ)
        print("[แปลงไฟล์เก่า] ไม่สำเร็จ:", e)
    store = Store(s.db_path)
    hub = Hub()
    inter = Interrogation(store, hub)
    link = WatchLink(s, inter)
    inter.link = link
    collector = Collector(store, hub)        # หน้าเก็บข้อมูลเทรน AI
    collector.link = link
    inter.collector = collector
    sync = ModelSync(link, hub)              # ส่ง data/model.json ตัวล่าสุดเข้านาฬิกาอัตโนมัติ

    @asynccontextmanager
    async def lifespan(app: FastAPI):
        """งานตอนเปิด (เริ่มคุยกับนาฬิกา, เริ่มตรวจโมเดล) และตอนปิดโปรแกรม (ปิดไฟล์, เขียนค่าค้าง, หยุด task)"""
        await link.start()
        sync.start()
        yield
        collector.stop()                     # ปิดไฟล์ CSV ให้เรียบร้อยตอนปิดโปรแกรม
        store.flush_samples()
        await sync.stop()
        await link.stop()

    app = FastAPI(title="Polygraph Studio", version=STUDIO_VERSION, lifespan=lifespan,
                  description="Backend ของเครื่องจับเท็จไร้สาย: เชื่อมนาฬิกา ESP32-C3 ↔ หน้าเว็บ")
    app.state.settings, app.state.store, app.state.link, app.state.inter = s, store, link, inter

    def watch_error(e: Exception) -> JSONResponse:
        """ติดต่อนาฬิกาไม่ได้ -> ตอบ HTTP 503 พร้อมข้อความไทย (หน้าเว็บแสดงเป็นแจ้งเตือน)"""
        return JSONResponse({"ok": False, "error": "WATCH_UNREACHABLE", "msg": str(e)}, status_code=503)

    def snapshot() -> Dict[str, Any]:
        """ข้อมูลทั้งหมด ณ ตอนนี้ — ส่งให้หน้าเว็บทันทีที่ต่อ WebSocket (ไม่ต้องรอข้อมูลรอบถัดไป)"""
        return {
            "version": STUDIO_VERSION, "status": link.status(), "live": link.live, "lie": link.lie,
            "device": {"hi": link.device, "info": link.info}, "session": inter.session_view(),
            "collect": collector.state(), "credits": CREDITS, "ai": sync.state(),
            "meta": {"features": [{"key": f[0], "name": f[1], "unit": f[2], "why": f[4]} for f in FEATURES],
                     "verdict_th": VERDICT_TH, "kind_th": KIND_TH},
        }

    # ------------------------------------------------------------ หน้าเว็บ + WebSocket
    @app.get("/", include_in_schema=False)
    async def index():
        """หน้าเว็บหลักของ Studio (frontend/index.html)"""
        return FileResponse(os.path.join(cfgmod.FRONTEND_DIR, "index.html"))

    app.mount("/assets", StaticFiles(directory=os.path.join(cfgmod.FRONTEND_DIR, "assets")), name="assets")

    @app.middleware("http")
    async def no_stale_frontend(request, call_next):
        """ให้เบราว์เซอร์ถาม Studio ทุกครั้งว่าไฟล์หน้าเว็บเปลี่ยนไหม (Cache-Control: no-cache)
        ไม่เปลี่ยน = ตอบ 304 เร็วเหมือนเดิม, เปลี่ยน = ได้ไฟล์ใหม่ทันที
        ทำไม: เบราว์เซอร์เคยจำ index.html/main.js รุ่นเก่าไว้ หลังอัปเดต Studio จึงยังเห็นเมนู/หน้าเก่า"""
        resp = await call_next(request)
        p = request.url.path
        if p == "/" or p.startswith("/assets/"):
            resp.headers["Cache-Control"] = "no-cache"
        return resp

    @app.websocket("/ws")
    async def ws_endpoint(ws: WebSocket):
        """WebSocket /ws: ส่งข้อมูลสดให้หน้าเว็บตลอดเวลาที่เปิดอยู่"""
        await hub.serve(ws, snapshot())

    # ------------------------------------------------------------ สถานะ
    @app.get("/api/status", tags=["status"])
    async def api_status():
        """สถานะการเชื่อมต่อนาฬิกา + เวอร์ชัน Studio + จำนวนหน้าเว็บที่เปิดอยู่"""
        return dict(link.status(), studio=STUDIO_VERSION, wsClients=hub.count, serverTime=time.time(),
                    activeSession=inter.active)

    @app.get("/api/snapshot", tags=["status"])
    async def api_snapshot():
        """ข้อมูลทั้งหมด ณ ตอนนี้ (เหมือนที่ส่งตอนต่อ WebSocket)"""
        return snapshot()

    @app.get("/api/templates", tags=["sessions"])
    async def api_templates():
        """ชุดคำถามสำเร็จรูปสำหรับสร้างเซสชัน"""
        return TEMPLATES

    # ------------------------------------------------------------ ส่งต่อคำสั่งไปนาฬิกา
    @app.get("/api/watch/info", tags=["watch"])
    async def watch_info():
        """ข้อมูลเครื่องของนาฬิกา (เฟิร์มแวร์, บูต, หน่วยความจำ) — ส่งต่อจาก /api/info ของนาฬิกา"""
        try:
            return await link.get("/api/info")
        except WatchError as e:
            return watch_error(e)

    @app.get("/api/watch/system", tags=["watch"])
    async def watch_system():
        """ข้อมูลระบบ (FreeRTOS task, watchdog, แฟลช, พลังงาน) — ส่งต่อจาก /api/system"""
        try:
            return await link.get("/api/system")
        except WatchError as e:
            return watch_error(e)

    @app.get("/api/watch/lie", tags=["watch"])
    async def watch_lie():
        """สถานะ LieEngine + ผล 24 ข้อล่าสุด — ส่งต่อจาก /api/lie"""
        try:
            return await link.get("/api/lie", n=24)
        except WatchError as e:
            return watch_error(e)

    @app.get("/api/watch/config", tags=["watch"])
    async def watch_config():
        """อ่านค่าตั้งของ LieEngine ในนาฬิกา"""
        try:
            return await link.get("/api/config")
        except WatchError as e:
            return watch_error(e)

    @app.post("/api/watch/config", tags=["watch"])
    async def watch_config_set(body: ConfigIn):
        """เปลี่ยนค่าตั้งของ LieEngine (true/false แปลงเป็น 1/0 ให้นาฬิกา)"""
        try:
            vals = {k: (1 if v is True else 0 if v is False else v) for k, v in body.values.items()}
            return await link.post("/api/config", **vals)
        except WatchError as e:
            return watch_error(e)

    @app.post("/api/watch/config/reset", tags=["watch"])
    async def watch_config_reset():
        """คืนค่าตั้ง LieEngine เป็นค่าเริ่มต้น"""
        try:
            return await link.post("/api/config/reset")
        except WatchError as e:
            return watch_error(e)

    @app.get("/api/watch/logs", tags=["watch"])
    async def watch_logs(file: str = "events"):
        """อ่านไฟล์ log ในนาฬิกา (events / results) เป็นข้อความ"""
        try:
            return PlainTextResponse(await link.get_text("/api/logs", file=file))
        except WatchError as e:
            return PlainTextResponse(str(e), status_code=503)

    @app.post("/api/watch/command", tags=["watch"])
    async def watch_command(cmd: CommandIn):
        """baseline · abort · reset · question · answer · power · restart · demo · stats_reset · logs_clear"""
        p = cmd.params or {}
        routes = {
            "baseline": ("/api/lie/baseline", {"sec": p.get("sec")}),
            "abort": ("/api/lie/abort", {}),
            "reset": ("/api/lie/reset", {}),
            "question": ("/api/lie/question", {"qid": p.get("qid"), "kind": p.get("kind", "test")}),
            "answer": ("/api/lie/answer", {"ans": "yes" if p.get("yes", True) else "no"}),
            "power": ("/api/power", {"mode": p.get("mode"), "sec": p.get("sec")}),
            "restart": ("/api/restart", {}),
            "demo": ("/api/demo", {"type": p.get("type"), "confirm": p.get("confirm")}),
            "stats_reset": ("/api/stats/reset", {}),
            "logs_clear": ("/api/logs/clear", {}),
            # v2.1: ตั้งค่าแบบเดียวกับหน้าเว็บในนาฬิกา
            "sleep": ("/api/sleep", {"auto": p.get("auto"), "now": p.get("now"), "sec": p.get("sec")}),
            "wifi": ("/api/wifi", {"level": p.get("level")}),
            "ml_mode": ("/api/ml/mode", {"mode": p.get("mode")}),
        }
        if cmd.action not in routes:
            return JSONResponse({"ok": False, "error": "BAD_ACTION", "msg": f"ไม่รู้จักคำสั่ง {cmd.action}"}, 400)
        path, params = routes[cmd.action]
        try:
            res = await link.post(path, **params)
        except WatchError as e:
            return watch_error(e)
        if res.get("ok"):
            store.add_event(inter.active, f"cmd_{cmd.action}", p)
        return res

    @app.get("/api/watch/ml", tags=["watch"])
    async def watch_ml():
        """สถานะ AI ในนาฬิกา (โหมด, ข้อมูลที่บันทึกเอง, โมเดลที่ติดตั้ง) — ส่งต่อจาก /api/ml"""
        try:
            return await link.get("/api/ml")
        except WatchError as e:
            return watch_error(e)

    # ------------------------------------------------------------ หน้าเก็บข้อมูลเทรน AI
    @app.get("/api/collect", tags=["collect"])
    async def collect_state():
        """สถานะรอบเก็บข้อมูล/ใช้งานจริงที่กำลังทำอยู่"""
        return collector.state()

    @app.post("/api/collect/start", tags=["collect"])
    async def collect_start(body: CollectStart):
        """เริ่มรอบใหม่ (mode: fix / manual / live)"""
        return collector.start(body.subject.strip(), body.operator.strip(), body.mode)

    @app.post("/api/collect/ask", tags=["collect"])
    async def collect_ask(body: CollectAsk):
        """เริ่มถาม 1 ข้อ (fix ต้องบอก label truth/lie, ส่ง text คำถามได้)"""
        try:
            return await collector.ask(body.label, body.text)
        except WatchError as e:
            return watch_error(e)

    @app.post("/api/collect/feedback", tags=["collect"])
    async def collect_feedback(body: CollectFeedback):
        """หน้า "ใช้งานจริง": หลังได้ผล ผู้ใช้บอกว่านาฬิกาตอบถูก / ผิด / ไม่ทราบ"""
        return collector.feedback(body.feedback)

    @app.post("/api/collect/label", tags=["collect"])
    async def collect_label(body: CollectLabel):
        """โหมด manual: บอกเฉลยหลังได้ผล (truth / lie / unknown)"""
        return collector.label(body.label)

    @app.post("/api/collect/abort", tags=["collect"])
    async def collect_abort():
        """ยกเลิกข้อที่กำลังถาม"""
        return await collector.abort()

    @app.post("/api/collect/mode", tags=["collect"])
    async def collect_mode(body: CollectMode):
        """สลับ fix <-> manual ระหว่างรอบ (ทำได้ตอนไม่มีข้อค้าง)"""
        return collector.set_mode(body.mode)

    @app.post("/api/collect/stop", tags=["collect"])
    async def collect_stop():
        """จบรอบ (ปิดไฟล์)"""
        return collector.stop()

    # ไฟล์ข้อมูล result_*.csv, เทรน AI, ส่งโมเดลเข้านาฬิกา (backend/data_api.py)
    data_api.register(app, link=link, hub=hub, sync=sync, collector=collector)

    @app.get("/api/credits", tags=["status"])
    async def credits():
        """ชื่อโครงงาน รายวิชา และผู้จัดทำ"""
        return CREDITS

    @app.post("/api/watch/ota", tags=["watch"])
    async def watch_ota(file: UploadFile = File(...)):
        """อัปโหลดเฟิร์มแวร์ใหม่เข้านาฬิกาผ่าน WiFi (ตรวจไฟล์ก่อนส่ง: ต้องขึ้นต้น 0xE9 และใหญ่พอ)"""
        data = await file.read()
        if len(data) < 100_000 or data[0] != 0xE9:
            # ไฟล์เฟิร์มแวร์ ESP32 ต้องขึ้นต้นด้วย 0xE9 และใหญ่หลายร้อย KB
            return JSONResponse({"ok": False, "error": "BAD_FILE",
                                 "msg": "ไม่ใช่ไฟล์ firmware.bin ของ ESP32 (เลือกจาก .pio/build/esp32c3/firmware.bin)"}, 400)
        try:
            res = await link.upload_firmware(file.filename or "firmware.bin", data)
        except WatchError as e:
            return watch_error(e)
        store.add_event(inter.active, "ota_upload", {"bytes": len(data), "ok": res.get("ok")})
        return res

    @app.post("/api/sim/{action}", tags=["simulator"])
    async def sim_action(action: str, params: Dict[str, Any] = {}):
        """ปุ่มควบคุมนาฬิกาจำลอง (ใช้ได้เฉพาะตอนต่อ tools/virtual_watch.py)"""
        try:
            return await link.post(f"/sim/{action}", **params)
        except WatchError as e:
            return watch_error(e)

    # ------------------------------------------------------------ เซสชันการสอบสวน
    @app.get("/api/sessions", tags=["sessions"])
    async def sessions_list():
        """รายการเซสชันทั้งหมด"""
        return store.list_sessions()

    @app.post("/api/sessions", tags=["sessions"])
    async def sessions_create(body: SessionCreate):
        """สร้างเซสชันใหม่จากชุดคำถามสำเร็จรูปหรือคำถามที่ส่งมา"""
        qs = body.questions
        if qs is None:
            qs = (TEMPLATES.get(body.template) or TEMPLATES["custom"])["questions"]
        return inter.create(body.subject.strip(), body.operator.strip(), body.notes.strip(), body.template, qs)

    @app.get("/api/sessions/active", tags=["sessions"])
    async def sessions_active():
        """เซสชันที่เปิดอยู่ตอนนี้"""
        return inter.session_view()

    @app.get("/api/sessions/{sid}", tags=["sessions"])
    async def sessions_get(sid: int):
        """ข้อมูลเซสชันตาม id"""
        v = inter.session_view(sid)
        return v if v else JSONResponse({"ok": False, "msg": "ไม่พบเซสชัน"}, 404)

    @app.post("/api/sessions/{sid}/end", tags=["sessions"])
    async def sessions_end(sid: int):
        """จบเซสชัน"""
        if sid == inter.active:
            inter.end()
        else:
            store.end_session(sid)
        return {"ok": True}

    @app.post("/api/sessions/{sid}/resume", tags=["sessions"])
    async def sessions_resume(sid: int):
        """เปิดเซสชันเก่ากลับมาถามต่อ (ผลเก่าในนาฬิกาไม่ถูกนำมาปน)"""
        if not store.get_session(sid):
            return JSONResponse({"ok": False, "msg": "ไม่พบเซสชัน"}, 404)
        if inter.active and inter.active != sid:
            store.end_session(inter.active)
        store._x("UPDATE sessions SET ended=NULL WHERE id=?", (sid,))
        inter.active = sid
        inter.start_boot = link.boot_key
        inter.start_seq = int(link.live.get("rs") or 0) if link.live else None
        inter.publish_session()
        return inter.session_view()

    @app.delete("/api/sessions/{sid}", tags=["sessions"])
    async def sessions_delete(sid: int):
        """ลบเซสชัน"""
        if sid == inter.active:
            inter.end()
        store.delete_session(sid)
        return {"ok": True}

    @app.post("/api/sessions/{sid}/questions", tags=["sessions"])
    async def questions_add(sid: int, q: QuestionIn):
        """เพิ่มคำถามในเซสชัน"""
        if not q.text.strip():
            return JSONResponse({"ok": False, "msg": "กรุณาพิมพ์คำถาม"}, 400)
        store.add_question(sid, q.kind, q.text.strip(), q.hint, q.grp, q.label)
        inter.publish_session()
        return inter.session_view(sid)

    @app.patch("/api/questions/{qid}", tags=["sessions"])
    async def questions_patch(qid: int, q: QuestionPatch):
        """แก้คำถาม"""
        store.update_question(qid, kind=q.kind, text=q.text, hint=q.hint)
        inter.publish_session()
        return {"ok": True}

    @app.delete("/api/questions/{qid}", tags=["sessions"])
    async def questions_delete(qid: int):
        """ลบคำถาม (เฉพาะข้อที่ยังไม่ได้ถาม)"""
        store.delete_question(qid)
        inter.publish_session()
        return {"ok": True}

    @app.post("/api/questions/{qid}/ask", tags=["sessions"])
    async def questions_ask(qid: int):
        """เริ่มถามคำถามข้อนี้ (สั่งนาฬิกาเริ่มวัด)"""
        try:
            return await inter.ask(qid)
        except WatchError as e:
            return watch_error(e)

    @app.post("/api/answer", tags=["sessions"])
    async def answer(body: AnswerIn):
        """ผู้ตอบตอบ ใช่/ไม่ใช่"""
        try:
            return await inter.answer(body.yes)
        except WatchError as e:
            return watch_error(e)

    @app.get("/api/sessions/{sid}/samples", tags=["sessions"])
    async def sessions_samples(sid: int, max_points: int = 2000):
        """ค่าสดของเซสชัน (ลดจำนวนจุดให้ไม่เกิน max_points สำหรับวาดกราฟ)"""
        return store.get_samples(sid, max_points)

    @app.get("/api/sessions/{sid}/results.csv", tags=["export"])
    async def export_results(sid: int):
        """ดาวน์โหลดผลทุกข้อของเซสชันเป็น CSV (มี BOM ให้ Excel อ่านภาษาไทยถูก)"""
        return Response(store.export_results_csv(sid), media_type="text/csv; charset=utf-8",
                        headers={"Content-Disposition": f'attachment; filename="session{sid}_results.csv"'})

    @app.get("/api/sessions/{sid}/samples.csv", tags=["export"])
    async def export_samples(sid: int):
        """ดาวน์โหลดค่าสดของเซสชันเป็น CSV"""
        return Response(store.export_samples_csv(sid), media_type="text/csv",
                        headers={"Content-Disposition": f'attachment; filename="session{sid}_signals.csv"'})

    @app.get("/api/sessions/{sid}/session.json", tags=["export"])
    async def export_json(sid: int):
        """ดาวน์โหลดเซสชันทั้งหมดเป็น JSON (คำถาม + ผล + ค่าสด)"""
        v = inter.session_view(sid)
        if v:
            v["samples"] = store.get_samples(sid, 10 ** 9)
        return JSONResponse(v, headers={"Content-Disposition": f'attachment; filename="session{sid}.json"'})

    @app.get("/report-data-dictionary", response_class=HTMLResponse, tags=["export"])
    async def data_dictionary():
        """ความหมายของทุกตาราง/คอลัมน์ในฐานข้อมูลและไฟล์ CSV (อ่านจาก DATA_DICTIONARY.md)"""
        path = os.path.join(cfgmod.ROOT, "DATA_DICTIONARY.md")
        try:
            with open(path, encoding="utf-8") as fh:
                return HTMLResponse(report.markdown_page("Data Dictionary", fh.read()))
        except OSError:
            return HTMLResponse("<p>ไม่พบไฟล์ DATA_DICTIONARY.md</p>", 404)

    @app.get("/report/{sid}", response_class=HTMLResponse, tags=["export"])
    async def report_page(sid: int):
        """หน้ารายงานของเซสชัน (พิมพ์/บันทึกเป็น PDF ได้)"""
        v = inter.session_view(sid)
        if not v:
            return HTMLResponse("<h1>ไม่พบเซสชัน</h1>", 404)
        return HTMLResponse(report.render(v, store.get_samples(sid, 1500), CREDITS))

    return app
