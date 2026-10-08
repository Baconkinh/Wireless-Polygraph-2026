"""
report.py — รายงานผลการทดสอบแบบพิมพ์/บันทึกเป็น PDF ได้ (HTML ไฟล์เดียว กราฟเป็น SVG ไม่ต้องใช้เน็ต)
เปิดที่ http://127.0.0.1:8000/report/<เลขเซสชัน>  แล้วกดปุ่ม "พิมพ์ / บันทึก PDF"
สลับโหมดสว่าง/มืดได้ (สีกราฟ/ตัวหนังสือใช้ CSS variable จึงเปลี่ยนตามทั้งหน้า) — ตอนพิมพ์บังคับเป็นโหมดสว่างเสมอ
"""
from __future__ import annotations

import datetime
import html
from typing import Any, Dict, List, Optional

VERDICT_COLOR = {"lie": "#e5484d", "truth": "#12a594", "inconclusive": "#f5a524", "invalid": "#8b8d98", None: "#c9ccd6"}
VERDICT_TH = {"lie": "โกหก", "truth": "พูดจริง", "inconclusive": "ไม่แน่ชัด", "invalid": "วัดไม่ได้"}


def _e(x: Any) -> str:
    return html.escape("" if x is None else str(x))


def _fmt_time(ts: Optional[float]) -> str:
    if not ts:
        return "-"
    return datetime.datetime.fromtimestamp(ts).strftime("%d/%m/%Y %H:%M:%S")


def _series_svg(t: List[float], y: List[Optional[float]], windows: List[Dict[str, Any]], t0: float,
                color: str, label: str, unit: str, w: int = 900, h: int = 170) -> str:
    """กราฟเส้น 1 สัญญาณ + แถบสีช่วงคำถาม (สีตามคำตัดสิน)"""
    pts = [(ti - t0, yi) for ti, yi in zip(t, y) if yi is not None]
    if len(pts) < 2:
        return f'<p class="muted">ไม่มีข้อมูล {label}</p>'
    xs = [p[0] for p in pts]
    ys = [p[1] for p in pts]
    x_max = max(xs[-1], 1.0)
    y_min, y_max = min(ys), max(ys)
    if y_max - y_min < 1e-6:
        y_max, y_min = y_max + 0.5, y_min - 0.5
    pad = (y_max - y_min) * 0.1
    y_min, y_max = y_min - pad, y_max + pad
    L, R, T, B = 52, 10, 10, 26
    sx = lambda x: L + (w - L - R) * x / x_max            # noqa: E731
    sy = lambda v: T + (h - T - B) * (1 - (v - y_min) / (y_max - y_min))  # noqa: E731
    out = [f'<svg viewBox="0 0 {w} {h}" class="chart" xmlns="http://www.w3.org/2000/svg">']
    for win in windows:
        x0, x1 = sx(max(0.0, win["start"] - t0)), sx(min(x_max, win["end"] - t0))
        if x1 > x0:
            out.append(f'<rect x="{x0:.1f}" y="{T}" width="{x1 - x0:.1f}" height="{h - T - B}" '
                       f'fill="{VERDICT_COLOR.get(win["verdict"], "#c9ccd6")}" opacity="0.18"/>')
            out.append(f'<text x="{x0 + 2:.1f}" y="{T + 11}" font-size="10" class="ax">{_e(win["label"])}</text>')
    for k in range(5):                                       # เส้นกริด + ค่าบนแกน y
        v = y_min + (y_max - y_min) * k / 4
        yy = sy(v)
        out.append(f'<line x1="{L}" x2="{w - R}" y1="{yy:.1f}" y2="{yy:.1f}" class="grid"/>')
        out.append(f'<text x="{L - 6}" y="{yy + 3:.1f}" font-size="10" text-anchor="end" class="ax">{v:.2f}</text>')
    step = max(1, int(x_max / 8))
    for s in range(0, int(x_max) + 1, step * 10 if x_max > 600 else step):
        xx = sx(s)
        out.append(f'<text x="{xx:.1f}" y="{h - 8}" font-size="10" text-anchor="middle" class="ax">{s}s</text>')
    path = " ".join(f"{'M' if i == 0 else 'L'}{sx(x):.1f},{sy(v):.1f}" for i, (x, v) in enumerate(pts))
    out.append(f'<path d="{path}" fill="none" stroke="{color}" stroke-width="1.6"/>')
    out.append(f'<text x="{L + 4}" y="{h - B - 6}" font-size="11" fill="{color}" font-weight="600">{_e(label)} ({_e(unit)})</text>')
    out.append("</svg>")
    return "".join(out)


