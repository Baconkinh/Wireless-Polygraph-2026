"""
train.py — เทรน AI จับเท็จจากไฟล์ data/result_<วันเวลา>.csv แล้วได้ data/model.json สำหรับใส่นาฬิกา

อ่านข้อมูลจากไหน (สำคัญ)
  * ค่าเริ่มต้น: ไฟล์ชื่อ result_<YYYYMMDD_HHMMSS>.csv ในโฟลเดอร์ data/ "เท่านั้น"
    (ไม่อ่านโฟลเดอร์ย่อย signals/ old_format/ models/ และไม่อ่านไฟล์ชื่ออื่น)
  * ยกเว้นไฟล์ที่อยู่ใน data/train_exclude.txt (ติ๊กออกได้ที่หน้า "ข้อมูล & เทรน AI" ใน Studio)
  * ในแต่ละไฟล์ใช้เฉพาะแถวที่ used_for_training = 1 และเฉลยเป็น truth/lie
  * ไฟล์รูปแบบเก่า (results_*.csv, training_samples.csv, watch_train_*.csv) ถูกแปลงให้อัตโนมัติก่อนเทรน
    (ต้นฉบับย้ายไป data/old_format/ ไม่ถูกแก้)

วิธีใช้ (หรือดับเบิลคลิก train_ai.bat)
  python ml/train.py                     เทรน -> ถ้าต่อ WiFi นาฬิกาอยู่ ส่งโมเดลเข้านาฬิกาให้เลย
  python ml/train.py --no-upload         เทรนอย่างเดียว ไม่ส่ง
  python ml/train.py --upload-only       ไม่เทรน แค่ส่ง data/model.json เข้านาฬิกา (train_ai_upload.bat)
  python ml/train.py --download          ดึงข้อมูลที่นาฬิกาบันทึกเอง (โหมด train บนหน้าเว็บนาฬิกา) มาก่อน
  python ml/train.py --csv a.csv b.csv   ระบุไฟล์เอง (ต้องเป็นไฟล์ผลรายข้อ เช่น result_*.csv หรือไฟล์รวม)
  python ml/train.py --compare-rf        เทียบกับ RandomForest แบบโค้ดของเพื่อน (ต้องมี scikit-learn)
  --host 127.0.0.1:8081                  ใช้กับนาฬิกาจำลอง

ผลลัพธ์: data/model.json (ตัวล่าสุด) + data/models/model_<ชื่อ>.json (เก็บทุกเวอร์ชัน)
  Polygraph Studio จะส่ง model.json ตัวล่าสุดเข้านาฬิกาให้อัตโนมัติเมื่อเชื่อมนาฬิกา (ปิดได้ในหน้า "ข้อมูล & เทรน AI")
ไม่ต้องติดตั้งไลบรารีเพิ่ม (ใช้แต่ของที่มากับ Python)
"""
from __future__ import annotations

import argparse
import json
import os
import sys
import tempfile
import time
from typing import Callable, Dict, Optional, Sequence

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
import datafiles as df  # noqa: E402
import polyml as pm  # noqa: E402

DATA = os.path.join(ROOT, "data")
MODEL_PATH = os.path.join(DATA, "model.json")


def pct(x: float) -> str:
    """แปลงสัดส่วนเป็นข้อความเปอร์เซ็นต์ เช่น 0.541 -> 54.1%"""
    return f"{x * 100:5.1f}%"


# ============================================================ อ่านข้อมูลจาก result_*.csv
def build_dataset(files: Sequence[str], features: Sequence[str]) -> pm.Dataset:
    """รวมแถวที่ใช้เทรนได้จากหลายไฟล์ -> Dataset (X, y, subject)
    ตัดข้อซ้ำด้วยกุญแจ (run_id, question_no): กันกรณีส่งไฟล์รวม + ไฟล์ต้นฉบับมาพร้อมกัน"""
    ds = pm.Dataset()
    seen = set()
    for path in files:
        ds.files.append(path)
        rows = df.read_result_file(path)
        good = list(df.training_rows(rows))
        ds.skipped += len(rows) - len(good)
        for r in good:
            k = df.row_key(r)
            if k in seen:
                ds.duplicates += 1
                continue
            seen.add(k)
            full = {c: float(r[c]) for c in df.FEATURE_COLS}
            ds.X.append([full[c] for c in features])
            ds.y.append(1 if r["label"] == "lie" else 0)
            ds.subject.append(pm.clean_subject(r.get("subject", "")))
            ds.p_model.append(pm._f(r.get("p_lie")))
    return ds


