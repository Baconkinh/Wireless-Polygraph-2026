"""
config.py — ค่าตั้งของ Polygraph Studio (แก้ได้ด้วย argument ตอนรัน)

ตัวอย่าง:
  python -m backend                      # ต่อนาฬิกาจริง (WiFi "Polygraph-Watch", 192.168.4.1)
  python -m backend --sim                # ต่อนาฬิกาจำลองในเครื่องเดียวกัน (tools/virtual_watch.py)
  python -m backend --lan                # ให้เครื่องอื่นในวง WiFi เปิดดูได้ (เช่นมือถือเป็นจอผู้ชม)
  python -m backend --watch 192.168.4.1 --port 8000 --no-browser
"""
from __future__ import annotations

import argparse
import os
from dataclasses import dataclass

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DATA_DIR = os.path.join(ROOT, "data")
FRONTEND_DIR = os.path.join(ROOT, "frontend")


@dataclass
class Settings:
    watch_host: str = "192.168.4.1"   # IP ของนาฬิกา (SoftAP ของ ESP32 ใช้ค่านี้เสมอ)
    watch_udp_port: int = 4210        # พอร์ต UDP ที่นาฬิการอรับ "hello"
    watch_http_port: int = 80         # REST API ของนาฬิกา
    local_udp_port: int = 4210        # พอร์ตฝั่งคอม (ถ้าไม่ว่างจะสุ่มพอร์ตให้เอง)
    http_host: str = "127.0.0.1"      # ที่อยู่ของ Studio (127.0.0.1 = เปิดได้เฉพาะเครื่องนี้)
    http_port: int = 8000
    db_path: str = os.path.join(DATA_DIR, "studio.db")
    ota_user: str = "admin"           # ต้องตรงกับ OTA_USER / OTA_PASSWORD ใน config.h ของเฟิร์มแวร์
    ota_password: str = "polygraph-ota"
    open_browser: bool = True
    simulator: bool = False


def load(argv=None) -> Settings:
    p = argparse.ArgumentParser(prog="python -m backend", description="Polygraph Studio backend")
    p.add_argument("--watch", help="IP ของนาฬิกา (ค่าเริ่มต้น 192.168.4.1)")
    p.add_argument("--watch-udp", type=int, help="พอร์ต UDP ของนาฬิกา (4210)")
    p.add_argument("--watch-http", type=int, help="พอร์ต HTTP ของนาฬิกา (80)")
    p.add_argument("--sim", action="store_true", help="ต่อนาฬิกาจำลองที่ 127.0.0.1:4211/8081")
    p.add_argument("--port", type=int, help="พอร์ตเว็บของ Studio (8000)")
    p.add_argument("--lan", action="store_true", help="เปิดให้เครื่องอื่นในวง WiFi เข้าดูได้")
    p.add_argument("--no-browser", action="store_true", help="ไม่ต้องเปิดเบราว์เซอร์อัตโนมัติ")
    p.add_argument("--db", help="ไฟล์ฐานข้อมูล SQLite")
    a = p.parse_args(argv)

    s = Settings()
    s.watch_host = os.environ.get("POLY_WATCH_HOST", s.watch_host)
    if a.sim:
        s.simulator = True
        s.watch_host, s.watch_udp_port, s.watch_http_port = "127.0.0.1", 4211, 8081
        s.local_udp_port = 0
    if a.watch:
        s.watch_host = a.watch
    if a.watch_udp:
        s.watch_udp_port = a.watch_udp
    if a.watch_http:
        s.watch_http_port = a.watch_http
    if a.port:
        s.http_port = a.port
    if a.lan:
        s.http_host = "0.0.0.0"
    if a.no_browser:
        s.open_browser = False
    if a.db:
        s.db_path = a.db
    os.makedirs(os.path.dirname(os.path.abspath(s.db_path)), exist_ok=True)
    return s
