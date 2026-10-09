// main.js — จุดเริ่มของหน้าเว็บ: เมนู, แถบสถานะด้านบน, การแจ้งเตือน
// โหลดแต่ละหน้าแบบ dynamic import + try/catch -> ถ้าหน้าใดหน้าหนึ่งมีปัญหา
// หน้าอื่นและแถบสถานะยังทำงานได้ (ไม่จอขาวทั้งแอป)
import { S, on } from './store.js';
import { connectWS } from './ws.js';
import { $, $$, fmt, toast, STATE_TH, vinfo } from './ui.js';

// หน้าแรก (home) = ใช้งานจริง — หน้า "Wireless Polygraph" แบบเซสชัน และ "ผลลัพธ์ & รายงาน" ถูกเอาออก 9 ต.ค. 2026
const TITLES = { home: 'ใช้งานจริง', collect: 'เก็บข้อมูลเทรน AI', data: 'ข้อมูล & เทรน AI',
  system: 'ระบบ & อุปกรณ์', guide: 'คู่มือ & หลักการ' };
const views = {};          // name -> module (หรือ fallback)
let current = 'home';

// ---------------------------------------------------------------- theme
function applyTheme(t) {
  document.documentElement.classList.toggle('light', t === 'light');
  try { localStorage.setItem('theme', t); } catch { /* */ }
}
let theme = 'light';                    // ค่าเริ่มต้นโหมดสว่าง (สลับมืดได้ที่ปุ่มมุมขวาบน)
try { theme = localStorage.getItem('theme') || 'light'; } catch { /* */ }
applyTheme(theme);

// ---------------------------------------------------------------- fallback view (ถ้าโหลดหน้าไม่ได้)
function fallbackView(name, err) {
  return {
    mount(el) {
      el.innerHTML = `<div class="card"><h3><svg class=i><use href=#i-alert></use></svg> หน้านี้โหลดไม่สำเร็จ</h3>
        <p class="muted">หน้า "${TITLES[name] || name}" เปิดไม่ได้ (${err ? err.message : 'ไม่พบไฟล์'})
        แต่ส่วนอื่นยังใช้ได้ และ Backend ทำงานปกติ</p>
        <p>ส่วนใหญ่เกิดจาก<b>เบราว์เซอร์จำไฟล์หน้าเว็บรุ่นเก่า</b>หลังอัปเดต Studio — ปิด run_studio.bat แล้วเปิดใหม่ จากนั้นกด <b>Ctrl+F5</b></p>
        <p>สั่งงานและดูข้อมูลได้ที่ <a href="/docs" target="_blank">หน้าเอกสาร API (/docs)</a></p></div>`;
    },
    show() {}, hide() {},
  };
}

// ---------------------------------------------------------------- router
async function loadView(name) {
  if (views[name]) return views[name];
  try {
    const mod = await import(`./views/${name}.js`);
    views[name] = mod;
    mod.mount(document.getElementById(`view-${name}`));
  } catch (e) {
    console.error('โหลดหน้า', name, 'ไม่ได้:', e);
    views[name] = fallbackView(name, e);
    views[name].mount(document.getElementById(`view-${name}`));
  }
  return views[name];
}

// เปลี่ยนหน้า: เน้นเมนู, แสดง section ของหน้านั้น, โหลดโมดูลหน้า (ครั้งแรก), เรียก show()/hide()
async function show(name) {
  // หน้า "ทดสอบ" (session) และ "ใช้งานจริง" (use) เดิม ถูกรวมเป็นหน้าหลักแล้ว — ลิงก์เก่ายังพามาที่หน้าหลัก
  // ลิงก์เก่า (#session, #use, #results) พามาหน้าแรก = ใช้งานจริง
  if (name === 'session' || name === 'use' || name === 'results') name = 'home';
  if (!TITLES[name]) name = 'home';
  current = name;
  $$('#nav button[data-view]').forEach((b) => b.classList.toggle('active', b.dataset.view === name));
  $$('.view').forEach((v) => v.classList.toggle('active', v.id === `view-${name}`));
  $('#view-title').textContent = TITLES[name];
  const mod = await loadView(name);
  Object.entries(views).forEach(([k, m]) => { if (k !== name && m.hide) m.hide(); });
  if (mod.show) mod.show();
  if (location.hash !== `#${name}`) history.replaceState(null, '', `#${name}`);
}
$$('#nav button[data-view]').forEach((b) => b.addEventListener('click', () => show(b.dataset.view)));
window.addEventListener('hashchange', () => show(location.hash.slice(1)));
window.goto = show;

// หน้าเว็บที่ฝังในนาฬิกา
$('#link-watchpage').addEventListener('click', () => {
  const st = S.status || {};
  // นาฬิกาจำลองเปิดหน้าเว็บแบบเดียวกับนาฬิกาจริงที่ http://127.0.0.1:8081/
  window.open(st.http || 'http://192.168.4.1', '_blank');
});

