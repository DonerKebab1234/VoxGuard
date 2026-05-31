/**
 * VoxGuard — app.js  (Phase 0–3)
 *
 * Bridge message protocol:
 *   JS → C++:  ping | getDevices | startCapture {deviceIndex} | stopCapture
 *               setSidetone {enabled} | setSidetoneLevel {value:0-100}
 *               setDuck {enabled}     | setDuckAmount {value:0-80}
 *               setMaster {enabled}   | calibrate {step:1|2}
 *               setThresholds {normal,tooLoud}
 *   C++ → JS:  pong | deviceList {devices} | captureStarted {ok} | captureStopped
 *               sidetoneSet {ok}
 *               meter {dbfs}
 *               calibStarted {ok} | calibProgress {step,progress,dbfs,ticksLeft}
 *               calibDone {step,dbfs[,normalDbfs,tooLoudDbfs]}
 *               loudnessState {state:"normal"|"elevated"|"tooLoud", avg, sustained, sustainThresh}
 */

'use strict';

// ── Meter setup ──────────────────────────────────────────────────
const SEGS      = 40;
const DB_MIN    = -60;
const DB_MAX    = 0;
const DB_RANGE  = DB_MAX - DB_MIN;

const YELLOW_AT = Math.round(((-12 - DB_MIN) / DB_RANGE) * SEGS); // 32
const RED_AT    = Math.round(((-6  - DB_MIN) / DB_RANGE) * SEGS); // 36

const meterBar = document.getElementById('meterBar');
const segments = [];
for (let i = 0; i < SEGS; i++) {
  const el = document.createElement('div');
  el.className = `seg ${i >= RED_AT ? 'red' : i >= YELLOW_AT ? 'yellow' : 'green'} unlit`;
  meterBar.appendChild(el);
  segments.push(el);
}

// ── Peak hold ────────────────────────────────────────────────────
const peakEl       = document.getElementById('meterPeak');
let   peakSeg      = 0;
let   peakTimeout  = null;
const PEAK_HOLD_MS = 1600;
const BAR_H        = 300;
const SEG_H        = (BAR_H - 6 - (SEGS - 1) * 2) / SEGS;

function placePeak(segIdx) {
  const fromBottom = segIdx * (SEG_H + 2) + 3;
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
  const level    = Math.max(0, Math.min(1, (dbfs - DB_MIN) / DB_RANGE));
  const litCount = Math.round(level * SEGS);

  for (let i = 0; i < SEGS; i++) {
    const on = i < litCount;
    segments[i].classList.toggle('lit',   on);
    segments[i].classList.toggle('unlit', !on);
  }
  updatePeak(litCount);

  readoutEl.textContent = dbfs <= DB_MIN + 0.5 ? '– –' : dbfs.toFixed(1);
  readoutEl.className   = 'readout-value';
  if      (dbfs > -6)  readoutEl.classList.add('zone-red');
  else if (dbfs > -12) readoutEl.classList.add('zone-yellow');
  else                 readoutEl.classList.add('zone-green');
}

// ── Zone legend — update from calibrated thresholds ──────────────
let calNormal  = -30; // updated once calibration completes
let calTooLoud = -15;

function updateZoneLegend(normalDbfs, tooLoudDbfs) {
  calNormal  = normalDbfs;
  calTooLoud = tooLoudDbfs;
  document.getElementById('zoneGreenRange').textContent  = `below ${normalDbfs.toFixed(0)}`;
  document.getElementById('zoneYellowRange').textContent = `${normalDbfs.toFixed(0)} to ${tooLoudDbfs.toFixed(0)}`;
  document.getElementById('zoneRedRange').textContent    = `above ${tooLoudDbfs.toFixed(0)}`;
}

// ── Loudness badge ────────────────────────────────────────────────
const badge      = document.getElementById('loudnessBadge');
const badgeLabel = document.getElementById('loudnessBadgeLabel');
const STATE_LABELS = { normal: 'Normal', elevated: 'Elevated', tooLoud: 'Too Loud' };

function updateLoudnessState(state) {
  badge.classList.remove('hidden', 'state-normal', 'state-elevated', 'state-tooLoud');
  badge.classList.add(`state-${state}`);
  badgeLabel.textContent = STATE_LABELS[state] ?? state;
}

// ── Session stats ─────────────────────────────────────────────────
let sessionBreaches  = 0;
let sessionStartTime = null;
let sessionTimerID   = null;

