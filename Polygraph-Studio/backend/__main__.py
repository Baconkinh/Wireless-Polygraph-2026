"""
รัน Polygraph Studio:   python -m backend   (ดูตัวเลือกทั้งหมด: python -m backend --help)
"""
import socket
import sys
import threading
import webbrowser

import uvicorn

from . import config
from .main import create_app


def lan_ips():
    ips = set()
    try:
        for info in socket.getaddrinfo(socket.gethostname(), None, socket.AF_INET):
            ips.add(info[4][0])
    except OSError:
        pass
    return sorted(ip for ip in ips if not ip.startswith("127."))


def port_free(host: str, port: int) -> bool:
    """เช็คก่อนเปิด: uvicorn เจอพอร์ตไม่ว่างจะพิมพ์ error ภาษาอังกฤษแล้วออกเงียบ ๆ (ผู้ใช้งง)"""
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as t:
        try:
            t.bind((host if host != "0.0.0.0" else "", port))
            return True
        except OSError:
            return False


def main():
    s = config.load()
    if not port_free(s.http_host, s.http_port):
        print("=" * 62)
        print(f" เปิด Polygraph Studio ไม่ได้: พอร์ต {s.http_port} มีโปรแกรมอื่นใช้อยู่")
        print(" ส่วนใหญ่คือ Studio ตัวเก่า (เช่นจาก run_demo_simulator.bat) ยังเปิดค้าง")
        print(" วิธีแก้: ปิดหน้าต่างดำของ Studio/Virtual Watch ตัวเก่าทั้งหมด แล้วรันใหม่")
        print(f"   หรือใช้พอร์ตอื่น: run_studio.bat --port {s.http_port + 1}")
        print("=" * 62)
        sys.exit(1)
    app = create_app(s)
    url = f"http://127.0.0.1:{s.http_port}"
    print("=" * 62)
    print(" Polygraph Studio")
    print(f"  เปิดหน้าเว็บ   : {url}")
    print(f"  เอกสาร API    : {url}/docs")
    print(f"  นาฬิกา        : {s.watch_host} (UDP {s.watch_udp_port}, HTTP {s.watch_http_port})"
          + ("  [นาฬิกาจำลอง]" if s.simulator else ""))
    if s.http_host == "0.0.0.0":
        for ip in lan_ips():
            print(f"  เครื่องอื่นเปิด : http://{ip}:{s.http_port}")
    print("  หยุดโปรแกรม   : กด Ctrl + C")
    print("=" * 62)
    if s.open_browser:
        threading.Timer(1.5, lambda: webbrowser.open(url)).start()
    try:
        uvicorn.run(app, host=s.http_host, port=s.http_port, log_level="warning")
    except OSError as e:
        print(f"เปิดพอร์ต {s.http_port} ไม่ได้ ({e}) — มี Studio เปิดอยู่แล้วหรือเปล่า? ลอง --port 8001")
        sys.exit(1)


if __name__ == "__main__":
    main()
