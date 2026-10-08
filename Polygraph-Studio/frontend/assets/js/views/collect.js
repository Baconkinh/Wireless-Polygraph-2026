// views/collect.js — หน้า "เก็บข้อมูลเทรน AI"
//
// ขั้นตอน: กรอกชื่อ/เลือกโหมด -> เริ่มรอบ -> วัดค่าปกติ -> ถามทีละข้อ (เส้นนับถอยหลังวิ่ง 12 วินาที)
//          -> ได้ผล -> (manual) ผู้ตอบบอกเฉลย -> บันทึกลง CSV ในคอม -> ข้อถัดไป
// โหมด fix    = ผู้ถามบอกล่วงหน้าว่าข้อนี้ให้ตอบจริงหรือให้โกหก (รู้เฉลยตั้งแต่ก่อนถาม)
// โหมด manual = ถามไปเลย ผู้ตอบเลือกเองว่าจะจริงหรือโกหก หมดเวลาแล้วค่อยบอกเฉลย
// ฝั่ง backend อยู่ที่ backend/collector.py + ml/recorder.py (ตัวเดียวกับ collect_data.bat)
import { S, on } from '../store.js';
import { $, esc, fmt, toast, confirmModal, vinfo } from '../ui.js';
import { post, cmd } from '../api.js';
import { tilesHtml, paintTiles, chartsHtml, makeCharts, I } from '../widgets.js';

let root, updCharts = () => {}, lastShell = null;
const LABEL_TH = { truth: 'ตอบจริง', lie: 'โกหก', unknown: 'ไม่รู้เฉลย', aborted: 'ยกเลิก' };

export function mount(el) {
  root = el;
  root.innerHTML = `
    <div class="card"><h3>${I('pulse')} สถานะผู้ถูกทดสอบ <span class="right small muted">มุมขวาของแต่ละช่อง = ค่าเฉลี่ยตอนวัด baseline</span></h3>
      ${tilesHtml('cl')}${chartsHtml('cl')}</div>
    <div class="grid home-cols" style="margin-top:16px">
      <div class="card" id="cl-main"></div>
      <div class="stack">
        <div class="card"><h3>${I('info')} สองโหมดของการให้เฉลย</h3>
          <div class="kv"><span><b>fix</b></span><span>ผู้ถามบอกผู้ตอบ <b>ก่อนถาม</b> ว่าข้อนี้ให้ตอบจริงหรือให้โกหก แล้วกดปุ่มให้ตรงกัน</span>
          <span><b>manual</b></span><span>ถามไปเลย ผู้ตอบเลือกเองว่าจะพูดจริงหรือโกหก พอหมดเวลา ผู้ตอบค่อยบอกว่าข้อนี้โกหกหรือไม่</span></div>
          <p class="small muted">ทุกข้อบันทึก 12 วินาทีเท่ากัน เพราะ AI ต้องเทียบข้อมูลช่วงเวลาเดียวกันทุกข้อ ·
            คำตัดสิน "จริง/โกหก" ที่เห็นมาจากนาฬิกา (LieEngine หรือโมเดล AI) ส่วนเฉลยคือคำตอบที่ถูกที่เราสอนให้ AI</p></div>
        <div class="card"><h3>${I('list')} ประวัติรอบนี้</h3><div id="cl-hist" class="small muted">ยังไม่มีข้อ</div></div>
        <div class="card"><h3>${I('doc')} บันทึกการทำงาน</h3><pre class="log" id="cl-log">-</pre></div>
      </div>
    </div>`;
  updCharts = makeCharts(root, 'cl');
  on('vitals', paintLive);
  on('collect', render);
  on('snapshot', render);
  on('lie', () => paintTiles(root, 'cl', true));
  render();
}
export function show() { render(); paintLive(); }
export function hide() {}

// ---------------------------------------------------------------- โครงหน้า (สร้างใหม่เมื่อเริ่ม/จบรอบเท่านั้น)
function render() {
  if (!root) return;
  const c = S.collect || {};
  const shell = c.active ? 'run' : 'setup';
  if (shell !== lastShell) { lastShell = shell; shell === 'run' ? renderRun() : renderSetup(); }
  if (shell === 'run') paintRun();
}

