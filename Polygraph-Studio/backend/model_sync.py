"""
model_sync.py — ส่งโมเดล AI ตัวล่าสุด (data/model.json) เข้านาฬิกาให้อัตโนมัติ

ทำไมต้องมี
  เดิมต้องเทรนแล้ว "จำ" ไปกดอัปโหลด model.json เองที่หน้าเว็บนาฬิกา หรือรัน train_ai_upload.bat
  ถ้าลืม นาฬิกาจะยังตัดสินด้วยสูตรเดิม/โมเดลเก่าโดยไม่รู้ตัว

ทำงานยังไง (วนทุก 4 วินาที ตอนเชื่อมนาฬิกาอยู่)
  1) อ่านข้อมูลสรุปของ data/model.json (ชื่อ + เวลาเทรน) — เปลี่ยนเมื่อกดเทรนใหม่ (จาก Studio หรือ train_ai.bat)
  2) ถามนาฬิกา GET /api/ml ว่าตอนนี้มีโมเดลชื่ออะไร เทรนเมื่อไร
  3) ถ้าเปิด "อัตโนมัติ" และสองอย่างไม่ตรงกัน -> ส่ง POST /api/ml/model (นาฬิกาตรวจ CRC32 แล้วเก็บลง NVS)
     ส่งไม่สำเร็จ จะรอ 30 วินาทีก่อนลองตัวเดิมใหม่ (ไม่ส่งรัว ๆ)
  ปิดอัตโนมัติได้ในหน้า "ข้อมูล & เทรน AI" แล้วเลือกส่งเองทีละไฟล์ (รวมถึงโมเดลเก่าใน data/models/)
  การตั้งค่าเก็บใน data/studio_settings.json
"""
from __future__ import annotations

import asyncio
import json
import os
import sys
import time
from typing import Any, Dict, Optional

from . import config as cfgmod
from .watch_link import WatchError

sys.path.insert(0, os.path.join(cfgmod.ROOT, "ml"))
import datafiles as df  # noqa: E402
import polyml as pm  # noqa: E402

SETTINGS_FILE = os.path.join(cfgmod.DATA_DIR, "studio_settings.json")
MODEL_PATH = os.path.join(cfgmod.DATA_DIR, "model.json")
RETRY_SEC = 30.0


def load_settings() -> Dict[str, Any]:
    """อ่าน data/studio_settings.json (ไม่มี/อ่านไม่ได้ = ค่าเริ่มต้นทั้งหมด)"""
    try:
        with open(SETTINGS_FILE, encoding="utf-8") as fh:
            d = json.load(fh)
        return d if isinstance(d, dict) else {}
    except (OSError, ValueError):
        return {}


def save_settings(d: Dict[str, Any]):
    """บันทึกค่าตั้งของ Studio (ตอนนี้มี auto_model_sync) ให้จำได้หลังปิดเปิดโปรแกรม"""
    os.makedirs(os.path.dirname(SETTINGS_FILE), exist_ok=True)
    with open(SETTINGS_FILE, "w", encoding="utf-8") as fh:
        json.dump(d, fh, ensure_ascii=False, indent=2)


def model_form(mj: Dict[str, Any]) -> Dict[str, str]:
    """แปลง model.json เป็นฟอร์มที่นาฬิการับ (/api/ml/model) — รูปแบบเดียวกับ polyml.upload_model"""
    g = lambda v: "%.7g" % v   # noqa: E731  ความละเอียดพอสำหรับ float 32 บิตในนาฬิกา
    return {"features": ",".join(mj["features"]), "mean": ",".join(g(v) for v in mj["mean"]),
            "scale": ",".join(g(v) for v in mj["scale"]), "w": ",".join(g(v) for v in mj["w"]),
            "b": g(mj["b"]), "acc": g(mj.get("cv_accuracy") or 0.0), "samples": str(int(mj.get("samples") or 0)),
            "trained": str(int(mj.get("trained_at") or 0)), "name": str(mj.get("name", "model"))[:pm.FW_NAME_MAX]}


def same_model(local: Optional[Dict[str, Any]], watch: Optional[Dict[str, Any]]) -> bool:
    """โมเดลในคอมกับในนาฬิกาเป็นตัวเดียวกันไหม (เทียบชื่อ + เวลาเทรน ซึ่งนาฬิกาเก็บไว้ใน NVS)"""
    if not local or not watch or not watch.get("loaded"):
        return False
    return (str(watch.get("name", "")) == str(local.get("name", ""))[:pm.FW_NAME_MAX]
            and int(watch.get("trainedAt") or 0) == int(local.get("trained_at") or 0))


