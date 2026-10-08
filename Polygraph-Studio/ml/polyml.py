"""
polyml.py — แกนกลางของ AI ใช้ร่วมกันโดย train.py, datafiles.py, collect.py และนาฬิกาจำลอง (tools/virtual_watch.py)
ใช้แต่ไลบรารีมาตรฐานของ Python — ไม่ต้อง pip install อะไรเพิ่ม

โมเดล: Logistic Regression (มี L2 regularization)
    p(โกหก) = 1 / (1 + exp(-(b + Σ w_i * (x_i - mean_i) / scale_i)))
นาฬิกาคำนวณสูตรเดียวกันนี้เองใน src/lie/ml_model.cpp (คูณ-บวก 12 ครั้งต่อคำถาม)

ทำไมไม่ใช้ RandomForest แบบโค้ดของเพื่อน:
  1) ข้อมูลมีแค่หลักสิบ–ร้อยข้อ โมเดลที่เรียบง่ายกว่า overfit น้อยกว่า
  2) ต้องรันบน ESP32-C3 ได้จริง (ป่า 100 ต้นใส่ในนาฬิกายาก) — train.py มีตัวเลือก --compare-rf
     ไว้เทียบความแม่นยำกับ RandomForest ถ้าเครื่องมี scikit-learn
  3) ค่า w บอกได้ว่าสัญญาณไหนมีผลต่อการตัดสินมากน้อยแค่ไหน (อธิบายอาจารย์ได้)

สมมติฐานของวิธีนี้ (ต้องรู้ข้อจำกัด):
  * log-odds ของการโกหกเป็นฟังก์ชันเส้นตรงของ feature (ความสัมพันธ์ซับซ้อนกว่านี้จะจับไม่ได้)
  * ตัวอย่างเป็นอิสระต่อกัน — ข้อมูลจากคนเดียวกันไม่อิสระจริง จึงประเมินแบบ leave-one-subject-out
    (เทรนด้วยคนอื่น ทดสอบกับคนที่ไม่เคยเห็น) เมื่อมีผู้ถูกทดสอบตั้งแต่ 3 คน
  * z_* กับ d_* ของสัญญาณเดียวกันสัมพันธ์กันสูง (collinear) — L2 ช่วยให้ค่า w ไม่แกว่ง
  * "โกหกตามคำสั่ง" ในโหมดเก็บข้อมูล ไม่เหมือนการโกหกจริงที่มีผลได้เสีย ความแม่นยำที่วัดได้
    จึงเป็นของสถานการณ์ทดลองนี้เท่านั้น
"""
from __future__ import annotations

import json
import math
import os
import random
import time
import urllib.error
import urllib.parse
import urllib.request
from typing import Dict, List, Optional, Sequence, Tuple

SIGNALS = ["gsr", "hr", "amp", "trm", "tmp"]          # ลำดับเดียวกับ lie::F_GSR..F_TMP
FEATURES = ["z_" + s for s in SIGNALS] + ["d_" + s for s in SIGNALS] + ["gsr_ok", "ppg_ok"]
TRAIN_HEADER = ["time", "subject", "qid", "label", "label_name", "quality", "gsr_ok", "ppg_ok"] + \
               ["z_" + s for s in SIGNALS] + ["d_" + s for s in SIGNALS] + ["p_model", "source"]
FEATURE_TH = {
    "z_gsr": "เหงื่อเพิ่มขึ้น (เทียบความแกว่งปกติ)", "z_hr": "ชีพจรเร่ง (เทียบความแกว่งปกติ)",
    "z_amp": "หลอดเลือดปลายนิ้วหดตัว", "z_trm": "มือสั่นมากขึ้น", "z_tmp": "ผิวเย็นลง",
    "d_gsr": "เหงื่อเพิ่มขึ้น (µS)", "d_hr": "ชีพจรเพิ่มขึ้น (bpm)", "d_amp": "แอมพลิจูดชีพจรลดลง (สัดส่วน)",
    "d_trm": "การสั่นเพิ่มขึ้น (m/s²)", "d_tmp": "อุณหภูมิผิวลดลง (°C)",
    "gsr_ok": "มีสัญญาณ GSR", "ppg_ok": "มีสัญญาณชีพจร",
}
MODEL_FORMAT = "polygraph-logreg-v1"
FW_MAX_FEATURES = 16          # ml::kMaxFeatures ในเฟิร์มแวร์
FW_NAME_MAX = 23              # Model::name[24] ในเฟิร์มแวร์


