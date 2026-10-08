// views/home.js — หน้าหลัก "Wireless Polygraph": รวมทุกอย่างที่ใช้บ่อยไว้หน้าเดียว
//
//  ┌──────────────── ค่าที่วัดได้ขณะนี้ (6 ช่อง) + กราฟเรียลไทม์ 4 กราฟ ────────────────┐
//  │ ซ้าย: ทดสอบแบบเซสชัน (ถามต่อกันหลายข้อ มีชุดคำถาม/รายงาน)  │ ขวา: ควบคุมด่วนทีละข้อ   │
//  │      = โมดูล session.js ตัวเดิม ฝังไว้ตรงนี้                │      ตั้งค่าพลังงาน/WiFi   │
//  │                                                           │      ผู้จัดทำ              │
//  └──────────────────────────────────────────────────────────────────────────────────────┘
// ข้อมูลสดมาทาง WebSocket (ws.js -> store.js) ส่วนคำสั่งส่งผ่าน REST /api/watch/command ไปนาฬิกา
import { S, on } from '../store.js';
import { $, esc, fmt, toast, confirmModal, vinfo } from '../ui.js';
import { cmd } from '../api.js';
import { tilesHtml, paintTiles, chartsHtml, makeCharts, I } from '../widgets.js';
import * as session from './session.js';

let root, updCharts = () => {}, quickNo = 0;
const SRC_TH = ['สูตรมาตรฐาน', 'สูตรปรับจากข้อควบคุม', 'โมเดล AI'];

