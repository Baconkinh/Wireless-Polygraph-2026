// views/home.js — หน้าแรก "ใช้งานจริง" (เมนูแรก) = use.js + ตั้งค่าพลังงาน + ผู้จัดทำ
//
//  ┌──────────── สัญญาณสด + กราฟ, ถามคำถาม (เลือกตัดสินด้วย AI / สูตรมาตรฐาน), ถูก/ผิด, ประวัติ ────────────┐
//  │                         = โมดูล use.js ทั้งหน้า ฝังไว้ตรงนี้ (#home-use)                                │
//  ├──────────────────────────────────────┬───────────────────────────────────────────────────────────────┤
//  │ พลังงานและการเชื่อมต่อ (sleep/ECO/WiFi)  │ เกี่ยวกับโครงงาน                                               │
//  └──────────────────────────────────────┴───────────────────────────────────────────────────────────────┘
// เปลี่ยนเมื่อ 9 ต.ค. 2026: เดิมหน้านี้มี "ทดสอบแบบเซสชัน" (ชุดคำถาม/รายงาน) + "ควบคุมด่วน" ซึ่งผู้ใช้บอกว่างงและใช้ยาก
// จึงเอาออก แล้วใช้หน้า "ใช้งานจริง" เป็นหน้าหลักแทน (เมนู "ใช้งานจริง" แยกถูกลบ ลิงก์ #use เดิมพามาหน้านี้)
// ข้อมูลสดมาทาง WebSocket (ws.js -> store.js) ส่วนคำสั่งส่งผ่าน REST /api/watch/command ไปนาฬิกา
import { S, on } from '../store.js';
import { $, esc, toast, confirmModal } from '../ui.js';
import { cmd } from '../api.js';
import { I } from '../widgets.js';
import * as use from './use.js';

let root;

// สร้างหน้าหลัก: หน้าใช้งานจริง (use.js) + การ์ดพลังงาน + ผู้จัดทำ
export function mount(el) {
  root = el;
  root.innerHTML = `
    <div id="home-use"></div>
    <div class="grid g2" style="margin-top:16px">
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
    </div>`;
  use.mount($('#home-use', root));

  // ตั้งค่าพลังงาน: ส่งคำสั่งผ่าน backend -> POST /api/sleep, /api/power, /api/wifi ของนาฬิกา
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

  on('vitals', paintPower);
  on('snapshot', () => { paintPower(); paintCredits(); });
  paintCredits();
}

// กลับมาที่หน้านี้: ให้ use.js วาดใหม่
export function show() { use.show(); paintPower(); }
// ออกจากหน้านี้
export function hide() { use.hide(); }

// ค่าตั้งพลังงานที่นาฬิกาใช้อยู่จริง (มากับค่าสด sby/eco/wp) — ไม่ทับตอนผู้ใช้กำลังเลือก
function paintPower() {
  const v = S.live;
  if (!root || !v) return;
  [['hm-sby', v.sby], ['hm-eco', v.eco], ['hm-wp', v.wp]].forEach(([id, val]) => {
    const s = $(`#${id}`, root);
    if (!s || val == null || document.activeElement === s) return;
    if (![...s.options].some((o) => +o.value === +val)) s.add(new Option(`${val} นาที`, val));
    s.value = String(val);
  });
}

// การ์ดผู้จัดทำ (ข้อมูลจาก backend: S.credits)
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
