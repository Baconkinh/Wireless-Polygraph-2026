// views/data.js — หน้า "ข้อมูล & เทรน AI"
//
// รวมทุกอย่างที่เกี่ยวกับไฟล์ข้อมูลและโมเดลไว้หน้าเดียว (ไม่ต้องสลับไปใช้ .bat)
//   1) ไฟล์ข้อมูล    : รายการ data/result_*.csv, ติ๊กว่าไฟล์ไหน "ใช้เทรน", ดาวน์โหลด, รวมเป็นไฟล์เดียว,
//                      นำเข้าไฟล์ CSV จากที่อื่น, ดึงข้อมูลที่นาฬิกาบันทึกเอง
//   2) เทรน AI       : กดปุ่มเดียว -> ml/train.py (ตัวเดียวกับ train_ai.bat) -> data/model.json
//   3) โมเดล AI      : เทียบโมเดลในคอมกับในนาฬิกา, ส่งอัตโนมัติ (ค่าเริ่มต้น) หรือเลือกส่งเอง
//   4) ประวัติทั้งหมด : ทุกข้อจากทุกไฟล์ (history.js)
// backend: backend/data_api.py + backend/model_sync.py
import { S, on } from '../store.js';
import { $, esc, toast, confirmModal, timeStr, bytes } from '../ui.js';
import { get, post } from '../api.js';
import { historyHtml, mountHistory, originBadge } from '../history.js';

const I = (n) => `<svg class=i><use href=#i-${n}></use></svg>`;
let root, hist, files = [];