# ============================================================ feature
def features_from(ok_mask: int, z: Sequence, feat: Sequence) -> Dict[str, float]:
    """เหมือน ml::extract() ใน C++ ทุกประการ: สัญญาณที่ใช้ไม่ได้ในข้อนั้น = 0 (ไม่เปลี่ยน)"""
    x: Dict[str, float] = {}
    for i, s in enumerate(SIGNALS):
        ok = (int(ok_mask) >> i) & 1
        x["z_" + s] = float(z[i]) if ok and z[i] is not None else 0.0
        x["d_" + s] = float(feat[i]) if ok and feat[i] is not None else 0.0
    x["gsr_ok"] = 1.0 if int(ok_mask) & 1 else 0.0
    x["ppg_ok"] = 1.0 if int(ok_mask) & 2 else 0.0
    return x


def train_row(x: Dict[str, float], *, epoch: int, subject: str, qid: int, label: int, quality: int,
              p_model: float, source: int) -> List[str]:
    """แถวรูปแบบเดียวกับ /train.csv ในนาฬิกา (makeTrainRow ใน ml_runtime.cpp)"""
    dec = {"d_gsr": 4, "d_hr": 2, "d_amp": 4, "d_trm": 4, "d_tmp": 3}
    row = [str(int(epoch)), clean_subject(subject), str(int(qid)), str(int(label)),
           "lie" if label else "truth", str(int(quality)), str(int(x["gsr_ok"])), str(int(x["ppg_ok"]))]
    row += [f"{x['z_' + s]:.3f}" for s in SIGNALS]
    row += [f"{x['d_' + s]:.{dec['d_' + s]}f}" for s in SIGNALS]
    row += [f"{p_model:.3f}", str(int(source))]
    return row


def clean_subject(s: str) -> str:
    """ชื่อผู้ถูกทดสอบ: ตัดเครื่องหมายที่ทำให้ CSV พัง (เหมือน mlrt::setSubject, ว่าง = "-")"""
    s = "".join(c for c in (s or "") if c not in ',"\r\n').strip()[:20]
    return s or "-"


# ============================================================ ข้อมูล
class Dataset:
    def __init__(self):
        """ชุดข้อมูลเทรน: X (feature), y (0 = จริง, 1 = โกหก), ชื่อผู้ตอบ (ใช้แบ่ง leave-one-subject-out)"""
        self.X: List[List[float]] = []
        self.y: List[int] = []
        self.subject: List[str] = []
        self.p_model: List[Optional[float]] = []
        self.files: List[str] = []
        self.skipped = 0
        self.duplicates = 0

    def __len__(self):
        """จำนวนข้อในชุดข้อมูล"""
        return len(self.y)

    def counts(self) -> Tuple[int, int]:
        """(จำนวนข้อตอบจริง, จำนวนข้อโกหก)"""
        n1 = sum(self.y)
        return len(self.y) - n1, n1


def _f(v) -> Optional[float]:
    """แปลงเป็นตัวเลข float — อ่านไม่ได้/NaN/อนันต์ = None"""
    try:
        x = float(v)
        return x if math.isfinite(x) else None
    except (TypeError, ValueError):
        return None


