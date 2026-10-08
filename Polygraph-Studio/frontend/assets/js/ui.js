// ui.js — ตัวช่วยสร้างหน้าเว็บ: element, ตัวเลข, toast, กล่องยืนยัน, สี/ชื่อของคำตัดสิน

export const $ = (sel, root = document) => root.querySelector(sel);
export const $$ = (sel, root = document) => [...root.querySelectorAll(sel)];

// escape ข้อความก่อนใส่ HTML (กันชื่อ/คำถามที่มี < > ทำหน้าเว็บพัง)
export function esc(s) {
  return String(s ?? '').replace(/[&<>"']/g, (c) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' }[c]));
}

// แปลงตัวเลขให้อ่านง่าย: null/NaN -> "--"
export function fmt(x, d = 1) {
  if (x === null || x === undefined || Number.isNaN(x)) return '--';
  return Number(x).toFixed(d);
}

// จำนวนไบต์ -> B / KB / MB
export function bytes(n) {
  if (n === null || n === undefined) return '--';
  if (n >= 1048576) return (n / 1048576).toFixed(2) + ' MB';
  if (n >= 1024) return (n / 1024).toFixed(1) + ' KB';
  return n + ' B';
}

// วินาที -> "x ชม. y นาที" อ่านง่าย
export function duration(sec) {
  sec = Math.max(0, Math.floor(sec || 0));
  const h = Math.floor(sec / 3600), m = Math.floor((sec % 3600) / 60), s = sec % 60;
  return h ? `${h} ชม. ${m} นาที` : m ? `${m} นาที ${s} วินาที` : `${s} วินาที`;
}

// epoch -> วันเวลาแบบไทย
export function timeStr(ts) {
  if (!ts) return '-';
  const d = new Date(ts * 1000);
  return d.toLocaleString('th-TH', { dateStyle: 'medium', timeStyle: 'medium' });
}

export const VERDICT = {
  lie: { th: 'โกหก', color: 'var(--lie)', cls: 'b-lie', icon: '' },
  truth: { th: 'พูดจริง', color: 'var(--truth)', cls: 'b-truth', icon: '' },
  inconclusive: { th: 'ไม่แน่ชัด', color: 'var(--inc)', cls: 'b-inconclusive', icon: '' },
  invalid: { th: 'วัดไม่ได้', color: 'var(--invalid)', cls: 'b-invalid', icon: '' },
  none: { th: '—', color: 'var(--text)', cls: 'b-none', icon: '' },
};
export const vinfo = (v) => VERDICT[v] || VERDICT.none;

export const KIND = {
  test: { th: 'คำถามจริง', cls: 'b-kind' },
  truth: { th: 'ควบคุม: ตอบจริง', cls: 'b-ctl-truth' },
  lie: { th: 'ควบคุม: สั่งให้โกหก', cls: 'b-ctl-lie' },
  warmup: { th: 'อุ่นเครื่อง', cls: 'b-warmup' },
};
export const kinfo = (k) => KIND[k] || KIND.test;
export const KIND_CODES = ['test', 'truth', 'lie', 'warmup'];
export const STATE_TH = ['ว่าง', 'กำลังวัด baseline', 'พร้อมถาม', 'กำลังวัดคำถาม'];

// bit ของ flags ที่นาฬิกาส่งมา (ต้องตรงกับ app.h ในเฟิร์มแวร์)
export const FLAGS = {
  1: ['danger', 'ไม่พบเซนเซอร์ชีพจร MAX30102 — ตรวจสาย I2C (SDA=GPIO6, SCL=GPIO7)'],
  2: ['danger', 'ไม่พบเซนเซอร์ความเร่ง MPU6050 — ตรวจสาย I2C'],
  4: ['warn', 'เซนเซอร์อุณหภูมิ (NTC) ขาด/ลัด — ตรวจหัวต่อขา GPIO0'],
  8: ['warn', 'แผ่น GSR สองแผ่นแตะกันเอง (ลัดวงจร) — แยกแผ่นออกจากกัน'],
  16: ['warn', 'สัญญาณชีพจรแรงจนชนเพดาน — กดนิ้วเบาลง หรือลดกระแส LED'],
  32: ['warn', 'แบตเตอรี่อ่อน — ควรชาร์จ'],
  64: ['danger', 'แบตเตอรี่ใกล้หมด! นาฬิกาจะเข้า deep sleep เพื่อป้องกันแบตเสื่อม'],
  128: ['info', 'เพิ่งกู้บัส I2C สำเร็จ (watchdog เซนเซอร์ทำงาน)'],
  256: ['danger', 'นาฬิกาต่อสาย USB กับคอมอยู่ — ห้ามวัด GSR กับคนขณะเสียบสาย (ความปลอดภัยทางไฟฟ้า)'],
  512: ['warn', 'ขยับตัวมาก — ให้ผู้ถูกทดสอบนั่งนิ่ง ๆ สัญญาณจะได้แม่น'],
  1024: ['info', 'เฟิร์มแวร์ใหม่จาก OTA กำลังรอยืนยัน (20 วินาที) — อย่าเพิ่งปิดเครื่อง'],
  4096: ['warn', 'ระบบไฟล์ LittleFS ใช้งานไม่ได้ — log จะไม่ถูกบันทึก'],
};

// ---------------------------------------------------------------- element builder
export function h(tag, attrs = {}, ...kids) {
  const el = document.createElement(tag);
  for (const [k, v] of Object.entries(attrs || {})) {
    if (k === 'class') el.className = v;
    else if (k === 'html') el.innerHTML = v;
    else if (k.startsWith('on')) el.addEventListener(k.slice(2), v);
    else if (v !== false && v !== null && v !== undefined) el.setAttribute(k, v === true ? '' : v);
  }
  for (const c of kids.flat()) {
    if (c === null || c === undefined || c === false) continue;
    el.append(c.nodeType ? c : document.createTextNode(String(c)));
  }
  return el;
}

// ---------------------------------------------------------------- toast
export function toast(msg, type = 'info', ms = 4500) {
  const box = document.getElementById('toasts');
  const el = h('div', { class: `toast ${type}` }, msg);
  box.append(el);
  setTimeout(() => { el.style.opacity = '0'; el.style.transition = '.4s'; }, ms);
  setTimeout(() => el.remove(), ms + 450);
}

// ---------------------------------------------------------------- กล่องยืนยัน
export function confirmModal(title, bodyHtml, okText = 'ยืนยัน', danger = false) {
  return new Promise((resolve) => {
    const bg = h('div', { class: 'modal-bg' });
    const close = (v) => { bg.remove(); document.removeEventListener('keydown', onKey); resolve(v); };
    const onKey = (e) => { if (e.key === 'Escape') close(false); };
    bg.append(h('div', { class: 'modal' },
      h('h3', {}, title),
      h('div', { html: bodyHtml }),
      h('div', { class: 'row', style: 'justify-content:flex-end;margin-top:18px' },
        h('button', { class: 'btn', onclick: () => close(false) }, 'ยกเลิก'),
        h('button', { class: `btn ${danger ? 'danger' : 'primary'}`, onclick: () => close(true) }, okText))));
    bg.addEventListener('click', (e) => { if (e.target === bg) close(false); });
    document.addEventListener('keydown', onKey);
    document.body.append(bg);
  });
}

// อ่านค่าตัวแปรสีจาก CSS (เปลี่ยนตามธีมสว่าง/มืด)
export function cssVar(name) {
  return getComputedStyle(document.documentElement).getPropertyValue(name).trim();
}
