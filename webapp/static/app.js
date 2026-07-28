/*
 * The analyser.
 *
 * Everything is drawn by hand onto canvases. No libraries, no CDN, no fonts
 * fetched at load: the tag works with no network and the instrument reading it
 * should too.
 *
 * The trace and the waterfall share one frequency axis and one horizontal
 * mapping, deliberately. That is the whole layout idea - find a peak in the
 * trace, follow it straight down to see whether it has been there for four
 * minutes or arrived just now.
 */

'use strict';

const $ = (id) => document.getElementById(id);

const el = {
  tag: $('tag'), capture: $('capture'), link: $('link'),
  nfft: $('nfft'), overlap: $('overlap'), window: $('window'), axis: $('axis'),
  span: $('span'), ref: $('ref'), range: $('range'), autoscale: $('autoscale'),
  dRes: $('d-res'), dWin: $('d-win'), dHop: $('d-hop'), dRate: $('d-rate'),
  empty: $('empty'), stack: $('stack'), lower: $('lower'),
  trace: $('trace'), fall: $('fall'), axisCanvas: $('freqaxis'), bands: $('bands'),
  marker: $('marker'), stats: $('stats'), scalekey: $('scalekey'),
  clearhold: $('clearhold'), emptyReason: $('empty-reason'),
};

const state = {
  mac: null,
  streaming: null,        // mac currently being captured, from the server
  data: null,             // last /api/spectrogram payload
  hold: null,             // element-wise max, persists until reset
  cursor: null,           // {x} in canvas pixels, or null
  timer: null,
};

/* Inferno, sampled at 10 points and interpolated. Perceptually uniform, so
 * equal steps in dB look like equal steps in brightness - a rainbow map would
 * invent edges at its colour boundaries that are not in the data. */
const RAMP = [
  [0, 0, 4], [22, 11, 57], [66, 10, 104], [106, 23, 110], [147, 38, 103],
  [188, 55, 84], [221, 81, 58], [243, 120, 25], [249, 166, 26], [252, 255, 164],
];

function ramp(t) {
  t = Math.max(0, Math.min(1, t));
  const p = t * (RAMP.length - 1);
  const i = Math.min(RAMP.length - 2, Math.floor(p));
  const f = p - i;
  const a = RAMP[i], b = RAMP[i + 1];
  return [
    a[0] + (b[0] - a[0]) * f,
    a[1] + (b[1] - a[1]) * f,
    a[2] + (b[2] - a[2]) * f,
  ];
}

/* ---- canvas plumbing --------------------------------------------------- */

function fit(canvas) {
  const dpr = window.devicePixelRatio || 1;
  const w = canvas.clientWidth;
  const h = canvas.getAttribute('height') | 0;
  if (canvas.width !== Math.round(w * dpr) || canvas.height !== Math.round(h * dpr)) {
    canvas.width = Math.round(w * dpr);
    canvas.height = Math.round(h * dpr);
  }
  const ctx = canvas.getContext('2d');
  ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
  ctx.clearRect(0, 0, w, h);
  return { ctx, w, h };
}

const css = (name) => getComputedStyle(document.documentElement).getPropertyValue(name).trim();

/* ---- settings ---------------------------------------------------------- */

function settings() {
  return {
    nfft: +el.nfft.value,
    overlap: +el.overlap.value,
    window: el.window.value,
    axis: el.axis.value,
    span: +el.span.value,
    ref: +el.ref.value,
    range: +el.range.value,
  };
}

function query() {
  const s = settings();
  return new URLSearchParams({
    mac: state.mac, since: s.span, nfft: s.nfft,
    overlap: s.overlap, window: s.window, axis: s.axis,
  }).toString();
}

/* ---- data -------------------------------------------------------------- */