def save_model(mj: dict, out: str = MODEL_PATH) -> str:
    """บันทึก model.json (ตัวล่าสุด) + สำเนาใน data/models/ — เขียนไฟล์ชั่วคราวก่อนแล้วค่อยเปลี่ยนชื่อ
    เพื่อไม่ให้ Studio (ที่คอยอ่าน model.json) อ่านเจอไฟล์ที่เขียนไม่เสร็จ"""
    d = os.path.dirname(os.path.abspath(out))
    os.makedirs(os.path.join(d, df.MODELS_DIR), exist_ok=True)
    fd, tmp = tempfile.mkstemp(dir=d, suffix=".tmp")
    with os.fdopen(fd, "w", encoding="utf-8") as fh:
        json.dump(mj, fh, ensure_ascii=False, indent=2)
    os.replace(tmp, out)
    hist = os.path.join(d, df.MODELS_DIR, f"model_{mj['name']}.json")
    with open(hist, "w", encoding="utf-8") as fh:
        json.dump(mj, fh, ensure_ascii=False, indent=2)
    return hist


def run_training(files: Sequence[str], features: Optional[Sequence[str]] = None, min_samples: int = 10,
                 out: str = MODEL_PATH, compare_rf: bool = False,
                 log: Callable[[str], None] = print) -> Dict:
    """เทรน + ประเมิน + บันทึกโมเดล (ใช้ทั้งจาก command line และจากปุ่มใน Studio)
    คืน dict: ok, msg, model (เนื้อหา model.json), metrics"""
    feats = list(features or pm.FEATURES)
    if not files:
        log("ไม่พบไฟล์ result_*.csv ในโฟลเดอร์ data/")
        log("  วิธีได้ข้อมูล: Studio > เก็บข้อมูลเทรน AI  หรือ  collect_data.bat  หรือ Studio > ใช้งานจริง (บอกถูก/ผิด)")
        return {"ok": False, "msg": "ไม่พบไฟล์ข้อมูล result_*.csv"}
    ds = build_dataset(files, feats)
    n0, n1 = ds.counts()
    subjects = sorted(set(ds.subject))
    log("ไฟล์ที่ใช้:")
    for f in files:
        log("  - " + (os.path.relpath(f, ROOT) if f.startswith(ROOT) else f))
    log(f"ข้อมูล: {len(ds)} ข้อ  (ตอบจริง {n0} / โกหก {n1})  ผู้ถูกทดสอบ {len(subjects)} คน: {', '.join(subjects)}")
    if ds.duplicates:
        log(f"  ตัดข้อซ้ำออก {ds.duplicates} ข้อ (ข้อเดียวกันอยู่หลายไฟล์)")
    if ds.skipped:
        log(f"  ไม่ใช้ {ds.skipped} แถว (ไม่รู้เฉลย / ยกเลิก / สัญญาณใช้ไม่ได้ / used_for_training = 0)")
    if len(ds) == 0 or min(n0, n1) < min_samples:
        log(f"\nข้อมูลยังน้อยเกินไป: ต้องมีอย่างน้อยคลาสละ {min_samples} ข้อ (ตอนนี้ จริง {n0} / โกหก {n1})")
        log("  เก็บเพิ่ม หรือถ้าแค่ทดลองให้ใส่ --min-samples 3")
        return {"ok": False, "msg": f"ข้อมูลน้อยเกินไป (จริง {n0} / โกหก {n1}) ต้องมีคลาสละ {min_samples} ข้อ"}

    # ---------------- ประเมินแบบไม่โกง: ข้อที่ใช้ทดสอบไม่เคยถูกใช้เทรน ----------------
    method, p_oof, _ = pm.cross_validate(ds, feats)
    m_ai = pm.metrics(ds.y, p_oof)
    majority = max(n0, n1) / len(ds)
    log(f"\nการประเมิน: {method}  (ทุกข้อถูกทำนายโดยโมเดลที่ไม่เคยเห็นข้อนั้น)")
    log(f"  ความแม่นยำ AI          : {pct(m_ai['accuracy'])}   ช่วงเชื่อมั่น 95%: "
        f"{pct(m_ai['ci95'][0])} – {pct(m_ai['ci95'][1])}")
    log(f"  balanced accuracy      : {pct(m_ai['balanced_accuracy'])}")
    log(f"  จับคนโกหกได้ (sensitivity): {pct(m_ai['sensitivity'])}   คนพูดจริงไม่ถูกกล่าวหา (specificity): "
        f"{pct(m_ai['specificity'])}")
    c = m_ai["confusion"]
    log("  ตาราง confusion            ทายว่าจริง  ทายว่าโกหก")
    log(f"    เฉลย: จริง               {c['tn']:8d}   {c['fp']:9d}")
    log(f"    เฉลย: โกหก               {c['fn']:8d}   {c['tp']:9d}")
    log(f"  เทียบ: เดาคลาสที่มากที่สุดทุกข้อ = {pct(majority)}")
    pm_ok = [(y, p) for y, p in zip(ds.y, ds.p_model) if p is not None]
    m_rule = pm.metrics([y for y, _ in pm_ok], [p for _, p in pm_ok]) if pm_ok else None
    if m_rule:
        log(f"  เทียบ: คำตัดสินของนาฬิกาตอนเก็บข้อมูล = {pct(m_rule['accuracy'])}  (n={m_rule['n']})")
    if m_ai["accuracy"] <= majority + 0.05:
        log("  [เตือน] AI ยังไม่ดีกว่าการเดาอย่างมีนัย — เก็บข้อมูลเพิ่ม/หลายคนขึ้น หรือเช็คการใส่นาฬิกา")
    if len(ds) < 40:
        log("  [เตือน] ข้อมูลน้อย ตัวเลขความแม่นยำแกว่งได้มาก (ดูช่วงเชื่อมั่นด้านบน)")

    if compare_rf:
        try:
            from sklearn.ensemble import RandomForestClassifier
            _, folds = pm.cv_splits(ds)
            p_rf = [0.5] * len(ds)
            for f in folds:
                te = set(f)
                tr = [i for i in range(len(ds)) if i not in te]
                rf = RandomForestClassifier(n_estimators=100, random_state=42)
                rf.fit([ds.X[i] for i in tr], [ds.y[i] for i in tr])
                for i, pr in zip(f, rf.predict_proba([ds.X[i] for i in f])):
                    p_rf[i] = float(pr[list(rf.classes_).index(1)]) if 1 in rf.classes_ else 0.0
            m_rf = pm.metrics(ds.y, p_rf)
            log(f"  เทียบ: RandomForest 100 ต้น (CV เดียวกัน) = {pct(m_rf['accuracy'])} — ใส่ในนาฬิกาไม่ได้ ใช้เทียบเท่านั้น")
        except ImportError:
            log("  (ข้าม --compare-rf: เครื่องนี้ไม่มี scikit-learn — ติดตั้งด้วย python -m pip install scikit-learn)")

    # ---------------- โมเดลสุดท้าย: เทรนด้วยข้อมูลทั้งหมด ----------------
    lam = pm.choose_lambda(ds.X, ds.y, feats, list(range(len(ds))))
    model = pm.Model.fit(ds.X, ds.y, feats, lam)
    order = sorted(range(len(feats)), key=lambda i: -abs(model.w[i]))
    log(f"\nโมเดลสุดท้าย (เทรนจากทั้ง {len(ds)} ข้อ, L2 lambda = {lam}):")
    log("  สัญญาณที่มีผลต่อการตัดสิน เรียงจากมากไปน้อย (+ = ยิ่งมากยิ่งน่าจะโกหก)")
    for i in order:
        bar = "#" * min(30, int(abs(model.w[i]) * 10 + 0.5))
        log(f"    {feats[i]:7s} {model.w[i]:+7.3f}  {bar:30s} {pm.FEATURE_TH.get(feats[i], '')}")

    stamp = int(time.time())
    name = time.strftime("lr-%Y%m%d-%H%M%S", time.localtime(stamp))[:pm.FW_NAME_MAX]
    info = {"name": name, "cv_accuracy": round(m_ai["accuracy"], 4), "cv_method": method,
            "cv_ci95": [round(v, 4) for v in m_ai["ci95"]], "balanced_accuracy": round(m_ai["balanced_accuracy"], 4),
            "sensitivity": round(m_ai["sensitivity"], 4), "specificity": round(m_ai["specificity"], 4),
            "confusion": c, "majority_accuracy": round(majority, 4),
            "rule_accuracy": round(m_rule["accuracy"], 4) if m_rule else None,
            "samples": len(ds), "n_truth": n0, "n_lie": n1, "subjects": subjects, "trained_at": stamp,
            "source_files": [os.path.basename(f) for f in files],
            "note": "label 0 = ตอบจริง, 1 = โกหก"}
    mj = model.to_json(**info)
    hist = save_model(mj, out)
    log(f"\nบันทึกโมเดล: {os.path.relpath(out, ROOT) if out.startswith(ROOT) else out}")
    log(f"        สำเนา: {os.path.relpath(hist, ROOT) if hist.startswith(ROOT) else hist}")
    return {"ok": True, "msg": f"เทรนเสร็จ: {name} ความแม่นยำ {pct(m_ai['accuracy']).strip()} จาก {len(ds)} ข้อ",
            "model": mj, "metrics": {k: v for k, v in info.items() if k != "note"}}


