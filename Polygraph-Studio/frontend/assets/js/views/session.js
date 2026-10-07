// views/session.js — หน้า "ทดสอบ": แผงควบคุมการวัด (เริ่มเซสชัน, baseline, ถามทีละข้อ, ดูผล)
import { S, on } from '../store.js';
import { $, h, esc, fmt, toast, confirmModal, vinfo, kinfo, KIND_CODES } from '../ui.js';
import { get, post, del, cmd } from '../api.js';

let root, templates = null, shellFor = null;

export function mount(el) {
  root = el;
  on('snapshot', render);
  on('session', render);
  on('status', paintBanners);
  on('vitals', paintLive);
  on('lie', paintReady);
}
export function show() { render(); }
export function hide() {}

// ---------------------------------------------------------------- เลือกว่าจะโชว์ฟอร์มหรือแผงควบคุม
function render() {
  if (!root) return;
  const s = S.session;
  if (!s) { if (shellFor !== 'setup') { shellFor = 'setup'; renderSetup(); } paintBanners(); return; }
  if (shellFor !== s.id) { shellFor = s.id; renderShell(); }
  paintQuestions();
  paintSummary();
  paintReady();
  paintLive();
  paintBanners();
}

function bannerHtml() {
  const out = [];
  const v = S.live, st = S.status || {};
  if (!S.connected) out.push(['danger', '<svg class=i><use href=#i-wifi></use></svg> ยังไม่ได้เชื่อมต่อนาฬิกา — ต่อ WiFi <b>Polygraph-Watch</b> (รหัส polygraph123) รอ ~5 วินาที']);
  if (st.simulator) out.push(['info', '<svg class=i><use href=#i-flask></use></svg> ใช้นาฬิกาจำลอง — ตั้งว่าผู้ถูกทดสอบจะโกหก/จริงได้ที่หน้า "ระบบ & อุปกรณ์"']);
  if (v && (v.fl & 256)) out.push(['danger', '<svg class=i><use href=#i-alert></use></svg> นาฬิกาเสียบ USB อยู่ — ห้ามวัด GSR กับคน ให้ใช้แบตเตอรี่เท่านั้น']);
  return out.map(([c, t]) => `<div class="banner ${c}">${t}</div>`).join('');
}
function paintBanners() { const b = $('#se-ban', root); if (b) b.innerHTML = bannerHtml(); }