def _p_bars_svg(rows: List[Dict[str, Any]], w: int = 900, h: int = 200) -> str:
    rows = [r for r in rows if r["result"]]
    if not rows:
        return '<p class="muted">ยังไม่มีผล</p>'
    L, R, T, B = 40, 10, 10, 30
    bw = (w - L - R) / len(rows)
    sy = lambda p: T + (h - T - B) * (1 - p)              # noqa: E731
    out = [f'<svg viewBox="0 0 {w} {h}" class="chart" xmlns="http://www.w3.org/2000/svg">']
    for p, txt in ((0.65, "เกณฑ์โกหก 65%"), (0.35, "เกณฑ์จริง 35%")):
        out.append(f'<line x1="{L}" x2="{w - R}" y1="{sy(p):.1f}" y2="{sy(p):.1f}" class="grid" stroke-dasharray="4 3"/>')
        out.append(f'<text x="{w - R - 2}" y="{sy(p) - 3:.1f}" font-size="10" text-anchor="end" class="ax">{txt}</text>')
    for i, q in enumerate(rows):
        r = q["result"]
        p = max(0.0, min(1.0, r.get("p") or 0.0))
        x = L + i * bw + bw * 0.15
        out.append(f'<rect x="{x:.1f}" y="{sy(p):.1f}" width="{bw * 0.7:.1f}" height="{sy(0) - sy(p):.1f}" '
                   f'rx="3" fill="{VERDICT_COLOR.get(r.get("verdict"))}"/>')
        out.append(f'<text x="{x + bw * 0.35:.1f}" y="{sy(p) - 3:.1f}" font-size="10" text-anchor="middle" class="tx">{round(p * 100)}%</text>')
        out.append(f'<text x="{x + bw * 0.35:.1f}" y="{h - 12}" font-size="10" text-anchor="middle" class="tx">ข้อ {q["ord"]}</text>')
    out.append(f'<text x="4" y="{sy(1) + 8:.1f}" font-size="10" class="ax">100%</text>')
    out.append(f'<text x="4" y="{sy(0):.1f}" font-size="10" class="ax">0%</text>')
    out.append("</svg>")
    return "".join(out)


