// main.js — จุดเริ่มของหน้าเว็บ: เมนู, แถบสถานะด้านบน, การแจ้งเตือน
// โหลดแต่ละหน้าแบบ dynamic import + try/catch -> ถ้าหน้าใดหน้าหนึ่งมีปัญหา
// หน้าอื่นและแถบสถานะยังทำงานได้ (ไม่จอขาวทั้งแอป)
import { S, on } from './store.js';
import { connectWS } from './ws.js';
import { $, $$, fmt, toast, STATE_TH, vinfo } from './ui.js';

const TITLES = { session: 'ทดสอบ', results: 'ผลลัพธ์ & รายงาน', system: 'ระบบ & อุปกรณ์', guide: 'คู่มือ & หลักการ' };
const views = {};          // name -> module (หรือ fallback)
let current = 'session';

// ---------------------------------------------------------------- theme
function applyTheme(t) {
  document.documentElement.classList.toggle('light', t === 'light');
  try { localStorage.setItem('theme', t); } catch { /* */ }
}
let theme = 'dark';
try { theme = localStorage.getItem('theme') || 'dark'; } catch { /* */ }
applyTheme(theme);

// ---------------------------------------------------------------- fallback view (ถ้าโหลดหน้าไม่ได้)
function fallbackView(name, err) {
  return {
    mount(el) {
      el.innerHTML = `<div class="card"><h3><svg class=i><use href=#i-alert></use></svg> หน้านี้โหลดไม่สำเร็จ</h3>
        <p class="muted">หน้า "${TITLES[name] || name}" เปิดไม่ได้ (${err ? err.message : 'ไม่พบไฟล์'})
        แต่ส่วนอื่นยังใช้ได้ และ Backend ทำงานปกติ</p>
        <p>สั่งงานและดูข้อมูลได้ที่ <a href="/docs" target="_blank">หน้าเอกสาร API (/docs)</a>
        · ดูรายงานได้ที่ <span class="mono">/report/&lt;เลขเซสชัน&gt;</span></p></div>`;
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

async function show(name) {
  if (!TITLES[name]) name = 'session';
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
  if (st.simulator) { toast('นาฬิกาจำลองไม่มีหน้าเว็บในตัว — มีเฉพาะนาฬิกาจริงที่ http://192.168.4.1', 'warn'); return; }
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
    $('#batt-text').textContent = v.bat ? `${v.bp}% · ${fmt(v.vb / 1000, 2)} V` : 'ไฟ USB';
  }
  if (st && st.stats) {
    const s = st.stats;
    $('#net-text').textContent = `${s.rtt_ms != null ? s.rtt_ms + ' ms' : '-- ms'} · หาย ${fmt(s.loss_pct, 1)}%`;
  }
}
on('status', topbar); on('vitals', topbar); on('snapshot', topbar); on('device', topbar);
on('ws', (ok) => { if (!ok) { $('#conn-dot').className = 'dot off'; $('#conn-text').textContent = 'Studio ขาดการเชื่อมต่อ... ต่อใหม่'; } });

// ---------------------------------------------------------------- การแจ้งเตือนเหตุการณ์
on('event', (e) => {
  switch (e.ev) {
    case 'baseline_done': toast(`วัด baseline เสร็จ (GSR ${fmt(e.gsr, 2)} µS, ชีพจร ${fmt(e.hr, 0)} bpm) — เริ่มถามได้`, 'ok', 7000); break;
    case 'baseline_fail': toast('baseline ใช้ไม่ได้: ไม่มีสัญญาณชีพจรและ GSR — ใส่นาฬิกาให้แนบผิวแล้ววัดใหม่', 'err', 9000); break;
    case 'low_batt': toast(`แบตเตอรี่อ่อน (${e.mv} mV) — ควรชาร์จ`, 'warn', 9000); break;
    case 'button': toast(`ปุ่มบนนาฬิกา: ${e.g} → ${e.act}`, 'info'); break;
    case 'result':
      if (current !== 'session') { const vi = vinfo(e.verdict); toast(`ผลข้อ #${e.qid}: ${vi.th} (โอกาสโกหก ${Math.round((e.p || 0) * 100)}%)`, 'info'); }
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
show(location.hash.slice(1) || 'session');
