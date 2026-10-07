"""
hub.py — กระจายข้อมูลสดไปยังเบราว์เซอร์ทุกหน้าที่เปิดอยู่ผ่าน WebSocket

แต่ละ client มีคิวของตัวเอง: client ช้า (เช่นแท็บที่ถูกพับ) จะไม่ทำให้ client อื่นช้าตาม
ถ้าคิวเต็ม จะทิ้งข้อความเก่าสุด (ข้อมูลสดเก่า ๆ ไม่มีประโยชน์)
"""
from __future__ import annotations

import asyncio
import json
from typing import Any, Dict, Set

from fastapi import WebSocket


def clean(x: Any) -> Any:
    """แทน NaN/Infinity ด้วย None ทุกชั้นของ dict/list"""
    if isinstance(x, float):
        return None if (x != x or x in (float("inf"), float("-inf"))) else x
    if isinstance(x, dict):
        return {k: clean(v) for k, v in x.items()}
    if isinstance(x, (list, tuple)):
        return [clean(v) for v in x]
    return x


class _Client:
    def __init__(self, ws: WebSocket):
        self.ws = ws
        self.queue: asyncio.Queue = asyncio.Queue(maxsize=300)
        self.task: asyncio.Task | None = None


class Hub:
    def __init__(self):
        self.clients: Set[_Client] = set()

    async def serve(self, ws: WebSocket, hello: Dict[str, Any]):
        """จัดการ client หนึ่งตัวจนกว่าจะปิดหน้าเว็บ"""
        await ws.accept()
        c = _Client(ws)
        self.clients.add(c)
        c.task = asyncio.create_task(self._sender(c))
        try:
            await ws.send_text(json.dumps({"type": "hello", "data": hello}, ensure_ascii=False))
            while True:                      # รอจนปิด (เบราว์เซอร์ส่ง ping เป็นระยะ)
                await ws.receive_text()
        except Exception:
            pass
        finally:
            self.clients.discard(c)
            if c.task:
                c.task.cancel()

    async def _sender(self, c: _Client):
        try:
            while True:
                msg = await c.queue.get()
                await c.ws.send_text(msg)
        except Exception:
            pass

    def publish(self, typ: str, data: Any):
        if not self.clients:
            return
        # JSON มาตรฐานไม่มี NaN (JSON.parse ของเบราว์เซอร์จะ error) -> แปลงเป็น null ก่อน
        msg = json.dumps({"type": typ, "data": clean(data)}, ensure_ascii=False, default=str)
        for c in list(self.clients):
            try:
                c.queue.put_nowait(msg)
            except asyncio.QueueFull:
                try:
                    c.queue.get_nowait()          # ทิ้งเก่าสุด
                    c.queue.put_nowait(msg)
                except Exception:
                    pass

    @property
    def count(self) -> int:
        return len(self.clients)
