// charts.js — กราฟทั้งหมด: Chart.js (กราฟเส้นย้อนหลัง), canvas oscilloscope (คลื่นชีพจร), SVG gauge
import { S, HIST_SEC, WAVE_LEN } from './store.js';
import { cssVar } from './ui.js';

const VCOL = { lie: '--lie', truth: '--truth', inconclusive: '--inc', invalid: '--invalid' };

export function chartTheme() {
  Chart.defaults.font.family = getComputedStyle(document.body).fontFamily;
  Chart.defaults.color = cssVar('--muted');
  Chart.defaults.borderColor = cssVar('--border');
  Chart.defaults.animation = false;
  Chart.defaults.plugins.legend.labels.boxWidth = 12;
  Chart.defaults.plugins.tooltip.mode = 'index';
  Chart.defaults.plugins.tooltip.intersect = false;
}

// ---------------------------------------------------------------- แถบสีช่วงคำถามบนกราฟ
const qwindows = {
  id: 'qwindows',
  beforeDatasetsDraw(chart, _args, opts) {
    if (!opts || !opts.enabled) return;
    const { ctx, chartArea: a, scales } = chart;
    const x = scales.x;
    const now = Date.now();
    ctx.save();
    for (const w of S.windows) {
      const x0 = Math.max(a.left, x.getPixelForValue((w.start - now) / 1000));
      const x1 = Math.min(a.right, x.getPixelForValue((w.end - now) / 1000));
      if (x1 <= x0) continue;
      const col = cssVar(VCOL[w.verdict] || '--accent');
      ctx.globalAlpha = w.verdict ? 0.18 : 0.10;
      ctx.fillStyle = col;
      ctx.fillRect(x0, a.top, x1 - x0, a.bottom - a.top);
      ctx.globalAlpha = 0.9;
      ctx.fillStyle = col;
      ctx.font = '600 11px ' + Chart.defaults.font.family;
      ctx.fillText(w.label || '', x0 + 4, a.top + 12);
    }
    ctx.restore();
  },
};
Chart.register(qwindows);

// ---------------------------------------------------------------- กราฟเส้นแบบเลื่อน (2 นาทีล่าสุด)
export function liveLine(canvas, series, { y = {}, y2 = null, windows = true } = {}) {
  const scales = {
    x: { type: 'linear', min: -HIST_SEC, max: 0, grid: { color: cssVar('--border') },
         ticks: { stepSize: 20, callback: (v) => (v === 0 ? 'ตอนนี้' : `${v}s`) } },
    y: Object.assign({ grid: { color: cssVar('--border') } }, y),
  };
  if (y2) scales.y2 = Object.assign({ position: 'right', grid: { drawOnChartArea: false } }, y2);
  return new Chart(canvas, {
    type: 'line',
    data: {
      datasets: series.map((s) => ({
        label: s.label, data: [], yAxisID: s.axis || 'y', borderColor: cssVar(s.color), backgroundColor: cssVar(s.color) + '22',
        fill: !!s.fill, pointRadius: 0, borderWidth: 2, tension: 0.25, spanGaps: false,
      })),
    },
    options: {
      responsive: true, maintainAspectRatio: false, parsing: false, normalized: true,
      scales, plugins: { qwindows: { enabled: windows }, legend: { display: series.length > 1 } },
    },
  });
}

export function updateLive(chart, keys) {
  const now = Date.now();
  const t = S.hist.t;
  chart.data.datasets.forEach((ds, i) => {
    const arr = S.hist[keys[i]];
    ds.data = t.map((ti, j) => ({ x: (ti - now) / 1000, y: arr[j] ?? null }));
  });
  chart.update('none');
}