// ---------------------------------------------------------------- ฟอร์มเริ่มเซสชัน
async function renderSetup() {
  root.innerHTML = `<div id="se-ban"></div>
    <div class="card"><h3><svg class=i><use href=#i-clip></use></svg> เริ่มการทดสอบใหม่</h3>
      <div class="sub">กรอกข้อมูล เลือกชุดคำถาม แล้วกดเริ่ม (เพิ่ม/แก้คำถามได้ภายหลัง)</div>
      <div class="form-grid">
        <label class="f">ชื่อผู้ถูกทดสอบ<input class="input" id="se-subject" placeholder="เช่น สมชาย"></label>
        <label class="f">ผู้ควบคุมการทดสอบ<input class="input" id="se-operator" placeholder="เช่น BoBoX"></label>
        <label class="f">ชุดคำถาม<select class="input" id="se-template"></select></label>
      </div>
      <p class="small muted" id="se-desc" style="margin:10px 0"></p>
      <ol class="small" id="se-preview" style="margin:0 0 12px;padding-left:20px"></ol>
      <label class="row small"><input type="checkbox" id="se-consent"> ผู้ถูกทดสอบยินยอมและรับทราบว่าเป็น<b>การสาธิตเพื่อการศึกษา</b></label><br>
      <label class="row small" style="margin-top:6px"><input type="checkbox" id="se-safety"> ถอดสาย USB ออกจากนาฬิกาแล้ว (ใช้แบตเตอรี่)</label>
      <div class="row" style="margin-top:14px"><button class="btn primary" id="se-start">▶ เริ่มเซสชัน</button>
        <button class="btn ghost" id="se-hist">ดูผลที่ผ่านมา</button></div>
    </div>
    <div class="card" style="margin-top:16px"><h3><svg class=i><use href=#i-list></use></svg> ลำดับการทำงาน</h3>
      <div class="flow">
        <div class="box">① ใส่นาฬิกา แตะเซนเซอร์</div><span class="arrow">→</span>
        <div class="box">② Baseline ~30 วินาที</div><span class="arrow">→</span>
        <div class="box">③ คำถามควบคุม (จริง/โกหก)</div><span class="arrow">→</span>
        <div class="box">④ คำถามจริง</div><span class="arrow">→</span>
        <div class="box">⑤ ดูรายงาน</div>
      </div></div>`;
  paintBanners();
  if (!templates) templates = await get('/api/templates', { quiet: true }).catch(() => ({}));
  const sel = $('#se-template', root);
  sel.innerHTML = Object.entries(templates || {}).map(([k, t]) => `<option value="${esc(k)}">${esc(t.name)}</option>`).join('');
  const preview = () => {
    const t = (templates || {})[sel.value]; if (!t) return;
    const name = $('#se-subject', root).value.trim() || '{ชื่อ}';
    $('#se-desc', root).textContent = t.desc || '';
    $('#se-preview', root).innerHTML = (t.questions || []).map((q) =>
      `<li><span class="badge ${kinfo(q.kind).cls}">${esc(kinfo(q.kind).th)}</span> ${esc((q.text || '').replace('{name}', name))}</li>`).join('')
      || '<li class="muted">ไม่มีคำถาม (เพิ่มเองภายหลัง)</li>';
  };
  sel.addEventListener('change', preview);
  $('#se-subject', root).addEventListener('input', preview);
  preview();
  $('#se-hist', root).addEventListener('click', () => window.goto('results'));
  $('#se-start', root).addEventListener('click', async () => {
    if (!$('#se-consent', root).checked) return toast('ต้องได้รับความยินยอมก่อน (ติ๊กช่องยินยอม)', 'warn');
    if (!$('#se-safety', root).checked) return toast('ถอดสาย USB ออกก่อน แล้วติ๊กยืนยัน', 'warn');
    const res = await post('/api/sessions', {
      subject: $('#se-subject', root).value, operator: $('#se-operator', root).value,
      template: sel.value, notes: '',
    });
    if (res && res.id) { S.session = res; shellFor = null; render(); toast('เริ่มเซสชันแล้ว — ต่อไปกด "วัด Baseline"', 'ok'); }
  });
}

