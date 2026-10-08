// history.js — การ์ด "ประวัติทั้งหมด": ดูทุกข้อที่เคยบันทึกในไฟล์ data/result_*.csv + ปุ่มดาวน์โหลด
//
// ทำไมต้องมี: แต่ละรอบถูกบันทึกเป็นไฟล์แยก (result_<วันเวลา>.csv) ทำให้ดูย้อนหลังยาก
// การ์ดนี้อ่านทุกไฟล์ผ่าน backend (GET /api/data/rows) แล้วแสดงรวมในตารางเดียว กรองตามไฟล์/เฉลยได้
// และดาวน์โหลดได้ทั้ง "ไฟล์ที่เลือก" (ไฟล์จริงในเครื่อง) หรือ "รวมทุกไฟล์เป็นไฟล์เดียว" (สร้างตอนกด ไม่เก็บซ้ำใน data/)
//
// ใช้ใน 3 หน้า: เก็บข้อมูลเทรน AI (ทุกโหมด), ใช้งานจริง (เฉพาะ mode = live), ข้อมูล & เทรน AI
import { on } from './store.js';
import { $, esc, vinfo } from './ui.js';
import { get } from './api.js';

const I = (n) => `<svg class=i><use href=#i-${n}></use></svg>`;
export const LABEL_TH = { truth: 'ตอบจริง', lie: 'โกหก', unknown: 'ไม่รู้เฉลย', aborted: 'ยกเลิก' };
export const FB_TH = { correct: 'ถูก', wrong: 'ผิด', unknown: 'ไม่ทราบ', inconclusive: 'ไม่แน่ชัด' };
const MODE_TH = { fix: 'fix', manual: 'manual', live: 'ใช้งานจริง', watch: 'จากนาฬิกา', legacy: 'ไฟล์เก่า' };
const PAGE = 100;

// html ของการ์ด — prefix ทำให้ใช้หลายหน้าพร้อมกันได้โดย id ไม่ชนกัน
export function historyHtml(prefix, title = 'ประวัติทั้งหมด (ทุกไฟล์ result_*.csv)') {
  return `<div class="card" id="${prefix}-hcard"><h3>${I('list')} ${title}
      <span class="right small muted" id="${prefix}-hcount"></span></h3>
    <div class="row" style="margin-bottom:10px">
      <select class="input sm-input" id="${prefix}-hfile"><option value="all">ทุกไฟล์</option></select>
      <select class="input sm-input" id="${prefix}-hlabel">
        <option value="">ทุกเฉลย</option><option value="truth">ตอบจริง</option><option value="lie">โกหก</option>
        <option value="unknown">ไม่รู้เฉลย</option><option value="aborted">ยกเลิก</option><option value="train">เฉพาะที่ใช้เทรน</option></select>
      <span class="spacer" style="flex:1"></span>
      <a class="btn sm" id="${prefix}-hdl1" href="#">${I('down')} ดาวน์โหลดไฟล์นี้</a>
      <a class="btn sm" id="${prefix}-hdlall" href="#">${I('down')} รวมทุกไฟล์เป็นไฟล์เดียว</a>
      <button class="btn sm ghost" id="${prefix}-hre">รีเฟรช</button>
    </div>
    <div style="overflow:auto"><table class="t small" id="${prefix}-htable"></table></div>
    <div class="row" style="margin-top:8px"><button class="btn sm ghost hidden" id="${prefix}-hmore">แสดงเพิ่ม</button></div>
  </div>`;
}