// สร้างหน้าหลัก: ค่าสด + กราฟ, ซ้าย = ทดสอบแบบเซสชัน, ขวา = ควบคุมด่วน/พลังงาน/ผู้จัดทำ
export function mount(el) {
  root = el;
  root.innerHTML = `
    <div class="card"><h3>${I('pulse')} ค่าที่วัดได้ขณะนี้ <span class="right small muted">อัปเดต 5 ครั้ง/วินาที · กราฟ 2 นาทีล่าสุด</span></h3>
      ${tilesHtml('hm')}${chartsHtml('hm')}</div>
    <div class="grid home-cols" style="margin-top:16px">
      <div class="home-left">
        <div class="section-tag">${I('list')} ทดสอบแบบเซสชัน — ถามต่อกันหลายข้อ (มีชุดคำถาม, สรุปผล, รายงาน)</div>
        <div id="home-session"></div>
      </div>
      <div class="stack">
        <div class="section-tag">${I('target')} ควบคุมด่วน — ถามทีละข้อ</div>
        <div class="card" id="hm-quick">
          <div class="lbl-c small muted">ผลการตรวจล่าสุด</div>
          <div class="vd-word" id="hm-vd">-</div>
          <div class="pbar"><div class="fill" id="hm-pfill"></div></div>
          <div class="row" id="hm-vmeta" style="justify-content:center;margin-top:8px"></div>
          <hr class="sep">
          <div class="statebar"><span id="hm-state">-</span></div>
          <div class="progress"><i id="hm-prog"></i></div>
          <div class="btn-grid">
            <button class="btn span-2" id="hm-base">${I('target')} วัดค่าปกติ (baseline)</button>
            <button class="btn primary span-2" id="hm-ask">${I('pulse')} เริ่มคำถาม</button>
            <button class="btn success" id="hm-yes">${I('check')} ตอบว่า "ใช่"</button>
            <button class="btn danger" id="hm-no">${I('x')} ตอบว่า "ไม่ใช่"</button>
            <button class="btn ghost" id="hm-abort">${I('stop')} ยกเลิกข้อนี้</button>
            <button class="btn ghost" id="hm-reset">${I('layers')} เริ่มใหม่ทั้งหมด</button>
          </div>
          <ol class="small muted steps">
            <li>ใส่นาฬิกาให้เซนเซอร์ชีพจรแนบผิว นั่งนิ่ง ๆ</li>
            <li>กด "วัดค่าปกติ" รอจนสถานะเป็น "พร้อมถาม"</li>
            <li>กด "เริ่มคำถาม" แล้วถามทันที เมื่อผู้ถูกทดสอบตอบ ให้กด "ใช่" หรือ "ไม่ใช่"</li>
            <li>รอ 12 วินาที ผลแสดงด้านบน — ตัดสินโดยนาฬิกา (LieEngine หรือโมเดล AI)</li>
          </ol>
        </div>
        <div class="card"><h3>${I('gear')} พลังงานและการเชื่อมต่อ</h3>
          <div class="setrow"><span>${I('clock')} หลับอัตโนมัติเมื่อไม่มีใครใช้</span>
            <select class="input" id="hm-sby"><option value="0">ปิด (ค่าเริ่มต้น)</option><option value="5">5 นาที</option>
              <option value="10">10 นาที</option><option value="30">30 นาที</option><option value="60">60 นาที</option></select></div>
          <div class="setrow"><span>${I('chip')} โหมดประหยัดไฟ (ECO)</span>
            <select class="input" id="hm-eco"><option value="0">ปิด (CPU 160 MHz)</option><option value="1">เปิด (CPU 80 MHz)</option></select></div>
          <div class="setrow"><span>${I('wifi')} กำลังส่ง WiFi</span>
            <select class="input" id="hm-wp"><option value="0">ต่ำ (แนะนำเมื่อใช้แบต)</option><option value="1">กลาง</option><option value="2">สูง</option></select></div>
          <div class="row" style="margin-top:10px"><button class="btn" id="hm-light">${I('clock')} พักเครื่องตอนนี้</button>
            <button class="btn" id="hm-deep">${I('clock')} หลับลึก 60 วินาที</button></div>
          <p class="small muted" style="margin-bottom:0">พักเครื่อง (light sleep): WiFi หลุด กดปุ่ม BOOT บนนาฬิกาเพื่อปลุก ค่าปกติยังอยู่ ·
            หลับลึก (deep sleep): ปิดเกือบทั้งหมด ตื่นเองแล้วบูตใหม่ ต้องวัดค่าปกติใหม่</p>
        </div>
        <div class="card" id="hm-credits"></div>
      </div>
    </div>`;
  updCharts = makeCharts(root, 'hm');
  session.mount($('#home-session', root));

  $('#hm-base', root).onclick = async () => { const r = await cmd('baseline'); if (r && r.ok) toast('เริ่มวัดค่าปกติ — นั่งนิ่ง ๆ หายใจปกติ', 'ok'); };
  $('#hm-ask', root).onclick = async () => {
    // ควบคุมด่วนใช้ qid 500-599 (ไม่ชนกับคำถามในเซสชัน 1-499 และหน้าเก็บข้อมูล 600-799)
    const qid = 500 + (quickNo % 100);
    const r = await cmd('question', { qid, kind: 'test' });
    if (r && r.ok) { quickNo++; toast('ถามคำถามได้เลย แล้วกดปุ่มตามคำตอบ', 'ok'); }
  };
  $('#hm-yes', root).onclick = () => cmd('answer', { yes: true });
  $('#hm-no', root).onclick = () => cmd('answer', { yes: false });
  $('#hm-abort', root).onclick = () => cmd('abort');
  $('#hm-reset', root).onclick = async () => {
    if (await confirmModal('เริ่มใหม่ทั้งหมด?', '<p>ล้างค่าปกติ (baseline) และผลในหน่วยความจำของนาฬิกา (ข้อมูลในไฟล์ไม่หาย)</p>', 'เริ่มใหม่', true)) cmd('reset');
  };
  $('#hm-sby', root).onchange = (e) => cmd('sleep', { auto: Number(e.target.value) })
    .then((r) => r && r.ok && toast(e.target.value === '0' ? 'ปิดการหลับอัตโนมัติแล้ว' : `นาฬิกาจะหลับเมื่อไม่มีใครใช้ ${e.target.value} นาที`, 'ok'));
  $('#hm-eco', root).onchange = (e) => cmd('power', { mode: e.target.value === '1' ? 'eco' : 'normal' });
  $('#hm-wp', root).onchange = (e) => cmd('wifi', { level: Number(e.target.value) });
  $('#hm-light', root).onclick = async () => {
    if (await confirmModal('พักเครื่อง?', '<p>WiFi จะหลุดจนกว่าจะกดปุ่ม BOOT บนนาฬิกา</p>', 'พักเครื่อง')) cmd('sleep', { now: 'light' });
  };
  $('#hm-deep', root).onclick = async () => {
    if (await confirmModal('หลับลึก 60 วินาที?', '<p>นาฬิกาจะบูตใหม่เมื่อตื่น ค่าปกติและผลในหน่วยความจำจะหาย</p>', 'หลับลึก', true)) cmd('sleep', { now: 'deep', sec: 60 });
  };

  on('vitals', paintLive);
  on('snapshot', () => { paintLive(); paintResult(); paintCredits(); });
  on('lie', paintResult);
  paintCredits();
}

