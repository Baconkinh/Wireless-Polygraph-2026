// history.js — การ์ด "ประวัติทั้งหมด": ดูทุกข้อที่เคยบันทึกในไฟล์ data/result_*.csv + ดาวน์โหลด + ลบ
//
// ทำไมต้องมี: แต่ละรอบถูกบันทึกเป็นไฟล์แยก (result_<วันเวลา>.csv) ทำให้ดูย้อนหลังยาก
// การ์ดนี้อ่านทุกไฟล์ผ่าน backend (GET /api/data/rows) แล้วแสดงรวมในตารางเดียว กรองตามไฟล์/เฉลย/ที่มาได้
// และดาวน์โหลดได้ทั้ง "ไฟล์ที่เลือก" (ไฟล์จริงในเครื่อง) หรือ "รวมทุกไฟล์เป็นไฟล์เดียว" (สร้างตอนกด ไม่เก็บซ้ำใน data/)
//
// การลบ (ปลอดภัย กู้คืนได้เสมอ):
//   ลบรายข้อ  -> backend สำรองไฟล์ทั้งไฟล์ไว้ data/trash/result_x.before_<เวลา>.csv ก่อน แล้วค่อยเขียนไฟล์ใหม่
//   ลบทั้งไฟล์ -> ย้ายไฟล์ไป data/trash/ (ไม่ลบถาวร) กู้คืนได้ที่หน้า "ข้อมูล & เทรน AI" การ์ดถังขยะ
//   ติ๊ก "ใช้เทรน" รายข้อ -> แก้คอลัมน์ used_for_training ให้ (ไม่ต้องเปิดไฟล์แก้เอง)
//
// ใช้ใน 3 หน้า: เก็บข้อมูลเทรน AI (ทุกโหมด), ใช้งานจริง (เฉพาะ mode = live), ข้อมูล & เทรน AI
import { on } from './store.js';
import { $, esc, vinfo, toast, confirmModal } from './ui.js';
import { get, post } from './api.js';

const I = (n) => `<svg class=i><use href=#i-${n}></use></svg>`;
export const LABEL_TH = { truth: 'ตอบจริง', lie: 'โกหก', unknown: 'ไม่รู้เฉลย', aborted: 'ยกเลิก' };
export const FB_TH = { correct: 'ถูก', wrong: 'ผิด', unknown: 'ไม่ทราบ', inconclusive: 'ไม่แน่ชัด' };
// แหล่งที่มาของข้อมูล (ค่าจาก backend: datafiles.origin_of)
export const ORIGIN_TH = {
  desktop: 'คอม (Studio)', desktop_cli: 'คอม (collect_data.bat)', desktop_sim: 'นาฬิกาจำลอง',
  mobile: 'มือถือ (หน้าเว็บนาฬิกา)', esp_backup: 'สำรองจากบอร์ด ESP32', legacy: 'คอม (Studio รุ่นเก่า)', import: 'นำเข้า',
};
const ORIGIN_CLS = { desktop: 'b-truth', desktop_cli: 'b-truth', legacy: 'b-none', mobile: 'b-kind', esp_backup: 'b-inconclusive', desktop_sim: 'b-lie' };
const MODE_TH = { fix: 'fix', manual: 'manual', live: 'ใช้งานจริง', watch: 'จากนาฬิกา', legacy: 'ไฟล์เก่า' };
const PAGE = 100;

// ป้ายแหล่งที่มา 1 ข้อ
export const originBadge = (o) => `<span class="badge ${ORIGIN_CLS[o] || 'b-none'}" title="${esc(o || '')}">${esc(ORIGIN_TH[o] || o || '-')}</span>`;

