// views/results.js — ประวัติการทดสอบทุกเซสชัน + สรุปผล + ลิงก์รายงาน/CSV
import { on } from '../store.js';
import { $, h, esc, timeStr, toast, confirmModal, vinfo, kinfo } from '../ui.js';
import { get, del } from '../api.js';

let root, listEl, detailEl, openId = null;

// สร้างหน้าผลลัพธ์: รายการเซสชันซ้าย + รายละเอียดขวา
export function mount(el) {
  root = el;
  root.innerHTML = `
    <div class="callout" style="margin-bottom:16px"><svg class=i><use href=#i-info></use></svg>
      หน้านี้แสดงรายงานของ "เซสชันทดสอบ" รุ่นก่อน (ฟีเจอร์เซสชันถูกเอาออกจากหน้าหลักเมื่อ 9 ต.ค. 2026 เพราะใช้ยาก)
      — ผลการใช้งานจริงตอนนี้ดูที่ตาราง "ประวัติ" ในหน้าหลัก หรือหน้า "ข้อมูล &amp; เทรน AI"</div>
    <div class="grid g-int">
      <div class="card"><h3><svg class=i><use href=#i-list></use></svg> ประวัติการทดสอบ <span class="right"><button class="btn sm" id="r-refresh">รีเฟรช</button></span></h3>
        <div id="r-list"><p class="muted">กำลังโหลด...</p></div></div>
      <div class="card" id="r-detail"><h3><svg class=i><use href=#i-doc></use></svg> รายละเอียด</h3><p class="muted">เลือกเซสชันทางซ้ายเพื่อดูผล</p></div>
    </div>`;
  listEl = $('#r-list', root);
  detailEl = $('#r-detail', root);
  $('#r-refresh', root).addEventListener('click', loadList);
  on('openSession', (id) => { openId = id; });   // ถูกเรียกเมื่อจบเซสชันจากหน้าทดสอบ
  on('result', () => { if (openId) loadDetail(openId, true); });
}

// กลับมาที่หน้านี้: โหลดรายการใหม่
export function show() { loadList(); if (openId) loadDetail(openId); }
// ออกจากหน้านี้
export function hide() {}

// โหลดรายการเซสชันทั้งหมด
async function loadList() {
  const rows = await get('/api/sessions', { quiet: true }).catch(() => null);
  if (!rows) { listEl.innerHTML = '<p class="muted">โหลดไม่ได้ — backend เปิดอยู่ไหม?</p>'; return; }
  if (!rows.length) { listEl.innerHTML = '<p class="muted">ยังไม่มีการทดสอบ — ไปที่หน้า "ทดสอบ" เพื่อเริ่ม</p>'; return; }
  listEl.innerHTML = '';
  rows.forEach((s) => {
    const active = !s.ended;
    const card = h('div', { class: 'qcard', style: 'cursor:pointer;grid-template-columns:1fr auto',
      onclick: () => loadDetail(s.id) }, h('div', {},
        h('div', { class: 'txt' }, `#${s.id} · ${s.subject || 'ไม่ระบุชื่อ'}`,
          active ? h('span', { class: 'badge b-truth', style: 'margin-left:8px' }, '● กำลังทดสอบ') : ''),
        h('div', { class: 'hint' }, `${timeStr(s.created)} · ${s.n_results || 0}/${s.n_questions || 0} ผล`)),
      h('div', { class: 'row' },
        h('span', { class: 'badge b-lie' }, `โกหก ${s.n_lie || 0}`),
        h('span', { class: 'badge b-truth' }, `จริง ${s.n_truth || 0}`)));
    listEl.append(card);
  });
}