# หมายเหตุ: การอ่านไฟล์ข้อมูลเทรนย้ายไปอยู่ที่ datafiles.py + train.build_dataset() แล้ว
# (อ่านเฉพาะ data/result_*.csv รูปแบบเดียว) — ไฟล์นี้เหลือแต่ส่วนคณิตศาสตร์และการคุยกับนาฬิกา


# ============================================================ คณิตศาสตร์
def sigmoid(z: float) -> float:
    if z > 30:
        return 1.0
    if z < -30:
        return 0.0
    return 1.0 / (1.0 + math.exp(-z))


def standardize_fit(X: List[List[float]]) -> Tuple[List[float], List[float]]:
    """หาค่าเฉลี่ยและส่วนเบี่ยงเบนของแต่ละ feature (ไว้แปลงทุกค่าให้อยู่สเกลเดียวกันก่อนเทรน)"""
    n, d = len(X), len(X[0])
    mean = [sum(r[j] for r in X) / n for j in range(d)]
    scale = []
    for j in range(d):
        var = sum((r[j] - mean[j]) ** 2 for r in X) / n
        sd = math.sqrt(var)
        scale.append(sd if sd > 1e-6 else 1.0)    # feature คงที่ (เช่น gsr_ok = 1 ทุกแถว) -> หาร 1
    return mean, scale


def _solve(A: List[List[float]], b: List[float]) -> List[float]:
    """แก้ Ax = b ด้วย Gaussian elimination + partial pivoting (A ขนาดเล็ก ≤ 17x17)"""
    n = len(b)
    M = [row[:] + [b[i]] for i, row in enumerate(A)]
    for c in range(n):
        p = max(range(c, n), key=lambda r: abs(M[r][c]))
        if abs(M[p][c]) < 1e-12:
            raise ArithmeticError("singular matrix")
        M[c], M[p] = M[p], M[c]
        for r in range(c + 1, n):
            f = M[r][c] / M[c][c]
            if f:
                for k in range(c, n + 1):
                    M[r][k] -= f * M[c][k]
    x = [0.0] * n
    for r in range(n - 1, -1, -1):
        x[r] = (M[r][n] - sum(M[r][k] * x[k] for k in range(r + 1, n))) / M[r][r]
    return x


def fit_logreg(Z: List[List[float]], y: List[int], lam: float = 1.0, iters: int = 60) -> Tuple[float, List[float]]:
    """Logistic regression ด้วย Newton's method (IRLS) บนข้อมูลที่ standardize แล้ว

    loss = Σ logloss + (lam/2)·||w||²   (ไม่ลงโทษ bias)
    lam > 0 ทำให้ Hessian หาอินเวอร์สได้เสมอ แม้ข้อมูลแยกกันได้สมบูรณ์ (ไม่งั้น w จะวิ่งไปอนันต์)
    """
    d = len(Z[0])
    beta = [0.0] * (d + 1)
    for _ in range(iters):
        g = [0.0] * (d + 1)
        H = [[0.0] * (d + 1) for _ in range(d + 1)]
        for xi, yi in zip(Z, y):
            row = [1.0] + xi
            p = sigmoid(sum(bk * rk for bk, rk in zip(beta, row)))
            wgt = max(p * (1.0 - p), 1e-9)
            err = p - yi
            for a in range(d + 1):
                g[a] += err * row[a]
                ra = row[a] * wgt
                Ha = H[a]
                for c in range(a, d + 1):
                    Ha[c] += ra * row[c]
        for a in range(d + 1):
            for c in range(a):
                H[a][c] = H[c][a]
        for a in range(1, d + 1):
            g[a] += lam * beta[a]
            H[a][a] += lam
        step = _solve(H, g)
        beta = [bk - sk for bk, sk in zip(beta, step)]
        if max(abs(s) for s in step) < 1e-9:
            break
    return beta[0], beta[1:]