// ผูกการทำงาน — คืน { refresh(), setFile(name) }
// opts.modes = 'live' หรือ 'fix,manual' (ว่าง = ทุกโหมด)
export function mountHistory(root, prefix, opts = {}) {
  const modes = opts.modes || '';
  let rows = [], shown = PAGE, timer = null;
  const el = (id) => $(`#${prefix}-${id}`, root);

  // โหลดรายชื่อไฟล์ result_*.csv มาใส่ตัวเลือก "ไฟล์" (กรองตามโหมดของหน้านั้น)
  async function loadFiles() {
    const r = await get('/api/data/files', { quiet: true });
    if (!r || !r.files) return;
    const sel = el('hfile'), cur = sel.value;
    const files = r.files.filter((f) => !modes || (f.modes || []).some((m) => modes.split(',').includes(m)));
    sel.innerHTML = `<option value="all">ทุกไฟล์ (${files.length})</option>` + files.slice().reverse()
      .map((f) => `<option value="${esc(f.name)}">${esc(f.name)} · ${esc((f.subjects || []).join(', '))} · ${f.rows} ข้อ</option>`).join('');
    if ([...sel.options].some((o) => o.value === cur)) sel.value = cur;
  }

  // โหลดทุกแถวของไฟล์ที่เลือก (หรือทุกไฟล์) แล้วตั้งลิงก์ดาวน์โหลด
  async function loadRows() {
    const file = el('hfile').value;
    const q = `/api/data/rows?file=${encodeURIComponent(file)}${modes ? `&mode=${modes}` : ''}`;
    const r = await get(q, { quiet: true });
    rows = (r && r.rows) || [];
    shown = PAGE;
    paint();
    // ลิงก์ดาวน์โหลด: ไฟล์เดียว = ไฟล์จริงในเครื่อง, รวม = backend สร้าง CSV ให้ตอนกด
    const one = el('hdl1');
    one.classList.toggle('hidden', file === 'all');
    one.href = file === 'all' ? '#' : `/api/data/file/${encodeURIComponent(file)}`;
    el('hdlall').href = `/api/data/merged.csv?files=all${modes ? `&mode=${modes}` : ''}`;
  }

  // วาดตาราง (กรองตามเฉลย, แสดงทีละ 100 แถว กดแสดงเพิ่มได้)
  function paint() {
    const lf = el('hlabel').value;
    const list = rows.filter((x) => !lf || (lf === 'train' ? String(x.used_for_training) === '1' : x.label === lf));
    el('hcount').textContent = `${list.length} ข้อ`;
    const live = modes === 'live';
    const head = `<tr><th>เวลา</th><th>ไฟล์ / ข้อ</th><th>qid</th><th>โหมด</th><th>ผู้ตอบ</th>${live ? '<th>คำถาม</th>' : ''}
      <th>เฉลย</th><th>นาฬิกาทาย</th><th>P(โกหก)</th><th>ตัดสินด้วย</th>${live ? '<th>ถูก/ผิด</th>' : ''}<th>ใช้เทรน</th><th>หมายเหตุ</th></tr>`;
    const body = list.slice(0, shown).map((x) => {
      const vi = vinfo(x.verdict);
      const p = x.p_lie !== '' && x.p_lie != null ? Math.round(parseFloat(x.p_lie) * 100) + '%' : '-';
      return `<tr><td class="mono">${esc((x.time_iso || '').slice(5, 19))}</td>
        <td class="mono">${esc((x._file || '').replace('result_', '').replace('.csv', ''))} #${esc(x.question_no)}</td>
        <td>${esc(x.watch_qid)}</td><td>${esc(MODE_TH[x.mode] || x.mode)}</td><td>${esc(x.subject)}</td>
        ${live ? `<td>${esc(x.question_text || '-')}</td>` : ''}
        <td>${x.label === 'truth' || x.label === 'lie' ? `<span class="badge ${x.label === 'lie' ? 'b-lie' : 'b-truth'}">${LABEL_TH[x.label]}</span>` : esc(LABEL_TH[x.label] || x.label)}</td>
        <td>${x.verdict ? `<span class="badge ${vi.cls}">${esc(vi.th)}</span>` : '-'}</td><td>${p}</td>
        <td class="small">${esc(x.decided_by || '-')}</td>
        ${live ? `<td>${esc(FB_TH[x.feedback] || '-')}</td>` : ''}
        <td>${String(x.used_for_training) === '1' ? 'ใช่' : '<span class="muted">ไม่</span>'}</td>
        <td class="small muted" style="max-width:260px">${esc(x.note || '')}</td></tr>`;
    }).join('');
    el('htable').innerHTML = head + (body || `<tr><td colspan="13" class="muted">ยังไม่มีข้อมูล</td></tr>`);
    el('hmore').classList.toggle('hidden', list.length <= shown);
  }

  // โหลดใหม่ทั้งรายชื่อไฟล์และแถว
  async function refresh() { await loadFiles(); await loadRows(); }
  el('hfile').onchange = loadRows;
  el('hlabel').onchange = paint;
  el('hre').onclick = refresh;
  el('hmore').onclick = () => { shown += PAGE; paint(); };
  // มีข้อใหม่ถูกบันทึก (backend ส่ง "data") -> รีเฟรช (หน่วง 0.5 วิ รวมหลายเหตุการณ์เป็นครั้งเดียว)
  on('data', () => { clearTimeout(timer); timer = setTimeout(refresh, 500); });
  refresh();
  return { refresh, setFile: (n) => { el('hfile').value = n; loadRows(); } };
}