async function refreshTags() {
  let payload;
  try {
    payload = await (await fetch('/api/tags')).json();
  } catch (e) {
    el.link.textContent = 'server unreachable';
    el.link.className = 'link bad';
    return;
  }

  const tags = payload.tags || [];
  state.streaming = payload.stream && payload.stream.connected ? payload.stream.mac : null;

  const want = state.mac;
  el.tag.innerHTML = '';
  for (const t of tags) {
    const o = document.createElement('option');
    o.value = t.mac;
    // Say which tags can actually be captured from. A tag with no service is
    // not broken, it is a stock RuuviTag, and the difference matters.
    o.textContent = t.mac + (t.streamable ? '' : '  (broadcast only)');
    el.tag.appendChild(o);
  }
  if (!tags.length) {
    const o = document.createElement('option');
    o.textContent = 'no tags seen';
    el.tag.appendChild(o);
    el.tag.disabled = true;
  } else {
    el.tag.disabled = false;
    state.mac = tags.some((t) => t.mac === want) ? want : tags[0].mac;
    el.tag.value = state.mac;
  }

  const cur = tags.find((t) => t.mac === state.mac);
  el.capture.disabled = !cur || !cur.streamable;
  const on = state.streaming && state.streaming === state.mac;
  el.capture.classList.toggle('on', !!on);
  el.capture.textContent = on ? 'Stop capture' : 'Start capture';

  if (payload.stream && payload.stream.error) {
    el.link.textContent = payload.stream.error;
    el.link.className = 'link bad';
  } else if (on) {
    const info = payload.stream.info || {};
    const hz = info.measured_hz || info.nominal_hz;
    el.link.textContent = `capturing · ${hz ? hz.toFixed(1) + ' Hz' : '—'}` +
      (info.measured_hz ? ' measured' : ' nominal');
    el.link.className = 'link on';
  } else {
    el.link.textContent = tags.length ? `${tags.length} tag${tags.length > 1 ? 's' : ''} seen` : 'no tags seen';
    el.link.className = 'link';
  }
}

async function refreshSpectrum() {
  if (!state.mac) return;
  let d;
  try {
    d = await (await fetch('/api/spectrogram?' + query())).json();
  } catch (e) {
    return;
  }

  if (d.error || d.empty) {
    state.data = null;
    el.empty.hidden = false;
    el.stack.hidden = true;
    el.lower.hidden = true;
    if (d.reason || d.error) el.emptyReason.textContent = d.reason || d.error;
    return;
  }

  el.empty.hidden = true;
  el.stack.hidden = false;
  el.lower.hidden = false;

  // The hold is only comparable against spectra with the same bins.
  if (!state.hold || state.hold.length !== d.freqs.length) state.hold = d.freqs.map(() => -Infinity);
  for (const col of d.columns) {
    if (!col) continue;
    for (let i = 0; i < col.length; i++) {
      if (col[i] !== null && col[i] > state.hold[i]) state.hold[i] = col[i];
    }
  }

  state.data = d;
  drawAll();
  showDerived(d);
  showStats(d);
}

function showDerived(d) {
  el.dRes.textContent = d.bin_hz.toFixed(3) + ' Hz';
  el.dWin.textContent = d.window_seconds.toFixed(2) + ' s';
  el.dHop.textContent = d.hop_seconds.toFixed(2) + ' s';
  el.dRate.textContent = d.fs_hz.toFixed(1) + ' Hz' + (d.fs_measured ? '' : ' nom');
  el.dRate.title = d.fs_measured
    ? 'Measured output data rate, read from the tag'
    : 'Nominal rate - the tag has not reported a measured one, so every frequency here may be off by the oscillator error (a few percent)';
}