def render(view: Dict[str, Any], samples: Dict[str, List], credits: Optional[Dict[str, Any]] = None) -> str:
    qs = view["questions"]
    t0 = samples["t"][0] if samples.get("t") else (view.get("created") or 0)
    windows = []
    for q in qs:
        if q.get("asked_at"):
            v = (q.get("result") or {}).get("verdict")
            windows.append({"start": q["asked_at"], "end": q["asked_at"] + 12, "verdict": v, "label": f"ข้อ {q['ord']}"})
    s = view["summary"]
    c = s["counts"]
    ctl = s["controls"]
    rows_html = []
    for q in qs:
        r = q.get("result")
        ex = q.get("explain") or {}
        top = ""
        if ex.get("signals"):
            sig = [x for x in ex["signals"] if x["available"]]
            sig.sort(key=lambda x: -(x["z"] or -99))
            top = "<br>".join(f'{_e(x["name"])}: {_e(x["value"])} (z={x["z"]})' for x in sig[:2])
        v = (r or {}).get("verdict")
        badge = (f'<span class="badge" style="background:{VERDICT_COLOR.get(v)}">{_e(VERDICT_TH.get(v, v))}</span>'
                 if r else '<span class="muted">ยังไม่ได้ถาม</span>')
        ans = {"yes": "ใช่", "no": "ไม่ใช่"}.get(q.get("answer"), "-")
        rows_html.append(
            f"<tr><td>{q['ord']}</td><td>{_e(q['text'])}<div class='muted small'>{_e(ex.get('summary', ''))}</div></td>"
            f"<td>{_e(q.get('kind_th'))}</td><td>{ans}</td><td>{badge}</td>"
            f"<td>{(str(round((r.get('p') or 0) * 100)) + '%') if r else '-'}</td>"
            f"<td>{(str(r.get('quality')) + '%') if r else '-'}</td><td class='small'>{top}</td></tr>")
    cit_html = ""
    for g in view.get("cit") or []:
        items = "".join(f"<li>{_e(x['text'])} — คะแนน {x['score']:.2f}</li>" for x in g["ranking"])
        cit_html += (f'<div class="card"><h3>เกมทายเลขลับ: ระบบเดาว่า "{_e(g["best_question"])}"</h3>'
                     f'<p>ความมั่นใจ: <b>{_e(g["confidence"])}</b> (ห่างจากอันดับ 2 = {g["margin"]} คะแนน, '
                     f'ตอบแล้ว {g["answered"]}/{g["total"]} ข้อ)</p><ol>{items}</ol></div>')
    gsr = _series_svg(samples.get("t", []), samples.get("gsr", []), windows, t0, "#3e63dd", "GSR", "µS")
    hr = _series_svg(samples.get("t", []), samples.get("hr", []), windows, t0, "#e5484d", "ชีพจร", "bpm")
    si = _series_svg(samples.get("t", []), samples.get("si", []), windows, t0, "#8e4ec6", "ดัชนีความตื่นตัว", "0-100")
    tmp = _series_svg(samples.get("t", []), samples.get("tmp", []), windows, t0, "#f08c00", "อุณหภูมิผิว", "°C")
    trm = _series_svg(samples.get("t", []), samples.get("trm", []), windows, t0, "#2b8a3e", "มือสั่น", "m/s²")
    cr = credits or {}
    members = " · ".join(f"{_e(m['name'])} {_e(m['id'])}" for m in cr.get("members", []))
    credit_html = (f'<div class="card small"><b>{_e(cr.get("project", ""))}</b><br>{_e(cr.get("course", ""))}<br>'
                   f'{_e(cr.get("org", ""))} · {_e(cr.get("year", ""))}<br>ผู้จัดทำ: {members}</div>') if cr else ""
    return f"""<!doctype html><html lang="th"><head><meta charset="utf-8">
<title>รายงานเครื่องจับเท็จ — เซสชัน {view['id']}</title>
<style>
:root{{--bg:#ffffff;--text:#1c2024;--muted:#80838d;--line:#e6e8ee;--grid:#e6e8ee;--axis:#777;--card:#ffffff}}
:root.dark{{--bg:#0f1526;--text:#e9edf8;--muted:#9aa3bd;--line:#26314f;--grid:#26314f;--axis:#9aa3bd;--card:#151d33}}
body{{font-family:"Leelawadee UI","Segoe UI","Noto Sans Thai","Sarabun",Tahoma,sans-serif;color:var(--text);background:var(--bg);margin:0;line-height:1.5}}
.page{{margin:24px auto;max-width:960px;padding:0 16px}}
h1{{margin:0 0 4px}} h2{{margin-top:28px;border-bottom:2px solid var(--line);padding-bottom:4px}}
.muted{{color:var(--muted)}} .small{{font-size:12px}} .grid5{{display:grid;grid-template-columns:repeat(5,1fr);gap:10px}}
.card{{border:1px solid var(--line);border-radius:12px;padding:12px;margin:10px 0;background:var(--card)}} .num{{font-size:26px;font-weight:700}}
table{{width:100%;border-collapse:collapse;font-size:14px}} td,th{{border-bottom:1px solid var(--line);padding:6px;text-align:left;vertical-align:top}}
.badge{{color:#fff;border-radius:999px;padding:2px 10px;font-size:13px;white-space:nowrap}} .chart{{width:100%;height:auto}}
.chart .grid{{stroke:var(--grid)}} .chart .ax{{fill:var(--axis)}} .chart .tx{{fill:var(--text)}}
.btn{{background:#3e63dd;color:#fff;border:0;border-radius:8px;padding:8px 16px;font-size:14px;cursor:pointer}}
.btn.ghost{{background:transparent;color:var(--text);border:1px solid var(--line)}}
.bar{{display:flex;gap:8px;justify-content:flex-end}}
@media print{{.noprint{{display:none}} :root.dark{{--bg:#fff;--text:#1c2024;--muted:#80838d;--line:#e6e8ee;--grid:#e6e8ee;--axis:#777;--card:#fff}}}}
</style>
<script>
// จำโหมดที่เลือกไว้ (ใช้ร่วมกับปุ่มสลับโหมดในหน้า Studio)
try {{ if ((localStorage.getItem('reportTheme') || localStorage.getItem('theme')) === 'dark') document.documentElement.classList.add('dark'); }} catch (e) {{}}
function toggleTheme() {{
  const dark = document.documentElement.classList.toggle('dark');
  try {{ localStorage.setItem('reportTheme', dark ? 'dark' : 'light'); }} catch (e) {{}}
}}
</script></head><body><div class="page">
<div class="noprint bar"><button class="btn ghost" onclick="toggleTheme()">สลับโหมดสว่าง / มืด</button>
<button class="btn" onclick="window.print()">พิมพ์ / บันทึก PDF</button></div>
<h1>รายงานผลการทดสอบเครื่องจับเท็จ</h1>
<div class="muted">เซสชัน #{view['id']} · ผู้ถูกทดสอบ: <b>{_e(view.get('subject') or '-')}</b> · ผู้ควบคุม: {_e(view.get('operator') or '-')}
 · เริ่ม {_fmt_time(view.get('created'))} · นาฬิกา {_e(view.get('device') or '-')} (fw {_e(view.get('fw') or '-')})</div>
<h2>สรุป</h2>
<div class="grid5">
 <div class="card"><div class="muted">ถามแล้ว</div><div class="num">{s['asked']}/{s['total']}</div></div>
 <div class="card"><div class="muted">โกหก</div><div class="num" style="color:{VERDICT_COLOR['lie']}">{c['lie']}</div></div>
 <div class="card"><div class="muted">พูดจริง</div><div class="num" style="color:{VERDICT_COLOR['truth']}">{c['truth']}</div></div>
 <div class="card"><div class="muted">ไม่แน่ชัด / วัดไม่ได้</div><div class="num">{c['inconclusive']} / {c['invalid']}</div></div>
 <div class="card"><div class="muted">ข้อควบคุมที่ระบบตอบถูก</div><div class="num">{ctl['correct']}/{ctl['total']}</div></div>
</div>
{cit_html}
<h2>โอกาสโกหกรายข้อ</h2>{_p_bars_svg(qs)}
<h2>สัญญาณตลอดการทดสอบ <span class="muted small">(แถบสี = ช่วง 12 วินาทีหลังถามแต่ละข้อ)</span></h2>
{hr}{gsr}{tmp}{trm}{si}
<h2>รายละเอียดทุกข้อ</h2>
<table><tr><th>#</th><th>คำถาม / คำอธิบาย</th><th>ชนิด</th><th>ตอบ</th><th>ผล</th><th>P(โกหก)</th><th>คุณภาพ</th><th>สัญญาณเด่น</th></tr>
{''.join(rows_html)}</table>
<h2>วิธีการและข้อจำกัด</h2>
<p class="small">นาฬิกาวัดสัญญาณ 5 ชนิด (ความนำไฟฟ้าผิว/เหงื่อ, อัตราการเต้นหัวใจ, แรงชีพจรปลายนิ้ว, การสั่นของมือ, อุณหภูมิผิว)
เปรียบเทียบช่วง 12 วินาทีหลังถามกับ 3 วินาทีก่อนถาม แปลงเป็นคะแนนมาตรฐาน (z-score) เทียบกับการแกว่งตามธรรมชาติที่วัดตอน baseline
แล้วรวมแบบถ่วงน้ำหนัก (ปรับน้ำหนักและเกณฑ์ให้เหมาะกับแต่ละคนจากคำถามควบคุม) ได้เป็นความน่าจะเป็นที่จะโกหก
<b>ข้อจำกัด:</b> สัญญาณเหล่านี้บอก "ความตื่นตัว/ความเครียด" ไม่ได้บอก "การโกหก" โดยตรง คนพูดจริงแต่ตื่นเต้นอาจถูกตัดสินว่าโกหก
และคนที่ควบคุมตัวเองได้ดีอาจผ่านได้ ผลจากอุปกรณ์นี้จึงใช้เพื่อการศึกษาและสาธิตเท่านั้น ไม่ใช่หลักฐานทางกฎหมายหรือการแพทย์</p>
{credit_html}
<p class="muted small">สร้างโดย Polygraph Studio · {datetime.datetime.now().strftime('%d/%m/%Y %H:%M:%S')}</p>
</div></body></html>"""