// สร้างหน้าครั้งแรก: ไฟล์ข้อมูล, เทรน AI, โมเดล, ประวัติ, คำอธิบายโฟลเดอร์ data
export function mount(el) {
  root = el;
  root.innerHTML = `
    <div class="callout" style="margin-bottom:16px"><b>ขั้นตอนเทรน AI:</b>
      1) เก็บข้อมูล (หน้า "เก็บข้อมูลเทรน AI" หรือหน้า "ใช้งานจริง" ที่บอกถูก/ผิด) → ได้ไฟล์ <span class="mono">data/result_&lt;วันเวลา&gt;.csv</span>
      → 2) ติ๊กไฟล์ที่จะใช้ด้านล่าง → 3) กด <b>เทรน AI</b> → 4) Studio ส่งโมเดลเข้านาฬิกาให้อัตโนมัติ (ไม่ต้องอัปโหลดเอง)</div>
    <div class="card"><h3>${I('db')} ไฟล์ข้อมูล <span class="mono small">data/result_*.csv</span>
        <span class="right small muted" id="dt-total"></span></h3>
      <div style="overflow:auto"><table class="t small" id="dt-files"></table></div>
      <div class="row" style="margin-top:12px">
        <a class="btn" id="dt-merge-sel" href="/api/data/merged.csv">${I('down')} รวมไฟล์ที่ติ๊กเป็นไฟล์เดียว (ดาวน์โหลด)</a>
        <a class="btn ghost" href="/api/data/merged.csv?files=all">${I('down')} รวมทุกไฟล์</a>
        <button class="btn" id="dt-pull" title="ที่มา = esp_backup">${I('wifi')} ดึงข้อมูลที่นาฬิกาบันทึกเอง (สำรองจากบอร์ด)</button>
        <label class="btn ghost" style="cursor:pointer" title="ที่มา = mobile ถ้าเป็นไฟล์ polygraph_train.csv">${I('up')} นำเข้าไฟล์ CSV (จากมือถือ)<input type="file" id="dt-import" accept=".csv" hidden></label>
      </div>
      <p class="small muted" style="margin-top:8px">ติ๊ก "ใช้เทรน" ออก = train.py จะไม่อ่านไฟล์นั้น (บันทึกใน data/train_exclude.txt — train_ai.bat ก็ใช้รายการเดียวกัน)
        · ไฟล์รวมสร้างตอนกดดาวน์โหลดเท่านั้น ไม่เก็บซ้ำใน data/ (ไม่งั้นข้อเดิมจะถูกนับ 2 ครั้ง)
        · "ดึงข้อมูลที่นาฬิกาบันทึกเอง" ใช้เมื่อเก็บข้อมูลผ่านหน้าเว็บนาฬิกาบนมือถือ (โหมด train) — ข้อที่มีอยู่แล้วจะไม่ถูกเพิ่มซ้ำ
        · คอลัมน์ <b>ที่มา</b>: คอม = Studio บันทึกเอง, มือถือ = ไฟล์ polygraph_train.csv ที่มือถือดาวน์โหลดแล้วนำเข้า,
          สำรองจากบอร์ด = คอมดึงจากหน่วยความจำนาฬิกาโดยตรง (ข้อมูลชุดเดียวกับมือถือ ระบบตัดข้อซ้ำให้)
        · ปุ่ม ${I('trash')} = ย้ายไฟล์ไปถังขยะ (ไม่ลบถาวร)</p>
    </div>
    <div class="card" style="margin-top:16px"><h3>${I('trash')} ถังขยะ <span class="mono small">data/trash/</span>
        <span class="right small muted" id="dt-trash-n"></span></h3>
      <p class="small muted">ไฟล์ที่ลบจากหน้าเว็บ และสำเนาก่อนลบรายข้อ/แก้ "ใช้เทรน" อยู่ที่นี่ทั้งหมด — กด "กู้คืน" เพื่อเอากลับ
        (ถ้าชื่อซ้ำกับไฟล์ที่ใช้อยู่ ไฟล์ที่ใช้อยู่จะถูกเก็บเข้าถังขยะแทน ไม่มีอะไรหาย) · อยากลบถาวรให้ลบโฟลเดอร์ data/trash/ เอง</p>
      <div style="overflow:auto;max-height:280px"><table class="t small" id="dt-trash"></table></div>
    </div>
    <div class="grid g2" style="margin-top:16px">
      <div class="card"><h3>${I('flask')} เทรน AI</h3>
        <p class="small muted">ใช้ <span class="mono">ml/train.py</span> ตัวเดียวกับ train_ai.bat: Logistic Regression 12 feature
          ประเมินแบบ leave-one-subject-out (≥ 3 คน) แล้วบันทึก <span class="mono">data/model.json</span> + สำเนาใน <span class="mono">data/models/</span></p>
        <div class="row"><button class="btn primary" id="dt-train">${I('flask')} เทรน AI จากไฟล์ที่ติ๊ก</button>
          <label class="small muted">ขั้นต่ำต่อคลาส <input class="input sm-input" id="dt-min" type="number" value="10" min="2" style="width:70px"></label></div>
        <div id="dt-train-res" style="margin-top:10px"></div>
        <pre class="log" id="dt-train-log" style="margin-top:10px">ยังไม่ได้เทรนในรอบนี้</pre>
      </div>
      <div class="card"><h3>${I('chip')} โมเดล AI ในคอม ↔ ในนาฬิกา</h3>
        <div id="dt-ai"></div>
        <div class="row" style="margin-top:12px">
          <span class="small">ส่งโมเดลล่าสุดเข้านาฬิกาอัตโนมัติ</span>
          <div class="seg" id="dt-auto"><button data-v="1">เปิด</button><button data-v="0">ปิด (เลือกส่งเอง)</button></div>
        </div>
        <div class="row" style="margin-top:10px">
          <button class="btn" id="dt-up">${I('up')} ส่ง model.json เข้านาฬิกาตอนนี้</button>
          <button class="btn ghost danger" id="dt-clear">${I('trash')} ลบโมเดลในนาฬิกา</button>
        </div>
        <div class="small muted" id="dt-ai-msg" style="margin-top:8px"></div>
        <h4 style="margin:16px 0 6px">โมเดลที่เคยเทรน <span class="small muted mono">data/models/</span></h4>
        <div style="overflow:auto;max-height:260px"><table class="t small" id="dt-hist"></table></div>
      </div>
    </div>
    <div style="margin-top:16px">${historyHtml('dt')}</div>
    <div class="card" style="margin-top:16px"><h3>${I('info')} ในโฟลเดอร์ data/ มีอะไรบ้าง</h3>
      <div class="kv small">
        <span class="mono">result_&lt;วันเวลา&gt;.csv</span><span><b>ไฟล์ข้อมูลรูปแบบเดียวที่ใช้เทรน</b> — 1 รอบ = 1 ไฟล์, 1 แถว = 1 ข้อ (เฉลย + คำตัดสิน + feature 12 ตัว)</span>
        <span class="mono">signals/signals_&lt;วันเวลา&gt;.csv</span><span>ค่าเซนเซอร์ 5 ครั้ง/วินาทีของรอบเดียวกัน ไว้ดูกราฟ/วิเคราะห์ (ไม่ได้ใช้เทรน)</span>
        <span class="mono">model.json</span><span>โมเดลล่าสุด — Studio ส่งเข้านาฬิกาอัตโนมัติ</span>
        <span class="mono">models/</span><span>สำเนาโมเดลทุกครั้งที่เทรน (ย้อนกลับไปใช้ตัวเก่าได้)</span>
        <span class="mono">old_format/</span><span>ไฟล์รูปแบบเก่า (results_, training_samples, watch_train_) ที่แปลงแล้ว — เก็บไว้เฉย ๆ ไม่ถูกอ่าน</span>
        <span class="mono">train_exclude.txt</span><span>รายชื่อไฟล์ที่ไม่ใช้เทรน</span>
        <span class="mono">studio.db</span><span>ฐานข้อมูล SQLite ของ Studio (เซสชัน, ผล, ค่าสด) เปิดด้วย DB Browser / DBeaver</span>
      </div>
      <p class="small muted" style="margin-top:8px">อย่าเปิดไฟล์ CSV ใน Excel แล้วกด Save ทับ — Excel จะเปลี่ยนรูปแบบเวลา (เช่น 16:48:58 กลายเป็น 48:58.0)
        ถ้าจะแก้ ให้ Save As เป็นไฟล์ใหม่ · รายละเอียดทุกคอลัมน์: <a href="/report-data-dictionary" target="_blank">Data Dictionary</a></p>
    </div>`;

  hist = mountHistory(root, 'dt');
  $('#dt-pull', root).onclick = pull;
  $('#dt-import', root).onchange = importFile;
  $('#dt-train', root).onclick = train;
  $('#dt-up', root).onclick = () => upload('model.json');
  $('#dt-clear', root).onclick = async () => {
    if (await confirmModal('ลบโมเดลในนาฬิกา?', '<p>นาฬิกาจะกลับไปตัดสินด้วยสูตรมาตรฐาน และจะปิดการส่งอัตโนมัติให้ (ไม่งั้นจะถูกส่งกลับเข้าไปใหม่)</p>', 'ลบ')) {
      const r = await post('/api/ai/clear_watch'); if (r && r.ok !== false) toast(r.msg || 'ลบแล้ว', 'ok');
      loadAi();
    }
  };
  root.querySelectorAll('#dt-auto button').forEach((b) => {
    b.onclick = async () => { await post('/api/ai/auto', { on: b.dataset.v === '1' }); loadAi(); };
  });
  on('data', (d) => { if (d && d.files) paintFiles(d); });
  on('ai', paintAi);
  on('train', paintTrain);
  loadFiles(); loadAi();
}
// กลับมาที่หน้านี้: โหลดรายการไฟล์/สถานะโมเดล/ประวัติใหม่
export function show() { loadFiles(); loadAi(); if (hist) hist.refresh(); }
// ออกจากหน้านี้ (ไม่ต้องทำอะไร)
export function hide() {}