function showStats(d) {
  const rows = [];
  const push = (k, v, cls) => rows.push(`<dt>${k}</dt><dd class="${cls || ''}">${v}</dd>`);

  push('Sample rate', d.fs_hz.toFixed(3) + ' Hz', d.fs_measured ? 'ok' : '');
  push('Rate source', d.fs_measured ? 'measured' : 'assumed', d.fs_measured ? 'ok' : 'bad');
  push('Columns', d.times.length);
  push('Lost samples', d.missing_samples, d.missing_samples ? 'bad' : 'ok');
  push('Windows dropped', d.lost_windows, d.lost_windows ? 'bad' : 'ok');
  if (d.peak) {
    push('Strongest tone', d.peak.hz.toFixed(2) + ' Hz');
    push('Its amplitude', (d.peak.amplitude_ug / 1000).toFixed(2) + ' mg');
  } else {
    push('Strongest tone', 'none above noise');
  }
  push('Flatness', d.flatness === null ? '—' : d.flatness.toFixed(3));
  el.stats.innerHTML = rows.join('');

  for (const n of d.notes || []) {
    const dt = document.createElement('dt');
    dt.textContent = 'Note';
    const dd = document.createElement('dd');
    dd.textContent = n;
    dd.className = 'bad';
    el.stats.append(dt, dd);
  }
}

/* ---- drawing ----------------------------------------------------------- */

const HZ = (d, x, w) => d.freqs[0] + (x / w) * (d.freqs[d.freqs.length - 1] - d.freqs[0]);
const X_OF = (d, hz, w) => {
  const lo = d.freqs[0], hi = d.freqs[d.freqs.length - 1];
  return ((hz - lo) / (hi - lo)) * w;
};

function drawAll() {
  drawTrace();
  drawFall();
  drawAxis();
  drawBands();
  drawScaleKey();
}

function drawTrace() {
  const d = state.data;
  const { ctx, w, h } = fit(el.trace);
  if (!d) return;

  const s = settings();
  const top = s.ref, bot = s.ref - s.range;
  const yOf = (db) => h - ((db - bot) / (top - bot)) * h;
  const n = d.freqs.length;

  // Grid. Horizontal lines every 10 dB, verticals on decade-ish frequencies.
  ctx.strokeStyle = css('--rule-soft');
  ctx.lineWidth = 1;
  ctx.beginPath();
  for (let db = Math.ceil(bot / 10) * 10; db <= top; db += 10) {
    const y = Math.round(yOf(db)) + 0.5;
    ctx.moveTo(0, y); ctx.lineTo(w, y);
  }
  for (const hz of [5, 10, 15, 20, 25, 30, 35, 40, 45]) {
    if (hz < d.freqs[0] || hz > d.freqs[n - 1]) continue;
    const x = Math.round(X_OF(d, hz, w)) + 0.5;
    ctx.moveTo(x, 0); ctx.lineTo(x, h);
  }
  ctx.stroke();

  const path = (values, colour, width, alpha) => {
    ctx.strokeStyle = colour;
    ctx.globalAlpha = alpha;
    ctx.lineWidth = width;
    ctx.lineJoin = 'round';
    ctx.beginPath();
    let pen = false;
    for (let i = 0; i < n; i++) {
      const v = values[i];
      // A null is a lost window and breaks the line. A value off the bottom of
      // the scale is a measurement, so it rides the floor rather than
      // disappearing - the trace pinned flat is how an analyser says "below
      // the range you asked for", and it is visibly different from a gap.
      if (v === null || v === undefined || !isFinite(v)) { pen = false; continue; }
      const x = (i / (n - 1)) * w;
      const y = Math.max(1, Math.min(h - 1, yOf(v)));
      if (pen) ctx.lineTo(x, y); else { ctx.moveTo(x, y); pen = true; }
    }
    ctx.stroke();
    ctx.globalAlpha = 1;
  };

  // Persistence. These are real previous spectra, not a fade effect: how
  // widely the recent traces spread at a frequency is how much that component
  // is wandering, which a single frame cannot show.
  const reduced = window.matchMedia('(prefers-reduced-motion: reduce)').matches;
  if (!reduced) {
    const cols = d.columns;
    const depth = Math.min(24, cols.length - 1);
    for (let k = depth; k >= 1; k--) {
      const col = cols[cols.length - 1 - k];
      if (!col) continue;
      path(col, css('--live'), 1, 0.10 * (1 - k / (depth + 1)));
    }
  }

  if (state.hold) path(state.hold.map((v) => (isFinite(v) ? v : null)), css('--hold'), 1, 0.9);

  const live = d.columns[d.columns.length - 1];
  if (live) path(live, css('--live'), 1.6, 1);

  // Reference level and floor, in the corners, over the plot. Keeping them
  // inside preserves the exact horizontal alignment with the waterfall below.
  ctx.font = '10px ' + css('--font-mono');
  ctx.fillStyle = css('--text-faint');
  ctx.textBaseline = 'top';
  ctx.fillText(`${top} dB`, 4, 3);
  ctx.textBaseline = 'bottom';
  ctx.fillText(`${bot} dB  ref 1 µg`, 4, h - 3);

  if (state.cursor !== null) cursorLine(ctx, w, h, state.cursor);
}

