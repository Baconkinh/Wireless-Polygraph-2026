// views/use.js — หน้า "ใช้งานจริง"
//
// ต่างจากหน้าเก็บข้อมูลยังไง
//   เก็บข้อมูล (fix/manual) : รู้เฉลยก่อน/หลัง แล้วสอน AI
//   ใช้งานจริง (live)       : ถามคำถามจริง นาฬิกาตัดสิน (ด้วยโมเดล AI ตัวล่าสุดที่ Studio ส่งให้อัตโนมัติ)
//                             แล้วผู้ใช้กดบอกว่า "นาฬิกาตอบถูก / ผิด / ไม่ทราบ"
//   -> ได้ตัวเลขความแม่นยำตอนใช้งานจริง และข้อที่รู้คำตอบถูกนำไปเทรนต่อได้ (ไฟล์ data/result_<วันเวลา>.csv, mode = live)
//
// ลำดับ: กรอกชื่อ -> เริ่ม -> วัดค่าปกติ -> พิมพ์คำถาม (ไม่บังคับ) -> ถาม -> รอ 12 วินาที -> ดูผล -> กด ถูก/ผิด/ไม่ทราบ
// backend: backend/collector.py (mode = live) + ml/recorder.py; โมเดล: backend/model_sync.py
import { S, on } from '../store.js';
import { $, esc, toast, confirmModal, vinfo } from '../ui.js';
import { get, post, cmd } from '../api.js';
import { tilesHtml, paintTiles, chartsHtml, makeCharts } from '../widgets.js';
import { historyHtml, mountHistory, FB_TH } from '../history.js';

const I = (n) => `<svg class=i><use href=#i-${n}></use></svg>`;
const SRC_TH = { rules: 'สูตรมาตรฐาน', rules_calibrated: 'สูตรที่ปรับจากข้อควบคุม', ai_model: 'โมเดล AI' };
let root, updCharts = () => {}, lastShell = null, hist;

// สร้างหน้าครั้งแรก: ค่าสด + กราฟ, การ์ดถามคำถาม, AI ที่ใช้, สถิติรอบนี้, ประวัติ
export function mount(el) {
  root = el;
  root.innerHTML = `
    <div class="card"><h3>${I('pulse')} สัญญาณสด</h3>${tilesHtml('us')}${chartsHtml('us')}</div>
    <div class="grid home-cols" style="margin-top:16px">
      <div class="card" id="us-main"></div>
      <div class="stack">
        <div class="card"><h3>${I('chip')} AI ที่นาฬิกาใช้ตัดสิน</h3><div id="us-ai" class="small"></div></div>
        <div class="card"><h3>${I('list')} รอบนี้</h3><div id="us-stat"></div><div id="us-hist" class="small muted" style="margin-top:8px">ยังไม่มีข้อ</div>
          <div class="small faint" style="margin-top:6px">ลบข้อที่ถามผิด/ทดลอง: ติ๊กหรือกด ${I('trash')} ในตาราง "ประวัติการใช้งานจริง" ด้านล่าง (กู้คืนได้)</div></div>
        <div class="card"><h3>${I('info')} ทำไมต้องกด ถูก / ผิด</h3>
          <p class="small">นาฬิกาไม่รู้คำตอบจริง เราจึงบอกหลังได้ผลว่านาฬิกาทายถูกหรือผิด เพื่อ
          (1) วัดความแม่นยำตอนใช้งานจริง และ (2) ข้อที่รู้คำตอบถูกจะถูกบันทึกเป็นข้อมูลเทรนต่อได้
          (นาฬิกาทาย "จริง" + ถูก = ตอบจริง, ทาย "จริง" + ผิด = โกหก) — กด "ไม่ทราบ" ถ้าไม่แน่ใจ ข้อนั้นจะไม่ถูกใช้เทรน</p></div>
      </div>
    </div>
    <div style="margin-top:16px">${historyHtml('us', 'ประวัติการใช้งานจริงทั้งหมด')}</div>`;
  updCharts = makeCharts(root, 'us');
  hist = mountHistory(root, 'us', { modes: 'live' });
  on('vitals', paintLive);
  on('collect', render);
  on('snapshot', () => { render(); paintAi(S.ai); });
  on('ai', paintAi);
  on('lie', () => paintTiles(root, 'us', true));
  render();
  get('/api/ai', { quiet: true }).then(paintAi);
}
// กลับมาที่หน้านี้: วาดใหม่ + โหลดสถานะโมเดล/ประวัติ
export function show() { render(); paintLive(); get('/api/ai', { quiet: true }).then(paintAi); if (hist) hist.refresh(); }
// ออกจากหน้านี้
export function hide() {}