// ---------------------------------------------------------------- ไฟล์
async function loadFiles() { const r = await get('/api/data/files', { quiet: true }); if (r && r.files) paintFiles(r); }

// วาดตารางไฟล์ + ช่องติ๊ก "ใช้เทรน" + ลิงก์ดาวน์โหลด/รวมไฟล์
function paintFiles(d) {
  files = d.files || [];
  const t = d.total || {};
  $('#dt-total', root).textContent = `${t.files || 0} ไฟล์ · ${t.rows || 0} ข้อ · ใช้เทรนได้ ${t.used || 0} ข้อ (เฉพาะไฟล์ที่ติ๊ก)`;
  const L = (f, k) => (f.labels || {})[k] || 0;
  $('#dt-files', root).innerHTML = `<tr><th>ใช้เทรน</th><th>ไฟล์</th><th>ช่วงเวลา</th><th>ผู้ตอบ</th><th>โหมด</th><th>ที่มา</th><th>ข้อ</th>
      <th>จริง / โกหก / ไม่รู้</th><th>ใช้เทรนได้</th><th>ใช้งานจริง ถูก/ผิด</th><th></th></tr>` +
    (files.slice().reverse().map((f) => `<tr>
      <td><input type="checkbox" data-n="${esc(f.name)}" ${f.excluded ? '' : 'checked'}></td>
      <td class="mono">${esc(f.name)}${f.error ? `<div class="small" style="color:var(--lie)">${esc(f.error)}</div>` : ''}</td>
      <td class="small mono">${esc((f.first || '').slice(0, 16))}${f.last && f.last !== f.first ? ' → ' + esc((f.last || '').slice(11, 16)) : ''}</td>
      <td>${esc((f.subjects || []).join(', '))}</td><td class="small">${esc((f.modes || []).join(', '))}</td>
      <td>${(f.origins || []).map(originBadge).join(' ') || '-'}</td><td>${f.rows || 0}</td>
      <td>${L(f, 'truth')} / ${L(f, 'lie')} / ${L(f, 'unknown') + L(f, 'aborted')}</td>
      <td><b>${f.used || 0}</b>${f.stale ? ` <span class="small muted" title="ผลค้างจากรอบก่อน (บั๊กเดิม) ถูกตั้งไม่ใช้เทรน">(ผลค้าง ${f.stale})</span>` : ''}</td>
      <td>${f.correct || f.wrong ? `${f.correct} / ${f.wrong}` : '-'}</td>
      <td><div class="acts"><a class="btn sm ghost" title="ดาวน์โหลดไฟล์นี้" href="/api/data/file/${encodeURIComponent(f.name)}">${I('down')}</a>
        <button class="btn sm ghost nowrap" data-view="${esc(f.name)}">ดู</button>
        <button class="btn sm ghost" title="ย้ายไฟล์นี้ไปถังขยะ" data-trash="${esc(f.name)}">${I('trash')}</button></div></td></tr>`).join('')
      || '<tr><td colspan="11" class="muted">ยังไม่มีไฟล์ result_*.csv — เริ่มเก็บข้อมูลที่หน้า "เก็บข้อมูลเทรน AI"</td></tr>');
  root.querySelectorAll('#dt-files input[type=checkbox]').forEach((c) => {
    c.onchange = () => post('/api/data/exclude', { name: c.dataset.n, exclude: !c.checked });
  });
  root.querySelectorAll('#dt-files button[data-view]').forEach((b) => {
    b.onclick = () => { hist.setFile(b.dataset.view); $('#dt-hcard', root).scrollIntoView({ behavior: 'smooth' }); };
  });
  root.querySelectorAll('#dt-files button[data-trash]').forEach((b) => {
    b.onclick = async () => {
      const n = b.dataset.trash;
      if (!(await confirmModal('ย้ายไฟล์ไปถังขยะ?', `<p><span class="mono">${esc(n)}</span> จะถูกย้ายไป <span class="mono">data/trash/</span>
        (ไฟล์ค่าสดของรอบนั้นด้วย) — train.py จะไม่อ่านอีก กู้คืนได้จากการ์ดถังขยะด้านล่าง</p>`, 'ย้ายไปถังขยะ', true))) return;
      const r = await post('/api/data/delete_file', { name: n });
      if (r && r.ok) toast(r.msg, 'ok', 6000);
      loadFiles();
    };
  });
  loadTrash();
  const sel = files.filter((f) => !f.excluded).map((f) => f.name);
  $('#dt-merge-sel', root).href = `/api/data/merged.csv?files=${encodeURIComponent(sel.join(','))}`;
  $('#dt-train', root).innerHTML = `${I('flask')} เทรน AI จากไฟล์ที่ติ๊ก (${t.used || 0} ข้อ)`;
}