function renderSetup() {
  $('#cl-main', root).innerHTML = `<h3>${I('db')} เริ่มรอบเก็บข้อมูล</h3>
    <div class="form-grid">
      <label class="f">ผู้ถูกทดสอบ (ผู้ตอบ) — ภาษาอังกฤษ<input class="input" id="cl-subject" placeholder="เช่น Somchai"></label>
      <label class="f">ผู้ถาม<input class="input" id="cl-operator" placeholder="เช่น Paphakorn"></label>
      <label class="f">โหมดการให้เฉลย<select class="input" id="cl-mode">
        <option value="fix">fix — บอกล่วงหน้าว่าข้อนี้ให้โกหก</option>
        <option value="manual">manual — ถามก่อน ค่อยบอกเฉลยทีหลัง</option></select></label>
    </div>
    <p class="small muted">ไฟล์ CSV จะถูกสร้างในโฟลเดอร์ <span class="mono">Polygraph-Studio\\data\\</span> เมื่อเริ่มข้อแรกเท่านั้น
      (กดเริ่มแล้วไม่ได้ถามสักข้อ = ไม่มีไฟล์ใหม่ ไม่เปลืองพื้นที่)</p>
    <button class="btn primary" id="cl-start">${I('pulse')} เริ่มเก็บข้อมูล</button>`;
  $('#cl-start', root).onclick = async () => {
    const subject = $('#cl-subject', root).value.trim();
    if (!subject) return toast('กรอกชื่อผู้ถูกทดสอบก่อน', 'warn');
    const r = await post('/api/collect/start', { subject, operator: $('#cl-operator', root).value.trim(), mode: $('#cl-mode', root).value });
    if (r && r.ok) { S.collect = r; render(); toast('เริ่มรอบเก็บข้อมูลแล้ว — ต่อไปกด "วัดค่าปกติ"', 'ok'); }
  };
}

function renderRun() {
  $('#cl-main', root).innerHTML = `<h3>${I('db')} ควบคุมการเก็บข้อมูล <span class="right"><select class="input sm-input" id="cl-mode2">
      <option value="fix">Mode : fix</option><option value="manual">Mode : manual</option></select></span></h3>
    <div class="statebar"><span id="cl-state">-</span></div>
    <div class="progress big"><i id="cl-prog"></i></div>
    <div class="detail" id="cl-detail"></div>
    <div class="btn-grid" style="margin-top:12px">
      <button class="btn span-2" id="cl-base">${I('target')} วัดค่าปกติ (baseline)</button>
      <div class="span-2 btn-grid" id="cl-ask-box"></div>
      <div class="span-2" id="cl-label-box"></div>
    </div>
    <div class="cnt4" id="cl-counts"></div>
    <div class="row" style="margin-top:12px">
      <button class="btn ghost" id="cl-abort">${I('stop')} ยกเลิกข้อนี้</button>
      <button class="btn danger" id="cl-stop">${I('x')} จบรอบเก็บข้อมูล</button>
      <a class="btn ghost" href="/report-data-dictionary" target="_blank" id="cl-dict">${I('book')} ความหมายคอลัมน์ในไฟล์</a>
    </div>`;
  $('#cl-base', root).onclick = async () => { const r = await cmd('baseline'); if (r && r.ok) toast('เริ่มวัดค่าปกติ — นั่งนิ่ง ๆ ~30 วินาที', 'ok'); };
  $('#cl-abort', root).onclick = () => post('/api/collect/abort');
  $('#cl-stop', root).onclick = async () => {
    if (await confirmModal('จบรอบเก็บข้อมูล?', '<p>ไฟล์ CSV ถูกบันทึกครบแล้ว นำไปเทรนได้ด้วย train_ai.bat</p>', 'จบรอบ')) post('/api/collect/stop');
  };
  $('#cl-mode2', root).onchange = async (e) => {
    const r = await post('/api/collect/mode', { mode: e.target.value });
    if (!r || !r.ok) e.target.value = (S.collect.run || {}).mode || 'fix';
  };
}

