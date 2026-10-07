"""
lie_engine.py — พอร์ต Python ของ LieEngine ในเฟิร์มแวร์ (src/lie/lie_engine.cpp) แบบบรรทัดต่อบรรทัด

ใช้ใน:
  * นาฬิกาจำลอง (tools/virtual_watch.py) -> สาธิตระบบได้แม้ไม่มีฮาร์ดแวร์
  * tests/test_parity.py -> ตรวจว่าให้ผลตรงกับเวอร์ชัน C++ บนนาฬิกาจริง

หลักการ (ย่อ): baseline -> null distribution -> z-score ของ 5 สัญญาณ -> คะแนนถ่วงน้ำหนัก
-> logistic -> P(โกหก) -> ตัดสิน   (อธิบายเต็มอยู่ในคอมเมนต์ของ lie_engine.h)

ถ้าแก้สูตรที่นี่ ต้องแก้ใน C++ ด้วย และรัน tests/test_parity.py ใหม่
"""
from __future__ import annotations

import math
from dataclasses import dataclass, field
from typing import List, Optional

F_GSR, F_HR, F_AMP, F_TRM, F_TMP, F_COUNT = 0, 1, 2, 3, 4, 5
FEATURE_KEYS = ["gsr", "hr", "amp", "trm", "tmp"]

STATE_IDLE, STATE_BASELINE, STATE_READY, STATE_QUESTION = 0, 1, 2, 3
STATE_NAMES = ["idle", "baseline", "ready", "question"]

KIND_TEST, KIND_TRUTH, KIND_LIE, KIND_WARMUP = 0, 1, 2, 3
KIND_NAMES = ["test", "truth", "lie", "warmup"]

V_NONE, V_TRUTH, V_LIE, V_INCONCLUSIVE, V_INVALID = 0, 1, 2, 3, 4
VERDICT_NAMES = ["none", "truth", "lie", "inconclusive", "invalid"]

R_GSR_CONTACT, R_PPG_CONTACT, R_MOTION, R_SHORT_PRE = 1, 2, 4, 8

DEFAULT_WEIGHT = [0.20, 0.35, 0.25, 0.12, 0.08]   # GSR, HR, AMP, TRM, TMP (v2.1: GSR ไม่ใช่ตัวหลัก)
K_MAX_BASE, K_MAX_WIN, K_MAX_PRE, K_HIST, K_MAX_RES, K_MAX_CTL = 450, 100, 25, 64, 24, 12


def clampf(x: float, lo: float, hi: float) -> float:
    return lo if x < lo else (hi if x > hi else x)


def sigmoid(x: float) -> float:
    if x > 30.0:
        return 1.0
    if x < -30.0:
        return 0.0
    return 1.0 / (1.0 + math.exp(-x))


def isnan(x) -> bool:
    return x is None or (isinstance(x, float) and math.isnan(x))


class Stat:
    """Welford mean/sd — สูตรเดียวกับ C++"""
    __slots__ = ("n", "mean", "m2")

    def __init__(self):
        self.n, self.mean, self.m2 = 0, 0.0, 0.0

    def add(self, x: float):
        self.n += 1
        d = x - self.mean
        self.mean += d / self.n
        self.m2 += d * (x - self.mean)

    def sd(self) -> float:
        return math.sqrt(self.m2 / (self.n - 1)) if self.n > 1 else 0.0


@dataclass
class Frame:
    gsr: float = 0.0
    hr: float = 0.0
    amp: float = 0.0
    tremor: float = 0.0
    motion: float = 0.0
    temp: float = float("nan")
    gsrOk: bool = False
    ppgOk: bool = False