def markdown_page(title: str, md: str) -> str:
    """แปลงไฟล์ .md แบบง่าย (หัวข้อ, ตาราง, รายการ, code block) เป็นหน้า HTML — ใช้แสดง DATA_DICTIONARY.md
    เขียนเองเพราะไม่อยากเพิ่มไลบรารี (ตอนต่อ WiFi นาฬิกาไม่มีเน็ตให้ติดตั้ง)"""
    out, in_code, in_table, in_list = [], False, False, False

    def close_blocks():
        nonlocal in_table, in_list
        if in_table:
            out.append("</table>")
            in_table = False
        if in_list:
            out.append("</ul>")
            in_list = False

    def inline(t: str) -> str:
        t = _e(t)
        t = __import__("re").sub(r"`([^`]+)`", r"<code>\1</code>", t)
        return __import__("re").sub(r"\*\*([^*]+)\*\*", r"<b>\1</b>", t)

    for line in md.splitlines():
        if line.startswith("```"):
            close_blocks()
            out.append("</pre>" if in_code else "<pre>")
            in_code = not in_code
            continue
        if in_code:
            out.append(_e(line))
            continue
        s = line.strip()
        if s.startswith("|"):
            cells = [c.strip() for c in s.strip("|").split("|")]
            if all(set(c) <= set("-: ") for c in cells):
                continue                                   # เส้นคั่นหัวตาราง |---|
            if not in_table:
                close_blocks()
                out.append("<table>")
                in_table = True
                out.append("<tr>" + "".join(f"<th>{inline(c)}</th>" for c in cells) + "</tr>")
            else:
                out.append("<tr>" + "".join(f"<td>{inline(c)}</td>" for c in cells) + "</tr>")
            continue
        if s.startswith("- "):
            if not in_list:
                close_blocks()
                out.append("<ul>")
                in_list = True
            out.append(f"<li>{inline(s[2:])}</li>")
            continue
        close_blocks()
        if s.startswith("#"):
            lv = min(4, len(s) - len(s.lstrip("#")))
            out.append(f"<h{lv}>{inline(s.lstrip('#').strip())}</h{lv}>")
        elif s:
            out.append(f"<p>{inline(s)}</p>")
    close_blocks()
    body = "\n".join(out)
    return f"""<!doctype html><html lang="th"><head><meta charset="utf-8"><title>{_e(title)}</title>
<style>body{{font-family:"Leelawadee UI","Segoe UI","Noto Sans Thai","Sarabun",Tahoma,sans-serif;max-width:1000px;margin:24px auto;padding:0 16px;line-height:1.55;color:#1c2024}}
table{{border-collapse:collapse;width:100%;font-size:14px;margin:8px 0 18px}}td,th{{border:1px solid #e1e4ea;padding:6px 8px;text-align:left;vertical-align:top}}
th{{background:#f3f5fb}}code,pre{{font-family:Consolas,monospace;background:#f3f5fb;border-radius:6px}}code{{padding:1px 5px}}pre{{padding:12px;overflow:auto}}
h1,h2{{border-bottom:2px solid #e6e8ee;padding-bottom:4px}}</style></head><body>{body}</body></html>"""
