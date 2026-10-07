"""
train.py — เทรน AI จับเท็จจากไฟล์ CSV ที่เก็บในโหมดเก็บข้อมูล แล้วได้ไฟล์ model.json ไปใส่นาฬิกา

วิธีใช้ (เปิด Command Prompt ในโฟลเดอร์ Polygraph-Studio):
  python ml/train.py                         เทรนจากทุกไฟล์ข้อมูลเทรนในโฟลเดอร์ data/
  python ml/train.py --download              ดาวน์โหลดข้อมูลจากนาฬิกาก่อน (ต้องต่อ WiFi นาฬิกา)
  python ml/train.py --upload                เทรนเสร็จแล้วส่งโมเดลเข้านาฬิกาเลย
  python ml/train.py --csv a.csv b.csv       ระบุไฟล์เอง
  python ml/train.py --compare-rf            เทียบกับ RandomForest แบบโค้ดของเพื่อน (ต้องมี scikit-learn)

ผลลัพธ์: data/model.json (+ สำเนามีวันที่) — อัปโหลดเข้านาฬิกาได้ 2 ทาง
  1) --upload   2) หน้าเว็บนาฬิกา 192.168.4.1 > การ์ด "โมเดล AI" > อัปโหลด model.json

ไม่ต้องติดตั้งไลบรารีเพิ่ม (ใช้แต่ของที่มากับ Python)
"""
from __future__ import annotations

import argparse
import glob
import json
import os
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
import polyml as pm  # noqa: E402

DATA = os.path.join(ROOT, "data")


def pct(x: float) -> str:
    return f"{x * 100:5.1f}%"


def find_csvs() -> list:
    files = sorted(glob.glob(os.path.join(DATA, "*.csv")))
    return [f for f in files if pm.is_training_csv(f)]