class Model:
    """โมเดล Logistic Regression ที่เทรนแล้ว: features, mean, scale, w, b (ค่าชุดเดียวกับที่ส่งเข้านาฬิกา)"""
    def __init__(self, features: Sequence[str], mean, scale, w, b, lam=1.0):
        """สร้างจากค่าที่รู้แล้ว (ใช้ตอนอ่าน model.json หรือหลังเทรน)"""
        self.features, self.mean, self.scale, self.w, self.b, self.lam = list(features), list(mean), list(scale), list(w), float(b), lam

    @classmethod
    def fit(cls, X: List[List[float]], y: List[int], features: Sequence[str], lam: float = 1.0) -> "Model":
        """เทรน: standardize ทุก feature แล้วหา w, b ด้วย Newton's method (fit_logreg)"""
        mean, scale = standardize_fit(X)
        Z = [[(v - m) / s for v, m, s in zip(r, mean, scale)] for r in X]
        b, w = fit_logreg(Z, y, lam)
        return cls(features, mean, scale, w, b, lam)

    def prob_row(self, row: Sequence[float]) -> float:
        """p(โกหก) ของ 1 แถว = sigmoid(b + Σ w·(x − mean)/scale) — สูตรเดียวกับ ml::predict() ในนาฬิกา"""
        z = self.b + sum(w * (v - m) / s for w, v, m, s in zip(self.w, row, self.mean, self.scale))
        return sigmoid(z)

    def prob(self, x: Dict[str, float]) -> float:
        """p(โกหก) จาก dict ชื่อ feature -> ค่า"""
        return self.prob_row([x[k] for k in self.features])

    # ---------------- ไฟล์ model.json ----------------
    def to_json(self, **info) -> dict:
        d = {"format": MODEL_FORMAT, "features": self.features,
             "mean": [round(v, 6) for v in self.mean], "scale": [round(v, 6) for v in self.scale],
             "w": [round(v, 6) for v in self.w], "b": round(self.b, 6), "lambda": self.lam}
        d.update(info)
        return d

    @classmethod
    def from_json(cls, d: dict) -> "Model":
        """อ่านโมเดลจาก model.json พร้อมตรวจว่าใช้กับนาฬิกาได้ (รูปแบบ, จำนวนค่า, ชื่อ feature)"""
        if d.get("format") not in (None, MODEL_FORMAT):
            raise ValueError("ไม่รู้จักรูปแบบโมเดล " + str(d.get("format")))
        n = len(d["features"])
        if not (len(d["mean"]) == len(d["scale"]) == len(d["w"]) == n):
            raise ValueError("จำนวน mean/scale/w ไม่เท่ากับจำนวน feature")
        unknown = [f for f in d["features"] if f not in FEATURES]
        if unknown:
            raise ValueError("feature ที่นาฬิกาไม่รู้จัก: " + ", ".join(unknown))
        return cls(d["features"], d["mean"], d["scale"], d["w"], d["b"], d.get("lambda", 1.0))


# ============================================================ การประเมิน
def stratified_folds(y: List[int], k: int, seed: int = 7) -> List[List[int]]:
    rnd = random.Random(seed)
    folds: List[List[int]] = [[] for _ in range(k)]
    for cls_ in (0, 1):
        idx = [i for i, v in enumerate(y) if v == cls_]
        rnd.shuffle(idx)
        for j, i in enumerate(idx):
            folds[j % k].append(i)
    return [f for f in folds if f]


def cv_splits(ds: Dataset, k: int = 5) -> Tuple[str, List[List[int]]]:
    """leave-one-subject-out ถ้ามี ≥3 คน (วัดความแม่นยำกับ "คนใหม่"), ไม่งั้น stratified k-fold"""
    subs = sorted(set(ds.subject))
    if len(subs) >= 3:
        return "leave-one-subject-out", [[i for i, s in enumerate(ds.subject) if s == sub] for sub in subs]
    n0, n1 = ds.counts()
    k = max(2, min(k, n0, n1))
    return f"stratified {k}-fold", stratified_folds(ds.y, k)


