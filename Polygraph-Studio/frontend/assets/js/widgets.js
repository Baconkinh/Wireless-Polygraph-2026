// widgets.js — ชิ้นส่วนหน้าจอที่ใช้ซ้ำหลายหน้า: ช่องค่าเซนเซอร์ 6 ช่อง + กราฟเส้นเรียลไทม์ 4 กราฟ
// ใช้ในหน้า "หน้าหลัก" และ "เก็บข้อมูลเทรน AI"
// ข้อมูลมาจาก store.js (S.live = ค่าล่าสุด, S.hist = ย้อนหลัง 2 นาที) ซึ่ง ws.js เติมให้ทุกครั้งที่นาฬิกาส่งค่า (5 ครั้ง/วินาที)
import { S } from './store.js';
import { $, fmt } from './ui.js';
import { chartTheme, liveLine, updateLive } from './charts.js';

const I = (n) => `<svg class=i><use href=#i-${n}></use></svg>`;

// ลำดับ feature ใน baseline ของนาฬิกา: [GSR, ชีพจร, แรงชีพจร, การสั่น, อุณหภูมิ]
const BASE_IDX = { gsr: 0, hr: 1, trm: 3, tmp: 4 };

// ---------------------------------------------------------------- ช่องค่าเซนเซอร์
export function tilesHtml(prefix) {
  const tile = (id, icon, name, unit) => `
    <div class="vital tile" id="${prefix}-${id}"><div class="name"><span>${I(icon)} ${name}</span><span class="base small"></span></div>
      <div class="v"><span class="num">--</span><small>${unit}</small></div><div class="meta"></div></div>`;
  return `<div class="tiles6">
    ${tile('hr', 'pulse', 'ชีพจร', 'bpm')}${tile('gsr', 'info', 'GSR (ความชื้นผิว)', 'µS')}${tile('tmp', 'target', 'อุณหภูมิผิว', '°C')}
    ${tile('trm', 'layers', 'มือสั่น', 'm/s²')}${tile('si', 'chart', 'ระดับความตื่นตัว', '/ 100')}${tile('bat', 'batt', 'แบตเตอรี่', '%')}
  </div>`;
}

// showBase = true: แสดง "ค่าเฉลี่ยตอน baseline" มุมขวาของช่อง (หน้าเก็บข้อมูลใช้เทียบว่าตอนนี้ต่างจากปกติแค่ไหน)
export function paintTiles(root, prefix, showBase = false) {
  const v = S.live;
  if (!v) return;
  const base = (S.lie && S.lie.baseline && S.lie.baseline.valid) ? S.lie.baseline.mean : null;
  const set = (id, val, meta, off, baseVal) => {
    const el = $(`#${prefix}-${id}`, root); if (!el) return;
    el.querySelector('.num').textContent = val;
    el.querySelector('.meta').textContent = meta || '';
    el.classList.toggle('off', !!off);
    const b = el.querySelector('.base');
    if (b) b.textContent = showBase && baseVal != null ? `baseline ${baseVal}` : '';
  };
  const bv = (k, d) => (base && base[BASE_IDX[k]] != null ? fmt(base[BASE_IDX[k]], d) : null);
  const hrOk = v.con && v.hr;
  set('hr', hrOk ? fmt(v.hr, 0) : (v.con ? 'กำลังจับ' : 'ไม่แตะผิว'), hrOk ? `HRV ${fmt(v.hrv, 0)} ms` : '', !hrOk, bv('hr', 0));
  set('gsr', v.gc ? fmt(v.gsr, 2) : 'ไม่มีสัญญาณ', v.gc ? `phasic ${fmt(v.gp, 3)}` : 'ใช้สัญญาณอื่นแทนได้', !v.gc, bv('gsr', 2));
  set('tmp', fmt(v.tmp, 1), '', v.tmp == null, bv('tmp', 1));
  set('trm', fmt(v.trm, 3), `ขยับตัว ${fmt(v.mot, 2)}`, false, bv('trm', 3));
  set('si', v.si >= 0 ? v.si : '--', v.si >= 0 ? '' : 'คำนวณหลังวัดค่าปกติ', false, null);
  const usb = (v.fl & 256) !== 0;
  // วัดแบตไม่ได้ (< 2.5 V ที่ GPIO4) ไม่ได้แปลว่าใช้ไฟ USB เสมอ -> บอกตามที่วัดได้จริง
  set('bat', v.bat ? v.bp : 'ไม่พบ', `${fmt(v.vb / 1000, 2)} V${usb ? ' · เสียบ USB' : ''} · CPU ${v.cpu} MHz`, !v.bat, null);
}

// ---------------------------------------------------------------- กราฟเรียลไทม์ 4 กราฟ (2 นาทีล่าสุด)
const CHARTS = [
  ['hr', 'ชีพจร (bpm)', '--hr'], ['gsr', 'GSR — ความชื้นผิว (µS)', '--gsr'],
  ['tmp', 'อุณหภูมิผิว (°C)', '--temp'], ['trm', 'มือสั่น (m/s²)', '--trm'],
];

// HTML ของกราฟเส้น 4 กราฟ (ชีพจร, GSR, อุณหภูมิ, มือสั่น)
export function chartsHtml(prefix) {
  return `<div class="charts4">${CHARTS.map(([k, label]) =>
    `<div class="chart-cell"><div class="small muted">${label}</div><div class="chart-box short"><canvas id="${prefix}-c-${k}"></canvas></div></div>`).join('')}</div>`;
}

// สร้างกราฟครั้งเดียว แล้วคืนฟังก์ชันอัปเดต (เรียกเมื่อมีค่าใหม่)
export function makeCharts(root, prefix) {
  if (typeof Chart === 'undefined') return () => {};      // โหลด Chart.js ไม่ได้ -> ข้ามกราฟ แต่หน้าอื่นยังทำงาน
  chartTheme();
  const charts = CHARTS.map(([k, label, color]) => ({
    k, chart: liveLine($(`#${prefix}-c-${k}`, root), [{ label, color }], { windows: true }),
  }));
  let last = 0;
  return () => {
    const now = Date.now();
    if (now - last < 400) return;                        // วาดใหม่ไม่เกิน 2.5 ครั้ง/วินาที (ประหยัด CPU)
    if (root.offsetParent === null) return;              // หน้านี้ถูกซ่อนอยู่ -> ไม่ต้องวาด
    last = now;
    charts.forEach(({ k, chart }) => updateLive(chart, [k]));
  };
}

export { I };
