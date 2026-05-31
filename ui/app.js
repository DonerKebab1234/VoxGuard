/**
 * VoxGuard — app.js
 * Handles the C++ ↔ JS bridge, builds the segmented meter, and drives all UI state.
 *
 * Message protocol (JSON strings in both directions):
 *   JS → C++:  { type: "ping" }
 *               { type: "getDevices" }
 *               { type: "startCapture", deviceIndex: N }
 *               { type: "stopCapture" }
 *               { type: "calibrate" }
 *   C++ → JS:  { type: "pong", ok: true }
 *               { type: "deviceList", devices: [{id,name,isDefault},...] }
 *               { type: "captureStarted", ok: bool }
 *               { type: "meter", dbfs: -23.5 }
 */

'use strict';

// ── Meter constants ──────────────────────────────────────────────
const SEGS       = 40;
const DB_MIN     = -60;
const DB_MAX     = 0;
const DB_RANGE   = DB_MAX - DB_MIN;           // 60

// Zone thresholds as segment indices (0 = bottom = silence)
const YELLOW_AT  = Math.round(((- 12 - DB_MIN) / DB_RANGE) * SEGS); // 32
const RED_AT     = Math.round(((- 6  - DB_MIN) / DB_RANGE) * SEGS); // 36

// ── Build meter segments (runs once) ────────────────────────────
const meterBar = document.getElementById('meterBar');
const segments = [];
for (let i = 0; i < SEGS; i++) {
  const el = document.createElement('div');
  const zone = i >= RED_AT ? 'red' : i >= YELLOW_AT ? 'yellow' : 'green';
  el.className = `seg ${zone} unlit`;
  meterBar.appendChild(el);
  segments.push(el);
}

// ── Peak-hold state ──────────────────────────────────────────────
const peakEl        = document.getElementById('meterPeak');
let   peakSeg       = 0;
let   peakTimeout   = null;
const PEAK_HOLD_MS  = 1600;
const BAR_H         = 300;   // must match CSS .meter-bar-wrap height
const SEG_H         = (BAR_H - 6 - (SEGS - 1) * 2) / SEGS; // approx segment height

function placePeak(segIdx) {
  // segIdx 0 = bottom. Bar fills bottom→top via flex-direction:column-reverse.
  // Top of segment i from bottom of bar:
  const fromBottom = segIdx * (SEG_H + 2) + 3;   // 3px padding
  peakEl.style.top = `${BAR_H - fromBottom - 2}px`;
}

function updatePeak(litCount) {
  if (litCount > peakSeg) {
    peakSeg = litCount;
    clearTimeout(peakTimeout);
    peakEl.classList.add('visible');
    peakTimeout = setTimeout(() => {
      peakSeg = 0;
      peakEl.classList.remove('visible');
    }, PEAK_HOLD_MS);
  }
  if (peakSeg > 0) placePeak(peakSeg);
}

// ── Meter render ─────────────────────────────────────────────────
const readoutEl = document.getElementById('readoutValue');

function renderMeter(dbfs) {
  const clamped  = Math.max(DB_MIN, Math.min(DB_MAX, dbfs));
  const level    = (clamped - DB_MIN) / DB_RANGE;   // 0..1
  const litCount = Math.round(level * SEGS);

  for (let i = 0; i < SEGS; i++) {
    const on = i < litCount;
    segments[i].classList.toggle('lit',   on);
    segments[i].classList.toggle('unlit', !on);
  }

  updatePeak(litCount);

  // Numeric display
  readoutEl.textContent = dbfs <= DB_MIN + 0.5 ? '– –' : clamped.toFixed(1);
  readoutEl.className   = 'readout-value';
  if (dbfs > -6)       readoutEl.classList.add('zone-red');
  else if (dbfs > -12) readoutEl.classList.add('zone-yellow');
  else                 readoutEl.classList.add('zone-green');
}

// ── Session stats ────────────────────────────────────────────────
let sessionBreaches  = 0;
let sessionStartTime = null;
let sessionTimerID   = null;

function startSessionTimer() {
  sessionStartTime = Date.now();
  sessionTimerID = setInterval(() => {
    const secs = Math.floor((Date.now() - sessionStartTime) / 1000);
    const m    = Math.floor(secs / 60);
    const s    = String(secs % 60).padStart(2, '0');
    document.getElementById('statTime').textContent = `${m}:${s}`;
  }, 1000);
}