// ---------------------------------------------------------------- แผงควบคุม
function renderShell() {
  const s = S.session;
  root.innerHTML = `<div id="se-ban"></div>
    <div class="card" style="margin-bottom:16px"><div class="row between">
      <div><b style="font-size:17px">เซสชัน #${s.id} · ${esc(s.subject || 'ไม่ระบุชื่อ')}</b>
        <div class="small muted">ผู้ควบคุม ${esc(s.operator || '-')}</div></div>
      <div class="row"><a class="btn sm" href="/report/${s.id}" target="_blank"><svg class=i><use href=#i-doc></use></svg> รายงาน</a>
        <button class="btn sm danger" id="se-end"><svg class=i><use href=#i-stop></use></svg> จบเซสชัน</button></div>
    </div></div>
    <div class="grid g-int">
      <div class="stack">
        <div class="card"><h3><svg class=i><use href=#i-sliders></use></svg> แผงควบคุม <span class="right badge b-none" id="se-state">-</span></h3>
          <div id="se-ready" class="small" style="margin-bottom:10px"></div>
          <div class="row">
            <button class="btn primary" id="se-baseline">① วัด Baseline</button>
            <button class="btn success sm" id="se-ans-yes" disabled>ตอบ: ใช่</button>
            <button class="btn danger sm" id="se-ans-no" disabled>ตอบ: ไม่ใช่</button>
            <button class="btn ghost sm" id="se-abort" disabled>ยกเลิกข้อนี้</button>
          </div>
          <div id="se-prog" class="small muted" style="margin-top:8px"></div>
        </div>
        <div class="card"><h3><svg class=i><use href=#i-list></use></svg> คำถาม <span class="right small muted" id="se-qn"></span></h3>
          <div class="qlist" id="se-qlist"></div>
          <hr class="sep">
          <div class="row">
            <select class="input" id="se-kind" style="width:auto">${KIND_CODES.map((k) => `<option value="${k}">${esc(kinfo(k).th)}</option>`).join('')}</select>
            <input class="input" id="se-text" placeholder="พิมพ์คำถามใหม่ (แบบตอบ ใช่/ไม่ใช่)" style="flex:1;min-width:180px">
            <button class="btn" id="se-add"><svg class=i><use href=#i-plus></use></svg> เพิ่ม</button>
          </div>
        </div>
      </div>
      <div class="stack">
        <div class="card"><h3><svg class=i><use href=#i-pulse></use></svg> ค่าสด</h3><div class="vitals" id="se-live"></div></div>
        <div class="card" id="se-last"><h3><svg class=i><use href=#i-doc></use></svg> ผลล่าสุด</h3><p class="muted">ยังไม่มีผลในเซสชันนี้</p></div>
        <div class="card" id="se-sum"></div>
      </div>
    </div>`;
  paintBanners();
  const liveBox = $('#se-live', root);
  [['hr', 'ชีพจร', 'bpm', '--hr'], ['gsr', 'GSR', 'µS', '--gsr'], ['tmp', 'อุณหภูมิผิว', '°C', '--temp'],
   ['si', 'ความตื่นตัว', '', '--stress']].forEach(([id, n, u, c]) =>
    liveBox.append(h('div', { class: 'vital', id: `sl-${id}`, style: `--c:var(${c})` },
      h('div', { class: 'name' }, n), h('div', { class: 'v' }, h('span', { class: 'num' }, '--'), h('small', {}, u)))));

  $('#se-end', root).addEventListener('click', async () => {
    if (!(await confirmModal('จบเซสชัน?', '<p>ผลถูกบันทึกแล้ว ดูย้อนหลังได้ที่หน้าผลลัพธ์</p>', 'จบ'))) return;
    const id = S.session.id; await post(`/api/sessions/${id}/end`);
    S.session = null; shellFor = null; render(); window.goto('results');
  });
  $('#se-baseline', root).addEventListener('click', async () => {
    const r = await cmd('baseline', {}); if (r && r.ok) toast('เริ่มวัด baseline — นั่งนิ่ง หายใจปกติ', 'ok');
  });
  $('#se-ans-yes', root).addEventListener('click', () => post('/api/answer', { yes: true }));
  $('#se-ans-no', root).addEventListener('click', () => post('/api/answer', { yes: false }));
  $('#se-abort', root).addEventListener('click', () => cmd('abort'));
  $('#se-add', root).addEventListener('click', addQuestion);
  $('#se-text', root).addEventListener('keydown', (e) => { if (e.key === 'Enter') addQuestion(); });
}

async function addQuestion() {
  const text = $('#se-text', root).value.trim();
  if (!text) return toast('พิมพ์คำถามก่อน', 'warn');
  const kind = $('#se-kind', root).value;
  const r = await post(`/api/sessions/${S.session.id}/questions`, { kind, text });
  if (r && r.id) { S.session = r; $('#se-text', root).value = ''; paintQuestions(); paintSummary(); }
}

// ---------------------------------------------------------------- วาดส่วนย่อย
function paintState() {
  const es = S.live ? S.live.es : (S.lie ? ['idle', 'baseline', 'ready', 'question'].indexOf(S.lie.state) : 0);
  return es;
}