// ---------------------------------------------------------------- แถบสถานะด้านบน
function topbar() {
  const st = S.status, v = S.live, c = S.connected;
  const dev = (S.device && S.device.hi) || {};
  $('#conn-dot').className = 'dot ' + (c ? 'on' : 'off');
  $('#conn-text').textContent = c ? `เชื่อมต่อ ${dev.id || ''}${st && st.simulator ? ' (จำลอง)' : ''}` : 'ไม่ได้เชื่อมต่อนาฬิกา';
  $('#side-dot').className = 'dot ' + (c ? 'on' : 'off');
  $('#side-device-text').innerHTML = c
    ? `${dev.id || '-'} · fw ${dev.fw || '-'}<br>boot #${dev.boot ?? '-'} · ${(st && st.watch) || ''}`
    : 'ต่อ WiFi <b>Polygraph-Watch</b><br>รหัส polygraph123';
  if (v) {
    $('#engine-text').textContent = (STATE_TH[v.es] || '-') + (v.es === 1 || v.es === 3 ? ` ${Math.round((v.ep || 0) * 100)}%` : '');
    // แสดง % เสมอเมื่อวัดแรงดันแบตได้ (v.bat) แม้จะเสียบ USB อยู่ด้วย
    // v.bat = เฟิร์มแวร์วัดแรงดันแบตที่ขา GPIO4 (ผ่านตัวแบ่ง 100k/100k) ได้เกิน 2.5 V
    // v.fl & 256 = ต่อสาย USB กับคอม (ตรวจจาก USB CDC) — เป็นคนละเรื่องกัน จึงแสดงแยก
    // (เดิมเขียนว่า "ไฟ USB" ทุกครั้งที่วัดแบตไม่ได้ ทำให้เข้าใจผิดเวลาเสียบแบตอยู่แต่สายวัดแรงดันไม่ต่อ)
    const usb = (v.fl & 256) !== 0;
    $('#batt-text').textContent = v.bat ? `${v.bp}% · ${fmt(v.vb / 1000, 2)} V${usb ? ' · USB' : ''}`
      : `วัดแบตไม่ได้ (${fmt(v.vb / 1000, 2)} V)${usb ? ' · USB' : ''}`;
    $('#pill-batt').title = v.bat ? 'แบตเตอรี่' : 'แรงดันที่ขา GPIO4 ต่ำกว่า 2.5 V จึงถือว่า "ไม่มีแบต" — ถ้าเสียบแบตอยู่จริง ให้ตรวจ'
      + ' สวิตช์แบต, สายที่ J5, ตัวต้านทาน R4/R5 (วัดที่ GPIO4 ควรได้ ~1.8-2.1 V) ดูคู่มือส่วนที่ 7';
  }
  if (st && st.stats) {
    const s = st.stats;
    $('#net-text').textContent = `${s.rtt_ms != null ? s.rtt_ms + ' ms' : '-- ms'} · หาย ${fmt(s.loss_pct, 1)}%`;
  }
}
on('status', topbar); on('vitals', topbar); on('snapshot', topbar); on('device', topbar);

// ---------------------------------------------------------------- ผู้จัดทำ (แถบซ้ายล่าง)
on('snapshot', () => {
  const c = S.credits; if (!c) return;
  $('#side-credits').innerHTML = `<b>ผู้จัดทำ</b><br>${c.members.map((m) => `${m.name} ${m.id}`).join('<br>')}
    <div style="margin-top:4px">${c.course.split(' (')[0]}</div>`;
});
on('ws', (ok) => { if (!ok) { $('#conn-dot').className = 'dot off'; $('#conn-text').textContent = 'Studio ขาดการเชื่อมต่อ... ต่อใหม่'; } });

// ---------------------------------------------------------------- การแจ้งเตือนเหตุการณ์
on('event', (e) => {
  switch (e.ev) {
    case 'baseline_done': toast(`วัด baseline เสร็จ (GSR ${fmt(e.gsr, 2)} µS, ชีพจร ${fmt(e.hr, 0)} bpm) — เริ่มถามได้`, 'ok', 7000); break;
    case 'baseline_fail': toast('baseline ใช้ไม่ได้: ไม่มีสัญญาณชีพจรและ GSR — ใส่นาฬิกาให้แนบผิวแล้ววัดใหม่', 'err', 9000); break;
    case 'low_batt': toast(`แบตเตอรี่อ่อน (${e.mv} mV) — ควรชาร์จ`, 'warn', 9000); break;
    case 'button': toast(`ปุ่มบนนาฬิกา: ${e.g} → ${e.act}`, 'info'); break;
    case 'model': toast('นาฬิกาได้รับโมเดล AI ใหม่แล้ว', 'ok'); break;
    case 'result':
      if (current !== 'home') { const vi = vinfo(e.verdict); toast(`ผลข้อ #${e.qid}: ${vi.th} (โอกาสโกหก ${Math.round((e.p || 0) * 100)}%)`, 'info'); }
      break;
    default: break;
  }
});
on('device', (d) => {
  if (d.rebooted) {
    const b = (d.info && d.info.boot) || {};
    toast(`นาฬิการีบูต — ${b.reasonTh || d.hi.reason}${b.planned && b.planned !== 'none' ? ` (${b.planned})` : ''} · ดูที่หน้า "ระบบ"`, 'warn', 12000);
  }
});

// ---------------------------------------------------------------- เริ่มทำงาน
$('#btn-theme').addEventListener('click', () => { theme = theme === 'dark' ? 'light' : 'dark'; applyTheme(theme); });
fetch('/api/status').then((r) => r.json()).then((s) => { $('#studio-ver').textContent = 'v' + (s.studio || ''); }).catch(() => {});
connectWS();
show(location.hash.slice(1) || 'home');