def metrics(y: List[int], p: List[float], thr: float = 0.5) -> dict:
    """วัดผลการทำนาย: ความแม่นยำ (+ ช่วงเชื่อมั่น 95%), sensitivity, specificity, logloss, ตาราง confusion"""
    tp = sum(1 for a, b in zip(y, p) if a == 1 and b >= thr)
    fn = sum(1 for a, b in zip(y, p) if a == 1 and b < thr)
    tn = sum(1 for a, b in zip(y, p) if a == 0 and b < thr)
    fp = sum(1 for a, b in zip(y, p) if a == 0 and b >= thr)
    n = len(y)
    acc = (tp + tn) / n if n else 0.0
    sens = tp / (tp + fn) if tp + fn else 0.0     # จับคนโกหกได้กี่ %
    spec = tn / (tn + fp) if tn + fp else 0.0     # คนพูดจริงไม่ถูกกล่าวหากี่ %
    eps = 1e-6
    ll = -sum(a * math.log(max(b, eps)) + (1 - a) * math.log(max(1 - b, eps)) for a, b in zip(y, p)) / n if n else 0.0
    lo, hi = wilson(tp + tn, n)
    return {"n": n, "accuracy": acc, "ci95": [lo, hi], "balanced_accuracy": (sens + spec) / 2,
            "sensitivity": sens, "specificity": spec, "logloss": ll,
            "confusion": {"tp": tp, "fn": fn, "fp": fp, "tn": tn}}


def wilson(k: int, n: int, z: float = 1.96) -> Tuple[float, float]:
    """ช่วงความเชื่อมั่น 95% ของสัดส่วน (Wilson score interval) — ข้อมูลน้อย ช่วงจะกว้าง"""
    if n == 0:
        return 0.0, 0.0
    p = k / n
    den = 1 + z * z / n
    c = (p + z * z / (2 * n)) / den
    h = z * math.sqrt(p * (1 - p) / n + z * z / (4 * n * n)) / den
    return max(0.0, c - h), min(1.0, c + h)


LAMBDAS = [0.1, 0.3, 1.0, 3.0, 10.0]


def choose_lambda(X, y, features, idx: List[int], k: int = 4) -> float:
    """เลือก lam ด้วย CV ภายในชุดเทรนเท่านั้น (nested CV — ไม่แอบดูชุดทดสอบ)"""
    yy = [y[i] for i in idx]
    if min(sum(yy), len(yy) - sum(yy)) < 2:
        return 1.0
    folds = stratified_folds(yy, max(2, min(k, sum(yy), len(yy) - sum(yy))), seed=11)
    best, best_ll = 1.0, float("inf")
    for lam in LAMBDAS:
        ll, n = 0.0, 0
        for f in folds:
            test = set(f)
            tr = [idx[i] for i in range(len(idx)) if i not in test]
            te = [idx[i] for i in f]
            if len(set(y[i] for i in tr)) < 2:
                continue
            m = Model.fit([X[i] for i in tr], [y[i] for i in tr], features, lam)
            for i in te:
                p = min(max(m.prob_row(X[i]), 1e-6), 1 - 1e-6)
                ll -= y[i] * math.log(p) + (1 - y[i]) * math.log(1 - p)
                n += 1
        if n and ll / n < best_ll:
            best, best_ll = lam, ll / n
    return best


def cross_validate(ds: Dataset, features: Sequence[str]) -> Tuple[str, List[float], List[float]]:
    """คืน (ชื่อวิธี, p ที่ทำนายแบบ out-of-fold ของทุกแถว, lam ที่เลือกในแต่ละ fold)"""
    method, folds = cv_splits(ds)
    p = [0.5] * len(ds)
    lams = []
    for f in folds:
        test = set(f)
        tr = [i for i in range(len(ds)) if i not in test]
        if len(set(ds.y[i] for i in tr)) < 2:      # fold นี้ชุดเทรนมีคลาสเดียว -> เดา 0.5
            continue
        lam = choose_lambda(ds.X, ds.y, features, tr)
        lams.append(lam)
        m = Model.fit([ds.X[i] for i in tr], [ds.y[i] for i in tr], features, lam)
        for i in f:
            p[i] = m.prob_row(ds.X[i])
    return method, p, lams


