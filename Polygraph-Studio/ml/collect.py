"""
collect.py — โปรแกรมเก็บข้อมูลเทรน AI แบบทีละขั้น (ดับเบิลคลิก collect_data.bat)

ลำดับการทำงาน (โปรแกรมพาทำเองทีละขั้น)
  1. ถามชื่อผู้ถูกทดสอบ / ผู้ถาม / โหมดการให้เฉลย (fix หรือ manual)
  2. วัดค่าปกติ (baseline) ~30 วินาที — ทำครั้งแรกครั้งเดียว (วัดใหม่ได้ด้วยปุ่ม b)
  3. วนถามทีละข้อ: โปรแกรมบอกเสมอว่า "ข้อถัดไปคือข้อที่เท่าไร (qid อะไร) จะบันทึกลงไฟล์ไหน"
       โหมด fix    : ก่อนถามกด t = ข้อนี้ให้ตอบจริง หรือ l = ข้อนี้ให้โกหก -> เริ่มบันทึกทันที
       โหมด manual : กด Enter เริ่มถาม -> ผู้ตอบตอบตามใจ -> หมดเวลาแล้วกด t (พูดจริง) / l (โกหก)
     ทุกข้อบันทึก 12 วินาทีเท่ากัน (เส้นนับถอยหลังวิ่งให้ดู) เพราะ AI ต้องเทียบข้อมูลช่วงเวลาเท่ากันทุกข้อ
  4. กด x เพื่อจบ — ถ้ายังไม่ได้ถามสักข้อ จะไม่มีไฟล์ใหม่เกิดขึ้น (ไม่เปลืองพื้นที่)

ปุ่มที่กดได้ตลอด (Windows ไม่ต้องกด Enter):
  a ยกเลิกข้อนี้   b วัด baseline ใหม่   m สลับโหมด fix <-> manual   s สถานะ
  d ดาวน์โหลดไฟล์ข้อมูลเทรนจากนาฬิกา   h ช่วยเหลือ   x ออก

ไฟล์ที่ได้อยู่ในโฟลเดอร์ data/ (ความหมายทุกคอลัมน์: DATA_DICTIONARY.md)
  signals_<วันเวลา>.csv   ค่าเซนเซอร์ 5 ครั้ง/วินาที — ทุกแถวมี question_no + label บอกว่ามาจากข้อไหน เฉลยอะไร
  results_<วันเวลา>.csv   1 แถว/ข้อ: เฉลย + คำตัดสินของนาฬิกา + ตัวเลขที่ใช้ตัดสิน
  training_samples.csv    ข้อที่ใช้เทรนได้ (ต่อท้ายไฟล์เดิม) -> train_ai.bat ใช้ไฟล์นี้

รัน:  python ml/collect.py                 นาฬิกาจริง (ต่อ WiFi Polygraph-Watch ก่อน)
      python ml/collect.py --sim           นาฬิกาจำลอง (tools/virtual_watch.py)
ใช้แต่ไลบรารีมาตรฐานของ Python
"""
from __future__ import annotations

import argparse
import json
import os
import queue
import socket
import sys
import threading
import time
import urllib.parse

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
import polyml as pm  # noqa: E402
from recorder import RunRecorder  # noqa: E402