@dataclass
class Config:
    frameHz: float = 5.0
    baselineSec: float = 30.0
    preSec: float = 3.0
    windowSec: float = 12.0
    recoverySec: float = 8.0
    weight: List[float] = field(default_factory=lambda: list(DEFAULT_WEIGHT))
    sdFloor: List[float] = field(default_factory=lambda: [0.05, 1.5, 0.04, 0.02, 0.03])
    s0: float = 2.0
    k: float = 1.5
    lieP: float = 0.65
    truthP: float = 0.35
    motionLimit: float = 1.5
    minGsrFrac: float = 0.8
    minPpgFrac: float = 0.6
    maxMotionFrac: float = 0.3


@dataclass
class Baseline:
    valid: bool = False
    mean: List[float] = field(default_factory=lambda: [0.0] * F_COUNT)
    sd: List[float] = field(default_factory=lambda: [0.0] * F_COUNT)
    nullMean: List[float] = field(default_factory=lambda: [0.0] * F_COUNT)
    nullSd: List[float] = field(default_factory=lambda: [0.0] * F_COUNT)
    nullN: int = 0
    durationSec: float = 0.0
    gsrFrac: float = 0.0
    ppgFrac: float = 0.0


@dataclass
class Calibration:
    nTruth: int = 0
    nLie: int = 0
    active: bool = False
    weak: bool = False
    meanTruth: float = 0.0
    meanLie: float = 0.0
    separation: float = 0.0
    s0: float = 0.0
    k: float = 0.0
    weight: List[float] = field(default_factory=lambda: [0.0] * F_COUNT)
    correct: int = 0
    scored: int = 0


@dataclass
class Result:
    seq: int = 0
    qid: int = 0
    kind: int = KIND_TEST
    verdict: int = V_NONE
    expected: int = V_NONE
    correct: bool = False
    quality: int = 0
    reasons: int = 0
    pLie: float = 0.0
    score: float = 0.0
    source: int = 0          # 0 สูตรมาตรฐาน, 1 สูตร+calibration, 2 โมเดล AI
    feat: List[float] = field(default_factory=lambda: [0.0] * F_COUNT)
    z: List[float] = field(default_factory=lambda: [0.0] * F_COUNT)
    pre: List[float] = field(default_factory=lambda: [float("nan")] * F_COUNT)
    okMask: int = 0
    peakLatency: float = -1.0
    answerAt: float = -1.0
    answerYes: int = -1
    tStart: float = 0.0

    def to_json(self) -> dict:
        """รูปแบบเดียวกับ /api/lie ของเฟิร์มแวร์"""
        def num(x, d):
            return None if isnan(x) or math.isinf(x) else round(x, d)
        return {
            "seq": self.seq, "qid": self.qid, "kind": KIND_NAMES[self.kind],
            "verdict": VERDICT_NAMES[self.verdict], "expected": VERDICT_NAMES[self.expected],
            "correct": self.correct, "p": num(self.pLie, 3), "score": num(self.score, 2),
            "src": self.source,          # ชื่อคีย์เดียวกับเฟิร์มแวร์ (/api/lie)
            "quality": self.quality, "reasons": self.reasons, "ok": self.okMask,
            "feat": [num(x, 4) for x in self.feat], "z": [num(x, 2) for x in self.z],
            "pre": [num(x, 3) for x in self.pre], "latency": num(self.peakLatency, 1),
            "answerAt": num(self.answerAt, 1), "answerYes": self.answerYes,
            "tStart": num(self.tStart, 1),
        }


class _Features:
    __slots__ = ("f", "ok", "pre", "gsrFrac", "ppgFrac", "motionFrac", "peakLatency", "preOk")

    def __init__(self):
        self.f = [0.0] * F_COUNT
        self.ok = [False] * F_COUNT
        self.pre = [float("nan")] * F_COUNT
        self.gsrFrac = self.ppgFrac = self.motionFrac = 0.0
        self.peakLatency = -1.0
        self.preOk = False