# ============================================================ ส่งโมเดลเข้านาฬิกา
def watch_reachable(host: str) -> bool:
    """ลองถามนาฬิกาเร็ว ๆ (2 วินาที) ว่าต่ออยู่ไหม"""
    return bool(pm.http_json(host, "/api/ml", timeout=2.0).get("ok"))


def upload(host: str, mj: dict, log: Callable[[str], None] = print) -> bool:
    """ส่งโมเดลเข้านาฬิกาแล้วพิมพ์ผล (คืน True ถ้าสำเร็จ)"""
    r = pm.upload_model(host, mj)
    if r.get("ok"):
        log(f"ส่งโมเดล {mj.get('name')} เข้านาฬิกา {host} แล้ว — นาฬิกาจะตัดสินด้วย AI ตัวนี้")
        return True
    log(f"ส่งโมเดลไม่สำเร็จ: {r.get('msg', r)}")
    return False


def pull_watch_data(host: str, log: Callable[[str], None] = print) -> Dict:
    """ดึงข้อมูลที่นาฬิกาบันทึกเอง (/api/ml/data.csv) แล้วแปลงเป็น result_*.csv (เฉพาะข้อใหม่)"""
    tmpdir = tempfile.mkdtemp()
    tmp = os.path.join(tmpdir, "watch_train.csv")
    try:
        pm.download_csv(host, tmp)
        rep = df.import_file(DATA, tmp, origin=f"นาฬิกา {host} (/api/ml/data.csv)")
    except Exception as e:  # noqa: BLE001 - แสดงสาเหตุให้ผู้ใช้แก้เอง
        rep = {"ok": False, "msg": f"ดึงข้อมูลจากนาฬิกาไม่ได้ ({e})"}
    finally:
        try:
            os.remove(tmp)
            os.rmdir(tmpdir)
        except OSError:
            pass
    log(rep.get("msg", ""))
    return rep


