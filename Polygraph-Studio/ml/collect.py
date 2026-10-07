"""
collect.py — เก็บข้อมูลจากนาฬิกาลงไฟล์ CSV ในคอม แบบเรียลไทม์ (ใช้แทน receive_data.py ของเพื่อน)

ต่อ WiFi "Polygraph-Watch" ก่อน แล้วรัน (ในโฟลเดอร์ Polygraph-Studio):
  python ml/collect.py --subject Somchai        นาฬิกาจริง
  python ml/collect.py --sim --subject Test1    นาฬิกาจำลอง (tools/virtual_watch.py)

ระหว่างรัน กดปุ่มบนคีย์บอร์ด (ไม่ต้องกด Enter บน Windows):
  b  วัดค่าปกติ (baseline)
  t  ถามข้อที่ให้ "ตอบตามจริง"        l  ถามข้อที่ "สั่งให้โกหก"        (โหมดเก็บข้อมูล)
  q  ถามคำถามจริง (ให้ระบบตัดสิน)     y / n  ผู้ถูกทดสอบตอบ ใช่ / ไม่ใช่  (โหมดใช้งานจริง)
  a  ยกเลิกข้อนี้     m  สลับโหมด เก็บข้อมูล <-> ใช้งานจริง     s  สถานะ
  d  ดาวน์โหลดไฟล์ข้อมูลเทรนจากนาฬิกา     h  ช่วยเหลือ     x  ออก

ไฟล์ที่ได้ (โฟลเดอร์ data/):
  training_samples.csv        1 แถว/ข้อ เฉพาะข้อที่รู้เฉลยในโหมดเก็บข้อมูล -> ใช้กับ train.py
  signals_<วันเวลา>.csv        ค่าทุกเซนเซอร์ 5 ครั้ง/วินาที (ไว้ทำกราฟ/วิเคราะห์เพิ่ม)
  results_<วันเวลา>.csv        ผลทุกข้อ ทุกโหมด
ไม่ต้องติดตั้งไลบรารีเพิ่ม
"""
from __future__ import annotations

import argparse
import csv
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

DATA = os.path.join(ROOT, "data")
STATE_TH = {0: "ยังไม่วัดค่าปกติ", 1: "กำลังวัดค่าปกติ", 2: "พร้อมถาม", 3: "กำลังวัดคำตอบ"}
VERDICT_TH = {"truth": "จริง", "lie": "โกหก", "inconclusive": "ไม่แน่ชัด", "invalid": "ใช้ไม่ได้"}
KIND_TH = {"truth": "ข้อควบคุม-ตอบจริง", "lie": "ข้อควบคุม-โกหก", "test": "คำถามจริง", "warmup": "วอร์มอัพ"}
SRC_TH = {0: "สูตรมาตรฐาน", 1: "สูตรปรับจากข้อควบคุม", 2: "โมเดล AI"}
HELP = __doc__[__doc__.index("ระหว่างรัน"):__doc__.index("ไฟล์ที่ได้")]