def main(argv=None) -> int:
    try:
        sys.stdout.reconfigure(errors="replace")   # กันโปรแกรมล้มเมื่อ console แสดงบางตัวอักษร (µ ²) ไม่ได้
    except (AttributeError, ValueError):
        pass
    ap = argparse.ArgumentParser(description="เทรนโมเดลจับเท็จ (logistic regression) จากข้อมูลโหมดเก็บข้อมูล")
    ap.add_argument("--csv", nargs="+", help="ไฟล์ข้อมูลเทรน (ค่าเริ่มต้น: ทุกไฟล์ที่ใช้ได้ใน data/)")
    ap.add_argument("--host", default="192.168.4.1", help="IP นาฬิกา (นาฬิกาจำลอง: 127.0.0.1:8081)")
    ap.add_argument("--download", action="store_true", help="ดาวน์โหลด /api/ml/data.csv จากนาฬิกาก่อนเทรน")
    ap.add_argument("--upload", action="store_true", help="ส่งโมเดลเข้านาฬิกาหลังเทรน")
    ap.add_argument("--features", help="เลือก feature เอง คั่นด้วย , (ค่าเริ่มต้น: ทั้ง 12 ตัว)")
    ap.add_argument("--out", default=os.path.join(DATA, "model.json"))
    ap.add_argument("--compare-rf", action="store_true", help="เทียบกับ RandomForest (ต้องมี scikit-learn)")
    ap.add_argument("--min-samples", type=int, default=10, help="จำนวนข้อขั้นต่ำต่อคลาส")
    a = ap.parse_args(argv)

    print("=" * 66)
    print(" เทรน AI จับเท็จ — Logistic Regression (ใช้บนนาฬิกาได้จริง)")
    print("=" * 66)

    if a.download:
        out = os.path.join(DATA, time.strftime("watch_train_%Y%m%d_%H%M%S.csv"))
        try:
            n = pm.download_csv(a.host, out)
            print(f"ดาวน์โหลดจากนาฬิกา {a.host} แล้ว: {os.path.relpath(out, ROOT)} ({n} ไบต์)")
        except Exception as e:  # noqa: BLE001 - แสดงสาเหตุให้ผู้ใช้แก้เอง
            print(f"ดาวน์โหลดไม่ได้ ({e}) — ต่อ WiFi 'Polygraph-Watch' อยู่หรือเปล่า? (ข้ามไปใช้ไฟล์ที่มี)")

    files = a.csv or find_csvs()
    if not files:
        print("ไม่พบไฟล์ข้อมูลเทรนในโฟลเดอร์ data/")
        print("  วิธีได้ข้อมูล: หน้าเว็บนาฬิกา > โหมด 'เก็บข้อมูลเทรน AI' > ถามข้อจริง/โกหก > 'ดาวน์โหลด CSV'")
        print("  แล้วย้ายไฟล์มาไว้ในโฟลเดอร์ data/ หรือรัน: python ml/train.py --download")
        return 1

    feats = [f.strip() for f in a.features.split(",")] if a.features else list(pm.FEATURES)
    bad = [f for f in feats if f not in pm.FEATURES]
    if bad:
        print("ไม่รู้จัก feature:", ", ".join(bad), "| ที่มี:", ", ".join(pm.FEATURES))
        return 1

    ds = pm.load_training(files, feats)
    n0, n1 = ds.counts()
    subjects = sorted(set(ds.subject))
    print("ไฟล์ที่ใช้:")
    for f in files:
        print("  -", os.path.relpath(f, ROOT) if f.startswith(ROOT) else f)
    print(f"ข้อมูล: {len(ds)} ข้อ  (ตอบจริง {n0} / โกหก {n1})  ผู้ถูกทดสอบ {len(subjects)} คน: {', '.join(subjects)}")
    if ds.duplicates:
        print(f"  ตัดข้อซ้ำออก {ds.duplicates} ข้อ (ข้อเดียวกันอยู่หลายไฟล์)")
    if ds.skipped:
        print(f"  ข้ามแถวที่อ่านไม่ได้ {ds.skipped} แถว")
    if min(n0, n1) < a.min_samples:
        print(f"\nข้อมูลยังน้อยเกินไป: ต้องมีอย่างน้อยคลาสละ {a.min_samples} ข้อ (ตอนนี้ {n0}/{n1})")
        print("  เก็บเพิ่มในโหมดเก็บข้อมูล หรือถ้าแค่ทดลองให้ใส่ --min-samples 3")
        return 1

    # ---------------- ประเมินแบบไม่โกง: ข้อที่ใช้ทดสอบไม่เคยถูกใช้เทรน ----------------
    method, p_oof, lams = pm.cross_validate(ds, feats)
    m_ai = pm.metrics(ds.y, p_oof)
    majority = max(n0, n1) / len(ds)
    print(f"\nการประเมิน: {method}  (ทุกข้อถูกทำนายโดยโมเดลที่ไม่เคยเห็นข้อนั้น)")
    print(f"  ความแม่นยำ AI          : {pct(m_ai['accuracy'])}   ช่วงเชื่อมั่น 95%: "
          f"{pct(m_ai['ci95'][0])} – {pct(m_ai['ci95'][1])}")
    print(f"  balanced accuracy      : {pct(m_ai['balanced_accuracy'])}")
    print(f"  จับคนโกหกได้ (sensitivity): {pct(m_ai['sensitivity'])}   คนพูดจริงไม่ถูกกล่าวหา (specificity): "
          f"{pct(m_ai['specificity'])}")
    c = m_ai["confusion"]
    print("  ตาราง confusion            ทายว่าจริง  ทายว่าโกหก")
    print(f"    เฉลย: จริง               {c['tn']:8d}   {c['fp']:9d}")
    print(f"    เฉลย: โกหก               {c['fn']:8d}   {c['tp']:9d}")
    print(f"  เทียบ: เดาคลาสที่มากที่สุดทุกข้อ = {pct(majority)}")
    pm_ok = [(y, p) for y, p in zip(ds.y, ds.p_model) if p is not None]
    m_rule = pm.metrics([y for y, _ in pm_ok], [p for _, p in pm_ok]) if pm_ok else None
    if m_rule:
        print(f"  เทียบ: ระบบเดิมในนาฬิกาตอนเก็บข้อมูล (สูตร) = {pct(m_rule['accuracy'])}  (n={m_rule['n']})")
    if m_ai["accuracy"] <= majority + 0.05:
        print("  [เตือน] AI ยังไม่ดีกว่าการเดาอย่างมีนัย — เก็บข้อมูลเพิ่ม/หลายคนขึ้น หรือเช็คการใส่นาฬิกา")
    if len(ds) < 40:
        print("  [เตือน] ข้อมูลน้อย ตัวเลขความแม่นยำแกว่งได้มาก (ดูช่วงเชื่อมั่นด้านบน)")

    if a.compare_rf:
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
            print(f"  เทียบ: RandomForest 100 ต้น (แบบโค้ดของเพื่อน, CV เดียวกัน) = {pct(m_rf['accuracy'])}"
                  " — ใส่ในนาฬิกาไม่ได้ ใช้เทียบเท่านั้น")
        except ImportError:
            print("  (ข้าม --compare-rf: เครื่องนี้ไม่มี scikit-learn — ติดตั้งด้วย python -m pip install scikit-learn)")

    # ---------------- โมเดลสุดท้าย: เทรนด้วยข้อมูลทั้งหมด ----------------
    lam = pm.choose_lambda(ds.X, ds.y, feats, list(range(len(ds))))
    model = pm.Model.fit(ds.X, ds.y, feats, lam)
    order = sorted(range(len(feats)), key=lambda i: -abs(model.w[i]))
    print(f"\nโมเดลสุดท้าย (เทรนจากทั้ง {len(ds)} ข้อ, L2 lambda = {lam}):")
    print("  สัญญาณที่มีผลต่อการตัดสิน เรียงจากมากไปน้อย (+ = ยิ่งมากยิ่งน่าจะโกหก)")
    for i in order:
        bar = "#" * min(30, int(abs(model.w[i]) * 10 + 0.5))
        print(f"    {feats[i]:7s} {model.w[i]:+7.3f}  {bar:30s} {pm.FEATURE_TH.get(feats[i], '')}")

    stamp = int(time.time())
    name = time.strftime("lr-%Y%m%d-%H%M", time.localtime(stamp))
    info = {"name": name, "cv_accuracy": round(m_ai["accuracy"], 4), "cv_method": method,
            "cv_ci95": [round(v, 4) for v in m_ai["ci95"]], "balanced_accuracy": round(m_ai["balanced_accuracy"], 4),
            "sensitivity": round(m_ai["sensitivity"], 4), "specificity": round(m_ai["specificity"], 4),
            "confusion": c, "majority_accuracy": round(majority, 4),
            "rule_accuracy": round(m_rule["accuracy"], 4) if m_rule else None,
            "samples": len(ds), "n_truth": n0, "n_lie": n1, "subjects": subjects, "trained_at": stamp,
            "source_files": [os.path.basename(f) for f in files],
            "note": "label 0 = ตอบจริง, 1 = โกหก (ตามคำสั่งในโหมดเก็บข้อมูล)"}
    mj = model.to_json(**info)
    os.makedirs(os.path.dirname(os.path.abspath(a.out)), exist_ok=True)
    with open(a.out, "w", encoding="utf-8") as fh:
        json.dump(mj, fh, ensure_ascii=False, indent=2)
    hist = os.path.join(os.path.dirname(os.path.abspath(a.out)), f"model_{name}.json")
    with open(hist, "w", encoding="utf-8") as fh:
        json.dump(mj, fh, ensure_ascii=False, indent=2)
    print(f"\nบันทึกโมเดล: {a.out}")
    print(f"        สำเนา: {hist}")

    if a.upload:
        r = pm.upload_model(a.host, mj)
        if r.get("ok"):
            print(f"ส่งโมเดลเข้านาฬิกา {a.host} แล้ว — โหมดใช้งานจริงจะตัดสินด้วย AI ตัวนี้")
        else:
            print(f"ส่งโมเดลไม่สำเร็จ: {r.get('msg', r)}")
            return 2
    else:
        print("ขั้นต่อไป: อัปโหลด model.json ที่หน้าเว็บนาฬิกา (การ์ด 'โมเดล AI') หรือรันซ้ำด้วย --upload")
    return 0


if __name__ == "__main__":
    sys.exit(main())