// ---------------------------------------------------------------- โครงหน้า: setup / รอบอื่นค้าง / กำลังใช้งาน
function render() {
  if (!root) return;
  const c = S.collect || {};
  const run = c.run;
  const shell = !c.active ? 'setup' : run && run.mode === 'live' ? 'run' : 'busy';
  if (shell !== lastShell) {
    lastShell = shell;
    if (shell === 'setup') renderSetup(); else if (shell === 'run') renderRun(); else renderBusy();
  }
  if (shell === 'run') paintRun();
}

// ฟอร์มเริ่มใช้งาน: ชื่อผู้ตอบ/ผู้ถาม (โหมด live)
function renderSetup() {
  $('#us-main', root).innerHTML = `<h3>${I('target')} เริ่มใช้งานจริง</h3>
    <div class="form-grid">
      <label class="f">ผู้ถูกทดสอบ (ผู้ตอบ)<input class="input" id="us-subject" placeholder="เช่น Somchai"></label>
      <label class="f">ผู้ถาม<input class="input" id="us-operator" placeholder="เช่น Paphakorn"></label>
    </div>
    <p class="small muted">ทุกข้อบันทึกลง <span class="mono">data/result_&lt;วันเวลา&gt;.csv</span> (mode = live) เมื่อเริ่มข้อแรก
      — ไม่ได้ถามสักข้อ = ไม่มีไฟล์ใหม่</p>
    <button class="btn primary" id="us-start">${I('pulse')} เริ่มใช้งาน</button>`;
  $('#us-start', root).onclick = async () => {
    const subject = $('#us-subject', root).value.trim();
    if (!subject) return toast('กรอกชื่อผู้ถูกทดสอบก่อน', 'warn');
    const r = await post('/api/collect/start', { subject, operator: $('#us-operator', root).value.trim(), mode: 'live' });
    if (r && r.ok) { S.collect = r; render(); toast('เริ่มแล้ว — ต่อไปกด "วัดค่าปกติ"', 'ok'); }
  };
}

// แสดงเมื่อหน้าเก็บข้อมูลกำลังใช้นาฬิกา (ถามได้ทีละรอบ)
function renderBusy() {
  const run = (S.collect || {}).run || {};
  $('#us-main', root).innerHTML = `<h3>${I('alert')} มีรอบเก็บข้อมูลค้างอยู่</h3>
    <p>ตอนนี้หน้า "เก็บข้อมูลเทรน AI" กำลังใช้นาฬิกาอยู่ (รอบ <span class="mono">${esc(run.run_id || '')}</span> โหมด ${esc(run.mode || '')})
      นาฬิกาถามได้ทีละรอบ</p>
    <div class="row"><button class="btn" onclick="goto('collect')">ไปหน้าเก็บข้อมูล</button>
      <button class="btn danger" id="us-stop-other">${I('x')} จบรอบนั้น</button></div>`;
  $('#us-stop-other', root).onclick = () => post('/api/collect/stop');
}

// โครงการ์ดถามคำถาม (baseline, ช่องพิมพ์คำถาม, ปุ่มถาม/ยกเลิก/จบ)
function renderRun() {
  $('#us-main', root).innerHTML = `<h3>${I('target')} ถามคำถาม</h3>
    <div class="statebar"><span id="us-state">-</span></div>
    <div class="progress big"><i id="us-prog"></i></div>
    <div class="row" style="margin-top:12px"><button class="btn" id="us-base">${I('target')} วัดค่าปกติ (baseline)</button></div>
    <label class="f" style="margin-top:12px">คำถาม (พิมพ์ไว้เพื่อบันทึก ไม่บังคับ)
      <input class="input" id="us-text" placeholder="เช่น เมื่อวานคุณไปโรงเรียนใช่ไหม?"></label>
    <div class="row" style="margin-top:10px"><button class="btn primary xl" id="us-ask">${I('pulse')} ถาม (บันทึก 12 วินาที)</button>
      <button class="btn ghost" id="us-abort">${I('stop')} ยกเลิกข้อนี้</button></div>
    <div id="us-result" style="margin-top:14px"></div>
    <div class="row" style="margin-top:14px">
      <button class="btn danger" id="us-stop">${I('x')} จบการใช้งาน</button>
      <span class="small muted mono" id="us-file"></span></div>`;
  $('#us-base', root).onclick = async () => { const r = await cmd('baseline'); if (r && r.ok) toast('เริ่มวัดค่าปกติ — นั่งนิ่ง ๆ ~30 วินาที', 'ok'); };
  $('#us-ask', root).onclick = async () => {
    const r = await post('/api/collect/ask', { label: null, text: $('#us-text', root).value.trim() });
    if (r && r.ok) toast('ถามได้เลย! กำลังบันทึก', 'ok');
  };
  $('#us-abort', root).onclick = () => post('/api/collect/abort');
  $('#us-stop', root).onclick = async () => {
    if (await confirmModal('จบการใช้งาน?', '<p>ทุกข้อถูกบันทึกในไฟล์แล้ว ดูย้อนหลังได้ที่ตารางด้านล่าง</p>', 'จบ')) post('/api/collect/stop');
  };
}