// html ของการ์ด — prefix ทำให้ใช้หลายหน้าพร้อมกันได้โดย id ไม่ชนกัน
export function historyHtml(prefix, title = 'ประวัติทั้งหมด (ทุกไฟล์ result_*.csv)') {
  return `<div class="card" id="${prefix}-hcard"><h3>${I('list')} ${title}
      <span class="right small muted" id="${prefix}-hcount"></span></h3>
    <div class="row" style="margin-bottom:10px">
      <select class="input sm-input" id="${prefix}-hfile"><option value="all">ทุกไฟล์</option></select>
      <select class="input sm-input" id="${prefix}-hlabel">
        <option value="">ทุกเฉลย</option><option value="truth">ตอบจริง</option><option value="lie">โกหก</option>
        <option value="unknown">ไม่รู้เฉลย</option><option value="aborted">ยกเลิก</option><option value="train">เฉพาะที่ใช้เทรน</option></select>
      <select class="input sm-input" id="${prefix}-horigin" title="แหล่งที่มาของข้อมูล">
        <option value="">ทุกแหล่งที่มา</option>${Object.entries(ORIGIN_TH).filter(([k]) => k !== 'import')
          .map(([k, v]) => `<option value="${k}">${esc(v)}</option>`).join('')}</select>
      <span class="spacer" style="flex:1"></span>
      <a class="btn sm" id="${prefix}-hdl1" href="#">${I('down')} ดาวน์โหลดไฟล์นี้</a>
      <a class="btn sm" id="${prefix}-hdlall" href="#">${I('down')} รวมทุกไฟล์เป็นไฟล์เดียว</a>
      <button class="btn sm ghost danger hidden" id="${prefix}-hdelfile">${I('trash')} ลบไฟล์นี้</button>
      <button class="btn sm ghost" id="${prefix}-hre">รีเฟรช</button>
    </div>
    <div class="row small hidden" id="${prefix}-hselbar" style="margin-bottom:8px">
      <span id="${prefix}-hseln"></span>
      <button class="btn sm danger" id="${prefix}-hdelsel">${I('trash')} ลบข้อที่เลือก</button>
      <button class="btn sm ghost" id="${prefix}-hnouse">ไม่ใช้เทรนข้อที่เลือก</button>
      <button class="btn sm ghost" id="${prefix}-hclr">ยกเลิกการเลือก</button>
    </div>
    <div style="overflow:auto"><table class="t small" id="${prefix}-htable"></table></div>
    <div class="row" style="margin-top:8px"><button class="btn sm ghost hidden" id="${prefix}-hmore">แสดงเพิ่ม</button>
      <span class="small muted">ลบแล้วกู้คืนได้ — สำเนาเก็บใน <span class="mono">data/trash/</span> (ดูได้ที่หน้า "ข้อมูล &amp; เทรน AI")</span></div>
  </div>`;
}