function drawFall() {
  const d = state.data;
  const { ctx, w, h } = fit(el.fall);
  if (!d) return;

  const s = settings();
  const top = s.ref, bot = s.ref - s.range;
  const nF = d.freqs.length;
  const nT = d.columns.length;

  // Build at data resolution, then let the canvas scale it. Drawing a rect per
  // cell would be tens of thousands of fills a second.
  const img = ctx.createImageData(nF, nT);
  for (let t = 0; t < nT; t++) {
    const col = d.columns[nT - 1 - t];       // newest at the top
    for (let f = 0; f < nF; f++) {
      const o = (t * nF + f) * 4;
      const v = col ? col[f] : null;
      if (v === null || v === undefined) {
        // A lost window. Left as the panel colour so it reads as absence
        // rather than as the bottom of the colour scale, which is silence.
        img.data[o] = 0x1b; img.data[o + 1] = 0x24; img.data[o + 2] = 0x31; img.data[o + 3] = 255;
        continue;
      }
      const [r, g, b] = ramp((v - bot) / (top - bot));
      img.data[o] = r; img.data[o + 1] = g; img.data[o + 2] = b; img.data[o + 3] = 255;
    }
  }

  const off = document.createElement('canvas');
  off.width = nF; off.height = nT;
  off.getContext('2d').putImageData(img, 0, 0);

  ctx.imageSmoothingEnabled = false;   // a bin is a bin; do not invent gradients
  ctx.drawImage(off, 0, 0, nF, nT, 0, 0, w, h);

  // Elapsed-time ticks down the left edge.
  ctx.font = '10px ' + css('--font-mono');
  ctx.fillStyle = 'rgba(223,230,239,0.55)';
  ctx.textBaseline = 'top';
  const totalS = d.times.length > 1 ? d.times[d.times.length - 1] - d.times[0] : 0;
  for (let frac = 0; frac <= 1.001; frac += 0.25) {
    const y = frac * h;
    const ago = totalS * frac;
    if (frac > 0) {
      ctx.fillRect(0, Math.round(y) - 0.5, 6, 1);
      ctx.fillText(`-${ago < 90 ? ago.toFixed(0) + 's' : (ago / 60).toFixed(1) + 'm'}`, 8, y - 5);
    } else {
      ctx.fillText('now', 8, 3);
    }
  }

  if (state.cursor !== null) cursorLine(ctx, w, h, state.cursor);
}

function cursorLine(ctx, w, h, x) {
  ctx.save();
  ctx.strokeStyle = css('--hold');
  ctx.globalAlpha = 0.75;
  ctx.lineWidth = 1;
  ctx.beginPath();
  ctx.moveTo(Math.round(x) + 0.5, 0);
  ctx.lineTo(Math.round(x) + 0.5, h);
  ctx.stroke();
  ctx.restore();
}

