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
from . import report
from .hub import Hub
from .sessions import FEATURES, KIND_TH, TEMPLATES, VERDICT_TH, Interrogation
from .store import Store
from .watch_link import WatchError, WatchLink

STUDIO_VERSION = "2.0.0"


# ---------------------------------------------------------------- request bodies
class SessionCreate(BaseModel):
    subject: str = ""
    operator: str = ""
    notes: str = ""
    template: str = "classroom"
    questions: Optional[List[Dict[str, Any]]] = None


class QuestionIn(BaseModel):
    kind: str = "test"
    text: str
    hint: str = ""
    grp: str = ""
    label: str = ""


class QuestionPatch(BaseModel):
    kind: Optional[str] = None
    text: Optional[str] = None
    hint: Optional[str] = None


class AnswerIn(BaseModel):
    yes: bool


class CommandIn(BaseModel):
    action: str
    params: Dict[str, Any] = {}


class ConfigIn(BaseModel):
    values: Dict[str, Any]


def create_app(settings: Optional[cfgmod.Settings] = None) -> FastAPI:
    s = settings or cfgmod.load([])
    store = Store(s.db_path)
    hub = Hub()
    inter = Interrogation(store, hub)
    link = WatchLink(s, inter)
    inter.link = link

    @asynccontextmanager
    async def lifespan(app: FastAPI):
        await link.start()
        yield
        store.flush_samples()
        await link.stop()

    app = FastAPI(title="Polygraph Studio", version=STUDIO_VERSION, lifespan=lifespan,
                  description="Backend ของเครื่องจับเท็จไร้สาย: เชื่อมนาฬิกา ESP32-C3 ↔ หน้าเว็บ")
    app.state.settings, app.state.store, app.state.link, app.state.inter = s, store, link, inter

    def watch_error(e: Exception) -> JSONResponse:
        return JSONResponse({"ok": False, "error": "WATCH_UNREACHABLE", "msg": str(e)}, status_code=503)

    def snapshot() -> Dict[str, Any]:
        return {
            "version": STUDIO_VERSION, "status": link.status(), "live": link.live, "lie": link.lie,
            "device": {"hi": link.device, "info": link.info}, "session": inter.session_view(),
            "meta": {"features": [{"key": f[0], "name": f[1], "unit": f[2], "why": f[4]} for f in FEATURES],
                     "verdict_th": VERDICT_TH, "kind_th": KIND_TH},
        }

    # ------------------------------------------------------------ หน้าเว็บ + WebSocket
    @app.get("/", include_in_schema=False)
    async def index():
        return FileResponse(os.path.join(cfgmod.FRONTEND_DIR, "index.html"))

    app.mount("/assets", StaticFiles(directory=os.path.join(cfgmod.FRONTEND_DIR, "assets")), name="assets")

    @app.websocket("/ws")
    async def ws_endpoint(ws: WebSocket):
        await hub.serve(ws, snapshot())

    # ------------------------------------------------------------ สถานะ
    @app.get("/api/status", tags=["status"])
    async def api_status():
        return dict(link.status(), studio=STUDIO_VERSION, wsClients=hub.count, serverTime=time.time(),
                    activeSession=inter.active)

    @app.get("/api/snapshot", tags=["status"])
    async def api_snapshot():
        return snapshot()

    @app.get("/api/templates", tags=["sessions"])
    async def api_templates():
        return TEMPLATES

    # ------------------------------------------------------------ ส่งต่อคำสั่งไปนาฬิกา
    @app.get("/api/watch/info", tags=["watch"])
    async def watch_info():
        try:
            return await link.get("/api/info")
        except WatchError as e:
            return watch_error(e)

    @app.get("/api/watch/system", tags=["watch"])
    async def watch_system():
        try:
            return await link.get("/api/system")
        except WatchError as e:
            return watch_error(e)

    @app.get("/api/watch/lie", tags=["watch"])
    async def watch_lie():
        try:
            return await link.get("/api/lie", n=24)
        except WatchError as e:
            return watch_error(e)

    @app.get("/api/watch/config", tags=["watch"])
    async def watch_config():
        try:
            return await link.get("/api/config")
        except WatchError as e:
            return watch_error(e)

    @app.post("/api/watch/config", tags=["watch"])
    async def watch_config_set(body: ConfigIn):
        try:
            vals = {k: (1 if v is True else 0 if v is False else v) for k, v in body.values.items()}
            return await link.post("/api/config", **vals)
        except WatchError as e:
            return watch_error(e)

    @app.post("/api/watch/config/reset", tags=["watch"])
    async def watch_config_reset():
        try:
            return await link.post("/api/config/reset")
        except WatchError as e:
            return watch_error(e)

    @app.get("/api/watch/logs", tags=["watch"])
    async def watch_logs(file: str = "events"):
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

    @app.post("/api/watch/ota", tags=["watch"])
    async def watch_ota(file: UploadFile = File(...)):
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
        return store.list_sessions()

    @app.post("/api/sessions", tags=["sessions"])
    async def sessions_create(body: SessionCreate):
        qs = body.questions
        if qs is None:
            qs = (TEMPLATES.get(body.template) or TEMPLATES["custom"])["questions"]
        return inter.create(body.subject.strip(), body.operator.strip(), body.notes.strip(), body.template, qs)

    @app.get("/api/sessions/active", tags=["sessions"])
    async def sessions_active():
        return inter.session_view()

    @app.get("/api/sessions/{sid}", tags=["sessions"])
    async def sessions_get(sid: int):
        v = inter.session_view(sid)
        return v if v else JSONResponse({"ok": False, "msg": "ไม่พบเซสชัน"}, 404)

    @app.post("/api/sessions/{sid}/end", tags=["sessions"])
    async def sessions_end(sid: int):
        if sid == inter.active:
            inter.end()
        else:
            store.end_session(sid)
        return {"ok": True}

    @app.post("/api/sessions/{sid}/resume", tags=["sessions"])
    async def sessions_resume(sid: int):
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
        if sid == inter.active:
            inter.end()
        store.delete_session(sid)
        return {"ok": True}

    @app.post("/api/sessions/{sid}/questions", tags=["sessions"])
    async def questions_add(sid: int, q: QuestionIn):
        if not q.text.strip():
            return JSONResponse({"ok": False, "msg": "กรุณาพิมพ์คำถาม"}, 400)
        store.add_question(sid, q.kind, q.text.strip(), q.hint, q.grp, q.label)
        inter.publish_session()
        return inter.session_view(sid)

    @app.patch("/api/questions/{qid}", tags=["sessions"])
    async def questions_patch(qid: int, q: QuestionPatch):
        store.update_question(qid, kind=q.kind, text=q.text, hint=q.hint)
        inter.publish_session()
        return {"ok": True}

    @app.delete("/api/questions/{qid}", tags=["sessions"])
    async def questions_delete(qid: int):
        store.delete_question(qid)
        inter.publish_session()
        return {"ok": True}

    @app.post("/api/questions/{qid}/ask", tags=["sessions"])
    async def questions_ask(qid: int):
        try:
            return await inter.ask(qid)
        except WatchError as e:
            return watch_error(e)

    @app.post("/api/answer", tags=["sessions"])
    async def answer(body: AnswerIn):
        try:
            return await inter.answer(body.yes)
        except WatchError as e:
            return watch_error(e)

    @app.get("/api/sessions/{sid}/samples", tags=["sessions"])
    async def sessions_samples(sid: int, max_points: int = 2000):
        return store.get_samples(sid, max_points)

    @app.get("/api/sessions/{sid}/results.csv", tags=["export"])
    async def export_results(sid: int):
        return Response(store.export_results_csv(sid), media_type="text/csv; charset=utf-8",
                        headers={"Content-Disposition": f'attachment; filename="session{sid}_results.csv"'})

    @app.get("/api/sessions/{sid}/samples.csv", tags=["export"])
    async def export_samples(sid: int):
        return Response(store.export_samples_csv(sid), media_type="text/csv",
                        headers={"Content-Disposition": f'attachment; filename="session{sid}_signals.csv"'})

    @app.get("/api/sessions/{sid}/session.json", tags=["export"])
    async def export_json(sid: int):
        v = inter.session_view(sid)
        if v:
            v["samples"] = store.get_samples(sid, 10 ** 9)
        return JSONResponse(v, headers={"Content-Disposition": f'attachment; filename="session{sid}.json"'})

    @app.get("/report/{sid}", response_class=HTMLResponse, tags=["export"])
    async def report_page(sid: int):
        v = inter.session_view(sid)
        if not v:
            return HTMLResponse("<h1>ไม่พบเซสชัน</h1>", 404)
        return HTMLResponse(report.render(v, store.get_samples(sid, 1500)))

    return app