// ถังขยะ: รายการไฟล์ที่ลบ + สำเนาก่อนแก้ พร้อมปุ่มกู้คืน
async function loadTrash() {
  const r = await get('/api/data/trash', { quiet: true });
  const items = (r && r.items) || [];
  $('#dt-trash-n', root).textContent = items.length ? `${items.length} ไฟล์` : '';
  $('#dt-trash', root).innerHTML = `<tr><th>ไฟล์ในถังขยะ</th><th>คืออะไร</th><th>ข้อ</th><th>เวลา</th><th></th></tr>` +
    (items.map((x) => `<tr><td class="mono">${esc(x.name)}</td>
      <td class="small">${x.backup_of_edit ? `สำเนาของ ${esc(x.original)} ก่อนลบรายข้อ/แก้` : 'ไฟล์ที่ถูกลบทั้งไฟล์'}</td>
      <td>${x.rows}</td><td class="small">${timeStr(x.mtime)}</td>
      <td><button class="btn sm ghost" data-restore="${esc(x.name)}">กู้คืน</button></td></tr>`).join('')
      || '<tr><td colspan="5" class="muted">ถังขยะว่าง</td></tr>');
  root.querySelectorAll('#dt-trash button[data-restore]').forEach((b) => {
    b.onclick = async () => {
      const r2 = await post('/api/data/restore', { name: b.dataset.restore });
      if (r2 && r2.ok) toast(r2.msg, 'ok', 7000);
      loadFiles(); if (hist) hist.refresh();
    };
  });
}