function drawAxis() {
  const d = state.data;
  const { ctx, w, h } = fit(el.axisCanvas);
  if (!d) return;

  ctx.strokeStyle = css('--rule');
  ctx.beginPath();
  ctx.moveTo(0, 0.5); ctx.lineTo(w, 0.5);
  ctx.stroke();

  ctx.font = '10px ' + css('--font-mono');
  ctx.fillStyle = css('--text-dim');
  ctx.textBaseline = 'top';

  const lo = d.freqs[0], hi = d.freqs[d.freqs.length - 1];
  const step = (hi - lo) > 40 ? 5 : (hi - lo) > 15 ? 2 : 1;
  const first = Math.ceil(lo / step) * step;
  for (let hz = first; hz <= hi; hz += step) {
    const x = X_OF(d, hz, w);
    ctx.fillRect(Math.round(x), 0, 1, 4);
    const label = String(Math.round(hz));
    const tw = ctx.measureText(label).width;
    ctx.fillText(label, Math.min(w - tw, Math.max(0, x - tw / 2)), 6);
  }
  ctx.fillStyle = css('--text-faint');
  ctx.fillText('Hz', w - 14, 6);
}

function drawScaleKey() {
  const s = settings();
  el.scalekey.innerHTML = '';
  const c = document.createElement('canvas');
  c.width = 120; c.height = 8;
  c.style.width = '120px'; c.style.height = '8px';
  const g = c.getContext('2d');
  for (let i = 0; i < 120; i++) {
    const [r, gg, b] = ramp(i / 119);
    g.fillStyle = `rgb(${r | 0},${gg | 0},${b | 0})`;
    g.fillRect(i, 0, 1, 8);
  }
  const lo = document.createElement('span');
  lo.textContent = (s.ref - s.range) + ' dB';
  const hi = document.createElement('span');
  hi.textContent = s.ref + ' dB';
  el.scalekey.append(lo, c, hi);
}

function drawBands() {
  const d = state.data;
  const { ctx, w, h } = fit(el.bands);
  if (!d || !d.bands) return;

  const series = [['1-10', css('--b1')], ['10-25', css('--b2')], ['25-50', css('--b3')]];
  let max = 0;
  for (const [k] of series) for (const v of d.bands[k] || []) if (v !== null && v > max) max = v;
  if (max <= 0) max = 1;

  // Log amplitude. Vibration levels are log-distributed: a linear axis scaled
  // to a knock renders everything quieter than it as a flat line on the floor.
  const top = Math.log10(max * 1.3);
  const bot = Math.log10(Math.max(max / 3000, 1));
  const yOf = (v) => h - ((Math.log10(Math.max(v, 1)) - bot) / (top - bot)) * h;

  ctx.strokeStyle = css('--rule-soft');
  ctx.beginPath();
  for (let f = 0; f <= 1.001; f += 0.25) {
    const y = Math.round(h * f) + 0.5;
    ctx.moveTo(0, y); ctx.lineTo(w, y);
  }
  ctx.stroke();

  for (const [k, colour] of series) {
    const vals = d.bands[k] || [];
    ctx.strokeStyle = colour;
    ctx.lineWidth = 1.4;
    ctx.beginPath();
    let pen = false;
    for (let i = 0; i < vals.length; i++) {
      const v = vals[i];
      if (v === null || !isFinite(v)) { pen = false; continue; }
      const x = (i / Math.max(1, vals.length - 1)) * w;
      const y = yOf(v);
      if (pen) ctx.lineTo(x, y); else { ctx.moveTo(x, y); pen = true; }
    }
    ctx.stroke();
  }

  ctx.font = '10px ' + css('--font-mono');
  ctx.fillStyle = css('--text-faint');
  ctx.textBaseline = 'top';
  ctx.fillText((10 ** top / 1000).toFixed(2) + ' mg', 4, 3);
  ctx.textBaseline = 'bottom';
  ctx.fillText('older', 4, h - 3);
  const nowW = ctx.measureText('newer').width;
  ctx.fillText('newer', w - nowW - 4, h - 3);
}

/* ---- marker ------------------------------------------------------------ */