def key_reader(q: "queue.Queue[str]"):
    """อ่านปุ่มทีละตัว: Windows ใช้ msvcrt (ไม่ต้องกด Enter), ระบบอื่นอ่านทีละบรรทัด"""
    try:
        import msvcrt
        while True:
            ch = msvcrt.getwch()
            if ch in ("\x00", "\xe0"):        # ปุ่มพิเศษ (ลูกศร/F1) มี 2 ไบต์ -> ทิ้ง
                msvcrt.getwch()
                continue
            q.put(ch.lower())
    except ImportError:
        for line in sys.stdin:
            if line.strip():
                q.put(line.strip()[0].lower())


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
        self.last_print = 0.0
        self.live: dict = {}
        self.subject = a.subject
        os.makedirs(DATA, exist_ok=True)
        stamp = time.strftime("%Y%m%d_%H%M%S")
        self.f_sig, self.w_sig = self._open(f"signals_{stamp}.csv",
            ["pc_time", "watch_ms", "mode", "state", "qid", "hr", "hrv", "ppg_contact", "gsr", "gsr_tonic",
             "gsr_phasic", "gsr_contact", "skin_temp", "tremor", "motion", "stress", "vbat_mv"])
        self.f_res, self.w_res = self._open(f"results_{stamp}.csv",
            ["pc_time", "subject", "seq", "qid", "kind", "verdict", "expected", "correct", "p_lie", "score",
             "quality", "reasons", "source", "mode"] + ["f_" + s for s in pm.SIGNALS] + ["z_" + s for s in pm.SIGNALS])
        self.f_tr, self.w_tr = self._open("training_samples.csv", pm.TRAIN_HEADER)
        self.n_train = [0, 0]

    def _open(self, name, header):
        path = os.path.join(DATA, name)
        new = not os.path.exists(path) or os.path.getsize(path) == 0
        fh = open(path, "a", encoding="utf-8", newline="")
        w = csv.writer(fh)
        if new:
            w.writerow(header)
            fh.flush()
        return fh, w

    # ------------------------------------------------------------ UDP
    def hello(self):
        # มีคำว่า w=0 / t= ต่อท้าย -> นาฬิกาส่ง JSON (ถ้าส่ง "hello" เฉย ๆ จะได้ CSV แบบเก่าของเพื่อน)
        msg = f"hello w=0 t={int(time.time())}".encode()
        try:
            self.sock.sendto(msg, self.watch)
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
            st = m.get("es")
            self.w_sig.writerow([f"{time.time():.2f}", m.get("ms"), m.get("md"), st, m.get("eq") if st == 3 else "",
                                 m.get("hr"), m.get("hrv"), m.get("con"), m.get("gsr"), m.get("gt"), m.get("gp"),
                                 m.get("gc"), m.get("tmp"), m.get("trm"), m.get("mot"), m.get("si"), m.get("vb")])
        elif t == "hi":
            print(f"เชื่อมต่อนาฬิกาแล้ว: {m.get('id')}  FW {m.get('fw')}  (บูตครั้งที่ {m.get('boot')})")
        elif t == "e":
            eid = m.get("eid")
            if eid in self.seen_eids:               # นาฬิกาส่ง event ซ้ำ 2 ครั้งกันหล่น
                return
            self.seen_eids = (self.seen_eids + [eid])[-64:]
            self.event(m)

    def event(self, m: dict):
        ev = m.get("ev")
        if ev == "result":
            self.result(m)
        elif ev == "baseline_done":
            print("วัดค่าปกติเสร็จแล้ว — พร้อมถามคำถาม")
        elif ev == "baseline_fail":
            print(f"วัดค่าปกติไม่สำเร็จ ({m.get('error')}) — ใส่นาฬิกาให้แนบผิวแล้วกด b ใหม่")
        elif ev == "q_start":
            print(f"เริ่มข้อ {m.get('qid')} ({KIND_TH.get(m.get('kind'), m.get('kind'))}) — ถามได้เลย แล้วรอผล")
        elif ev == "mode":
            print("นาฬิกาเปลี่ยนเป็น", "โหมดเก็บข้อมูล" if m.get("mode") == "train" else "โหมดใช้งานจริง")
        elif ev == "button":
            print(f"ปุ่มบนนาฬิกา: {m.get('g')} -> {m.get('act')}")

    def result(self, m: dict):
        kind, verdict = m.get("kind"), m.get("verdict")
        # ใช้ชื่อผู้ถูกทดสอบที่ตั้งในนาฬิกา "ตอนนี้" (อาจถูกเปลี่ยนจากหน้าเว็บระหว่างเก็บ)
        # -> ชื่อในไฟล์ของคอมตรงกับไฟล์ในนาฬิกาเสมอ
        r = pm.http_json(self.http, "/api/ml", timeout=2)
        if r.get("ok"):
            self.subject = pm.clean_subject(r.get("subject", ""))
        f, z, ok = m.get("f") or [0] * 5, m.get("z") or [0] * 5, int(m.get("ok") or 0)
        mode = int(m.get("md", self.live.get("md", 0)) or 0)
        self.w_res.writerow([f"{time.time():.2f}", self.subject, m.get("seq"), m.get("qid"), kind, verdict,
                             m.get("exp"), m.get("cor"), m.get("p"), m.get("score"), m.get("q"), m.get("rs"),
                             m.get("src"), mode] + list(f) + list(z))
        self.f_res.flush()
        p = float(m.get("p") or 0)
        line = (f"ผลข้อ {m.get('qid')}: {VERDICT_TH.get(verdict, verdict)}  (โอกาสโกหก {p * 100:.0f}%, "
                f"ตัดสินด้วย{SRC_TH.get(m.get('src'), '?')}, คุณภาพสัญญาณ {m.get('q')}%)")
        if kind in ("truth", "lie"):
            line += f"  เฉลย: {'โกหก' if kind == 'lie' else 'จริง'}"
        print(line)
        if mode == 1 and kind in ("truth", "lie"):
            if verdict == "invalid":
                print("   ข้อนี้สัญญาณไม่ดี ไม่บันทึกเป็นข้อมูลเทรน (ลองนั่งนิ่ง ๆ แล้วถามใหม่)")
                return
            label = 1 if kind == "lie" else 0
            x = pm.features_from(ok, z, f)
            self.w_tr.writerow(pm.train_row(x, epoch=int(time.time()), subject=self.subject, qid=int(m.get("qid") or 0),
                                            label=label, quality=int(m.get("q") or 0), p_model=p,
                                            source=int(m.get("src") or 0)))
            self.f_tr.flush()
            self.n_train[label] += 1
            print(f"   บันทึกลง training_samples.csv แล้ว (รอบนี้: จริง {self.n_train[0]} / โกหก {self.n_train[1]})")

    # ------------------------------------------------------------ คำสั่ง
    def cmd(self, k: str):
        H = self.http
        calls = {"b": ("/api/lie/baseline", "เริ่มวัดค่าปกติ — นั่งนิ่ง ๆ หายใจปกติ ~30 วินาที"),
                 "t": ("/api/lie/question?kind=truth", None), "l": ("/api/lie/question?kind=lie", None),
                 "q": ("/api/lie/question?kind=test", None),
                 "y": ("/api/lie/answer?ans=yes", "บันทึกคำตอบ: ใช่"), "n": ("/api/lie/answer?ans=no", "บันทึกคำตอบ: ไม่ใช่"),
                 "a": ("/api/lie/abort", "ยกเลิกข้อนี้แล้ว")}
        if k in calls:
            path, msg = calls[k]
            if k in ("t", "l") and self.live.get("md") == 0:
                print("(ตอนนี้นาฬิกาอยู่โหมดใช้งานจริง ข้อนี้จะไม่ถูกบันทึกเป็นข้อมูลเทรน — กด m เพื่อเปลี่ยนโหมด)")
            r = pm.http_post(H, path)
            if r.get("ok"):
                if msg:
                    print(msg)
            else:
                print("ทำไม่ได้:", r.get("msg", r))
        elif k == "m":
            new = "detect" if self.live.get("md") == 1 else "train"
            r = pm.http_post(H, "/api/ml/mode?mode=" + new)
            print(r.get("msg") if r.get("ok") else ("เปลี่ยนโหมดไม่ได้: " + str(r.get("msg", r))))
        elif k == "s":
            self.status(force=True)
        elif k == "d":
            out = os.path.join(DATA, time.strftime("watch_train_%Y%m%d_%H%M%S.csv"))
            try:
                n = pm.download_csv(H, out)
                print(f"ดาวน์โหลดแล้ว: data/{os.path.basename(out)} ({n} ไบต์)")
            except Exception as e:  # noqa: BLE001
                print("ดาวน์โหลดไม่ได้:", e)
        elif k == "h":
            print(HELP)
        elif k in ("x", "\x1b", "\x03"):
            raise KeyboardInterrupt

    def status(self, force=False):
        v = self.live
        if not v:
            return
        if not force and time.time() - self.last_print < 5:
            return
        self.last_print = time.time()
        hr = f"{v['hr']:.0f}" if v.get("hr") is not None and v.get("con") else "--"
        gsr = f"{v['gsr']:.2f}" if v.get("gsr") is not None and v.get("gc") else "ไม่มี"
        mode = "เก็บข้อมูล" if v.get("md") == 1 else "ใช้งานจริง"
        print(f"[{time.strftime('%H:%M:%S')}] ชีพจร {hr} | GSR {gsr} | ผิว {v.get('tmp')} C | สั่น {v.get('trm')} | "
              f"{STATE_TH.get(v.get('es'), '?')} | โหมด{mode} | AI {'มี' if v.get('ml') else 'ไม่มี'}")

    def run(self):
        q: "queue.Queue[str]" = queue.Queue()
        threading.Thread(target=key_reader, args=(q,), daemon=True).start()
        if self.subject == "-":                      # ไม่ได้ใส่ --subject -> ใช้ชื่อที่ตั้งไว้ในนาฬิกา
            r = pm.http_json(self.http, "/api/ml", timeout=3)
            self.subject = pm.clean_subject(r.get("subject", "")) if r.get("ok") else "-"
        else:
            pm.http_post(self.http, "/api/ml/subject?name=" + urllib.parse.quote(self.subject), timeout=3)
        pm.http_post(self.http, f"/api/time?epoch={int(time.time())}", timeout=3)
        print(f"ผู้ถูกทดสอบ: {self.subject}   ไฟล์อยู่ในโฟลเดอร์ data/   กด h ดูปุ่มทั้งหมด, x ออก")
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
                print("ไม่ได้รับข้อมูลจากนาฬิกาเกิน 5 วินาที — ยังต่อ WiFi นาฬิกาอยู่ไหม? (จะลองต่อใหม่เรื่อย ๆ)")
                warned = True
            if self.last_rx and now - self.last_rx < 1:
                warned = False
            while not q.empty():
                self.cmd(q.get())
            self.status()
            if int(now) % 5 == 0:
                self.f_sig.flush()

    def close(self):
        for fh in (self.f_sig, self.f_res, self.f_tr):
            fh.close()