// ---------------------------------------------------------------- ส่วนที่เปลี่ยนบ่อย
function paintRun() {
  const c = S.collect || {}, run = c.run;
  if (!run || !$('#us-result', root)) return;
  const cur = run.current;
  $('#us-file', root).textContent = `data/${run.files.results}${run.files_created ? '' : ' (สร้างเมื่อเริ่มข้อแรก)'}`;
  // ผลล่าสุด: ระหว่างรอ "ถูก/ผิด" แสดงผลของข้อปัจจุบัน ไม่งั้นแสดงข้อที่เพิ่งจบ
  const box = $('#us-result', root);
  const waiting = cur && cur.status === 'waiting_label';
  if (waiting) {
    const r = cur.result || {}, vi = vinfo(r.verdict);
    const sure = r.verdict === 'truth' || r.verdict === 'lie';
    box.innerHTML = `<div class="callout ${r.verdict === 'lie' ? 'danger' : r.verdict === 'truth' ? 'ok' : 'warn'}">
      <div class="small muted">ข้อที่ ${cur.question_no}${cur.text ? ' · ' + esc(cur.text) : ''}</div>
      <div style="font-size:30px;font-weight:800;color:${vi.color}">${esc(vi.th)}</div>
      <div class="small">โอกาสโกหก ${Math.round((r.p || 0) * 100)}% · ตัดสินด้วย${esc(SRC_TH[decided(r.src)] || '-')} · คุณภาพสัญญาณ ${r.quality ?? '-'}%</div>
      <div style="margin-top:10px"><b>${sure ? 'นาฬิกาตอบถูกไหม?' : 'นาฬิกาตอบไม่แน่ชัด — ที่จริงผู้ตอบพูดจริงหรือโกหก?'}</b></div>
      <div class="row" style="margin-top:8px">${sure
        ? `<button class="btn success" data-f="correct">${I('check')} ถูก</button><button class="btn danger" data-f="wrong">${I('x')} ผิด</button>`
        : `<button class="btn success" data-f="truth">${I('check')} พูดจริง</button><button class="btn danger" data-f="lie">${I('x')} โกหก</button>`}
        <button class="btn ghost" data-f="unknown">ไม่ทราบ</button></div></div>`;
    box.querySelectorAll('button[data-f]').forEach((b) => { b.onclick = () => post('/api/collect/feedback', { feedback: b.dataset.f }); });
  } else {
    const last = (run.history || []).slice(-1)[0];
    box.innerHTML = last ? `<div class="small muted">ข้อล่าสุด (#${last.question_no}): นาฬิกาทาย
      <span class="badge ${vinfo(last.verdict).cls}">${esc(vinfo(last.verdict).th)}</span> · ผู้ใช้บอกว่า ${esc(FB_TH[last.feedback] || '-')}</div>` : '';
  }
  // สถิติรอบนี้
  const k = run.counts || {};
  const n = (k.correct || 0) + (k.wrong || 0);
  $('#us-stat', root).innerHTML = `<div class="cnt4">${[['ถูก', k.correct, '--truth'], ['ผิด', k.wrong, '--lie'], ['ไม่ทราบ', k.unknown, '--inc'],
    ['ความแม่นยำ', n ? Math.round(100 * (k.correct || 0) / n) + '%' : '-', '--accent']]
    .map(([t, v, col]) => `<div class="vital" style="--c:var(${col})"><div class="name">${t}</div><div class="v">${v ?? 0}</div></div>`).join('')}</div>`;
  const h = run.history || [];
  $('#us-hist', root).innerHTML = h.length ? `<table class="t"><tr><th>ข้อ</th><th>คำถาม</th><th>นาฬิกาทาย</th><th>P</th><th>ถูก/ผิด</th></tr>
    ${h.slice().reverse().map((x) => `<tr><td>${x.question_no}</td><td>${esc(x.text || '-')}</td>
      <td><span class="badge ${vinfo(x.verdict).cls}">${esc(vinfo(x.verdict).th)}</span></td>
      <td>${x.p != null ? Math.round(x.p * 100) + '%' : '-'}</td><td>${esc(FB_TH[x.feedback] || (x.label === 'aborted' ? 'ยกเลิก' : '-'))}</td></tr>`).join('')}</table>` : 'ยังไม่มีข้อ';
  paintLive();
}