// ---------------------------------------------------------------- Oscilloscope คลื่นชีพจร
export function startScope(canvas) {
  const ctx = canvas.getContext('2d');
  let lo = -500, hi = 500;
  function draw() {
    const dpr = window.devicePixelRatio || 1;
    const w = canvas.clientWidth, hgt = canvas.clientHeight;
    if (canvas.width !== Math.round(w * dpr) || canvas.height !== Math.round(hgt * dpr)) {
      canvas.width = Math.round(w * dpr); canvas.height = Math.round(hgt * dpr);
    }
    ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
    ctx.clearRect(0, 0, w, hgt);
    // เส้นกริดทุก 1 วินาที
    ctx.strokeStyle = cssVar('--border'); ctx.lineWidth = 1;
    for (let s = 1; s < WAVE_LEN / 100; s++) {
      const x = (s * 100 / WAVE_LEN) * w; ctx.beginPath(); ctx.moveTo(x, 0); ctx.lineTo(x, hgt); ctx.stroke();
    }
    const n = WAVE_LEN;
    let mn = Infinity, mx = -Infinity;
    for (let i = 0; i < n; i++) { const v = S.wave[i]; if (v < mn) mn = v; if (v > mx) mx = v; }
    // ปรับสเกลแบบนุ่มนวล (ไม่กระตุกเวลาสัญญาณเปลี่ยน)
    const pad = Math.max(40, (mx - mn) * 0.15);
    lo += ((mn - pad) - lo) * 0.08; hi += ((mx + pad) - hi) * 0.08;
    if (hi - lo < 100) { const m = (hi + lo) / 2; hi = m + 50; lo = m - 50; }
    const Y = (v) => hgt - 8 - ((v - lo) / (hi - lo)) * (hgt - 16);
    const grad = ctx.createLinearGradient(0, 0, w, 0);
    grad.addColorStop(0, cssVar('--hr') + '33'); grad.addColorStop(0.7, cssVar('--hr') + 'cc'); grad.addColorStop(1, cssVar('--hr'));
    ctx.strokeStyle = grad; ctx.lineWidth = 2.2; ctx.lineJoin = 'round';
    ctx.beginPath();
    for (let i = 0; i < n; i++) {
      const v = S.wave[(S.waveHead + i) % n];
      const x = (i / (n - 1)) * w;
      i ? ctx.lineTo(x, Y(v)) : ctx.moveTo(x, Y(v));
    }
    ctx.stroke();
    ctx.fillStyle = cssVar('--lie');
    for (let i = 0; i < n; i++) {
      const k = (S.waveHead + i) % n;
      if (S.beats[k]) { ctx.beginPath(); ctx.arc((i / (n - 1)) * w, Y(S.wave[k]) - 6, 4, 0, Math.PI * 2); ctx.fill(); }
    }
    const v = S.live;
    if (!v || !v.con) {
      ctx.fillStyle = cssVar('--muted'); ctx.font = '600 16px ' + Chart.defaults.font.family; ctx.textAlign = 'center';
      ctx.fillText(v ? 'วางนิ้ว/ผิวให้แตะเซนเซอร์ชีพจร (MAX30102)' : 'ยังไม่ได้รับข้อมูลจากนาฬิกา', w / 2, hgt / 2);
      ctx.textAlign = 'start';
    }
  }
  function loop() {
    if (canvas.offsetParent !== null) draw();   // วาดเฉพาะตอนมองเห็น (ประหยัด CPU)
    requestAnimationFrame(loop);
  }
  requestAnimationFrame(loop);
}

// ---------------------------------------------------------------- Gauge ครึ่งวงกลม
function polar(cx, cy, r, deg) {
  const a = (deg * Math.PI) / 180;
  return [cx + r * Math.cos(a), cy - r * Math.sin(a)];
}
function arc(cx, cy, r, d0, d1) {
  const [x0, y0] = polar(cx, cy, r, d0), [x1, y1] = polar(cx, cy, r, d1);
  return `M ${x0} ${y0} A ${r} ${r} 0 0 1 ${x1} ${y1}`;
}

export function makeGauge(svg, { label = '' } = {}) {
  const cx = 110, cy = 112, r = 88;
  const seg = (p0, p1, col) => `<path d="${arc(cx, cy, r, 180 - p0 * 1.8, 180 - p1 * 1.8)}" stroke="var(${col})" stroke-width="16" fill="none" stroke-linecap="butt" opacity=".85"/>`;
  svg.innerHTML = `
    <path d="${arc(cx, cy, r, 180, 0)}" stroke="var(--bg2)" stroke-width="20" fill="none"/>
    ${seg(0, 35, '--truth')}${seg(35, 65, '--inc')}${seg(65, 100, '--lie')}
    <g class="needle" style="transition: transform .6s cubic-bezier(.2,.8,.2,1); transform-origin: ${cx}px ${cy}px">
      <line x1="${cx}" y1="${cy}" x2="${cx - r + 14}" y2="${cy}" stroke="var(--text)" stroke-width="4" stroke-linecap="round"/>
    </g>
    <circle cx="${cx}" cy="${cy}" r="8" fill="var(--text)"/>
    <text class="val" x="${cx}" y="${cy - 26}" text-anchor="middle">--</text>
    <text class="cap" x="18" y="${cy + 18}">ผ่อนคลาย</text>
    <text class="cap" x="202" y="${cy + 18}" text-anchor="end">เครียดมาก</text>
    <text class="cap" x="${cx}" y="${cy + 18}" text-anchor="middle">${label}</text>`;
  const needle = svg.querySelector('.needle'), val = svg.querySelector('.val');
  return {
    set(v) {
      if (v === null || v === undefined || v < 0) {
        val.textContent = '--'; needle.style.transform = 'rotate(0deg)'; return;
      }
      val.textContent = Math.round(v);
      needle.style.transform = `rotate(${(Math.min(100, Math.max(0, v)) / 100) * 180}deg)`;
    },
  };
}