// ดึงข้อมูลที่นาฬิกาบันทึกเองมาเป็น result_*.csv (เฉพาะข้อใหม่)
async function pull() {
  const b = $('#dt-pull', root); b.disabled = true;
  const r = await post('/api/data/pull_watch');
  b.disabled = false;
  if (r && r.ok) toast(r.msg, r.added ? 'ok' : 'info', 7000);
  loadFiles();
}

// อัปโหลดไฟล์ CSV จากเครื่องไปแปลงเป็น result_*.csv
async function importFile(e) {
  const f = e.target.files[0]; if (!f) return;
  const fd = new FormData(); fd.append('file', f);
  try {
    const res = await fetch('/api/data/import', { method: 'POST', body: fd });
    const r = await res.json();
    toast(r.msg || 'นำเข้าแล้ว', r.ok ? 'ok' : 'err', 8000);
  } catch { toast('นำเข้าไม่สำเร็จ', 'err'); }
  e.target.value = '';
  loadFiles();
}

// ---------------------------------------------------------------- เทรน
async function train() {
  const b = $('#dt-train', root); b.disabled = true;
  $('#dt-train-log', root).textContent = 'กำลังเทรน... (ข้อมูลเยอะอาจใช้เวลาหลายวินาที)';
  const r = await post('/api/ai/train', { min_samples: parseInt($('#dt-min', root).value, 10) || 10 });
  b.disabled = false;
  paintTrain({ running: false, log: (r && r.log) || [], result: r });
  if (r && r.ok) toast(r.msg, 'ok', 8000);
  loadAi();
}

// แสดงผลการเทรน (log + สรุปความแม่นยำ)
function paintTrain(t) {
  if (!t) return;
  if (t.log && t.log.length) $('#dt-train-log', root).textContent = t.log.join('\n');
  const r = t.result;
  if (!r) return;
  const m = r.metrics;
  $('#dt-train-res', root).innerHTML = r.ok && m
    ? `<div class="callout ok"><b>${esc(m.name)}</b> · ความแม่นยำ ${(m.cv_accuracy * 100).toFixed(1)}%
       (ช่วงเชื่อมั่น 95%: ${(m.cv_ci95[0] * 100).toFixed(0)}–${(m.cv_ci95[1] * 100).toFixed(0)}%) · ${m.samples} ข้อ (จริง ${m.n_truth} / โกหก ${m.n_lie})
       · เดาคลาสที่มากที่สุด = ${(m.majority_accuracy * 100).toFixed(0)}%</div>`
    : `<div class="callout warn">${esc(r.msg || 'เทรนไม่สำเร็จ')}</div>`;
}