def main(argv=None) -> int:
    """จุดเริ่มของ train_ai.bat / train_ai_upload.bat: อ่าน argument -> แปลงไฟล์เก่า -> (ดึงข้อมูลนาฬิกา) -> เทรน -> ส่ง"""
    try:
        sys.stdout.reconfigure(errors="replace")   # กันโปรแกรมล้มเมื่อ console แสดงบางตัวอักษร (µ ²) ไม่ได้
    except (AttributeError, ValueError):
        pass
    ap = argparse.ArgumentParser(description="เทรนโมเดลจับเท็จ (logistic regression) จาก data/result_*.csv")
    ap.add_argument("--csv", nargs="+", help="ไฟล์ผลรายข้อ (ค่าเริ่มต้น: data/result_*.csv ยกเว้นใน train_exclude.txt)")
    ap.add_argument("--host", default="192.168.4.1", help="IP นาฬิกา (นาฬิกาจำลอง: 127.0.0.1:8081)")
    ap.add_argument("--download", action="store_true", help="ดึงข้อมูลที่นาฬิกาบันทึกเองมาก่อนเทรน")
    up = ap.add_mutually_exclusive_group()
    up.add_argument("--upload", action="store_true", help="ต้องส่งโมเดลเข้านาฬิกา (ส่งไม่ได้ = แจ้ง error)")
    up.add_argument("--no-upload", action="store_true", help="ไม่ส่งโมเดลเข้านาฬิกา")
    up.add_argument("--upload-only", action="store_true", help="ไม่เทรน แค่ส่ง data/model.json เข้านาฬิกา")
    ap.add_argument("--features", help="เลือก feature เอง คั่นด้วย , (ค่าเริ่มต้น: ทั้ง 12 ตัว)")
    ap.add_argument("--out", default=MODEL_PATH)
    ap.add_argument("--compare-rf", action="store_true", help="เทียบกับ RandomForest (ต้องมี scikit-learn)")
    ap.add_argument("--min-samples", type=int, default=10, help="จำนวนข้อขั้นต่ำต่อคลาส")
    a = ap.parse_args(argv)

    print("=" * 66)
    print(" เทรน AI จับเท็จ — Logistic Regression (ใช้บนนาฬิกาได้จริง)")
    print("=" * 66)

    if a.upload_only:
        try:
            with open(a.out, encoding="utf-8") as fh:
                mj = json.load(fh)
        except (OSError, ValueError):
            print(f"ไม่พบ {a.out} — เทรนก่อนด้วย train_ai.bat")
            return 1
        print(f"ส่งโมเดล {mj.get('name')} (ความแม่นยำ {mj.get('cv_accuracy')}, {mj.get('samples')} ข้อ) ...")
        return 0 if upload(a.host, mj) else 2

    rep = df.migrate(DATA, log=lambda s: print("[แปลงไฟล์เก่า] " + s))
    if rep["moved"]:
        print(f"[แปลงไฟล์เก่า] ย้ายไฟล์เดิม {len(rep['moved'])} ไฟล์ไปไว้ใน data/old_format, signals, models (ไม่ได้ลบ)\n")

    if a.download:
        pull_watch_data(a.host)

    feats = [f.strip() for f in a.features.split(",")] if a.features else list(pm.FEATURES)
    bad = [f for f in feats if f not in pm.FEATURES]
    if bad:
        print("ไม่รู้จัก feature:", ", ".join(bad), "| ที่มี:", ", ".join(pm.FEATURES))
        return 1
    files = a.csv or df.training_files(DATA)
    ex = df.load_exclude(DATA)
    if ex and not a.csv:
        print("ไม่ใช้ไฟล์ (อยู่ใน data/train_exclude.txt): " + ", ".join(ex))
    res = run_training(files, feats, a.min_samples, a.out, a.compare_rf)
    if not res["ok"]:
        return 1

    if a.no_upload:
        print("ไม่ได้ส่งเข้านาฬิกา (--no-upload) — Studio จะส่งให้อัตโนมัติเมื่อเชื่อมนาฬิกา หรือใช้ train_ai_upload.bat")
        return 0
    if a.upload or watch_reachable(a.host):
        return 0 if upload(a.host, res["model"]) else 2
    print("ตอนนี้ต่อนาฬิกาไม่ได้ — ไม่เป็นไร: เปิด run_studio.bat ตอนต่อ WiFi นาฬิกา Studio จะส่ง model.json ให้อัตโนมัติ")
    print("  (หรือดับเบิลคลิก train_ai_upload.bat ตอนต่อ WiFi นาฬิกาแล้ว)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
