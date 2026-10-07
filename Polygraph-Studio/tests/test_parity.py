"""
test_parity.py — ตรวจว่า LieEngine ฝั่ง Python (นาฬิกาจำลอง) ให้ผลเหมือนเวอร์ชัน C++ บนนาฬิกาจริง

  scenario.txt     : สัญญาณจำลอง 5 Hz + คำสั่ง (baseline/คำถาม/คำตอบ/ยกเลิก/รีเซ็ต) รวมกรณีผิดปกติ
  golden_cpp.txt   : ผลที่ได้จากการรัน lie_engine.cpp (คอมไพล์ด้วย g++) กับ scenario เดียวกัน

รัน:  python tests/test_parity.py      (ไม่ต้องติดตั้งอะไรเพิ่ม)
C++ คำนวณด้วย float 32 บิต ส่วน Python ใช้ 64 บิต -> ยอมให้ต่างกันเล็กน้อยในทศนิยมท้าย ๆ
แต่ "คำตัดสิน" ต้องตรงกันทุกข้อ
"""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))
from backend import lie_engine as le  # noqa: E402


def run_python(path):
    e = le.Engine()
    out, last = [], 0
    for line in open(path, encoding="utf-8"):
        p = line.split()
        if not p:
            continue
        if p[0] == "F":
            g, hr, amp, trm, mot, tmp = map(float, p[1:7])
            e.push(le.Frame(gsr=g, hr=hr, amp=amp, tremor=trm, motion=mot, temp=tmp,
                            gsrOk=p[7] == "1", ppgOk=p[8] == "1"))
        elif p[0] == "C":
            cmd = p[1]
            ok = True
            if cmd == "baseline":
                ok = e.start_baseline(float(p[2]))
            elif cmd == "question":
                ok = e.start_question(int(p[2]), le.KIND_NAMES.index(p[3]))
            elif cmd == "answer":
                ok = e.mark_answer(p[2] == "1")
            elif cmd == "abort":
                e.abort()
            elif cmd == "reset":
                e.reset_session()
            out.append(("C", cmd, ok))
        while last < e.resultSeq:
            last += 1
            r = next((x for x in e.res if x.seq == last), None)
            if r:
                out.append(("R", r))
    return out, e


def main():
    golden = [l.split() for l in open(os.path.join(HERE, "golden_cpp.txt"), encoding="utf-8")]
    py, eng = run_python(os.path.join(HERE, "scenario.txt"))
    g_res = [g for g in golden if g[0] == "R"]
    p_res = [x[1] for x in py if x[0] == "R"]
    g_cmd = [g for g in golden if g[0] == "C"]
    p_cmd = [x for x in py if x[0] == "C"]
    fails = 0
    assert len(g_cmd) == len(p_cmd), "จำนวนคำสั่งไม่เท่ากัน"
    for g, p in zip(g_cmd, p_cmd):
        if (g[2] == "1") != p[2]:
            print("คำสั่งให้ผลต่างกัน:", g, p)
            fails += 1
    if len(g_res) != len(p_res):
        print(f"จำนวนผลต่างกัน C++={len(g_res)} Python={len(p_res)}")
        return 1
    for g, r in zip(g_res, p_res):
        seq, qid, kind, verdict = int(g[1]), int(g[2]), g[3], g[4]
        p, score, quality, reasons, ok = float(g[5]), float(g[6]), int(g[7]), int(g[8]), int(g[9])
        z = [float(x) for x in g[10:15]]
        bad = []
        if r.seq != seq or r.qid != qid or le.KIND_NAMES[r.kind] != kind:
            bad.append("id")
        if le.VERDICT_NAMES[r.verdict] != verdict:
            bad.append(f"verdict {verdict} vs {le.VERDICT_NAMES[r.verdict]}")
        if abs(r.pLie - p) > 2e-3:
            bad.append(f"p {p:.4f} vs {r.pLie:.4f}")
        if abs(r.score - score) > 2e-3:
            bad.append(f"score {score:.4f} vs {r.score:.4f}")
        if r.quality != quality or r.reasons != reasons or r.okMask != ok:
            bad.append("quality/reasons/ok")
        if any(abs(a - b) > 2e-2 for a, b in zip(z, r.z)):
            bad.append("z")
        status = "OK " if not bad else "FAIL"
        fails += 1 if bad else 0
        print(f"{status} q{qid:<3} {kind:<7} C++={verdict:<12} Py={le.VERDICT_NAMES[r.verdict]:<12} "
              f"p {p:.3f}/{r.pLie:.3f}  {'; '.join(bad)}")
    print(f"\nสรุป: {len(g_res)} ผล, ไม่ตรง {fails}")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