// ---------------------------------------------------------------- โมเดล
async function loadAi() {
  const r = await get('/api/ai', { quiet: true });
  if (!r) return;
  paintAi(r);
  const h = r.history || [];
  $('#dt-hist', root).innerHTML = `<tr><th>ชื่อ</th><th>เทรนเมื่อ</th><th>ข้อ</th><th>ความแม่นยำ</th><th></th></tr>` +
    (h.map((m) => `<tr><td class="mono">${esc(m.name)}</td><td class="small">${timeStr(m.trained_at)}</td><td>${m.samples ?? '-'}</td>
      <td>${m.accuracy != null ? (m.accuracy * 100).toFixed(0) + '%' : '-'}</td>
      <td><button class="btn sm ghost" data-f="${esc(m.file)}">ส่งตัวนี้</button></td></tr>`).join('')
      || '<tr><td colspan="5" class="muted">ยังไม่มี</td></tr>');
  root.querySelectorAll('#dt-hist button[data-f]').forEach((b) => {
    b.onclick = async () => {
      if (await confirmModal('ส่งโมเดลเก่าตัวนี้เข้านาฬิกา?', '<p>ระบบจะปิด "ส่งอัตโนมัติ" ให้ ไม่งั้น model.json ตัวล่าสุดจะถูกส่งกลับไปทับ</p>', 'ส่ง')) upload('models/' + b.dataset.f);
    };
  });
}

// ส่งไฟล์โมเดลที่เลือกเข้านาฬิกา
async function upload(file) {
  const r = await post('/api/ai/upload', { file });
  if (r && r.ok) toast(r.msg, 'ok', 7000);
  loadAi();
}

// แสดงโมเดลในคอม vs ในนาฬิกา + สถานะตรงกันไหม + ปุ่มเปิด/ปิดอัตโนมัติ
function paintAi(a) {
  if (!a || !root) return;
  S.ai = a;
  const l = a.local, w = a.watch;
  const pct = (x) => (x != null ? (x * 100).toFixed(0) + '%' : '-');
  const sync = !a.connected ? '<span class="badge b-none">ไม่ได้เชื่อมนาฬิกา</span>'
    : a.in_sync ? '<span class="badge b-truth">นาฬิกาใช้โมเดลล่าสุดแล้ว</span>'
    : '<span class="badge b-inconclusive">ยังไม่ตรงกัน</span>';
  $('#dt-ai', root).innerHTML = `<div class="kv">
      <span>ในคอม (data/model.json)</span><span>${l ? `<b class="mono">${esc(l.name)}</b> · แม่นยำ ${pct(l.accuracy)} · ${l.samples} ข้อ · ${timeStr(l.trained_at)}` : 'ยังไม่มี — กดเทรนก่อน'}</span>
      <span>ในนาฬิกา</span><span>${!a.connected ? '-' : w && w.loaded ? `<b class="mono">${esc(w.name)}</b> · แม่นยำ ${pct(w.accuracy)} · ${w.samples} ข้อ` : 'ไม่มีโมเดล (ใช้สูตรมาตรฐาน)'}</span>
      <span>สถานะ</span><span>${sync}${a.busy ? ' กำลังส่ง...' : ''}</span>
      <span>ข้อมูลในนาฬิกา</span><span>${a.watch_data ? `โหมด ${esc(a.watch_mode || '-')} · บันทึกเอง จริง ${a.watch_data.truth} / โกหก ${a.watch_data.lie} ข้อ (${bytes(a.watch_data.bytes)})` : '-'}</span>
    </div>`;
  root.querySelectorAll('#dt-auto button').forEach((b) => b.classList.toggle('on', (b.dataset.v === '1') === !!a.auto));
  $('#dt-ai-msg', root).textContent = a.msg || '';
}