function paintReady() {
  const el = $('#se-ready', root); if (!el) return;
  const lie = S.lie || {};
  const bl = lie.baseline || {};
  const cal = lie.calibration || {};
  const items = [];
  items.push(check(S.connected, 'เชื่อมต่อนาฬิกา', 'ยังไม่ได้ต่อ WiFi นาฬิกา'));
  items.push(check(bl.valid, 'วัด Baseline แล้ว', 'ยังไม่ได้วัด baseline — กดปุ่ม ①'));
  items.push(check(cal.active, 'ปรับเกณฑ์เฉพาะบุคคลแล้ว (calibration)',
    cal.weak ? 'ข้อควบคุมแยกจริง/โกหกไม่ชัด — ใช้เกณฑ์มาตรฐาน' : 'ถามคำถามควบคุม จริง+โกหก อย่างละ 1 ข้อเพื่อปรับเกณฑ์', true));
  const settled = S.live && S.live.set;
  if (bl.valid) items.push(check(settled, 'สัญญาณนิ่ง พร้อมถามข้อต่อไป', 'รอสัญญาณนิ่ง (10–20 วินาที)', true));
  el.innerHTML = items.join('');
}
function check(ok, okText, badText, warnOnly) {
  const cls = ok ? 'ok' : (warnOnly ? 'warn' : 'bad');
  const ic = ok ? '✓' : (warnOnly ? '!' : '×');
  return `<div class="check"><div class="st ${cls}">${ic}</div><div>${esc(ok ? okText : badText)}</div></div>`;
}

function paintQuestions() {
  const s = S.session; if (!s) return;
  const box = $('#se-qlist', root); if (!box) return;
  const es = paintState();
  const ready = (S.lie && S.lie.baseline && S.lie.baseline.valid);
  const busy = es === 1 || es === 3;
  box.innerHTML = '';
  (s.questions || []).forEach((q) => {
    const r = q.result;
    const vi = vinfo(r && r.verdict);
    const ki = kinfo(q.kind);
    const isCurrent = s.current_question === q.id;
    const card = h('div', { class: 'qcard' + (isCurrent ? ' current' : '') + (r ? ' done' : '') },
      h('div', { class: 'num' }, String(q.ord)),
      h('div', {},
        h('div', { class: 'txt' }, q.text),
        h('div', { class: 'hint' }, h('span', { class: `badge ${ki.cls}`, style: 'margin-right:6px' }, ki.th),
          q.hint || '', r ? h('span', { class: `badge ${vi.cls}`, style: 'margin-left:6px' }, `${vi.icon} ${vi.th} ${Math.round((r.p || 0) * 100)}%`) : '')),
      h('div', { class: 'acts' },
        h('button', { class: 'btn sm primary', disabled: !ready || busy ? true : false,
          onclick: () => askQ(q.id) }, r ? 'ถามซ้ำ' : 'ถาม'),
        q.asked_at ? null : h('button', { class: 'btn sm ghost', onclick: () => delQ(q.id), title: 'ลบข้อนี้', html: '<svg class=i><use href=#i-trash></use></svg>' })));
    box.append(card);
  });
  $('#se-qn', root).textContent = `${(s.questions || []).length} ข้อ`;
}

async function askQ(id) {
  const r = await post(`/api/questions/${id}/ask`);
  if (r && r.ok) toast('เริ่มถามแล้ว — อ่านคำถามออกเสียงทันที แล้วกดคำตอบ', 'ok', 5000);
}
async function delQ(id) { await del(`/api/questions/${id}`); }