def main(argv=None) -> int:
    try:
        sys.stdout.reconfigure(errors="replace")
    except (AttributeError, ValueError):
        pass
    ap = argparse.ArgumentParser(description="เก็บข้อมูลจากนาฬิกาลง CSV ในคอม")
    ap.add_argument("--subject", default="", help="ชื่อผู้ถูกทดสอบ (ภาษาอังกฤษ) ไม่ใส่ = ใช้ชื่อที่ตั้งในนาฬิกา")
    ap.add_argument("--host", default="192.168.4.1")
    ap.add_argument("--udp", type=int, default=4210)
    ap.add_argument("--sim", action="store_true", help="ต่อนาฬิกาจำลอง 127.0.0.1 (UDP 4211, HTTP 8081)")
    a = ap.parse_args(argv)
    a.subject = pm.clean_subject(a.subject)
    if a.sim:
        a.host, a.udp = "127.0.0.1", 4211
        a.http = "127.0.0.1:8081"
    else:
        a.http = a.host
    c = Collector(a)
    print("=" * 66)
    print(f" เก็บข้อมูลจากนาฬิกา {a.host} (UDP {a.udp}) ลงโฟลเดอร์ {DATA}")
    print("=" * 66)
    try:
        c.run()
    except KeyboardInterrupt:
        print("\nหยุดแล้ว — ไฟล์ถูกบันทึกครบ")
    finally:
        c.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
