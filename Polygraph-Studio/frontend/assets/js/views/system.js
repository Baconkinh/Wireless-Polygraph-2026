// views/system.js — หน้า "ระบบ & อุปกรณ์": แสดงของจริงในตัวนาฬิกาสำหรับนำเสนอ (bonus)
//   FreeRTOS tasks · flash map (หน่วยความจำ) · Watchdog + สาธิต · Sleep · OTA · ตั้งค่า
//   + ปุ่มควบคุมนาฬิกาจำลอง (ตอนรันโหมด --sim)
import { S, on } from '../store.js';
import { $, h, esc, fmt, bytes, duration, toast, confirmModal } from '../ui.js';
import { get, post, cmd } from '../api.js';

let root, timer = null, lastSys = null;

const PART_COLOR = { nvs: '#5b8cff', otadata: '#7c5cff', app0: '#22d3a3', app1: '#12a594',
  spiffs: '#ffb84d', coredump: '#ff4d6d' };

// สร้างหน้าระบบ & อุปกรณ์
export function mount(el) {
  root = el;
  root.innerHTML = `<div id="sys-body"><p class="muted">กำลังโหลดข้อมูลระบบ...</p></div>`;
  on('device', refresh);
}
// เข้าหน้านี้: โหลดข้อมูลระบบจากนาฬิกา + รีเฟรชทุก 3 วินาที
export function show() { refresh(); timer = setInterval(refresh, 3000); }
// ออกจากหน้านี้: หยุดรีเฟรช (ไม่ถามนาฬิกาโดยไม่จำเป็น)
export function hide() { clearInterval(timer); timer = null; }

// อ่าน /api/watch/system แล้ววาดใหม่ (ไม่ได้เชื่อมต่อ = แสดงคำแนะนำ)
async function refresh() {
  if (!S.connected) {
    $('#sys-body', root).innerHTML = `<div class="banner danger"><svg class=i><use href=#i-wifi></use></svg> ยังไม่ได้เชื่อมต่อนาฬิกา — ต่อ WiFi <b>Polygraph-Watch</b> ก่อน</div>` + simControls();
    bindSim();
    return;
  }
  const sys = await get('/api/watch/system', { quiet: true }).catch(() => null);
  if (!sys || sys.ok === false) { $('#sys-body', root).innerHTML = '<div class="banner warn">ดึงข้อมูลระบบไม่ได้ (ลองใหม่อัตโนมัติ)</div>'; return; }
  lastSys = sys;
  render(sys);
}