DATA = os.path.join(ROOT, "data")
STATE_TH = {0: "ยังไม่วัดค่าปกติ", 1: "กำลังวัดค่าปกติ", 2: "พร้อมถาม", 3: "กำลังบันทึกข้อนี้"}
VERDICT_TH = {"truth": "จริง", "lie": "โกหก", "inconclusive": "ไม่แน่ชัด", "invalid": "ใช้ไม่ได้"}
LABEL_TH = {"truth": "ตอบจริง", "lie": "โกหก"}
SRC_TH = {0: "สูตรมาตรฐาน", 1: "สูตรปรับจากข้อควบคุม", 2: "โมเดล AI"}
HELP = """
ปุ่มที่ใช้ได้
  (fix)    t = ข้อนี้ให้ตอบจริง    l = ข้อนี้ให้โกหก        -> เริ่มบันทึกข้อทันที
  (manual) Enter = เริ่มถาม  แล้วหลังหมดเวลา  t = ผู้ตอบพูดจริง  l = ผู้ตอบโกหก  k = ไม่รู้/ข้าม
  a = ยกเลิกข้อนี้       b = วัด baseline ใหม่      m = สลับโหมด fix <-> manual
  s = สถานะ             d = ดาวน์โหลดไฟล์ข้อมูลเทรนจากนาฬิกา      h = ช่วยเหลือ     x = ออก
ใครตัดสินว่าจริงหรือโกหก?  นาฬิกาเป็นคนตัดสิน (LieEngine หรือโมเดล AI ถ้าติดตั้งแล้ว) จากชีพจร แรงชีพจร
  GSR การสั่น และอุณหภูมิผิว ใน 12 วินาทีหลังถาม เทียบกับ baseline — ส่วนเฉลย (t/l) เป็น "คำตอบที่ถูก"
  ที่เราบอกให้ AI เรียนรู้ ไม่ได้ไปเปลี่ยนคำตัดสินของนาฬิกา
"""


def key_reader(q: "queue.Queue[str]"):
    """อ่านปุ่มทีละตัว: Windows ใช้ msvcrt (ไม่ต้องกด Enter), ระบบอื่นอ่านทีละบรรทัด"""
    try:
        import msvcrt
        while True:
            ch = msvcrt.getwch()
            if ch in ("\x00", "\xe0"):        # ปุ่มพิเศษ (ลูกศร/F1) มี 2 ไบต์ -> ทิ้ง
                msvcrt.getwch()
                continue
            q.put("enter" if ch in ("\r", "\n") else ch.lower())
    except ImportError:
        for line in sys.stdin:
            line = line.strip().lower()
            q.put(line[0] if line else "enter")


def ask_line(prompt: str, default: str = "") -> str:
    """ถามข้อความ 1 บรรทัดตอนเริ่มโปรแกรม (ก่อนเปิดตัวอ่านปุ่ม)"""
    try:
        s = input(prompt).strip()
    except EOFError:
        s = ""
    return s or default