class ModelSync:
    """ตัวคอยดูแลให้โมเดลในนาฬิกาตรงกับ data/model.json (รายละเอียดที่หัวไฟล์)"""
    def __init__(self, link, hub):
        """อ่านค่าตั้ง (ส่งอัตโนมัติ: เปิดเป็นค่าเริ่มต้น) — ยังไม่เริ่มวน จนกว่าจะเรียก start()"""
        self.link = link
        self.hub = hub
        st = load_settings()
        self.auto: bool = bool(st.get("auto_model_sync", True))     # ค่าเริ่มต้น: ส่งให้อัตโนมัติ
        self.local: Optional[Dict[str, Any]] = None    # สรุป data/model.json
        self.watch: Optional[Dict[str, Any]] = None    # ผลจาก GET /api/ml (model, mode, data)
        self.msg = ""
        self.busy = False
        self._mtime = None
        self._last_try: Dict[str, float] = {}          # ชื่อโมเดล -> เวลาที่ลองส่งล่าสุด
        self._task: Optional[asyncio.Task] = None
        self._last_pub = ""

    # ------------------------------------------------------------------ สถานะ
    def state(self) -> Dict[str, Any]:
        wm = (self.watch or {}).get("model") if self.watch else None
        return {"auto": self.auto, "local": self.local, "watch": wm,
                "watch_mode": (self.watch or {}).get("mode"), "watch_data": (self.watch or {}).get("data"),
                "in_sync": same_model(self.local, wm), "connected": bool(self.link.connected),
                "msg": self.msg, "busy": self.busy}

    def publish(self, force: bool = False):
        """ส่งสถานะไปหน้าเว็บ (ข้อความชนิด "ai") เฉพาะตอนมีอะไรเปลี่ยน — ไม่ส่งซ้ำทุก 4 วินาที"""
        s = self.state()
        key = json.dumps(s, sort_keys=True, ensure_ascii=False)
        if force or key != self._last_pub:
            self._last_pub = key
            self.hub.publish("ai", s)

    # ------------------------------------------------------------------ วนตรวจ
    def start(self):
        self._task = asyncio.get_running_loop().create_task(self._loop())

    async def stop(self):
        """หยุดวนตรวจ (ตอนปิดโปรแกรม)"""
        if self._task:
            self._task.cancel()

    def _read_local(self):
        """อ่านข้อมูลสรุปของ data/model.json ใหม่เมื่อไฟล์เปลี่ยน (ดูจากเวลาแก้ไขไฟล์)"""
        try:
            mt = os.path.getmtime(MODEL_PATH)
        except OSError:
            self.local, self._mtime = None, None
            return
        if mt != self._mtime:                 # อ่านใหม่เฉพาะตอนไฟล์เปลี่ยน
            self._mtime = mt
            self.local = df.model_info(MODEL_PATH)

    async def refresh_watch(self):
        """ถามนาฬิกาว่าตอนนี้มีโมเดลอะไร (GET /api/ml) — ไม่ได้เชื่อมต่อ = None"""
        if not self.link.connected:
            self.watch = None
            return
        try:
            self.watch = await self.link.get("/api/ml")
        except WatchError:
            self.watch = None

    async def _loop(self):
        """วนทุก 4 วินาทีตลอดอายุโปรแกรม (error ใด ๆ ไม่ทำให้หยุดวน)"""
        while True:
            try:
                await self.tick()
            except asyncio.CancelledError:
                raise
            except Exception as e:  # noqa: BLE001 - วนต่อเสมอ ไม่ให้ทั้ง Studio ล้มเพราะส่วนนี้
                self.msg = f"ตรวจโมเดลไม่สำเร็จ: {e}"
            self.publish()
            await asyncio.sleep(4.0)

    async def tick(self):
        """ตรวจ 1 รอบ: ถ้าเปิดอัตโนมัติ + มี model.json + นาฬิกาต่ออยู่ + โมเดลไม่ตรงกัน -> ส่ง"""
        self._read_local()
        await self.refresh_watch()
        if not (self.auto and self.local and self.watch and self.watch.get("ok", True)):
            return
        wm = self.watch.get("model") or {}
        if same_model(self.local, wm):
            return
        name = self.local["name"]
        if time.time() - self._last_try.get(name, 0) < RETRY_SEC:
            return
        self._last_try[name] = time.time()
        await self.upload_file(MODEL_PATH, reason="อัตโนมัติ")

    # ------------------------------------------------------------------ ส่ง/ลบ
    async def upload_file(self, path: str, reason: str = "ส่งเอง") -> Dict[str, Any]:
        """ส่งไฟล์โมเดล (model.json หรือไฟล์ใน data/models/) เข้านาฬิกา"""
        try:
            with open(path, encoding="utf-8") as fh:
                mj = json.load(fh)
            pm.Model.from_json(mj)               # ตรวจรูปแบบก่อนส่ง
        except (OSError, ValueError, KeyError, TypeError) as e:
            self.msg = f"ไฟล์โมเดลใช้ไม่ได้: {e}"
            return {"ok": False, "msg": self.msg}
        self.busy = True
        self.publish()
        try:
            res = await self.link.post_form("/api/ml/model", model_form(mj))
        except WatchError as e:
            res = {"ok": False, "msg": str(e)}
        finally:
            self.busy = False
        acc = mj.get("cv_accuracy")
        if res.get("ok"):
            self.msg = (f"{reason}: ส่งโมเดล {mj.get('name')} เข้านาฬิกาแล้ว"
                        + (f" (ความแม่นยำตอนเทรน {acc * 100:.0f}%)" if isinstance(acc, (int, float)) else ""))
        else:
            self.msg = f"{reason}: ส่งโมเดลไม่สำเร็จ — {res.get('msg', res)}"
        await self.refresh_watch()
        self.publish(force=True)
        return {"ok": bool(res.get("ok")), "msg": self.msg}

    async def clear_watch(self) -> Dict[str, Any]:
        """ลบโมเดลในนาฬิกา -> นาฬิกากลับไปใช้สูตรมาตรฐาน"""
        try:
            res = await self.link.post("/api/ml/model/clear")
        except WatchError as e:
            res = {"ok": False, "msg": str(e)}
        self.msg = res.get("msg", "")
        await self.refresh_watch()
        self.publish(force=True)
        return res

    def set_auto(self, on: bool):
        """เปิด/ปิดส่งอัตโนมัติ แล้วบันทึกค่าลงไฟล์"""
        self.auto = bool(on)
        st = load_settings()
        st["auto_model_sync"] = self.auto
        save_settings(st)
        self._last_try.clear()
        self.msg = ("เปิดส่งอัตโนมัติ: นาฬิกาจะใช้ data/model.json ตัวล่าสุดเสมอ" if self.auto
                    else "ปิดส่งอัตโนมัติ: เลือกส่งโมเดลเองได้ด้านล่าง")
        self.publish(force=True)
