// api.js — เรียก REST API ของ Studio (ซึ่งส่งต่อไปนาฬิกาอีกที) + แสดงข้อความผิดพลาดเป็นภาษาไทย
import { toast } from './ui.js';

// เรียก REST API ของ Studio แล้วคืน JSON — ถ้าผิดพลาดแสดงข้อความไทยมุมจอ (quiet = ไม่ต้องแสดง)
export async function api(path, { method = 'GET', body, quiet = false } = {}) {
  let res;
  try {
    res = await fetch(path, {
      method,
      headers: body !== undefined ? { 'Content-Type': 'application/json' } : undefined,
      body: body !== undefined ? JSON.stringify(body) : undefined,
    });
  } catch (e) {
    if (!quiet) toast('ติดต่อ Polygraph Studio ไม่ได้ — โปรแกรม backend ยังเปิดอยู่ไหม?', 'err');
    throw e;
  }
  let data = null;
  try { data = await res.json(); } catch { data = null; }
  if (!res.ok || (data && data.ok === false)) {
    const msg = (data && (data.msg || data.detail || data.error)) || `HTTP ${res.status}`;
    if (!quiet) toast(typeof msg === 'string' ? msg : JSON.stringify(msg), 'err');
    return data || { ok: false, msg };
  }
  return data;
}

export const get = (p, opt) => api(p, opt);
export const post = (p, body, opt) => api(p, Object.assign({ method: 'POST', body: body === undefined ? {} : body }, opt));
export const del = (p) => api(p, { method: 'DELETE' });
export const patch = (p, body) => api(p, { method: 'PATCH', body });

// ส่งคำสั่งไปนาฬิกา: baseline / abort / reset / power / restart / demo ...
export const cmd = (action, params = {}) => post('/api/watch/command', { action, params });