class Collector:
    def __init__(self, a):
        self.a = a
        self.http = a.http
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.bind(("0.0.0.0", 0))               # พอร์ตสุ่ม: เปิดพร้อม Studio ได้ ไม่ชนพอร์ต 4210
        self.sock.settimeout(0.2)
        self.watch = (a.host, a.udp)
        self.seen_eids: list = []
        self.last_rx = 0.0
        self.last_hello = 0.0
        self.live: dict = {}
        self.window_sec = 12.0
        self.rec = RunRecorder(DATA, a.subject, a.operator, a.mode, log=lambda s: print("   " + s))
        self.progress_shown = False

    # ------------------------------------------------------------ UDP (ค่าสด + เหตุการณ์)
    def hello(self):
        # มีคำว่า w=0 / t= ต่อท้าย -> นาฬิกาส่ง JSON (ถ้าส่ง "hello" เฉย ๆ จะได้ CSV แบบเก่าของเพื่อน)
        try:
            self.sock.sendto(f"hello w=0 t={int(time.time())}".encode(), self.watch)
        except OSError as e:
            print(f"ส่ง hello ไม่ได้: {e}")
        self.last_hello = time.time()

    def handle(self, data: bytes):
        try:
            m = json.loads(data.decode("utf-8", "replace"))
        except ValueError:
            return
        self.last_rx = time.time()
        t = m.get("t")
        if t == "v":
            self.live = m
            self.rec.on_vitals(m)
            self.show_progress()
        elif t == "hi":
            print(f"เชื่อมต่อนาฬิกาแล้ว: {m.get('id')}  FW {m.get('fw')}")
        elif t == "e":
            eid = m.get("eid")
            if eid in self.seen_eids:               # นาฬิกาส่ง event ซ้ำ 2 ครั้งกันหล่น
                return
            self.seen_eids = (self.seen_eids + [eid])[-64:]
            self.event(m)

    def show_progress(self):
        """เส้นนับถอยหลังระหว่างบันทึกข้อ (เขียนทับบรรทัดเดิมด้วย \\r)"""
        cur = self.rec.current
        if not cur or cur["status"] != "asking" or self.live.get("es") != 3:
            return
        p = max(0.0, min(1.0, float(self.live.get("ep") or 0)))
        left = (1 - p) * self.window_sec
        bar = "#" * int(p * 30) + "-" * (30 - int(p * 30))
        print(f"\r   [{bar}] เหลือ {left:4.1f} วินาที  (ข้อที่ {cur['question_no']})   ", end="", flush=True)
        self.progress_shown = True

    def end_progress(self):
        if self.progress_shown:
            print()
            self.progress_shown = False

    def event(self, m: dict):
        ev = m.get("ev")
        if ev == "result":
            self.end_progress()
            r = {"qid": m.get("qid"), "verdict": m.get("verdict"), "p": m.get("p"), "src": m.get("src"),
                 "quality": m.get("q"), "reasons": m.get("rs"), "ok": m.get("ok"), "feat": m.get("f"), "z": m.get("z")}
            before = self.rec.current
            if not before or before["watch_qid"] != r["qid"] or before["status"] != "asking":
                return                                  # ไม่ใช่ข้อของรอบนี้ (เช่นถามจากหน้าเว็บ)
            p = float(m.get("p") or 0)
            print(f"   ผลข้อที่ {before['question_no']}: นาฬิกาทายว่า {VERDICT_TH.get(m.get('verdict'), m.get('verdict'))} "
                  f"(โอกาสโกหก {p * 100:.0f}%, ตัดสินด้วย{SRC_TH.get(m.get('src'), '?')}, คุณภาพสัญญาณ {m.get('q')}%)")
            cur = self.rec.on_result(r)
            if cur is None:
                return
            if cur["status"] == "waiting_label":
                print("   >>> เฉลยข้อนี้: ผู้ตอบพูดจริงหรือโกหก?  กด t = พูดจริง   l = โกหก   k = ไม่รู้/ข้าม")
            else:
                self.prompt_next()
        elif ev == "baseline_done":
            print("\nวัดค่าปกติเสร็จแล้ว")
            self.prompt_next()
        elif ev == "baseline_fail":
            print(f"\nวัดค่าปกติไม่สำเร็จ ({m.get('error')}) — ใส่นาฬิกาให้แนบผิวแล้วกด b ใหม่")

    # ------------------------------------------------------------ ข้อความนำทาง
    def prompt_next(self):
        n = self.rec.next_question()
        f = os.path.basename(self.rec.paths["signals"])
        made = "" if self.rec.files_created else " (จะสร้างไฟล์เมื่อเริ่มข้อแรก)"
        print("-" * 70)
        print(f" ข้อถัดไป: ข้อที่ {n['question_no']}  (qid {n['watch_qid']})   โหมด {self.rec.mode}"
              f"   ผู้ถาม {self.rec.operator} -> ผู้ตอบ {self.rec.subject}")
        print(f" บันทึกลง: data/{f}{made}")
        if self.rec.mode == "fix":
            print(" บอกผู้ตอบก่อนว่าข้อนี้ให้ตอบจริงหรือโกหก แล้วกด  t = ตอบจริง   l = โกหก   (h = ช่วยเหลือ)")
        else:
            print(" กด Enter แล้วถามได้เลย ผู้ตอบเลือกเองว่าจะจริงหรือโกหก หมดเวลาแล้วค่อยบอกเฉลย  (h = ช่วยเหลือ)")

    # ------------------------------------------------------------ คำสั่ง
    def start_question(self, label=None):
        if self.live.get("es") != 2:
            print(f"   ยังถามไม่ได้: นาฬิกา{STATE_TH.get(self.live.get('es'), 'ยังไม่ส่งข้อมูล')} "
                  + ("— กด b เพื่อวัดค่าปกติก่อน" if self.live.get("es") in (0, None) else ""))
            return
        n = self.rec.next_question()
        kind = label if self.rec.mode == "fix" else "test"     # fix: ให้นาฬิการู้เฉลยด้วย (ใช้ปรับเกณฑ์ได้)
        r = pm.http_post(self.http, f"/api/lie/question?qid={n['watch_qid']}&kind={kind}")
        if not r.get("ok"):
            print("   เริ่มข้อไม่ได้:", r.get("msg", r))
            return
        self.rec.start_question(self.rec.mode, label)
        print("   ถามได้เลย! กำลังบันทึก 12 วินาที (กด a เพื่อยกเลิกข้อนี้)")

    def cmd(self, k: str):
        cur = self.rec.current
        waiting = cur is not None and cur["status"] == "waiting_label"
        if waiting and k in ("t", "l", "k"):
            if k == "k":
                self.rec._finish_unlabeled()
                print("   ข้ามข้อนี้ (ไม่รู้เฉลย -> ไม่นำไปเทรน)")
            else:
                self.rec.set_label("truth" if k == "t" else "lie")
            self.prompt_next()
        elif k in ("t", "l") and self.rec.mode == "fix" and not cur:
            self.start_question("truth" if k == "t" else "lie")
        elif k == "enter" and self.rec.mode == "manual" and not cur:
            self.start_question(None)
        elif k == "a":
            pm.http_post(self.http, "/api/lie/abort")
            self.end_progress()
            self.rec.cancel_question("ผู้ใช้กดยกเลิก")
            self.prompt_next()
        elif k == "b":
            r = pm.http_post(self.http, "/api/lie/baseline")
            print("เริ่มวัดค่าปกติ — นั่งนิ่ง ๆ หายใจปกติ ~30 วินาที" if r.get("ok") else f"วัดไม่ได้: {r.get('msg', r)}")
        elif k == "m":
            if cur:
                print("   เปลี่ยนโหมดระหว่างข้อไม่ได้ — จบข้อนี้ก่อน")
            else:
                self.rec.mode = "manual" if self.rec.mode == "fix" else "fix"
                print(f"   เปลี่ยนเป็นโหมด {self.rec.mode}")
                self.prompt_next()
        elif k == "s":
            self.status()
        elif k == "d":
            out = os.path.join(DATA, time.strftime("watch_train_%Y%m%d_%H%M%S.csv"))
            try:
                n = pm.download_csv(self.http, out)
                print(f"ดาวน์โหลดแล้ว: data/{os.path.basename(out)} ({n} ไบต์)")
            except Exception as e:  # noqa: BLE001 - แสดงสาเหตุให้ผู้ใช้
                print("ดาวน์โหลดไม่ได้:", e)
        elif k == "h":
            print(HELP)
        elif k in ("x", "\x1b", "\x03"):
            raise KeyboardInterrupt

    def status(self):
        v = self.live
        c = self.rec.counts
        hr = f"{v['hr']:.0f}" if v.get("hr") is not None and v.get("con") else "--"
        gsr = f"{v['gsr']:.2f}" if v.get("gsr") is not None and v.get("gc") else "ไม่มี"
        print(f"[{time.strftime('%H:%M:%S')}] ชีพจร {hr} | GSR {gsr} | ผิว {v.get('tmp')} C | "
              f"นาฬิกา: {STATE_TH.get(v.get('es'), '?')} | เก็บแล้ว: จริง {c['truth']} / โกหก {c['lie']} "
              f"/ ใช้ไม่ได้ {c['invalid']} / ยกเลิก {c['aborted']}")

    # ------------------------------------------------------------ วนรอบหลัก
    def run(self):
        q: "queue.Queue[str]" = queue.Queue()
        pm.http_post(self.http, "/api/ml/subject?name=" + urllib.parse.quote(self.rec.subject), timeout=3)
        pm.http_post(self.http, f"/api/time?epoch={int(time.time())}", timeout=3)
        cfg = pm.http_json(self.http, "/api/lie?n=0", timeout=3)
        self.window_sec = float(((cfg.get("config") or {}).get("windowSec")) or 12.0)
        threading.Thread(target=key_reader, args=(q,), daemon=True).start()
        print("กำลังต่อนาฬิกา...")
        t_wait = time.time()
        while time.time() - t_wait < 6 and not self.live:
            if time.time() - self.last_hello > 1.0:
                self.hello()
            try:
                self.handle(self.sock.recvfrom(4096)[0])
            except (socket.timeout, OSError):
                pass
        if not self.live:
            print("ยังไม่ได้รับข้อมูลจากนาฬิกา — ต่อ WiFi \"Polygraph-Watch\" แล้วหรือยัง? (โปรแกรมจะลองต่อไปเรื่อย ๆ)")
        if self.live.get("es") == 2:
            print("นาฬิกามีค่าปกติ (baseline) อยู่แล้ว — กด b ถ้าต้องการวัดใหม่")
            self.prompt_next()
        else:
            print("ขั้นที่ 1: กด b เพื่อวัดค่าปกติ (baseline) ~30 วินาที — ให้ผู้ตอบนั่งนิ่ง ๆ หายใจปกติ")
        warned = False
        while True:
            now = time.time()
            if now - self.last_hello > 2.0:
                self.hello()
            try:
                data, _ = self.sock.recvfrom(4096)
                self.handle(data)
            except socket.timeout:
                pass
            except OSError:
                time.sleep(0.2)
            if self.last_rx and now - self.last_rx > 5 and not warned:
                print("\nไม่ได้รับข้อมูลจากนาฬิกาเกิน 5 วินาที — ยังต่อ WiFi นาฬิกาอยู่ไหม? (จะลองต่อใหม่เรื่อย ๆ)")
                warned = True
            if self.last_rx and now - self.last_rx < 1:
                warned = False
            while not q.empty():
                self.cmd(q.get())

    def close(self):
        self.rec.close()
        if self.rec.files_created:
            print(f"บันทึกครบแล้วในโฟลเดอร์ data/  (รอบ {self.rec.run_id})")
        else:
            print("ยังไม่ได้ถามสักข้อ — ไม่ได้สร้างไฟล์ใหม่")