const decided = (src) => ({ 0: 'rules', 1: 'rules_calibrated', 2: 'ai_model' }[src] || src);

// อัปเดตทุกครั้งที่ได้ค่าสด: ข้อความสถานะ, เส้นนับถอยหลัง, เปิด/ปิดปุ่ม
function paintLive() {
  if (!root) return;
  paintTiles(root, 'us', true);
  updCharts();
  const v = S.live, c = S.collect || {};
  if (!v || !$('#us-state', root)) return;
  const cur = (c.run || {}).current;
  const es = v.es, p = Math.round((v.ep || 0) * 100), win = c.window_sec || 12;
  let st;
  if (es === 1) st = `กำลังวัดค่าปกติ ${p}% — ให้ผู้ตอบนั่งนิ่ง ๆ หายใจปกติ`;
  else if (es === 3) st = `กำลังวัดคำตอบข้อที่ ${cur ? cur.question_no : '-'} — เหลือ ${Math.max(0, Math.ceil((1 - v.ep) * win))} วินาที`;
  else if (cur && cur.status === 'waiting_label') st = 'ได้ผลแล้ว — กดบอกว่านาฬิกาตอบถูกหรือผิด';
  else if (es === 2) st = v.set ? 'พร้อมถาม: พิมพ์คำถาม (ถ้าต้องการ) แล้วกด "ถาม"' : 'รอให้ร่างกายกลับสู่ปกติสักครู่';
  else st = 'ขั้นแรก: กด "วัดค่าปกติ (baseline)"';
  $('#us-state', root).textContent = st;
  const bar = $('#us-prog', root);
  bar.style.width = (es === 3 ? 100 - p : es === 1 ? p : 0) + '%';
  bar.className = es === 3 ? 'countdown' : '';
  $('#us-base', root).disabled = es === 1 || es === 3;
  $('#us-ask', root).disabled = es !== 2 || (cur && cur.status === 'asking');
  $('#us-abort', root).disabled = !cur;
}

// แสดงโมเดลที่นาฬิกาใช้ตัดสิน และตรงกับตัวล่าสุดในคอมไหม
function paintAi(a) {
  if (!a || !root || !$('#us-ai', root)) return;
  S.ai = a;
  const w = a.watch;
  const model = w && w.loaded ? `<b class="mono">${esc(w.name)}</b> (แม่นยำตอนเทรน ${w.accuracy != null ? Math.round(w.accuracy * 100) + '%' : '-'}, ${w.samples} ข้อ)`
    : 'ไม่มีโมเดล — ใช้สูตรมาตรฐาน';
  $('#us-ai', root).innerHTML = `<div class="kv"><span>ในนาฬิกา</span><span>${a.connected ? model : 'ยังไม่ได้เชื่อมนาฬิกา'}</span>
    <span>ล่าสุดในคอม</span><span>${a.local ? `<span class="mono">${esc(a.local.name)}</span>` : 'ยังไม่ได้เทรน'}</span>
    <span>ส่งอัตโนมัติ</span><span>${a.auto ? 'เปิด' : 'ปิด'} ${a.connected ? (a.in_sync ? '<span class="badge b-truth">ตรงกัน</span>' : '<span class="badge b-inconclusive">ยังไม่ตรง</span>') : ''}</span></div>
    <div class="muted" style="margin-top:6px">${esc(a.msg || '')} · ตั้งค่าที่หน้า <a href="#data">ข้อมูล &amp; เทรน AI</a></div>`;
}