# ============================================================ คุยกับนาฬิกา (HTTP)
def _opener():
    # ไม่ใช้ proxy ของระบบ: Windows บางเครื่องตั้ง proxy ไว้ ทำให้ส่งไป 192.168.4.1 ไม่ถึง
    return urllib.request.build_opener(urllib.request.ProxyHandler({}))


def http_json(host: str, path: str, data: Optional[dict] = None, timeout: float = 5.0) -> dict:
    """เรียก REST API ของนาฬิกา (GET หรือ POST แบบฟอร์ม) แล้วคืน JSON — ต่อไม่ติดก็คืน {ok: False} ไม่ล้ม"""
    url = f"http://{host}{path}"
    body = urllib.parse.urlencode(data).encode() if data is not None else None
    req = urllib.request.Request(url, data=body, method="POST" if data is not None else "GET",
                                 headers={"Content-Type": "application/x-www-form-urlencoded"} if body else {})
    try:
        with _opener().open(req, timeout=timeout) as r:
            return json.loads(r.read().decode("utf-8") or "{}")
    except urllib.error.HTTPError as e:          # นาฬิกาตอบ 4xx/5xx พร้อมข้อความไทย
        try:
            return json.loads(e.read().decode("utf-8"))
        except ValueError:
            return {"ok": False, "msg": f"HTTP {e.code}"}
    except (urllib.error.URLError, OSError, ValueError) as e:   # ต่อไม่ติด/หมดเวลา -> ไม่ให้โปรแกรมล้ม
        return {"ok": False, "msg": f"ติดต่อนาฬิกาไม่ได้ ({getattr(e, 'reason', e)})"}


def http_post(host: str, path: str, timeout: float = 5.0) -> dict:
    """POST เปล่า ๆ (ค่าอยู่ใน query string แล้ว)"""
    return http_json(host, path, data={}, timeout=timeout)


def upload_model(host: str, model_json: dict) -> dict:
    """ส่งโมเดล (model.json) เข้านาฬิกาที่ /api/ml/model — นาฬิกาตรวจแล้วเก็บลง NVS (อยู่ถาวรแม้ปิดเครื่อง)"""
    g = lambda v: "%.7g" % v   # noqa: E731  ความละเอียดพอสำหรับ float 32 บิตในนาฬิกา
    form = {"features": ",".join(model_json["features"]),
            "mean": ",".join(g(v) for v in model_json["mean"]),
            "scale": ",".join(g(v) for v in model_json["scale"]),
            "w": ",".join(g(v) for v in model_json["w"]), "b": g(model_json["b"]),
            "acc": g(model_json.get("cv_accuracy", 0.0)), "samples": str(int(model_json.get("samples", 0))),
            "trained": str(int(model_json.get("trained_at", 0))),
            "name": str(model_json.get("name", "model"))[:FW_NAME_MAX]}
    return http_json(host, "/api/ml/model", data=form)


def download_csv(host: str, out_path: str, timeout: float = 20.0) -> int:
    """ดาวน์โหลดข้อมูลที่นาฬิกาบันทึกเอง (/api/ml/data.csv) ลงไฟล์ (คืนจำนวนไบต์)"""
    with _opener().open(f"http://{host}/api/ml/data.csv", timeout=timeout) as r:
        data = r.read()
    os.makedirs(os.path.dirname(os.path.abspath(out_path)), exist_ok=True)
    with open(out_path, "wb") as fh:
        fh.write(data)
    return len(data)


def now_epoch() -> int:
    """เวลาปัจจุบันแบบ epoch (วินาที)"""
    return int(time.time())