function paintSummary() {
  const s = S.session; const el = $('#se-sum', root); if (!s || !el) return;
  const sum = s.summary || { counts: {}, controls: {} };
  const c = sum.counts || {};
  const ctl = sum.controls || {};
  el.innerHTML = `<h3><svg class=i><use href=#i-chart></use></svg> สรุปเซสชัน</h3>
    <div class="grid g2">
      <div class="vital" style="--c:var(--lie)"><div class="name">โกหก</div><div class="v">${c.lie || 0}</div></div>
      <div class="vital" style="--c:var(--truth)"><div class="name">พูดจริง</div><div class="v">${c.truth || 0}</div></div>
      <div class="vital" style="--c:var(--inc)"><div class="name">ไม่แน่ชัด</div><div class="v">${c.inconclusive || 0}</div></div>
      <div class="vital" style="--c:var(--accent)"><div class="name">ควบคุมถูก</div><div class="v">${ctl.correct || 0}<small>/${ctl.total || 0}</small></div></div>
    </div>
    ${(s.cit || []).map((g) => `<div class="callout ok" style="margin-top:10px"><svg class=i><use href=#i-target></use></svg> เกมทายเลขลับ: เดาว่า <b>"${esc(g.best_question)}"</b> (มั่นใจ ${esc(g.confidence)})</div>`).join('')}`;
}

function paintLive() {
  const v = S.live; if (!v || !$('#se-live', root)) return;
  set('hr', v.con && v.hr ? fmt(v.hr, 0) : '—');
  set('gsr', v.gc ? fmt(v.gsr, 2) : '—');
  set('tmp', fmt(v.tmp, 1));
  set('si', v.si >= 0 ? v.si : '--');
  const es = v.es;
  $('#se-state', root).textContent = ['ว่าง', 'กำลังวัด Baseline', 'พร้อมถาม', 'กำลังวัดคำถาม'][es] || '-';
  $('#se-state', root).className = 'right badge ' + (es === 2 ? 'b-truth' : es === 3 ? 'b-kind' : 'b-none');
  const busy = es === 1 || es === 3;
  $('#se-ans-yes', root).disabled = es !== 3;
  $('#se-ans-no', root).disabled = es !== 3;
  $('#se-abort', root).disabled = !busy;
  $('#se-baseline', root).disabled = busy;
  $('#se-prog', root).textContent = busy ? `${['','baseline','','คำถาม #' + v.eq][es] || ''} ${Math.round((v.ep || 0) * 100)}% (${fmt(v.eel, 0)} วินาที)` : '';
}
function set(id, val) { const el = $(`#sl-${id} .num`, root); if (el) el.textContent = val; }

// อัปเดต "ผลล่าสุด" เมื่อมีผลใหม่
on('result', (d) => {
  const el = $('#se-last', root); if (!el || !d) return;
  const r = d.result, ex = d.explain || {};
  const vi = vinfo(r.verdict);
  const sigs = (ex.signals || []).filter((x) => x.available).sort((a, b) => (b.z || 0) - (a.z || 0));
  el.style.setProperty('--vcolor', vi.color);
  el.innerHTML = `<h3><svg class=i><use href=#i-doc></use></svg> ผลล่าสุด — ข้อ #${r.qid}</h3>
    <div class="row between"><div class="big" style="color:${vi.color};font-weight:800">${vi.icon} ${vi.th}</div>
      <div class="pill">โอกาสโกหก ${Math.round((r.p || 0) * 100)}%</div></div>
    <div class="small muted" style="margin:6px 0">${esc(ex.summary || '')}</div>
    ${sigs.map((sg) => `<div class="sig"><div>${esc(sg.name)}</div>
      <div class="track"><i style="width:${Math.min(100, (sg.z || 0) / 6 * 100)}%;background:${vi.color}"></i></div>
      <div class="small">${esc(sg.value)} <span class="faint">(${esc(sg.level)})</span></div></div>`).join('')}
    ${(ex.warnings || []).map((w) => `<div class="callout warn" style="margin-top:8px"><svg class=i><use href=#i-alert></use></svg> ${esc(w)}</div>`).join('')}`;
});