function onMove(ev) {
  const d = state.data;
  if (!d) return;
  const rect = ev.currentTarget.getBoundingClientRect();
  const x = ev.clientX - rect.left;
  state.cursor = x;

  const hz = HZ(d, x, rect.width);
  const i = Math.max(0, Math.min(d.freqs.length - 1,
    Math.round((hz - d.freqs[0]) / (d.freqs[1] - d.freqs[0]))));
  const live = d.columns[d.columns.length - 1];
  const v = live ? live[i] : null;
  const hold = state.hold ? state.hold[i] : null;

  el.marker.textContent =
    `${d.freqs[i].toFixed(2)} Hz   ` +
    `${v === null || v === undefined ? '  —  ' : v.toFixed(1) + ' dB'}   ` +
    `hold ${hold !== null && isFinite(hold) ? hold.toFixed(1) + ' dB' : '—'}`;
  el.marker.classList.add('on');

  drawTrace();
  drawFall();
}

function onLeave() {
  state.cursor = null;
  el.marker.classList.remove('on');
  if (state.data) { drawTrace(); drawFall(); }
}

/* ---- actions ----------------------------------------------------------- */

async function toggleCapture() {
  const on = state.streaming === state.mac;
  el.capture.disabled = true;
  try {
    await fetch('/api/stream', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ mac: on ? null : state.mac }),
    });
  } catch (e) {
    el.link.textContent = 'capture request failed';
    el.link.className = 'link bad';
  }
  await refreshTags();
}

function autoscale() {
  const d = state.data;
  if (!d) return;
  const all = [];
  for (const col of d.columns) {
    if (!col) continue;
    for (const v of col) if (v !== null && isFinite(v)) all.push(v);
  }
  if (!all.length) return;
  all.sort((a, b) => a - b);
  const q = (p) => all[Math.min(all.length - 1, Math.floor(p * all.length))];

  // Top just above the loudest thing present, floor a little under the bulk of
  // it. Clamped to the options the selects offer so the controls keep telling
  // the truth about what is displayed.
  const wantRef = Math.ceil((q(0.999) + 3) / 20) * 20;
  const wantRange = Math.ceil((wantRef - q(0.02)) / 20) * 20;
  const pick = (sel, v) => {
    const opts = [...sel.options].map((o) => +o.value);
    sel.value = String(opts.reduce((a, b) => (Math.abs(b - v) < Math.abs(a - v) ? b : a)));
  };
  pick(el.ref, wantRef);
  pick(el.range, wantRange);
  drawAll();
}

function resetHold() {
  state.hold = null;
  if (state.data) drawAll();
}

/* ---- wiring ------------------------------------------------------------ */

function schedule() {
  clearInterval(state.timer);
  // Faster while capturing: a column arrives every hop, and the point of the
  // instrument is watching it arrive.
  const period = state.streaming === state.mac ? 1000 : 4000;
  state.timer = setInterval(refreshSpectrum, period);
}

for (const c of [el.nfft, el.overlap, el.window, el.axis, el.span]) {
  c.addEventListener('change', () => { resetHold(); refreshSpectrum(); });
}
for (const c of [el.ref, el.range]) {
  c.addEventListener('change', () => { if (state.data) drawAll(); });
}
el.tag.addEventListener('change', () => {
  state.mac = el.tag.value;
  resetHold();
  refreshTags().then(refreshSpectrum).then(schedule);
});
el.capture.addEventListener('click', () => toggleCapture().then(schedule));
el.autoscale.addEventListener('click', autoscale);
el.clearhold.addEventListener('click', resetHold);

for (const c of [el.trace, el.fall]) {
  c.addEventListener('mousemove', onMove);
  c.addEventListener('mouseleave', onLeave);
}

let resizeTimer;
window.addEventListener('resize', () => {
  clearTimeout(resizeTimer);
  resizeTimer = setTimeout(() => { if (state.data) drawAll(); }, 100);
});

async function boot() {
  await refreshTags();
  await refreshSpectrum();
  schedule();
  setInterval(refreshTags, 5000);
}

boot();