// ---------------------------------------------------------------- อัปเดตส่วนที่เปลี่ยนบ่อย
function paintRun() {
  const c = S.collect || {}, run = c.run;
  if (!run || !$('#cl-detail', root)) return;
  const cur = run.current, nx = run.next;
  const sel = $('#cl-mode2', root);
  if (document.activeElement !== sel) sel.value = run.mode;
  sel.disabled = !!cur;
  const fileNote = run.files_created ? '' : ' <span class="muted">(สร้างเมื่อเริ่มข้อแรก)</span>';
  const q = cur || nx;
  $('#cl-detail', root).innerHTML = `
    <div class="kv">
      <span>${cur ? 'ข้อที่กำลังเก็บ' : 'ข้อถัดไป'}</span><span><b>ข้อที่ ${q.question_no}</b> &nbsp;(qid ${q.watch_qid})
        ${cur && cur.label ? ` · เฉลย <span class="badge ${cur.label === 'lie' ? 'b-lie' : 'b-truth'}">${LABEL_TH[cur.label]}</span>` : ''}</span>
      <span>บันทึกลงไฟล์</span><span class="mono">data/${esc(run.files.signals)}${fileNote}<br>data/${esc(run.files.results)} · data/${esc(run.files.training)}</span>
      <span>ใครถาม → ใครตอบ</span><span>${esc(run.operator)} → <b>${esc(run.subject)}</b></span>
      <span>โหมด</span><span>${run.mode === 'fix' ? 'fix — บอกเฉลยก่อนถาม' : 'manual — ผู้ตอบบอกเฉลยหลังตอบ'}</span>
    </div>`;
  // ปุ่มถาม: fix = 2 ปุ่ม (จริง/โกหก), manual = ปุ่มเดียว
  const askBox = $('#cl-ask-box', root);
  const want = run.mode + (cur ? '-busy' : '');
  if (askBox.dataset.k !== want) {
    askBox.dataset.k = want;
    askBox.innerHTML = run.mode === 'fix'
      ? `<button class="btn success" data-l="truth">${I('check')} ถามข้อที่ให้ตอบตามจริง</button>
         <button class="btn danger" data-l="lie">${I('x')} ถามข้อที่สั่งให้โกหก</button>`
      : `<button class="btn primary span-2" data-l="">${I('pulse')} เริ่มถาม (ผู้ตอบเลือกเองว่าจะจริงหรือโกหก)</button>`;
    askBox.querySelectorAll('button').forEach((b) => {
      b.onclick = async () => {
        const r = await post('/api/collect/ask', { label: b.dataset.l || null });
        if (r && r.ok) toast(run.mode === 'fix' ? 'ถามได้เลย! กำลังบันทึก 12 วินาที' : 'ถามได้เลย! หมดเวลาแล้วให้ผู้ตอบบอกเฉลย', 'ok');
      };
    });
  }
  // เฉลยหลังตอบ (manual)
  const lb = $('#cl-label-box', root);
  const waiting = cur && cur.status === 'waiting_label';
  lb.innerHTML = waiting ? `<div class="callout warn" style="margin-top:4px"><b>เฉลยข้อที่ ${cur.question_no}:</b> ผู้ตอบพูดจริงหรือโกหก?
      <div class="row" style="margin-top:8px"><button class="btn success" data-l="truth">${I('check')} พูดจริง</button>
      <button class="btn danger" data-l="lie">${I('x')} โกหก</button>
      <button class="btn ghost" data-l="unknown">ไม่รู้ / ข้าม</button></div></div>` : '';
  lb.querySelectorAll('button').forEach((b) => { b.onclick = () => post('/api/collect/label', { label: b.dataset.l }); });
  const k = run.counts || {};
  $('#cl-counts', root).innerHTML = [['ตอบจริง', k.truth, '--truth'], ['โกหก', k.lie, '--lie'], ['สัญญาณใช้ไม่ได้', k.invalid, '--inc'], ['ยกเลิก', k.aborted, '--invalid']]
    .map(([n, val, col]) => `<div class="vital" style="--c:var(${col})"><div class="name">${n}</div><div class="v">${val || 0}</div></div>`).join('');
  // ประวัติ + log
  const hist = run.history || [];
  $('#cl-hist', root).innerHTML = hist.length ? `<table class="t"><tr><th>ข้อ</th><th>qid</th><th>โหมด</th><th>เฉลย</th><th>นาฬิกาทาย</th><th>P(โกหก)</th><th>ใช้เทรน</th></tr>
    ${hist.slice().reverse().map((x) => `<tr><td>${x.question_no}</td><td>${x.watch_qid}</td><td>${x.mode}</td><td>${LABEL_TH[x.label] || x.label}</td>
      <td><span class="badge ${vinfo(x.verdict).cls}">${esc(vinfo(x.verdict).th)}</span></td><td>${x.p != null ? Math.round(x.p * 100) + '%' : '-'}</td>
      <td>${x.used ? 'ใช่' : 'ไม่'}</td></tr>`).join('')}</table>` : 'ยังไม่มีข้อ';
  $('#cl-log', root).textContent = (c.log || []).join('\n') || '-';
  paintLive();
}

function paintLive() {
  if (!root) return;
  paintTiles(root, 'cl', true);
  updCharts();
  const v = S.live, c = S.collect || {};
  if (!v || !$('#cl-state', root)) return;
  const run = c.run || {}, cur = run.current;
  const es = v.es, p = Math.round((v.ep || 0) * 100);
  const win = c.window_sec || 12;
  let st;
  if (es === 1) st = `กำลังวัดค่าปกติ ${p}% — ให้ผู้ตอบนั่งนิ่ง ๆ หายใจปกติ`;
  else if (es === 3) st = `กำลังบันทึกข้อที่ ${cur ? cur.question_no : '-'} — เหลือ ${Math.max(0, Math.ceil((1 - v.ep) * win))} วินาที`;
  else if (cur && cur.status === 'waiting_label') st = `ข้อที่ ${cur.question_no} ได้ผลแล้ว — รอเฉลยจากผู้ตอบ`;
  else if (es === 2) st = v.set ? 'พร้อมถามข้อถัดไป' : 'รอให้ร่างกายกลับสู่ปกติสักครู่ แล้วค่อยถาม';
  else st = 'ขั้นแรก: กด "วัดค่าปกติ (baseline)"';
  $('#cl-state', root).textContent = st;
  // เส้นนับถอยหลัง: วิ่งจาก 100% ลงไป 0% ระหว่างบันทึกข้อ (บอกว่าเวลาใกล้หมด)
  const bar = $('#cl-prog', root);
  bar.style.width = (es === 3 ? 100 - p : es === 1 ? p : 0) + '%';
  bar.className = es === 3 ? 'countdown' : '';
  $('#cl-base', root).disabled = es === 1 || es === 3;
  $('#cl-abort', root).disabled = !cur;
  root.querySelectorAll('#cl-ask-box button').forEach((b) => { b.disabled = es !== 2 || !!cur; });
}

export { fmt };