function recordBreach() {
  sessionBreaches++;
  document.getElementById('statBreaches').textContent = sessionBreaches;
  const score = Math.max(0, 100 - sessionBreaches * 5);
  document.getElementById('statScore').textContent = score;
}

// ── Bridge ───────────────────────────────────────────────────────
const bridge = window.chrome?.webview ?? null;

const statusDot   = document.getElementById('statusDot');
const statusLabel = document.getElementById('statusLabel');
const deviceSel   = document.getElementById('deviceSelect');
const startBtn    = document.getElementById('startBtn');

function setStatus(dot, label) {
  statusDot.className  = 'status-dot' + (dot ? ` ${dot}` : '');
  statusLabel.textContent = label;
}

function sendMsg(obj) {
  if (bridge) bridge.postMessage(JSON.stringify(obj));
}

// ── Handle messages from C++ ─────────────────────────────────────
function onCppMessage(raw) {
  let msg;
  try { msg = JSON.parse(raw); } catch { return; }

  switch (msg.type) {

    case 'pong':
      setStatus('active', 'Ready');
      break;

    case 'deviceList':
      deviceSel.innerHTML = '<option value="-1">Default Microphone</option>';
      (msg.devices ?? []).forEach(d => {
        const opt = document.createElement('option');
        opt.value       = d.id;
        opt.textContent = d.name + (d.isDefault ? ' ★' : '');
        deviceSel.appendChild(opt);
      });
      break;

    case 'captureStarted':
      if (msg.ok) {
        setStatus('active', 'Capturing');
        startBtn.textContent = 'Stop';
        startBtn.classList.add('capturing');
        startSessionTimer();
      } else {
        setStatus('', 'Capture failed');
      }
      break;

    case 'meter':
      renderMeter(Number(msg.dbfs));
      // TODO Phase 3: check against calibrated thresholds, call recordBreach()
      break;
  }
}

// ── Bridge wiring ────────────────────────────────────────────────
if (bridge) {
  setStatus('connecting', 'Connecting…');
  bridge.addEventListener('message', e => onCppMessage(e.data));

  // Kick off handshake — fire immediately (DOMContentLoaded may already be past)
  sendMsg({ type: 'ping' });
  sendMsg({ type: 'getDevices' });

} else {
  // No bridge: browser / preview mode — simulate a moving meter
  setStatus('', 'Preview');
  let fake = -40;
  setInterval(() => {
    fake += (Math.random() - 0.44) * 5;
    fake  = Math.max(-60, Math.min(0, fake));
    renderMeter(fake);
  }, 55);
}

// ── Start / Stop button ──────────────────────────────────────────
let capturing = false;
startBtn.addEventListener('click', () => {
  if (!bridge) return;
  if (!capturing) {
    capturing = true;
    const idx = parseInt(deviceSel.value, 10);
    sendMsg({ type: 'startCapture', deviceIndex: idx });
  } else {
    capturing = false;
    startBtn.textContent = 'Start';
    startBtn.classList.remove('capturing');
    setStatus('active', 'Ready');
    clearInterval(sessionTimerID);
    sendMsg({ type: 'stopCapture' });
  }
});

// ── Sliders ──────────────────────────────────────────────────────
function bindSlider(sliderId, valId, suffix, onChangeFn) {
  const slider = document.getElementById(sliderId);
  const valEl  = document.getElementById(valId);
  if (!slider || !valEl) return;
  slider.addEventListener('input', () => {
    valEl.textContent = slider.value + suffix;
    if (onChangeFn) onChangeFn(Number(slider.value));
  });
}

bindSlider('sidetoneLevel', 'sidetoneLevelVal', '%',
  v => sendMsg({ type: 'setSidetoneLevel', value: v }));

bindSlider('duckAmount', 'duckAmountVal', '%',
  v => sendMsg({ type: 'setDuckAmount', value: v }));

// ── Toggle listeners ─────────────────────────────────────────────
document.getElementById('sidetoneEnable')?.addEventListener('change', e => {
  sendMsg({ type: 'setSidetone', enabled: e.target.checked });
});
document.getElementById('duckEnable')?.addEventListener('change', e => {
  sendMsg({ type: 'setDuck', enabled: e.target.checked });
});
document.getElementById('masterEnable')?.addEventListener('change', e => {
  sendMsg({ type: 'setMaster', enabled: e.target.checked });
});

// ── Calibrate button ─────────────────────────────────────────────
document.getElementById('calibrateBtn')?.addEventListener('click', () => {
  sendMsg({ type: 'calibrate' });
  // Phase 3 will open a guided calibration panel
});
