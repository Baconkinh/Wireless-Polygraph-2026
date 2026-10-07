// store.js — สถานะกลางของหน้าเว็บ + ระบบ event เล็ก ๆ (publish/subscribe)
// ทุก view อ่านค่าจาก S และ "ฟัง" การเปลี่ยนแปลงด้วย on('vitals', fn) เป็นต้น

export const HIST_SEC = 120;          // เก็บกราฟย้อนหลัง 2 นาที
export const WAVE_LEN = 600;          // คลื่น PPG 6 วินาที (100 Hz)

export const S = {
  connected: false,
  status: null,        // สถานะลิงก์ (จาก backend)
  live: null,          // ค่าสดล่าสุดจากนาฬิกา
  lie: null,           // สถานะ LieEngine (baseline, calibration, ผล)
  device: { hi: {}, info: {} },
  session: null,       // เซสชันที่เปิดอยู่
  meta: null,
  lastResult: null,    // {result, question, explain}
  hist: { t: [], gsr: [], gp: [], hr: [], si: [], trm: [], mot: [], tmp: [] },
  windows: [],         // ช่วงคำถาม {start, end, verdict, label} (เวลา ms ของเครื่องนี้)
  wave: new Float32Array(WAVE_LEN),
  beats: new Uint8Array(WAVE_LEN),
  waveHead: 0,
  lastBeatAt: 0,
  lastVitalsAt: 0,
};

const subs = {};
export function on(evt, fn) { (subs[evt] = subs[evt] || []).push(fn); }
export function emit(evt, data) { (subs[evt] || []).forEach((fn) => { try { fn(data); } catch (e) { console.error(evt, e); } }); }

export function pushVitals(v) {
  const now = Date.now();
  const h = S.hist;
  h.t.push(now);
  h.gsr.push(v.gc ? v.gsr : null);
  h.gp.push(v.gc ? v.gp : null);
  h.hr.push(v.con && v.hr ? v.hr : null);
  h.si.push(v.si >= 0 ? v.si : null);
  h.trm.push(v.trm);
  h.mot.push(v.mot);
  h.tmp.push(v.tmp);
  const cut = now - HIST_SEC * 1000;
  while (h.t.length && h.t[0] < cut) Object.values(h).forEach((a) => a.shift());
  S.windows = S.windows.filter((w) => w.end > cut);
  S.live = v;
  S.lastVitalsAt = now;
}

export function pushWave(w) {
  const d = w.d || [];
  for (let i = 0; i < d.length; i++) {
    S.wave[S.waveHead] = d[i];
    const beat = (w.b >> i) & 1;
    S.beats[S.waveHead] = beat;
    if (beat) S.lastBeatAt = Date.now();
    S.waveHead = (S.waveHead + 1) % WAVE_LEN;
  }
}