class Engine:
    def __init__(self, cfg: Optional[Config] = None):
        self.cfg = Config()
        self.state = STATE_IDLE
        self.frames = 0
        self.phaseStart = 0
        self.baseTarget = self.winTarget = self.preFrames = 0
        self.baseBuf: List[Frame] = []
        self.winBuf: List[Frame] = []
        self.preBuf: List[Frame] = []
        self.hist: List[Frame] = []      # ใหม่สุดอยู่ท้าย (เก็บไม่เกิน K_HIST)
        self.qid = 0
        self.kind = KIND_TEST
        self.answerAt = -1.0
        self.answerYes = -1
        self.base = Baseline()
        self.cal = Calibration(weight=list(self.cfg.weight))
        self.ctl: List[tuple] = []        # (lie: bool, okMask: int, z: list)
        self.res: List[Result] = []       # ผลในเซสชันนี้ (ใหม่สุดท้ายรายการ)
        self.resultSeq = 0
        self.firstSeq = 0
        self.refGsr = 0.0
        self.stress = -1.0
        self.settled = False
        self.lastWindowEnd = 0
        self.revision = 0
        self.err = ""
        self.scorer = None           # ฟังก์ชัน (result) -> P(โกหก) หรือ None (เหมือน setScorer ใน C++)
        self.configure(cfg or Config())

    # ------------------------------------------------------------------ config
    def configure(self, c: Config):
        import copy
        c = copy.deepcopy(c)
        c.frameHz = clampf(c.frameHz, 1.0, 5.0)
        c.preSec = clampf(c.preSec, 1.0, K_MAX_PRE / c.frameHz)
        c.windowSec = clampf(c.windowSec, 6.0, K_MAX_WIN / c.frameHz)
        min_base = c.preSec + c.windowSec + 3.0
        c.baselineSec = clampf(c.baselineSec, 20.0 if min_base < 20.0 else min_base, K_MAX_BASE / c.frameHz)
        c.recoverySec = clampf(c.recoverySec, 0.0, 60.0)
        ws = 0.0
        for i in range(F_COUNT):
            if not (c.weight[i] >= 0.0):
                c.weight[i] = 0.0
            ws += c.weight[i]
            if not (c.sdFloor[i] > 1e-4):
                c.sdFloor[i] = 1e-4
        c.weight = [(w / ws) if ws > 0 else DEFAULT_WEIGHT[i] for i, w in enumerate(c.weight)]
        c.k = clampf(c.k, 0.1, 10.0)
        c.s0 = clampf(c.s0, -5.0, 10.0)
        c.lieP = clampf(c.lieP, 0.5, 0.99)
        c.truthP = clampf(c.truthP, 0.01, 0.5)
        c.motionLimit = clampf(c.motionLimit, 0.1, 20.0)
        c.minGsrFrac = clampf(c.minGsrFrac, 0.0, 1.0)
        c.minPpgFrac = clampf(c.minPpgFrac, 0.0, 1.0)
        c.maxMotionFrac = clampf(c.maxMotionFrac, 0.0, 1.0)
        self.cfg = c
        if not self.cal.active:
            self.cal.weight = list(c.weight)
        self.revision += 1

    # ------------------------------------------------------------------ commands
    def start_baseline(self, sec: float = 0.0) -> bool:
        if self.state == STATE_QUESTION:
            self.err = "QUESTION_ACTIVE"
            return False
        c = self.cfg
        s = sec if sec > 0 else c.baselineSec
        min_base = c.preSec + c.windowSec + 3.0
        s = clampf(s, 20.0 if min_base < 20.0 else min_base, K_MAX_BASE / c.frameHz)
        self.baseTarget = int(s * c.frameHz + 0.5)
        self.baseBuf = []
        self.base.valid = False
        self.ctl = []
        self.cal = Calibration(weight=list(c.weight))
        self.stress = -1.0
        self.state = STATE_BASELINE
        self.phaseStart = self.frames
        self.err = ""
        self.revision += 1
        return True

    def start_question(self, qid: int, kind: int) -> bool:
        if self.state != STATE_READY:
            self.err = {STATE_QUESTION: "QUESTION_ACTIVE", STATE_BASELINE: "BASELINE_RUNNING"}.get(self.state, "NO_BASELINE")
            return False
        c = self.cfg
        self.preFrames = max(1, min(K_MAX_PRE, int(c.preSec * c.frameHz + 0.5)))
        self.winTarget = max(1, min(K_MAX_WIN, int(c.windowSec * c.frameHz + 0.5)))
        n = min(len(self.hist), self.preFrames)
        self.preBuf = self.hist[len(self.hist) - n:] if n else []
        self.winBuf = []
        self.qid, self.kind = qid, kind
        self.answerAt, self.answerYes = -1.0, -1
        self.state = STATE_QUESTION
        self.phaseStart = self.frames
        self.err = ""
        self.revision += 1
        return True

    def mark_answer(self, yes: bool) -> bool:
        if self.state != STATE_QUESTION:
            self.err = "NO_QUESTION"
            return False
        self.answerAt = (self.frames - self.phaseStart) / self.cfg.frameHz
        self.answerYes = 1 if yes else 0
        self.revision += 1
        return True

    def abort(self):
        if self.state == STATE_BASELINE:
            self.state = STATE_IDLE
        elif self.state == STATE_QUESTION:
            self.state = STATE_READY
        self.lastWindowEnd = self.frames
        self.revision += 1

    def reset_session(self):
        self.state = STATE_IDLE
        self.base.valid = False
        self.ctl = []
        self.cal = Calibration(weight=list(self.cfg.weight))
        self.firstSeq = self.resultSeq
        self.res = []
        self.stress = -1.0
        self.settled = False
        self.err = ""
        self.revision += 1

    # ------------------------------------------------------------------ data
    def push(self, f: Frame):
        self.frames += 1
        self.hist.append(f)
        if len(self.hist) > K_HIST:
            self.hist.pop(0)
        if self.state == STATE_BASELINE:
            if len(self.baseBuf) < K_MAX_BASE:
                self.baseBuf.append(f)
            if len(self.baseBuf) >= self.baseTarget:
                self._finish_baseline()
        elif self.state == STATE_QUESTION:
            if len(self.winBuf) < K_MAX_WIN:
                self.winBuf.append(f)
            if len(self.winBuf) >= self.winTarget:
                self._finish_question()
        self._update_stress(f)
        self._update_settled()

    def time(self) -> float:
        return self.frames / self.cfg.frameHz

    # ------------------------------------------------------------------ features
    def _compute_features(self, pre: List[Frame], win: List[Frame]) -> _Features:
        c = self.cfg
        o = _Features()
        nPre, nWin = len(pre), len(win)
        o.preOk = nPre > 0
        sG = sH = sA = sT = sP = 0.0
        cG = cH = cA = cT = cP = 0
        for f in pre:
            if f.gsrOk:
                sG += f.gsr; cG += 1
            if f.ppgOk and f.hr > 0.0:
                sH += f.hr; cH += 1
                if f.amp > 0.0:
                    sA += f.amp; cA += 1
            sT += f.tremor; cT += 1
            if not isnan(f.temp):
                sP += f.temp; cP += 1
        half = (nPre + 1) // 2
        nan = float("nan")
        preG = sG / cG if cG else nan
        preH = sH / cH if cH else nan
        preA = sA / cA if cA else nan
        preT = sT / cT if cT else nan
        preP = sP / cP if cP else nan
        o.pre = [preG, preH, preA, preT, preP]
        if nWin <= 0:
            return o
        maxG, maxH, minA, minP, sumT = -1e9, -1e9, 1e9, 1e9, 0.0
        maxGi = -1
        wG = wH = wA = wT = wP = mot = 0
        for j, f in enumerate(win):
            if f.gsrOk:
                wG += 1
                if f.gsr > maxG:
                    maxG, maxGi = f.gsr, j
            if f.ppgOk and f.hr > 0.0:
                wH += 1
                if f.hr > maxH:
                    maxH = f.hr
                if f.amp > 0.0:
                    wA += 1
                    if f.amp < minA:
                        minA = f.amp
            sumT += f.tremor; wT += 1
            if not isnan(f.temp):
                wP += 1
                if f.temp < minP:
                    minP = f.temp
            if f.motion > c.motionLimit:
                mot += 1
        o.gsrFrac = wG / nWin
        o.ppgFrac = wH / nWin
        o.motionFrac = mot / nWin
        if cG > 0 and cG >= half and o.gsrFrac >= c.minGsrFrac:
            o.f[F_GSR] = maxG - preG; o.ok[F_GSR] = True; o.peakLatency = maxGi / c.frameHz
        if cH > 0 and cH >= half and o.ppgFrac >= c.minPpgFrac:
            o.f[F_HR] = maxH - preH; o.ok[F_HR] = True
        if cA > 0 and cA >= half and preA > 1.0 and wA / nWin >= c.minPpgFrac:
            o.f[F_AMP] = (preA - minA) / preA; o.ok[F_AMP] = True
        if cT > 0 and wT > 0:
            o.f[F_TRM] = sumT / wT - preT; o.ok[F_TRM] = True
        if cP > 0 and cP >= half and wP >= (nWin + 1) // 2:
            o.f[F_TMP] = preP - minP; o.ok[F_TMP] = True
        return o

    @staticmethod
    def _score_of(z, ok_mask: int, w) -> float:
        s = ws = 0.0
        for i in range(F_COUNT):
            if ok_mask & (1 << i):
                s += w[i] * clampf(z[i], -2.0, 6.0)
                ws += w[i]
        return s / ws if ws > 0 else 0.0

    # ------------------------------------------------------------------ baseline
    def _finish_baseline(self):
        c = self.cfg
        n = len(self.baseBuf)
        st = [Stat() for _ in range(F_COUNT)]
        gsrCnt = ppgCnt = 0
        for f in self.baseBuf:
            if f.gsrOk:
                st[F_GSR].add(f.gsr); gsrCnt += 1
            if f.ppgOk and f.hr > 0.0:
                st[F_HR].add(f.hr); ppgCnt += 1
                if f.amp > 0.0:
                    st[F_AMP].add(f.amp)
            st[F_TRM].add(f.tremor)
            if not isnan(f.temp):
                st[F_TMP].add(f.temp)
        b = self.base
        b.durationSec = n / c.frameHz
        b.gsrFrac = gsrCnt / n if n else 0.0
        b.ppgFrac = ppgCnt / n if n else 0.0
        if b.gsrFrac < 0.7 and b.ppgFrac < 0.7:
            self.err = "BASELINE_NO_SIGNAL"
            b.valid = False
            self.state = STATE_IDLE
            self.revision += 1
            return
        b.mean = [s.mean for s in st]
        b.sd = [s.sd() for s in st]
        P = max(1, min(K_MAX_PRE, int(c.preSec * c.frameHz + 0.5)))
        W = max(1, min(K_MAX_WIN, int(c.windowSec * c.frameHz + 0.5)))
        step = max(1, int(c.frameHz + 0.5))
        ns = [Stat() for _ in range(F_COUNT)]
        windows = 0
        s = P
        while s + W <= n:
            fe = self._compute_features(self.baseBuf[s - P:s], self.baseBuf[s:s + W])
            windows += 1
            for i in range(F_COUNT):
                if fe.ok[i]:
                    ns[i].add(fe.f[i])
            s += step
        b.nullN = windows
        for i in range(F_COUNT):
            if ns[i].n >= 3:
                b.nullMean[i] = ns[i].mean
                b.nullSd[i] = ns[i].sd()
            else:
                b.nullMean[i] = 0.0
                b.nullSd[i] = 2.0 * b.sd[i]
        b.valid = True
        self.refGsr = b.mean[F_GSR]
        self.state = STATE_READY
        self.lastWindowEnd = self.frames
        self.err = ""
        self.revision += 1

    # ------------------------------------------------------------------ question
    def _finish_question(self):
        c = self.cfg
        fe = self._compute_features(self.preBuf, self.winBuf)
        self.resultSeq += 1
        r = Result(seq=self.resultSeq, qid=self.qid, kind=self.kind,
                   tStart=self.phaseStart / c.frameHz, answerAt=self.answerAt,
                   answerYes=self.answerYes,
                   peakLatency=fe.peakLatency if fe.ok[F_GSR] else -1.0)
        mask = 0
        for i in range(F_COUNT):
            r.pre[i] = fe.pre[i]
            if fe.ok[i]:
                sd = self.base.nullSd[i] if self.base.nullSd[i] > c.sdFloor[i] else c.sdFloor[i]
                r.feat[i] = fe.f[i]
                r.z[i] = (fe.f[i] - self.base.nullMean[i]) / sd
                mask |= 1 << i
        r.okMask = mask
        w = self.cal.weight if self.cal.active else c.weight
        s0 = self.cal.s0 if self.cal.active else c.s0
        k = self.cal.k if self.cal.active else c.k
        r.score = self._score_of(r.z, mask, w)
        r.pLie = sigmoid(k * (r.score - s0))
        r.source = 1 if self.cal.active else 0
        if self.scorer is not None:
            p_ext = self.scorer(r)
            if p_ext is not None:
                r.pLie = clampf(float(p_ext), 0.0, 1.0)
                r.source = 2
        reasons = 0
        if not fe.ok[F_GSR]:
            reasons |= R_GSR_CONTACT
        if not fe.ok[F_HR]:
            reasons |= R_PPG_CONTACT
        if fe.motionFrac > c.maxMotionFrac:
            reasons |= R_MOTION
        need_pre = self.preFrames // 2 if self.preFrames // 2 > 1 else 1
        if len(self.preBuf) < need_pre:
            reasons |= R_SHORT_PRE
        r.reasons = reasons
        r.quality = int(clampf(100.0 * (0.3 * fe.gsrFrac + 0.5 * fe.ppgFrac + 0.2 * (1.0 - fe.motionFrac)) + 0.5, 0.0, 100.0))
        no_main = (not fe.ok[F_GSR]) and (not fe.ok[F_HR])
        if no_main or (reasons & (R_MOTION | R_SHORT_PRE)):
            r.verdict = V_INVALID
        elif r.pLie >= c.lieP:
            r.verdict = V_LIE
        elif r.pLie <= c.truthP:
            r.verdict = V_TRUTH
        else:
            r.verdict = V_INCONCLUSIVE
        if self.kind == KIND_TRUTH:
            r.expected = V_TRUTH
        elif self.kind == KIND_LIE:
            r.expected = V_LIE
        if r.expected != V_NONE and r.verdict != V_INVALID:
            self.cal.scored += 1
            if r.verdict == r.expected:
                self.cal.correct += 1
                r.correct = True
        self.res.append(r)
        if len(self.res) > K_MAX_RES:
            self.res.pop(0)
        if self.kind in (KIND_TRUTH, KIND_LIE) and r.verdict != V_INVALID:
            self.ctl.append((self.kind == KIND_LIE, mask, list(r.z)))
            if len(self.ctl) > K_MAX_CTL:
                self.ctl.pop(0)
            self._recalibrate()
        self.state = STATE_READY
        self.lastWindowEnd = self.frames
        self.revision += 1

    def _recalibrate(self):
        c, cal = self.cfg, self.cal
        nT = sum(1 for x in self.ctl if not x[0])
        nL = sum(1 for x in self.ctl if x[0])
        cal.nTruth, cal.nLie = nT, nL
        if nT == 0 or nL == 0:
            cal.active = False
            cal.weak = False
            cal.meanTruth = cal.meanLie = cal.separation = 0.0
            cal.weight = list(c.weight)
            return
        zT = [0.0] * F_COUNT; zL = [0.0] * F_COUNT
        cT = [0] * F_COUNT; cL = [0] * F_COUNT
        for lie, mask, z in self.ctl:
            for i in range(F_COUNT):
                if not (mask & (1 << i)):
                    continue
                if lie:
                    zL[i] += z[i]; cL[i] += 1
                else:
                    zT[i] += z[i]; cT[i] += 1
        d = [0.0] * F_COUNT
        dsum = 0.0
        for i in range(F_COUNT):
            if cT[i] > 0 and cL[i] > 0:
                diff = zL[i] / cL[i] - zT[i] / cT[i]
                d[i] = diff if diff > 0.0 else 0.0
            dsum += d[i]
        w = [(0.5 * c.weight[i] + 0.5 * d[i] / dsum) if dsum > 0.5 else c.weight[i] for i in range(F_COUNT)]
        sT = sL = 0.0
        for lie, mask, z in self.ctl:
            s = self._score_of(z, mask, w)
            if lie:
                sL += s
            else:
                sT += s
        mT, mL = sT / nT, sL / nL
        sep = mL - mT
        cal.meanTruth, cal.meanLie, cal.separation = mT, mL, sep
        if sep >= 0.75:
            cal.active = True
            cal.weak = False
            cal.s0 = 0.5 * (mT + mL)
            cal.k = clampf(5.0 / sep, 1.0, 6.0)
            cal.weight = w
        else:
            cal.active = False
            cal.weak = True
            cal.weight = list(c.weight)

    # ------------------------------------------------------------------ stress
    def _update_stress(self, f: Frame):
        b = self.base
        if not b.valid:
            self.stress = -1.0
            return
        num = den = 0.0
        if f.gsrOk and b.gsrFrac > 0.3:
            sd = b.sd[F_GSR] if b.sd[F_GSR] > 0.05 else 0.05
            num += 0.6 * clampf((f.gsr - self.refGsr) / sd, -3.0, 8.0); den += 0.6
        if f.ppgOk and f.hr > 0.0 and b.ppgFrac > 0.3:
            sd = b.sd[F_HR] if b.sd[F_HR] > 2.0 else 2.0
            num += 0.3 * clampf((f.hr - b.mean[F_HR]) / sd, -3.0, 8.0); den += 0.3
        sd = b.sd[F_TRM] if b.sd[F_TRM] > 0.02 else 0.02
        num += 0.1 * clampf((f.tremor - b.mean[F_TRM]) / sd, -3.0, 8.0); den += 0.1
        raw = num / den if den > 0 else 0.0
        target = 100.0 * sigmoid(0.8 * raw - 1.2)
        a1 = 1.0 - math.exp(-1.0 / (self.cfg.frameHz * 1.0))
        if self.stress < 0:
            self.stress = target
        else:
            self.stress += a1 * (target - self.stress)
        if self.state == STATE_READY and self.settled and f.gsrOk:
            a60 = 1.0 - math.exp(-1.0 / (self.cfg.frameHz * 60.0))
            self.refGsr += a60 * (f.gsr - self.refGsr)

    def _update_settled(self):
        c = self.cfg
        if self.state != STATE_READY:
            self.settled = False
            return
        if (self.frames - self.lastWindowEnd) < c.recoverySec * c.frameHz:
            self.settled = False
            return
        # เหมือน C++: GSR (1 s เทียบ 3 s ก่อน) ถ้ามีครบ, ไม่มีก็ใช้ชีพจร (4 s เทียบ 4 s ก่อน
        # = เฉลี่ยครบรอบหายใจ), ไม่มีทั้งคู่ = พร้อม
        g_avg, g_span = int(c.frameHz + 0.5), int(3.0 * c.frameHz + 0.5)
        h_avg = int(4.0 * c.frameHz + 0.5)
        h_span = h_avg
        n = len(self.hist)
        gsr_all, ppg_all = n >= g_span + g_avg, n >= h_span + h_avg
        ga = gb = ha = hb = 0.0
        if gsr_all:
            for i in range(g_avg):
                fa, fb = self.hist[-1 - i], self.hist[-1 - g_span - i]
                if not fa.gsrOk or not fb.gsrOk:
                    gsr_all = False
                    break
                ga += fa.gsr
                gb += fb.gsr
        if ppg_all:
            for i in range(h_avg):
                fa, fb = self.hist[-1 - i], self.hist[-1 - h_span - i]
                if not fa.ppgOk or not fb.ppgOk:
                    ppg_all = False
                    break
                ha += fa.hr
                hb += fb.hr
        if gsr_all:
            self.settled = abs((ga - gb) / g_avg / 3.0) < 0.03
        elif ppg_all:
            self.settled = abs((ha - hb) / h_avg) < 3.0
        else:
            self.settled = True

    # ------------------------------------------------------------------ status/json
    def status(self) -> dict:
        c = self.cfg
        elapsed = progress = 0.0
        if self.state == STATE_BASELINE:
            elapsed = (self.frames - self.phaseStart) / c.frameHz
            progress = len(self.baseBuf) / self.baseTarget if self.baseTarget else 0.0
        elif self.state == STATE_QUESTION:
            elapsed = (self.frames - self.phaseStart) / c.frameHz
            progress = len(self.winBuf) / self.winTarget if self.winTarget else 0.0
        return {
            "state": self.state, "progress": progress, "elapsed": elapsed,
            "qid": self.qid if self.state == STATE_QUESTION else 0, "kind": self.kind,
            "baselineValid": self.base.valid, "calibrated": self.cal.active, "calibWeak": self.cal.weak,
            "stress": -1 if self.stress < 0 else int(self.stress + 0.5), "settled": self.settled,
            "results": self.resultSeq - self.firstSeq, "revision": self.revision,
            "lastSeq": self.resultSeq,
        }

    def lie_json(self, n: int = 24) -> dict:
        """โครงสร้างเดียวกับ GET /api/lie ของเฟิร์มแวร์"""
        st = self.status()
        c, b, cal = self.cfg, self.base, self.cal
        r4 = lambda v, d: None if isnan(v) else round(v, d)  # noqa: E731
        return {
            "state": STATE_NAMES[self.state], "progress": round(st["progress"], 2),
            "elapsed": round(st["elapsed"], 1), "qid": st["qid"], "kind": KIND_NAMES[self.kind],
            "stress": st["stress"], "settled": st["settled"], "revision": self.revision,
            "resultCount": st["results"], "error": self.err, "time": round(self.time(), 1),
            "config": {"baselineSec": c.baselineSec, "windowSec": c.windowSec, "preSec": c.preSec,
                       "s0": round(c.s0, 2), "k": round(c.k, 2), "lieP": round(c.lieP, 2),
                       "truthP": round(c.truthP, 2), "w": [round(x, 3) for x in c.weight]},
            "baseline": {"valid": b.valid, "duration": round(b.durationSec, 1), "gsrFrac": round(b.gsrFrac, 2),
                         "ppgFrac": round(b.ppgFrac, 2), "nullN": b.nullN,
                         "mean": [r4(x, 3) for x in b.mean], "sd": [r4(x, 4) for x in b.sd],
                         "nullMean": [r4(x, 4) for x in b.nullMean], "nullSd": [r4(x, 4) for x in b.nullSd]},
            "calibration": {"active": cal.active, "weak": cal.weak, "nTruth": cal.nTruth, "nLie": cal.nLie,
                            "meanTruth": round(cal.meanTruth, 2), "meanLie": round(cal.meanLie, 2),
                            "separation": round(cal.separation, 2), "s0": round(cal.s0, 2), "k": round(cal.k, 2),
                            "correct": cal.correct, "scored": cal.scored,
                            "w": [round(x, 3) for x in cal.weight]},
            "results": [r.to_json() for r in reversed(self.res[-n:])] if n > 0 else [],
        }