def main(argv=None) -> int:
    try:
        sys.stdout.reconfigure(errors="replace")
    except (AttributeError, ValueError):
        pass
    ap = argparse.ArgumentParser(description="เก็บข้อมูลเทรน AI จากนาฬิกาลง CSV ในคอม (ทีละขั้น)")
    ap.add_argument("--subject", default="", help="ชื่อผู้ถูกทดสอบ (ภาษาอังกฤษ)")
    ap.add_argument("--operator", default="", help="ชื่อผู้ถาม")
    ap.add_argument("--mode", choices=["fix", "manual"], default="", help="โหมดการให้เฉลย")
    ap.add_argument("--host", default="192.168.4.1")
    ap.add_argument("--udp", type=int, default=4210)
    ap.add_argument("--sim", action="store_true", help="ต่อนาฬิกาจำลอง 127.0.0.1 (UDP 4211, HTTP 8081)")
    a = ap.parse_args(argv)
    print("=" * 70)
    print(" เก็บข้อมูลเทรน AI — Wireless Polygraph")
    print("=" * 70)
    if not a.subject:
        a.subject = ask_line("ชื่อผู้ถูกทดสอบ (ภาษาอังกฤษ เช่น Somchai): ", "-")
    if not a.operator:
        a.operator = ask_line("ชื่อผู้ถาม (Enter = ข้าม): ", "-")
    if not a.mode:
        print("โหมดการให้เฉลย:  1 = fix (บอกล่วงหน้าว่าข้อนี้ให้โกหก)   2 = manual (ถามก่อน ค่อยบอกเฉลยทีหลัง)")
        a.mode = "manual" if ask_line("เลือก 1 หรือ 2 (Enter = 1): ", "1") == "2" else "fix"
    if a.sim:
        a.host, a.udp, a.http = "127.0.0.1", 4211, "127.0.0.1:8081"
    else:
        a.http = a.host
    c = Collector(a)
    print(f"ผู้ตอบ: {c.rec.subject} | ผู้ถาม: {c.rec.operator} | โหมด: {c.rec.mode} | นาฬิกา: {a.host}")
    try:
        c.run()
    except KeyboardInterrupt:
        print("\nหยุดแล้ว")
    finally:
        c.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
