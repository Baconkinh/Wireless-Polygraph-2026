// ws.js — WebSocket รับข้อมูลสดจาก Studio (ต่อใหม่อัตโนมัติถ้าหลุด)
import { S, emit, pushVitals, pushWave } from './store.js';

let ws = null;
let retry = 1000;

export function connectWS() {
  const proto = location.protocol === 'https:' ? 'wss' : 'ws';
  ws = new WebSocket(`${proto}://${location.host}/ws`);
  ws.onopen = () => { retry = 1000; emit('ws', true); };
  ws.onclose = () => {
    emit('ws', false);
    setTimeout(connectWS, retry);
    retry = Math.min(retry * 1.6, 8000);
  };
  ws.onerror = () => ws.close();
  ws.onmessage = (ev) => {
    let m;
    try { m = JSON.parse(ev.data); } catch { return; }
    const d = m.data;
    switch (m.type) {
      case 'hello':
        S.status = d.status; S.connected = !!(d.status && d.status.connected);
        S.live = d.live && Object.keys(d.live).length ? d.live : null;
        S.lie = d.lie && Object.keys(d.lie).length ? d.lie : null;
        S.device = d.device || S.device; S.session = d.session; S.meta = d.meta;
        emit('snapshot', d);
        break;
      case 'vitals': pushVitals(d); emit('vitals', d); break;
      case 'wave': pushWave(d); break;
      case 'status': S.status = d; S.connected = !!d.connected; emit('status', d); break;
      case 'lie': S.lie = d; emit('lie', d); break;
      case 'session': S.session = d; emit('session', d); break;
      case 'result': S.lastResult = d; emit('result', d); break;
      case 'event': emit('event', d); break;
      case 'device': S.device = { hi: d.hi, info: d.info }; emit('device', d); break;
      default: break;
    }
  };
}

// keep-alive (บาง proxy/แอนตี้ไวรัสตัดการเชื่อมต่อที่เงียบนาน)
setInterval(() => { if (ws && ws.readyState === 1) ws.send('ping'); }, 20000);