// วาดข้อมูลระบบ: บูต/watchdog, หน่วยความจำ, แผนที่แฟลช, FreeRTOS task, พลังงาน, OTA
function render(sys) {
  const info = (S.device && S.device.info) || {};
  const boot = info.boot || {};
  const mem = sys.memory || {}, flash = sys.flash || {}, ota = sys.ota || {}, wd = sys.wdt || {},
    pw = sys.power || {}, net = sys.net || {}, sen = sys.sensors || {}, chip = sys.chip || {}, st = sys.stats || {};
  const heapPct = mem.heapTotal ? (100 * mem.heapFree / mem.heapTotal) : 0;

  // ---- flash map ----
  const total = (sys.partitions || []).reduce((a, p) => a + p.size, 0) || 0x400000;
  const segs = (sys.partitions || []).map((p) => {
    const w = (p.size / total * 100).toFixed(2);
    const col = PART_COLOR[p.label] || '#888';
    return `<div class="${p.running ? 'running' : ''}" style="width:${w}%;background:${col}" title="${p.label} @ 0x${(p.addr || 0).toString(16)} · ${bytes(p.size)}">
      <b>${esc(p.label)}</b><span>${bytes(p.size)}</span></div>`;
  }).join('');
  const partRows = (sys.partitions || []).map((p) => `<tr>
    <td><span style="color:${PART_COLOR[p.label] || '#888'}">■</span> ${esc(p.label)}${p.running ? ' <span class="badge b-truth">รันอยู่</span>' : ''}</td>
    <td>${esc(p.type)}</td><td class="mono">0x${(p.addr || 0).toString(16).padStart(6, '0')}</td>
    <td>${bytes(p.size)}</td><td>${esc(p.firmware || p.state || '')}</td></tr>`).join('');

  // ---- tasks ----
  const taskRows = (sys.tasks || []).sort((a, b) => (b.prio - a.prio)).map((t) => {
    const stackPct = t.stack ? (100 * (t.stack - t.stackFree) / t.stack) : null;
    return `<tr${t.ours ? '' : ' style="opacity:.6"'}>
      <td>${esc(t.name)}${t.ours ? ' <span class="badge b-kind">เรา</span>' : ''}</td>
      <td>${t.prio}</td><td>${esc(t.state)}</td>
      <td>${t.cpu != null ? fmt(t.cpu, 1) + '%' : '-'}</td>
      <td>${bytes(t.stackFree)}${stackPct != null ? ` <span class="faint">(ใช้ ${fmt(stackPct, 0)}%)</span>` : ''}</td></tr>`;
  }).join('');

  $('#sys-body', root).innerHTML = `
  ${boot.coredump ? `<div class="banner danger"><svg class=i><use href=#i-alert></use></svg> การบูตครั้งก่อนมาจากโปรแกรมล่มใน task "<b>${esc(boot.cdTask)}</b>" ที่ PC ${esc(boot.cdPc)} (core dump)</div>` : ''}
  ${wd.demo && wd.demo !== 'none' ? `<div class="banner warn"><svg class=i><use href=#i-clock></use></svg> มีการสาธิต watchdog ค้างอยู่ (${esc(wd.demo)}) — เครื่องกำลังจะรีเซ็ต</div>` : ''}

  <div class="grid g4">
    <div class="vital" style="--c:var(--accent)"><div class="name">ชิป</div><div class="v" style="font-size:18px">${esc(chip.model || '-')}</div><div class="meta">rev ${chip.rev} · ${chip.cpuMHz} MHz · ${fmt(chip.tempC, 1)}°C</div></div>
    <div class="vital" style="--c:var(--truth)"><div class="name">RAM ว่าง</div><div class="v">${bytes(mem.heapFree)}</div><div class="meta">ต่ำสุด ${bytes(mem.heapMin)} · ${fmt(heapPct, 0)}%</div></div>
    <div class="vital" style="--c:var(--gsr)"><div class="name">เวลาทำงาน</div><div class="v" style="font-size:20px">${duration(sys.uptime)}</div><div class="meta">บูตครั้งที่ ${st.boots}</div></div>
    <div class="vital" style="--c:var(--temp)"><div class="name">การบูตล่าสุด</div><div class="v" style="font-size:16px">${esc(boot.reason || '-')}</div><div class="meta">${esc((boot.reasonTh || '').slice(0, 40))}</div></div>
  </div>

  <div class="card" style="margin-top:16px">
    <h3><svg class=i><use href=#i-db></use></svg> หน่วยความจำ (Flash 4 MB) <span class="right small muted">แผนที่พาร์ทิชันจริงในเครื่อง</span></h3>
    <div class="flashmap">${segs}</div>
    <div class="legend">
      <div><i style="background:#5b8cff"></i><div><b>nvs</b> — Preferences (ค่าตั้ง) + EEPROM emulation (สถิติ)</div></div>
      <div><i style="background:#7c5cff"></i><div><b>otadata</b> — เลือกว่าบูต app0 หรือ app1 + สถานะ rollback</div></div>
      <div><i style="background:#22d3a3"></i><div><b>app0 / app1</b> — เฟิร์มแวร์ 2 ช่องสลับกัน (OTA)</div></div>
      <div><i style="background:#ffb84d"></i><div><b>spiffs</b> — LittleFS (log เหตุการณ์ + ผลการทดสอบ)</div></div>
      <div><i style="background:#ff4d6d"></i><div><b>coredump</b> — ภาพหน่วยความจำตอนโปรแกรมล่ม</div></div>
    </div>
    <table class="t" style="margin-top:10px"><thead><tr><th>พาร์ทิชัน</th><th>ชนิด</th><th>ตำแหน่ง</th><th>ขนาด</th><th>เฟิร์มแวร์/สถานะ</th></tr></thead><tbody>${partRows}</tbody></table>
    <div class="small muted" style="margin-top:8px">NVS: ใช้ ${sys.nvs?.used} / ว่าง ${sys.nvs?.free} entries · LittleFS: ${bytes(sys.fs?.used)} / ${bytes(sys.fs?.total)}</div>
  </div>

  <div class="grid g2" style="margin-top:16px">
    <div class="card">
      <h3><svg class=i><use href=#i-layers></use></svg> FreeRTOS Tasks <span class="right small muted">${sys.taskCount} task · คอร์เดียว priority สูง = ได้ CPU ก่อน</span></h3>
      <table class="t"><thead><tr><th>Task</th><th>Prio</th><th>สถานะ</th><th>CPU</th><th>Stack เหลือ</th></tr></thead><tbody>${taskRows}</tbody></table>
    </div>
    <div class="card">
      <h3><svg class=i><use href=#i-shield></use></svg> Watchdog (3 ชั้น)</h3>
      <div class="wdt-layers">
        <div class="wdt-layer"><div class="lv">1</div><div><b>Interrupt WDT</b><div class="hint">จับ interrupt ค้าง · timeout ${wd.iwdtMs} ms</div></div><div></div></div>
        <div class="wdt-layer"><div class="lv">2</div><div><b>Task WDT</b><div class="hint">ทุก task ต้องรายงานตัว · timeout ${wd.twdtS} s (panic)</div></div><div></div></div>
        <div class="wdt-layer"><div class="lv">3</div><div><b>Timer-interrupt WDT</b><div class="hint">supervisor ป้อนทุกรอบ · timeout ${wd.hwTimeoutMs} ms</div></div><div><span class="feed" id="wd-feed"></span></div></div>
      </div>
      <div class="small muted" style="margin:8px 0">ป้อนแล้ว ${wd.hwFeeds} ครั้ง (ล่าสุด ${wd.hwLastFeedMs} ms ก่อน) · รีเซ็ตจาก WDT สะสม ${wd.wdtResets} ครั้ง · panic ${wd.panicResets}</div>
      <hr class="sep">
      <div class="small muted"><svg class=i><use href=#i-flask></use></svg> สาธิต (เครื่องจะรีเซ็ตจริง แล้วรายงานสาเหตุกลับมา):</div>
      <div class="row" style="margin-top:8px">
        <button class="btn sm warn" data-demo="twdt">Task WDT ค้าง</button>
        <button class="btn sm warn" data-demo="hwwdt">Timer WDT</button>
        <button class="btn sm warn" data-demo="panic">Panic + core dump</button>
        <button class="btn sm warn" data-demo="intwdt">Interrupt WDT</button>
      </div>
    </div>
  </div>

  <div class="grid g2" style="margin-top:16px">
    <div class="card">
      <h3><svg class=i><use href=#i-batt></use></svg> พลังงาน &amp; Sleep</h3>
      <div class="kv">
        <span>แบตเตอรี่</span><span>${pw.battPresent ? `${pw.battPct}% · ${fmt(pw.vbatMv / 1000, 2)} V` : 'ใช้ไฟ USB'}</span>
        <span>CPU</span><span>${chip.cpuMHz} MHz ${pw.eco ? '(ECO)' : '(เต็มสปีด)'}</span>
        <span>Light sleep</span><span>${pw.lightSleeps} ครั้ง</span>
        <span>Deep sleep</span><span>${pw.deepSleeps} ครั้ง · ตื่นจาก timer ${pw.rtcWakeCount}</span>
        <span>Auto-standby</span><span>${pw.standbyMin ? pw.standbyMin + ' นาที' : 'ปิด'}</span>
      </div>
      <div class="row" style="margin-top:10px">
        <button class="btn sm" data-power="${pw.eco ? 'normal' : 'eco'}">${pw.eco ? 'ปิด ECO' : 'เปิด ECO (80 MHz)'}</button>
        <button class="btn sm" data-power="light">Light sleep 10 วินาที</button>
        <button class="btn sm danger" data-restart="1">รีสตาร์ทนาฬิกา</button>
      </div>
    </div>
    <div class="card">
      <h3><svg class=i><use href=#i-wifi></use></svg> เครือข่าย &amp; เซนเซอร์</h3>
      <div class="kv">
        <span>WiFi</span><span>${esc(net.ssid)} · ช่อง ${net.channel} · ${net.stations} เครื่องต่อ</span>
        <span>สัญญาณ</span><span>${net.rssi} dBm · ส่ง UDP ${net.udpSent} แพ็กเก็ต</span>
        <span>MAX30102 (ชีพจร)</span><span>${sen.max30102 ? `<svg class=i><use href=#i-check></use></svg> part 0x${(sen.maxPart || 0).toString(16)}` : '<svg class=i><use href=#i-x></use></svg> ไม่พบ'}</span>
        <span>MPU6050 (ความเร่ง)</span><span>${sen.mpu ? `<svg class=i><use href=#i-check></use></svg> ${esc(sen.mpuChip)} (0x${(sen.mpuWho || 0).toString(16)})` : '<svg class=i><use href=#i-x></use></svg> ไม่พบ'}</span>
        <span>I2C errors / กู้บัส</span><span>${sen.i2cErrors} / ${sen.i2cRecoveries}</span>
        <span>FIFO overflow</span><span>${sen.fifoOverflows}</span>
      </div>
    </div>
  </div>

  <div class="card" style="margin-top:16px">
    <h3><svg class=i><use href=#i-up></use></svg> OTA — อัปเดตเฟิร์มแวร์ไร้สาย <span class="right small muted">รันอยู่ช่อง ${esc(ota.running)} · อัปเดตมาแล้ว ${ota.updates} ครั้ง</span></h3>
    <p class="small muted">เลือกไฟล์ <span class="mono">.pio/build/esp32c3/firmware.bin</span> แล้วอัปโหลดผ่าน WiFi (ไม่ต้องเสียบสาย)
    — นาฬิกาจะเขียนลงช่องที่ไม่ได้รันอยู่ แล้วรีบูต ถ้าเฟิร์มแวร์ใหม่ล่ม bootloader จะย้อนเวอร์ชันเดิมให้เอง (rollback)</p>
    <div class="row"><input type="file" id="ota-file" accept=".bin"><button class="btn" id="ota-go">อัปโหลด</button>
      <span id="ota-status" class="small muted"></span></div>
    <progress id="ota-prog" max="100" value="0" style="width:100%;display:none;margin-top:8px"></progress>
  </div>

  <div class="card" style="margin-top:16px">
    <h3><svg class=i><use href=#i-sliders></use></svg> ตั้งค่า LieEngine <span class="right small muted">เก็บใน NVS ของนาฬิกา</span></h3>
    <div id="cfg-box"><button class="btn sm" id="cfg-load">โหลดค่าปัจจุบัน</button></div>
  </div>

  ${simControls()}

  <div class="card" style="margin-top:16px">
    <h3><svg class=i><use href=#i-doc></use></svg> Log เหตุการณ์ในนาฬิกา <span class="right"><button class="btn sm" id="log-load">โหลด</button></span></h3>
    <pre class="log" id="log-box">กดโหลดเพื่อดู /events.log จากนาฬิกา</pre>
  </div>`;

  bindActions();
  bindSim();
}

// ---------------------------------------------------------------- นาฬิกาจำลอง
function simControls() {
  const st = S.status || {};
  if (!st.simulator) return '';
  return `<div class="card" style="margin-top:16px;border-color:var(--accent2)">
    <h3><svg class=i><use href=#i-flask></use></svg> นาฬิกาจำลอง — กำหนด "ความจริง" ของผู้ถูกทดสอบ</h3>
    <p class="small muted">ใช้ตอนซ้อม/สาธิตโดยไม่มีบอร์ด — เลือกว่าผู้ถูกทดสอบจำลองจะโกหกหรือพูดจริงในคำถามจริงข้อถัดไป</p>
    <div class="row">
      <span>คำถามจริงข้อถัดไป:</span>
      <button class="btn sm danger" data-sim="next" data-mode="lie">โกหก</button>
      <button class="btn sm success" data-sim="next" data-mode="truth">พูดจริง</button>
      <button class="btn sm warn" data-sim="next" data-mode="nervous">จริงแต่ตื่นเต้น</button>
    </div>
    <div class="row" style="margin-top:8px">
      <span>โหมดทุกข้อ:</span>
      <button class="btn sm" data-sim="mode" data-mode="random">สุ่ม</button>
      <button class="btn sm" data-sim="mode" data-mode="lie">โกหกหมด</button>
      <button class="btn sm" data-sim="mode" data-mode="truth">จริงหมด</button>
    </div>
    <div class="row" style="margin-top:8px">
      <button class="btn sm" data-sim="contact" data-ppg="0" data-gsr="1">จำลอง: ชีพจรหลุด</button>
      <button class="btn sm" data-sim="contact" data-ppg="1" data-gsr="0">จำลอง: GSR หลุด</button>
      <button class="btn sm" data-sim="contact" data-ppg="1" data-gsr="1">แตะครบ</button>
      <button class="btn sm" data-sim="motion" data-level="3">จำลอง: ขยับแรง</button>
      <button class="btn sm" data-sim="motion" data-level="0.01">อยู่นิ่ง</button>
      <button class="btn sm" data-sim="new_subject">เปลี่ยนคนใหม่</button>
    </div>
  </div>`;
}

// ผูกปุ่มควบคุมนาฬิกาจำลอง (ใช้ได้เฉพาะตอนต่อนาฬิกาจำลอง)
function bindSim() {
  root.querySelectorAll('[data-sim]').forEach((b) => b.addEventListener('click', async () => {
    const a = b.dataset.sim;
    const params = {};
    if (b.dataset.mode) params.mode = b.dataset.mode;
    if (b.dataset.ppg) { params.ppg = +b.dataset.ppg; params.gsr = +b.dataset.gsr; }
    if (b.dataset.level) params.level = +b.dataset.level;
    const r = await post(`/api/sim/${a}`, params);
    if (r && r.ok) toast(r.msg || 'ตั้งค่านาฬิกาจำลองแล้ว', 'ok', 2500);
  }));
}

// ---------------------------------------------------------------- ปุ่มต่าง ๆ
function bindActions() {
  root.querySelectorAll('[data-demo]').forEach((b) => b.addEventListener('click', async () => {
    const type = b.dataset.demo;
    const ok = await confirmModal('สาธิต Watchdog', `<p>เครื่องจะ<b>รีเซ็ตจริง</b> (${esc(type)}) เพื่อสาธิตว่า watchdog ทำงาน
      หลังรีบูตจะรายงานกลับมาว่ารีเซ็ตเพราะอะไร เหมาะกับตอนนำเสนอ</p>`, 'สาธิตเลย', true);
    if (!ok) return;
    const r = await cmd('demo', { type, confirm: 'yes' });
    if (r && r.ok) toast((r.msg || 'กำลังสาธิต...') + ' รอเครื่องรีบูต', 'warn', 10000);
  }));
  root.querySelectorAll('[data-power]').forEach((b) => b.addEventListener('click', async () => {
    const r = await cmd('power', { mode: b.dataset.power, sec: b.dataset.power === 'light' ? 10 : 0 });
    if (r && r.ok) toast(r.msg || 'สั่งแล้ว', 'ok');
  }));
  const rb = root.querySelector('[data-restart]');
  if (rb) rb.addEventListener('click', async () => {
    if (!(await confirmModal('รีสตาร์ทนาฬิกา?', '<p>นาฬิกาจะรีบูต (~3 วินาที)</p>', 'รีสตาร์ท', true))) return;
    const r = await cmd('restart');
    if (r && r.ok) toast('กำลังรีสตาร์ท...', 'warn');
  });
  $('#log-load', root)?.addEventListener('click', async () => {
    const t = await get('/api/watch/logs?file=events', { quiet: true }).catch(() => null);
    $('#log-box', root).textContent = (typeof t === 'string' ? t : (t && t.msg)) || 'โหลดไม่ได้';
  });
  $('#cfg-load', root)?.addEventListener('click', loadConfig);
  bindOta();
}

// โหลดค่าตั้ง LieEngine มาแสดงเป็นฟอร์มแก้ไข
async function loadConfig() {
  const c = await get('/api/watch/config', { quiet: true }).catch(() => null);
  if (!c || c.ok === false) { toast('โหลดค่าตั้งไม่ได้', 'err'); return; }
  const box = $('#cfg-box', root);
  const field = (k, label, min, max, step, val) =>
    `<label class="f">${label}<input class="input" type="number" data-cfg="${k}" min="${min}" max="${max}" step="${step}" value="${val}"></label>`;
  box.innerHTML = `<div class="form-grid">
    ${field('baselineSec', 'Baseline (วินาที)', 20, 90, 1, c.baselineSec)}
    ${field('windowSec', 'ช่วงวัดหลังถาม (วินาที)', 6, 20, 1, c.windowSec)}
    ${field('lieP', 'เกณฑ์โกหก (0.5-0.99)', 0.5, 0.99, 0.01, c.lieP)}
    ${field('truthP', 'เกณฑ์จริง (0.01-0.5)', 0.01, 0.5, 0.01, c.truthP)}
    ${field('contactThr', 'เกณฑ์แตะชีพจร (IR)', 5000, 250000, 1000, c.contactThr)}
    ${field('vccMv', 'แรงดัน 3V3 จริง (mV)', 3000, 3600, 10, c.vccMv)}
    ${field('standbyMin', 'Auto-standby (นาที, 0=ปิด)', 0, 240, 1, c.standbyMin)}
  </div>
  <div class="row" style="margin-top:10px"><button class="btn primary sm" id="cfg-save">บันทึกลงนาฬิกา</button>
    <button class="btn sm" id="cfg-reset">คืนค่าโรงงาน</button></div>`;
  $('#cfg-save', root).addEventListener('click', async () => {
    const values = {};
    box.querySelectorAll('[data-cfg]').forEach((i) => { values[i.dataset.cfg] = parseFloat(i.value); });
    const r = await post('/api/watch/config', { values });
    if (r && r.ok !== false) toast('บันทึกลง NVS ของนาฬิกาแล้ว', 'ok');
  });
  $('#cfg-reset', root).addEventListener('click', async () => {
    if (!(await confirmModal('คืนค่าโรงงาน?', '<p>ค่าตั้ง LieEngine กลับเป็นค่าเริ่มต้น</p>', 'คืนค่า', true))) return;
    await post('/api/watch/config/reset');
    loadConfig();
    toast('คืนค่าแล้ว', 'ok');
  });
}

// ผูกปุ่มอัปโหลดเฟิร์มแวร์ (OTA)
function bindOta() {
  const go = $('#ota-go', root);
  if (!go) return;
  go.addEventListener('click', async () => {
    const f = $('#ota-file', root).files[0];
    if (!f) return toast('เลือกไฟล์ firmware.bin ก่อน', 'warn');
    const prog = $('#ota-prog', root), status = $('#ota-status', root);
    prog.style.display = 'block'; prog.value = 10; status.textContent = 'กำลังอัปโหลด...';
    const fd = new FormData();
    fd.append('file', f, f.name);
    try {
      const r = await fetch('/api/watch/ota', { method: 'POST', body: fd });
      const j = await r.json();
      prog.value = 100;
      status.textContent = j.ok ? 'สำเร็จ — นาฬิกากำลังรีบูตด้วยเฟิร์มแวร์ใหม่' : 'ไม่สำเร็จ: ' + (j.msg || 'ล้มเหลว');
      toast(status.textContent, j.ok ? 'ok' : 'err', 8000);
    } catch (e) {
      status.textContent = 'อัปโหลดไม่สำเร็จ';
      toast('อัปโหลดไม่สำเร็จ: ' + e.message, 'err');
    }
  });
}

// กระพริบไฟ "ป้อน watchdog" ตามข้อมูลสด
on('vitals', () => {
  const f = $('#wd-feed', root);
  if (f) { f.classList.add('blink'); setTimeout(() => f.classList.remove('blink'), 400); }
});