// กลับมาที่หน้านี้: วาดค่าล่าสุด
export function show() { session.show(); paintLive(); paintResult(); }
// ออกจากหน้านี้
export function hide() {}

// ---------------------------------------------------------------- ค่าสด + สถานะ + ปุ่ม
function paintLive() {
  if (!root) return;
  paintTiles(root, 'hm');
  updCharts();
  const v = S.live;
  if (!v) return;
  const es = v.es, p = Math.round((v.ep || 0) * 100);
  const cfg = (S.lie && S.lie.config) || {};
  let st = 'ขั้นแรก: กด "วัดค่าปกติ"';
  if (es === 1) st = `กำลังวัดค่าปกติ เหลือ ${Math.max(0, Math.ceil((1 - v.ep) * (cfg.baselineSec || 30)))} วินาที — นั่งนิ่ง ๆ`;
  else if (es === 3) st = `กำลังวัดคำตอบ เหลือ ${Math.max(0, Math.ceil((1 - v.ep) * (cfg.windowSec || 12)))} วินาที`;
  else if (es === 2) st = v.set ? 'พร้อมถามข้อถัดไป' : 'รอให้ร่างกายกลับสู่ปกติสักครู่ แล้วค่อยถาม';
  $('#hm-state', root).textContent = st;
  $('#hm-prog', root).style.width = (es === 1 || es === 3 ? p : es === 2 ? 100 : 0) + '%';
  $('#hm-base', root).disabled = es === 1 || es === 3;
  $('#hm-ask', root).disabled = es !== 2;
  $('#hm-yes', root).disabled = es !== 3;
  $('#hm-no', root).disabled = es !== 3;
  $('#hm-abort', root).disabled = !(es === 1 || es === 3);
  // ตั้งค่าที่นาฬิกาใช้อยู่จริง (ไม่ทับตอนผู้ใช้กำลังเลือก)
  [['hm-sby', v.sby], ['hm-eco', v.eco], ['hm-wp', v.wp]].forEach(([id, val]) => {
    const s = $(`#${id}`, root);
    if (!s || val == null || document.activeElement === s) return;
    if (![...s.options].some((o) => +o.value === +val)) s.add(new Option(`${val} นาที`, val));
    s.value = String(val);
  });
}

// ---------------------------------------------------------------- ผลล่าสุด (จาก /api/lie ที่ backend ซิงก์ไว้)
function paintResult() {
  if (!root) return;
  const r = S.lie && S.lie.results && S.lie.results[0];
  const vd = $('#hm-vd', root), fill = $('#hm-pfill', root), meta = $('#hm-vmeta', root);
  if (!r) { vd.textContent = '-'; vd.style.color = ''; fill.style.width = '0'; meta.innerHTML = '<span class="pill small">ยังไม่มีผล — วัดค่าปกติแล้วเริ่มคำถาม</span>'; return; }
  const vi = vinfo(r.verdict);
  const p = Math.round((r.p || 0) * 100);
  vd.textContent = vi.th; vd.style.color = vi.color;
  root.querySelector('#hm-quick').style.setProperty('--vcolor', vi.color);
  fill.style.width = p + '%';
  meta.innerHTML = [`ข้อ ${r.qid}`, `โอกาสโกหก ${p}%`, `ตัดสินด้วย${SRC_TH[r.src] || 'สูตรมาตรฐาน'}`, `คุณภาพสัญญาณ ${r.quality}%`]
    .map((t) => `<span class="pill small">${esc(t)}</span>`).join('');
}

// ---------------------------------------------------------------- ผู้จัดทำ
function paintCredits() {
  const el = root && $('#hm-credits', root);
  const c = S.credits;
  if (!el || !c) return;
  el.innerHTML = `<h3>${I('book')} เกี่ยวกับโครงงาน</h3>
    <div class="kv"><span>โครงงาน</span><span>${esc(c.project)}</span>
    <span>รายวิชา</span><span>${esc(c.course)}</span>
    <span>หน่วยงาน</span><span>${esc(c.org)}</span>
    <span>ภาคการศึกษา</span><span>${esc(c.year)}</span>
    <span>ผู้จัดทำ</span><span>${c.members.map((m) => `${esc(m.name)} ${esc(m.id)}`).join('<br>')}</span></div>`;
}

export { fmt };