// โหลดรายละเอียดเซสชันที่เลือก (ผลทุกข้อ + ลิงก์ดาวน์โหลด/รายงาน)
async function loadDetail(id, keepScroll) {
  openId = id;
  const s = await get(`/api/sessions/${id}`, { quiet: true }).catch(() => null);
  if (!s || !s.id) { detailEl.innerHTML = '<h3><svg class=i><use href=#i-doc></use></svg> รายละเอียด</h3><p class="muted">ไม่พบเซสชัน</p>'; return; }
  const sum = s.summary || { counts: {}, controls: {} };
  const c = sum.counts || {};
  const ctl = sum.controls || {};
  const cit = (s.cit || []).map((g) => `
    <div class="callout ok" style="margin-top:8px"><svg class=i><use href=#i-target></use></svg> เกมทายเลขลับ: ระบบเดาว่า <b>"${esc(g.best_question)}"</b>
    (ความมั่นใจ ${esc(g.confidence)}, ตอบ ${g.answered}/${g.total} ข้อ)</div>`).join('');
  const rows = (s.questions || []).map((q) => {
    const r = q.result;
    const vi = vinfo(r && r.verdict);
    const ki = kinfo(q.kind);
    const p = r ? Math.round((r.p || 0) * 100) : null;
    const ans = { yes: 'ใช่', no: 'ไม่ใช่' }[q.answer] || '-';
    const expl = (q.explain && q.explain.summary) ? `<div class="hint">${esc(q.explain.summary)}</div>` : '';
    return `<tr><td>${q.ord}</td>
      <td>${esc(q.text)}${expl}</td>
      <td><span class="badge ${ki.cls}">${esc(ki.th)}</span></td>
      <td>${ans}</td>
      <td>${r ? `<span class="badge ${vi.cls}">${vi.icon} ${vi.th}</span>` : '<span class="muted">ยังไม่ถาม</span>'}</td>
      <td>${p !== null ? `<div class="row"><div class="bar-cell" style="flex:1"><i style="width:${p}%;background:${vi.color}"></i></div>${p}%</div>` : '-'}</td>
      <td>${r ? (r.quality + '%') : '-'}</td></tr>`;
  }).join('');
  detailEl.innerHTML = `
    <div class="row between"><h3 style="margin:0"><svg class=i><use href=#i-doc></use></svg> เซสชัน #${s.id} · ${esc(s.subject || 'ไม่ระบุชื่อ')}</h3>
      <div class="row">
        <a class="btn sm primary" href="/report/${s.id}" target="_blank"><svg class=i><use href=#i-doc></use></svg> รายงาน (พิมพ์/PDF)</a>
        <a class="btn sm" href="/api/sessions/${s.id}/results.csv"><svg class=i><use href=#i-down></use></svg> ผล CSV</a>
        <a class="btn sm" href="/api/sessions/${s.id}/samples.csv"><svg class=i><use href=#i-down></use></svg> สัญญาณ CSV</a>
        <button class="btn sm danger" id="r-del">ลบ</button>
      </div></div>
    <div class="small muted">ผู้ควบคุม ${esc(s.operator || '-')} · ${timeStr(s.created)}${s.ended ? ' → ' + timeStr(s.ended) : ' · กำลังทดสอบ'}
      · นาฬิกา ${esc(s.device || '-')} ${esc(s.notes || '')}</div>
    <div class="grid g4" style="margin:12px 0">
      <div class="vital" style="--c:var(--lie)"><div class="name">โกหก</div><div class="v">${c.lie || 0}</div></div>
      <div class="vital" style="--c:var(--truth)"><div class="name">พูดจริง</div><div class="v">${c.truth || 0}</div></div>
      <div class="vital" style="--c:var(--inc)"><div class="name">ไม่แน่ชัด</div><div class="v">${c.inconclusive || 0}</div></div>
      <div class="vital" style="--c:var(--accent)"><div class="name">ข้อควบคุมที่ถูก</div><div class="v">${ctl.correct || 0}<small>/${ctl.total || 0}</small></div></div>
    </div>
    ${cit}
    <table class="t" style="margin-top:8px"><thead><tr><th>#</th><th>คำถาม</th><th>ชนิด</th><th>ตอบ</th><th>ผล</th><th>โอกาสโกหก</th><th>คุณภาพ</th></tr></thead>
      <tbody>${rows || '<tr><td colspan="7" class="muted">ไม่มีคำถาม</td></tr>'}</tbody></table>`;
  $('#r-del', detailEl).addEventListener('click', async () => {
    if (!(await confirmModal('ลบเซสชันนี้?', '<p>ลบถาวร ย้อนกลับไม่ได้</p>', 'ลบ', true))) return;
    await del(`/api/sessions/${id}`);
    openId = null;
    detailEl.innerHTML = '<h3><svg class=i><use href=#i-doc></use></svg> รายละเอียด</h3><p class="muted">เลือกเซสชันทางซ้าย</p>';
    loadList();
    toast('ลบแล้ว', 'ok');
  });
  if (!keepScroll) detailEl.scrollIntoView({ behavior: 'smooth', block: 'nearest' });
}