function startSessionTimer() {
  sessionStartTime = Date.now();
  sessionTimerID   = setInterval(() => {
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

// ── Calibration panel logic ───────────────────────────────────────
const calibPanel      = document.getElementById('calibPanel');
const calibTitle      = document.getElementById('calibTitle');
const calibDesc       = document.getElementById('calibDesc');
const calibCountdown  = document.getElementById('calibCountdown');
const calibLive       = document.getElementById('calibLive');
const calibArc        = document.getElementById('calibRingArc');
const calibResult     = document.getElementById('calibResult');
const calibResultVal  = document.getElementById('calibResultValue');
const calibResultLbl  = document.getElementById('calibResultLabel');
const calibSummary    = document.getElementById('calibSummary');
const calibActionBtn  = document.getElementById('calibActionBtn');
const calibCancelBtn  = document.getElementById('calibCancelBtn');
const calibDot1       = document.getElementById('calibDot1');
const calibDot2       = document.getElementById('calibDot2');
const calibStepLine   = document.getElementById('calibStepLine');
const summaryNormal   = document.getElementById('summaryNormal');
const summaryTooLoud  = document.getElementById('summaryTooLoud');

const RING_CIRC = 314.159; // 2π × 50

let calibStep      = 0;   // 0=closed, 1=step1, 2=step2
let calibMeasuring = false;
let step1Result    = null;

const STEP_CONFIG = {
  1: {
    title: 'Normal Voice',
    desc:  'Speak at your normal gaming volume for 5 seconds. Keep a steady, conversational tone.',
    resultLabel: 'Normal voice',
    arcClass: '',
  },
  2: {
    title: 'Loudest Acceptable',
    desc:  'Speak as loud as you\'re OK with sounding to friends. This sets the "too loud" threshold.',
    resultLabel: 'Max level',
    arcClass: 'zone-yellow',
  },
};

function showCalibPanel(step) {
  calibStep      = step;
  calibMeasuring = false;
  calibResult.classList.add('hidden');
  calibSummary.classList.add('hidden');

  const cfg = STEP_CONFIG[step];
  calibTitle.textContent       = cfg.title;
  calibDesc.textContent        = cfg.desc;
  calibArc.style.strokeDashoffset = String(RING_CIRC);
  calibArc.setAttribute('class', 'ring-arc ' + (cfg.arcClass || ''));
  calibCountdown.textContent   = '5.0';
  calibLive.textContent        = '–';
  calibActionBtn.textContent   = 'Start Measurement';
  calibActionBtn.disabled      = false;

  // Step dots
  calibDot1.className = 'calib-step-dot' + (step === 1 ? ' active' : ' done');
  calibDot2.className = 'calib-step-dot' + (step === 2 ? ' active' : '');
  calibStepLine.className = 'calib-step-line' + (step === 2 ? ' done' : '');

  calibPanel.classList.remove('hidden');
}

function closeCalibPanel() {
  calibPanel.classList.add('hidden');
  calibStep      = 0;
  calibMeasuring = false;
}

// Called by C++ → calibProgress messages
function onCalibProgress(msg) {
  if (!calibMeasuring) return;
  const remaining = 5.0 * (1 - msg.progress);
  calibCountdown.textContent = remaining.toFixed(1);
  calibLive.textContent      = msg.dbfs.toFixed(1) + ' dBFS';
  calibArc.style.strokeDashoffset = String(RING_CIRC * (1 - msg.progress));
}

// Called by C++ → calibDone messages
function onCalibDone(msg) {
  calibMeasuring = false;
  calibArc.style.strokeDashoffset = '0';
  calibCountdown.textContent = '✓';
  calibLive.textContent      = '';

  const cfg = STEP_CONFIG[calibStep];
  calibResultLbl.textContent  = cfg.resultLabel;
  calibResultVal.textContent  = msg.dbfs.toFixed(1);
  calibResult.classList.remove('hidden');

  if (msg.step === 1) {
    step1Result = msg.dbfs;
    calibActionBtn.textContent = 'Measure Loud Voice →';
    calibActionBtn.disabled    = false;
  } else {
    // Both steps done
    summaryNormal.textContent  = msg.normalDbfs.toFixed(1);
    summaryTooLoud.textContent = msg.tooLoudDbfs.toFixed(1);
    calibSummary.classList.remove('hidden');
    calibResult.classList.add('hidden');
    updateZoneLegend(msg.normalDbfs, msg.tooLoudDbfs);
    calibActionBtn.textContent = 'Done';
    calibActionBtn.disabled    = false;
  }
}

calibActionBtn.addEventListener('click', () => {
  if (!calibMeasuring && calibStep > 0) {
    const label = calibActionBtn.textContent;

    if (label === 'Done') {
      closeCalibPanel();
      return;
    }

    if (label === 'Measure Loud Voice →') {
      showCalibPanel(2);
      return;
    }

    // Start the measurement
    calibMeasuring = true;
    calibActionBtn.disabled    = true;
    calibActionBtn.textContent = 'Measuring…';

    if (bridge) {
      sendMsg({ type: 'calibrate', step: calibStep });
    } else {
      // Preview mode: run fake calibration inline
      window._fakeCalib?.(calibStep);
    }
  }
});

calibCancelBtn.addEventListener('click', closeCalibPanel);

document.getElementById('calibrateBtn').addEventListener('click', () => {
  if (!bridge && !window.chrome?.webview) {
    // Browser preview mode — run a fake calibration for visual testing
    step1Result = null;
    showCalibPanel(1);
    return;
  }
  showCalibPanel(1);
});

// ── Bridge ────────────────────────────────────────────────────────
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

// ── Handle messages from C++ ──────────────────────────────────────
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

    case 'captureStopped':
      setStatus('active', 'Ready');
      break;

    case 'meter':
      renderMeter(Number(msg.dbfs));
      break;

    case 'calibStarted':
      // C++ confirmed it started — no action needed, progress ticks will follow
      break;

    case 'calibProgress':
      onCalibProgress(msg);
      break;

    case 'calibDone':
      onCalibDone(msg);
      break;

    case 'loudnessState':
      updateLoudnessState(msg.state);
      if (msg.state === 'tooLoud') recordBreach();
      break;
  }
}

// ── Bridge wiring ─────────────────────────────────────────────────
if (bridge) {
  setStatus('connecting', 'Connecting…');
  bridge.addEventListener('message', e => onCppMessage(e.data));
  sendMsg({ type: 'ping' });
  sendMsg({ type: 'getDevices' });
} else {
  // Browser preview mode — fake moving meter + fake calibration
  setStatus('', 'Preview');
  let fake = -40;
  setInterval(() => {
    fake += (Math.random() - 0.44) * 5;
    fake  = Math.max(-60, Math.min(0, fake));
    renderMeter(fake);
  }, 55);

  // Simulate calibration flow for visual testing
  window._fakeCalib = (step) => {
    const fakeMeasure = () => {
      let prog = 0;
      const iv = setInterval(() => {
        prog += 0.05;
        if (prog >= 1) { clearInterval(iv); prog = 1; }
        onCalibProgress({ step, progress: Math.min(prog, 1), dbfs: fake });
        if (prog >= 1) {
          const result = step === 1
            ? { step: 1, dbfs: -28.5 }
            : { step: 2, dbfs: -14.2, normalDbfs: -28.5, tooLoudDbfs: -14.2 };
          onCalibDone(result);
        }
      }, 100);
    };
    // Wait for "Start Measurement" click (already bound above), just auto-trigger after 300ms
    setTimeout(fakeMeasure, 300);
  };

  // _fakeCalib is called by calibActionBtn's click handler (defined above) in preview mode
}

// ── Start / Stop button ───────────────────────────────────────────
let capturing = false;
startBtn.addEventListener('click', () => {
  if (!bridge) return;
  if (!capturing) {
    capturing = true;
    sendMsg({ type: 'startCapture', deviceIndex: parseInt(deviceSel.value, 10) });
  } else {
    capturing = false;
    startBtn.textContent = 'Start';
    startBtn.classList.remove('capturing');
    clearInterval(sessionTimerID);
    sendMsg({ type: 'stopCapture' });
  }
});

// ── Sliders ───────────────────────────────────────────────────────
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

// ── Toggles ───────────────────────────────────────────────────────
document.getElementById('sidetoneEnable')?.addEventListener('change', e => {
  sendMsg({ type: 'setSidetone', enabled: e.target.checked });
});
document.getElementById('duckEnable')?.addEventListener('change', e => {
  sendMsg({ type: 'setDuck', enabled: e.target.checked });
});
document.getElementById('masterEnable')?.addEventListener('change', e => {
  sendMsg({ type: 'setMaster', enabled: e.target.checked });
});
