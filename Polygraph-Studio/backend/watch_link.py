"""
watch_link.py — การเชื่อมต่อกับนาฬิกา 2 ช่องทาง

  UDP (เร็ว)   : ส่ง "hello" ทุก 2 วินาที -> นาฬิกาส่งค่าสด 5 Hz, คลื่น PPG 10 Hz, เหตุการณ์ กลับมา
  HTTP (ชัวร์) : สั่งงาน (เริ่ม baseline/คำถาม), อ่านผลทั้งหมด, ตั้งค่า, ข้อมูลระบบ, OTA

ทำไมใช้ 2 ทาง: UDP ไม่ต้องรอ ACK จึงหน่วงต่ำเหมาะกับกราฟสด ส่วนคำสั่ง/ผลการตัดสินต้องไม่หาย
จึงใช้ HTTP (TCP) — และทุกครั้งที่นาฬิกาบอกว่ามีผลใหม่ (rv เปลี่ยน) จะดึง /api/lie มาเทียบซ้ำ
"""
from __future__ import annotations

import asyncio
import json
import time
from collections import deque
from typing import Any, Dict, Optional

import httpx


class WatchError(Exception):
    pass


class WatchLink(asyncio.DatagramProtocol):
    def __init__(self, cfg, listener):
        self.cfg = cfg
        self.listener = listener            # Interrogation (sessions.py)
        self.transport: Optional[asyncio.DatagramTransport] = None
        self.local_port = None
        self.connected = False
        self.last_rx = 0.0
        self.device: Dict[str, Any] = {}    # จากแพ็กเก็ต "hi"
        self.info: Dict[str, Any] = {}      # จาก /api/info
        self.live: Dict[str, Any] = {}      # ค่าสดล่าสุด
        self.lie: Dict[str, Any] = {}       # สถานะ LieEngine ล่าสุด (/api/lie)
        self.boot_key = ""                  # "<id>#<boot>" ใช้แยกผลของการบูตแต่ละครั้ง
        self.stats = {"rx_v": 0, "rx_w": 0, "rx_e": 0, "lost_v": 0, "lost_w": 0, "rx_bytes": 0,
                      "rtt_ms": None, "reboots": 0, "http_errors": 0}
        self._seq_v = None
        self._seq_w = None
        self._eids = deque(maxlen=300)
        self._rv = None
        self._lie_task: Optional[asyncio.Task] = None
        self._lie_again = False
        self._pings: Dict[str, float] = {}
        self._ping_n = 0
        self._last_uptime = None
        self._tasks = []
        self.http = httpx.AsyncClient(base_url=f"http://{cfg.watch_host}:{cfg.watch_http_port}",
                                      timeout=httpx.Timeout(4.0, connect=2.5))

    # ------------------------------------------------------------------ lifecycle
    async def start(self):
        loop = asyncio.get_running_loop()
        try:
            self.transport, _ = await loop.create_datagram_endpoint(
                lambda: self, local_addr=("0.0.0.0", self.cfg.local_udp_port))
        except OSError:
            # พอร์ต 4210 ถูกโปรแกรมอื่นใช้อยู่ (เช่น udp_listen.py ตัวเก่า) -> ให้ OS สุ่มพอร์ตให้
            self.transport, _ = await loop.create_datagram_endpoint(lambda: self, local_addr=("0.0.0.0", 0))
        self.local_port = self.transport.get_extra_info("sockname")[1]
        self._tasks = [asyncio.create_task(self._hello_loop()),
                       asyncio.create_task(self._ping_loop()),
                       asyncio.create_task(self._monitor_loop())]

    async def stop(self):
        for t in self._tasks:
            t.cancel()
        if self.transport:
            self.transport.close()
        await self.http.aclose()

    # ------------------------------------------------------------------ UDP
    def _send(self, text: str):
        if self.transport:
            try:
                self.transport.sendto(text.encode(), (self.cfg.watch_host, self.cfg.watch_udp_port))
            except OSError:
                pass   # ยังไม่ได้ต่อ WiFi ของนาฬิกา -> ลองใหม่รอบหน้า

    async def _hello_loop(self):
        while True:
            # t = เวลาจริงจากคอม (นาฬิกาไม่มี RTC) ใช้ประทับเวลาใน log ของนาฬิกา
            self._send(f"hello t={int(time.time())} w=1 id=studio")
            await asyncio.sleep(2.0)

    async def _ping_loop(self):
        while True:
            await asyncio.sleep(5.0)
            if self.connected:
                self._ping_n += 1
                key = str(self._ping_n)
                self._pings[key] = time.perf_counter()
                self._send(f"ping {key}")
                for k in [k for k in self._pings if int(k) < self._ping_n - 5]:
                    self._pings.pop(k, None)

    async def _monitor_loop(self):
        while True:
            await asyncio.sleep(1.0)
            alive = time.time() - self.last_rx < 3.5
            if alive != self.connected:
                self.connected = alive
                if not alive:
                    self._seq_v = self._seq_w = None
            self.listener.on_status(self.status())

    def datagram_received(self, data: bytes, addr):
        # รับเฉพาะจาก IP ของนาฬิกา (กันแพ็กเก็ตแปลกปลอมในวง WiFi)
        if addr[0] != self.cfg.watch_host:
            return
        try:
            msg = json.loads(data.decode("utf-8", "replace"))
        except ValueError:
            return
        self.last_rx = time.time()
        self.stats["rx_bytes"] += len(data)
        if not self.connected:
            self.connected = True
            self.listener.on_status(self.status())
        t = msg.get("t")
        if t == "v":
            self._on_vitals(msg)
        elif t == "w":
            self.stats["rx_w"] += 1
            seq = msg.get("seq")
            if self._seq_w is not None and isinstance(seq, int) and seq > self._seq_w + 1:
                self.stats["lost_w"] += seq - self._seq_w - 1
            self._seq_w = seq
            self.listener.on_wave(msg)
        elif t == "e":
            self.stats["rx_e"] += 1
            key = f"{self.boot_key}/{msg.get('eid')}"
            if key in self._eids:          # นาฬิกาส่งเหตุการณ์ซ้ำ 2 รอบกันหาย -> ตัดตัวซ้ำ
                return
            self._eids.append(key)
            self.listener.on_event(msg)
            if msg.get("ev") in ("result", "baseline_done", "baseline_fail", "session_reset"):
                self.schedule_lie_sync()
        elif t == "hi":
            self._on_hi(msg)
        elif t == "pong":
            t0 = self._pings.pop(str(msg.get("n", "")).strip(), None)
            if t0 is not None:
                self.stats["rtt_ms"] = round((time.perf_counter() - t0) * 1000, 1)

    def _on_vitals(self, v: Dict[str, Any]):
        self.stats["rx_v"] += 1
        seq = v.get("seq")
        if self._seq_v is not None and isinstance(seq, int):
            if seq > self._seq_v + 1:
                self.stats["lost_v"] += seq - self._seq_v - 1
        self._seq_v = seq
        # uptime ลดลง = นาฬิการีบูตไปแล้ว (เผื่อแพ็กเก็ต "hi" มาช้า)
        up = v.get("ms")
        if isinstance(up, int) and self._last_uptime is not None and up + 3000 < self._last_uptime:
            self._seq_v = None
        self._last_uptime = up
        self.live = v
        self.listener.on_vitals(v)
        if v.get("rv") != self._rv:
            self._rv = v.get("rv")
            self.schedule_lie_sync()

    def _on_hi(self, hi: Dict[str, Any]):
        new_key = f"{hi.get('id')}#{hi.get('boot')}"
        rebooted = bool(self.boot_key) and new_key != self.boot_key
        self.device = hi
        self.boot_key = new_key
        self._seq_v = self._seq_w = None
        if rebooted:
            self.stats["reboots"] += 1
        asyncio.get_running_loop().create_task(self._after_hi(rebooted))

    async def _after_hi(self, rebooted: bool):
        try:
            self.info = await self.get("/api/info")
        except WatchError:
            pass
        self.listener.on_device(self.device, self.info, rebooted)
        self.schedule_lie_sync()

    # ------------------------------------------------------------------ lie sync
    def schedule_lie_sync(self):
        if self._lie_task and not self._lie_task.done():
            self._lie_again = True
            return
        self._lie_task = asyncio.get_running_loop().create_task(self._lie_sync())

    async def _lie_sync(self):
        while True:
            self._lie_again = False
            try:
                self.lie = await self.get("/api/lie", n=24)
                await self.listener.on_lie(self.lie)
            except WatchError:
                pass
            if not self._lie_again:
                break

    # ------------------------------------------------------------------ HTTP
    async def get(self, path: str, **params) -> Dict[str, Any]:
        try:
            r = await self.http.get(path, params=params or None)
        except httpx.HTTPError as e:
            self.stats["http_errors"] += 1
            raise WatchError(f"นาฬิกาไม่ตอบ ({type(e).__name__}) — ต่อ WiFi \"Polygraph-Watch\" อยู่หรือไม่?") from e
        return self._parse(r)

    async def post(self, path: str, **params) -> Dict[str, Any]:
        try:
            r = await self.http.post(path, params={k: v for k, v in params.items() if v is not None})
        except httpx.HTTPError as e:
            self.stats["http_errors"] += 1
            raise WatchError(f"นาฬิกาไม่ตอบ ({type(e).__name__}) — ต่อ WiFi \"Polygraph-Watch\" อยู่หรือไม่?") from e
        return self._parse(r)

    async def get_text(self, path: str, **params) -> str:
        try:
            r = await self.http.get(path, params=params or None)
        except httpx.HTTPError as e:
            raise WatchError(f"นาฬิกาไม่ตอบ ({type(e).__name__})") from e
        if r.status_code != 200:
            raise WatchError(f"HTTP {r.status_code}: {r.text[:200]}")
        return r.text

    @staticmethod
    def _parse(r: httpx.Response) -> Dict[str, Any]:
        try:
            data = r.json()
        except ValueError:
            raise WatchError(f"นาฬิกาตอบกลับไม่ใช่ JSON (HTTP {r.status_code})")
        if isinstance(data, dict) and r.status_code >= 400 and "ok" not in data:
            data["ok"] = False
        return data

    async def upload_firmware(self, filename: str, content: bytes) -> Dict[str, Any]:
        files = {"firmware": (filename, content, "application/octet-stream")}
        try:
            r = await self.http.post("/update", files=files, auth=(self.cfg.ota_user, self.cfg.ota_password),
                                     timeout=httpx.Timeout(120.0, connect=5.0))
        except httpx.HTTPError as e:
            raise WatchError(f"อัปโหลดไม่สำเร็จ: {type(e).__name__}") from e
        if r.status_code == 401:
            return {"ok": False, "error": "AUTH", "msg": "รหัส OTA ไม่ถูกต้อง"}
        return self._parse(r)

    # ------------------------------------------------------------------ status
    def status(self) -> Dict[str, Any]:
        rx = self.stats["rx_v"] + self.stats["lost_v"]
        return {
            "connected": self.connected,
            "watch": f"{self.cfg.watch_host}:{self.cfg.watch_udp_port}",
            "http": f"http://{self.cfg.watch_host}:{self.cfg.watch_http_port}",
            "simulator": self.cfg.simulator or str(self.device.get("id", "")).startswith("SIM"),
            "localPort": self.local_port,
            "lastRxAgo": round(time.time() - self.last_rx, 1) if self.last_rx else None,
            "device": self.device,
            "stats": dict(self.stats, loss_pct=round(100.0 * self.stats["lost_v"] / rx, 2) if rx else 0.0),
        }