// ผูกการทำงาน — คืน { refresh(), setFile(name) }
// opts.modes = 'live' หรือ 'fix,manual' (ว่าง = ทุกโหมด)
export function mountHistory(root, prefix, opts = {}) {
  const modes = opts.modes || '';
  let rows = [], shown = PAGE, timer = null;
  const sel = new Set();                               // ข้อที่ติ๊กเลือก (key = ไฟล์|เลขข้อ|เวลา)
  const el = (id) => $(`#${prefix}-${id}`, root);
  const keyOf = (x) => `${x._file}|${x.question_no}|${x.time_iso || ''}`;

  // โหลดรายชื่อไฟล์ result_*.csv มาใส่ตัวเลือก "ไฟล์" (กรองตามโหมดของหน้านั้น)
  async function loadFiles() {
    const r = await get('/api/data/files', { quiet: true });
    if (!r || !r.files) return;
    const s = el('hfile'), cur = s.value;
    const files = r.files.filter((f) => !modes || (f.modes || []).some((m) => modes.split(',').includes(m)));
    s.innerHTML = `<option value="all">ทุกไฟล์ (${files.length})</option>` + files.slice().reverse()
      .map((f) => `<option value="${esc(f.name)}">${esc(f.name)} · ${esc((f.subjects || []).join(', '))} · ${f.rows} ข้อ</option>`).join('');
    if ([...s.options].some((o) => o.value === cur)) s.value = cur;
  }

  // โหลดทุกแถวของไฟล์ที่เลือก (หรือทุกไฟล์) แล้วตั้งลิงก์ดาวน์โหลด
  async function loadRows() {
    const file = el('hfile').value;
    const q = `/api/data/rows?file=${encodeURIComponent(file)}${modes ? `&mode=${modes}` : ''}`;
    const r = await get(q, { quiet: true });
    rows = (r && r.rows) || [];
    shown = PAGE;
    const keys = new Set(rows.map(keyOf));
    [...sel].forEach((k) => { if (!keys.has(k)) sel.delete(k); });   // ข้อที่ถูกลบไปแล้วไม่ต้องจำ
    paint();
    // ลิงก์ดาวน์โหลด: ไฟล์เดียว = ไฟล์จริงในเครื่อง, รวม = backend สร้าง CSV ให้ตอนกด
    const one = el('hdl1');
    one.classList.toggle('hidden', file === 'all');
    one.href = file === 'all' ? '#' : `/api/data/file/${encodeURIComponent(file)}`;
    el('hdelfile').classList.toggle('hidden', file === 'all');
    el('hdlall').href = `/api/data/merged.csv?files=all${modes ? `&mode=${modes}` : ''}`;
  }

  // แถวที่ผ่านตัวกรอง (เฉลย + แหล่งที่มา)
  function filtered() {
    const lf = el('hlabel').value, of = el('horigin').value;
    return rows.filter((x) => (!lf || (lf === 'train' ? String(x.used_for_training) === '1' : x.label === lf))
      && (!of || x._origin === of));
  }

  // วาดตาราง (แสดงทีละ 100 แถว กดแสดงเพิ่มได้)
  function paint() {
    const list = filtered();
    el('hcount').textContent = `${list.length} ข้อ`;
    const live = modes === 'live';
    const head = `<tr><th><input type="checkbox" data-all title="เลือกทุกข้อที่แสดง"></th><th>เวลา</th><th>ไฟล์ / ข้อ</th><th>qid</th>
      <th>โหมด</th><th>ที่มา</th><th>ผู้ตอบ</th>${live ? '<th>คำถาม</th>' : ''}
      <th>เฉลย</th><th>นาฬิกาทาย</th><th>P(โกหก)</th><th>ตัดสินด้วย</th>${live ? '<th>ถูก/ผิด</th>' : ''}<th>ใช้เทรน</th><th>หมายเหตุ</th><th></th></tr>`;
    const body = list.slice(0, shown).map((x, i) => {
      const vi = vinfo(x.verdict);
      const p = x.p_lie !== '' && x.p_lie != null ? Math.round(parseFloat(x.p_lie) * 100) + '%' : '-';
      const canTrain = (x.label === 'truth' || x.label === 'lie');
      const used = String(x.used_for_training) === '1';
      return `<tr><td><input type="checkbox" data-sel="${i}" ${sel.has(keyOf(x)) ? 'checked' : ''}></td>
        <td class="mono">${esc((x.time_iso || '').slice(5, 19))}</td>
        <td class="mono">${esc((x._file || '').replace('result_', '').replace('.csv', ''))} #${esc(x.question_no)}</td>
        <td>${esc(x.watch_qid)}</td><td>${esc(MODE_TH[x.mode] || x.mode)}</td><td>${originBadge(x._origin)}</td><td>${esc(x.subject)}</td>
        ${live ? `<td>${esc(x.question_text || '-')}</td>` : ''}
        <td>${canTrain ? `<span class="badge ${x.label === 'lie' ? 'b-lie' : 'b-truth'}">${LABEL_TH[x.label]}</span>` : esc(LABEL_TH[x.label] || x.label)}</td>
        <td>${x.verdict ? `<span class="badge ${vi.cls}">${esc(vi.th)}</span>` : '-'}</td><td>${p}</td>
        <td class="small">${esc(x.decided_by || '-')}</td>
        ${live ? `<td>${esc(FB_TH[x.feedback] || '-')}</td>` : ''}
        <td>${canTrain ? `<input type="checkbox" data-use="${i}" ${used ? 'checked' : ''} title="ติ๊ก = train.py ใช้ข้อนี้">` : '<span class="muted">-</span>'}</td>
        <td class="small muted" style="max-width:260px">${esc(x.note || '')}</td>
        <td><button class="btn sm ghost" data-del="${i}" title="ลบข้อนี้ (กู้คืนได้จากถังขยะ)">${I('trash')}</button></td></tr>`;
    }).join('');
    el('htable').innerHTML = head + (body || `<tr><td colspan="16" class="muted">ยังไม่มีข้อมูล</td></tr>`);
    el('hmore').classList.toggle('hidden', list.length <= shown);
    const view = list.slice(0, shown);
    el('htable').querySelectorAll('input[data-sel]').forEach((c) => {
      c.onchange = () => { const k = keyOf(view[+c.dataset.sel]); c.checked ? sel.add(k) : sel.delete(k); paintSel(); };
    });
    const all = el('htable').querySelector('input[data-all]');
    all.checked = view.length > 0 && view.every((x) => sel.has(keyOf(x)));
    all.onchange = () => { view.forEach((x) => (all.checked ? sel.add(keyOf(x)) : sel.delete(keyOf(x)))); paint(); };
    el('htable').querySelectorAll('input[data-use]').forEach((c) => {
      c.onchange = async () => {
        const x = view[+c.dataset.use];
        const r = await post('/api/data/set_used', { name: x._file, key: key(x), used: c.checked });
        if (r && r.ok) toast(r.msg || 'บันทึกแล้ว', 'ok'); else c.checked = !c.checked;
      };
    });
    el('htable').querySelectorAll('button[data-del]').forEach((b) => {
      b.onclick = () => removeRows([view[+b.dataset.del]]);
    });
    paintSel();
  }

  // แถบ "เลือกแล้ว N ข้อ" + ปุ่มลบ/ไม่ใช้เทรนข้อที่เลือก
  function paintSel() {
    el('hselbar').classList.toggle('hidden', sel.size === 0);
    el('hseln').textContent = `เลือกไว้ ${sel.size} ข้อ`;
  }
  const key = (x) => ({ question_no: String(x.question_no), time_iso: x.time_iso || '' });
  const selectedRows = () => rows.filter((x) => sel.has(keyOf(x)));

  // ลบหลายข้อ (แยกส่งทีละไฟล์) — backend สำรองไฟล์ไว้ในถังขยะก่อนทุกครั้ง
  async function removeRows(list) {
    if (!list.length) return;
    const byFile = {};
    list.forEach((x) => { (byFile[x._file] = byFile[x._file] || []).push(key(x)); });
    const nf = Object.keys(byFile).length;
    const ok = await confirmModal(`ลบ ${list.length} ข้อ?`,
      `<p>ข้อที่เลือกจะถูกลบออกจากไฟล์ ${nf > 1 ? `(${nf} ไฟล์)` : `<span class="mono">${esc(list[0]._file)}</span>`}
        — ระบบสำรองไฟล์เดิมไว้ใน <span class="mono">data/trash/</span> ก่อน กู้คืนได้ที่หน้า "ข้อมูล &amp; เทรน AI"</p>
       <p class="small muted">ถ้าแค่ไม่อยากให้ AI ใช้ข้อนี้ ให้เอาติ๊ก "ใช้เทรน" ออกแทน (ข้อมูลยังอยู่)</p>`, 'ลบ', true);
    if (!ok) return;
    let n = 0;
    for (const [name, keys] of Object.entries(byFile)) {
      const r = await post('/api/data/delete_rows', { name, keys });
      if (r && r.ok) n += r.changed || 0;
    }
    list.forEach((x) => sel.delete(keyOf(x)));
    toast(`ลบแล้ว ${n} ข้อ`, 'ok');
    refresh();
  }

  // ลบทั้งไฟล์ที่เลือกอยู่ = ย้ายไปถังขยะ
  async function removeFile() {
    const name = el('hfile').value;
    if (name === 'all') return;
    if (!(await confirmModal('ลบทั้งไฟล์?', `<p>ย้าย <span class="mono">${esc(name)}</span> (และไฟล์ค่าสดของรอบนั้น) ไปที่
      <span class="mono">data/trash/</span> — train.py จะไม่อ่านอีก กู้คืนได้ที่หน้า "ข้อมูล &amp; เทรน AI"</p>`, 'ย้ายไปถังขยะ', true))) return;
    const r = await post('/api/data/delete_file', { name });
    if (r && r.ok) { toast(r.msg, 'ok', 6000); el('hfile').value = 'all'; refresh(); }
  }

  // ไม่ใช้เทรนทุกข้อที่เลือก (ทีละข้อ — จำนวนน้อย ไม่ต้องทำ batch)
  async function noUseSelected() {
    const list = selectedRows().filter((x) => String(x.used_for_training) === '1');
    for (const x of list) await post('/api/data/set_used', { name: x._file, key: key(x), used: false }, { quiet: true });
    toast(`ตั้ง "ไม่ใช้เทรน" ${list.length} ข้อ`, 'ok');
    refresh();
  }

  // โหลดใหม่ทั้งรายชื่อไฟล์และแถว
  async function refresh() { await loadFiles(); await loadRows(); }
  el('hfile').onchange = loadRows;
  el('hlabel').onchange = paint;
  el('horigin').onchange = paint;
  el('hre').onclick = refresh;
  el('hdelfile').onclick = removeFile;
  el('hdelsel').onclick = () => removeRows(selectedRows());
  el('hnouse').onclick = noUseSelected;
  el('hclr').onclick = () => { sel.clear(); paint(); };
  el('hmore').onclick = () => { shown += PAGE; paint(); };
  // มีข้อใหม่ถูกบันทึก (backend ส่ง "data") -> รีเฟรช (หน่วง 0.5 วิ รวมหลายเหตุการณ์เป็นครั้งเดียว)
  on('data', () => { clearTimeout(timer); timer = setTimeout(refresh, 500); });
  refresh();
  return { refresh, setFile: (n) => { el('hfile').value = n; loadRows(); } };
}
