'use strict';

// ── WebSocket connection ─────────────────────────────────────────────────
let ws = null;

function wsConnect() {
  const proto = location.protocol === 'https:' ? 'wss:' : 'ws:';
  ws = new WebSocket(proto + '//' + location.host + '/ws');

  ws.onopen = function () {
    setBadge('ws-badge', 'Online', 'online');
  };

  ws.onclose = function () {
    setBadge('ws-badge', 'Offline', 'offline');
    wsInitialized = false;
    setTimeout(wsConnect, 3000);
  };

  ws.onerror = function () { ws.close(); };

  ws.onmessage = function (evt) {
    try {
      const msg = JSON.parse(evt.data);
      if      (msg.type === 'status')    handleStatus(msg);
      else if (msg.type === 'log')       appendLog(msg);
      else if (msg.type === 'log_clear') clearLogTable();
    } catch (_) {}
  };
}

// ── Status update ────────────────────────────────────────────────────────
let wsInitialized = false;

function handleStatus(s) {
  if (s.fg) {
    setText('st-fg-temp', s.fg.temp !== undefined ? s.fg.temp.toFixed(1) : '—');
    setText('st-fg-hum',  s.fg.hum  !== undefined ? s.fg.hum.toFixed(0)  : '—');
  }
  if (s.s200) {
    setText('st-s200-spd', s.s200.spd !== undefined ? s.s200.spd.toFixed(0) : '—');
    setText('st-s200-dir', s.s200.dir !== undefined ? s.s200.dir.toFixed(0) : '—');
  }

  // On first message: sync all editable controls to the device's current state.
  if (!wsInitialized) {
    wsInitialized = true;
    if (s.fg) {
      if (s.fg.addr !== undefined) {
        const el = document.getElementById('fg-addr');
        if (el) el.value = s.fg.addr;
      }
      if (s.fg.mode !== undefined) {
        const r = document.querySelector('input[name="fg-mode"][value="' + s.fg.mode + '"]');
        if (r) r.checked = true;
      }
      if (s.fg.temp !== undefined) setSliderInput('fg-temp-sl', 'fg-temp-in', s.fg.temp);
      if (s.fg.hum  !== undefined) setSliderInput('fg-hum-sl',  'fg-hum-in',  s.fg.hum);
    }
    if (s.s200) {
      if (s.s200.addr !== undefined) {
        const el = document.getElementById('s200-addr');
        if (el) el.value = s.s200.addr;
      }
      if (s.s200.mode !== undefined) {
        const r = document.querySelector('input[name="s200-mode"][value="' + s.s200.mode + '"]');
        if (r) r.checked = true;
      }
      if (s.s200.spd  !== undefined) setSliderInput('s200-spd-sl',  's200-spd-in',  s.s200.spd);
      if (s.s200.dir  !== undefined) setSliderInput('s200-dir-sl',  's200-dir-in',  s.s200.dir);
      if (s.s200.heat !== undefined) setSliderInput('s200-heat-sl', 's200-heat-in', s.s200.heat);
    }
  }
  if (s.wifi) {
    setText('st-wifi-mode', s.wifi.mode || '—');
    setText('st-wifi-ip',   s.wifi.ip   || '—');
    setText('st-wifi-rssi', s.wifi.rssi !== undefined ? s.wifi.rssi : '—');
  }
  setText('st-time', s.time || '—');
  const ntp = document.getElementById('st-ntp');
  if (ntp) {
    if (s.ntp_synced) {
      ntp.textContent = 'NTP synced';
      ntp.className   = 'badge ntp-on';
    } else {
      ntp.textContent = 'NTP pending';
      ntp.className   = 'badge ntp-off';
    }
  }
}

// ── Slider ↔ number-input sync ───────────────────────────────────────────
function linkSlider(sliderId, inputId) {
  const sl  = document.getElementById(sliderId);
  const inp = document.getElementById(inputId);
  if (!sl || !inp) return;
  sl.addEventListener('input',  () => { inp.value = sl.value;  });
  inp.addEventListener('input', () => { sl.value  = inp.value; });
}

linkSlider('fg-temp-sl',  'fg-temp-in');
linkSlider('fg-hum-sl',   'fg-hum-in');
linkSlider('s200-spd-sl', 's200-spd-in');
linkSlider('s200-dir-sl', 's200-dir-in');
linkSlider('s200-heat-sl','s200-heat-in');

// ── After Apply: update both slider and input with the clamped value ──────
function setSliderInput(sliderId, inputId, value) {
  const sl  = document.getElementById(sliderId);
  const inp = document.getElementById(inputId);
  if (sl)  sl.value  = value;
  if (inp) inp.value = value;
}

// ── HTTP POST helper ─────────────────────────────────────────────────────
function post(url, body) {
  return fetch(url, {
    method:  'POST',
    headers: { 'Content-Type': 'application/json' },
    body:    JSON.stringify(body),
  }).then(r => r.ok ? r.json() : null).catch(() => null);
}

// ── FG6485A ──────────────────────────────────────────────────────────────
function postFgAddr() {
  const addr = parseInt(document.getElementById('fg-addr').value, 10);
  post('/config/sensor', { sensor: 'fg6485a', addr });
}

function postFgManual() {
  const mode = Number(document.querySelector('input[name="fg-mode"]:checked').value);
  const temp = parseFloat(document.getElementById('fg-temp-in').value);
  const hum  = parseInt(document.getElementById('fg-hum-in').value, 10);
  post('/config/sensor', { sensor: 'fg6485a', mode, temp, hum }).then(r => {
    if (!r) return;
    if (r.temp !== undefined) setSliderInput('fg-temp-sl', 'fg-temp-in', r.temp);
    if (r.hum  !== undefined) setSliderInput('fg-hum-sl',  'fg-hum-in',  r.hum);
  });
}

// ── S200 ─────────────────────────────────────────────────────────────────
function postS200Addr() {
  const addr = parseInt(document.getElementById('s200-addr').value, 10);
  post('/config/sensor', { sensor: 's200', addr });
}

function postS200Manual() {
  const mode = Number(document.querySelector('input[name="s200-mode"]:checked').value);
  const spd  = parseInt(document.getElementById('s200-spd-in').value, 10);
  const dir  = parseInt(document.getElementById('s200-dir-in').value, 10);
  const heat = parseFloat(document.getElementById('s200-heat-in').value);
  post('/config/sensor', { sensor: 's200', mode, spd, dir, heat }).then(r => {
    if (!r) return;
    if (r.spd  !== undefined) setSliderInput('s200-spd-sl',  's200-spd-in',  r.spd);
    if (r.dir  !== undefined) setSliderInput('s200-dir-sl',  's200-dir-in',  r.dir);
    if (r.heat !== undefined) setSliderInput('s200-heat-sl', 's200-heat-in', r.heat);
  });
}

// ── WiFi ─────────────────────────────────────────────────────────────────
function postWifi() {
  const ssid = document.getElementById('wifi-ssid').value;
  const pass = document.getElementById('wifi-pass').value;
  post('/config/wifi', { ssid, pass });
}

// ── NTP ──────────────────────────────────────────────────────────────────
function postNtp() {
  const server = document.getElementById('ntp-server').value;
  post('/config/ntp', { server });
}

// ── Manual time ──────────────────────────────────────────────────────────
function postTime() {
  const val = document.getElementById('manual-time').value;
  if (val) post('/config/time', { time: val });
}

// ── Modbus log ───────────────────────────────────────────────────────────
const LOG_MAX = 200;

function appendLog(entry) {
  const tbody = document.getElementById('log-body');
  if (!tbody) return;
  const tr = document.createElement('tr');
  tr.innerHTML =
    '<td>' + esc(entry.ts      || '') + '</td>' +
    '<td>' + esc(entry.dir     || '') + '</td>' +
    '<td><code>' + esc(entry.hex || '') + '</code></td>' +
    '<td>' + esc(entry.summary || '') + '</td>';
  tbody.insertBefore(tr, tbody.firstChild);
  while (tbody.rows.length > LOG_MAX) tbody.deleteRow(tbody.rows.length - 1);
}

function clearLogTable() {
  const tbody = document.getElementById('log-body');
  if (tbody) tbody.innerHTML = '';
}

function postLogClear() {
  post('/log/clear', {});
}

// ── Utilities ────────────────────────────────────────────────────────────
function setText(id, val) {
  const el = document.getElementById(id);
  if (el) el.textContent = val;
}

function setBadge(id, text, cls) {
  const el = document.getElementById(id);
  if (!el) return;
  el.textContent = text;
  el.className   = 'badge ' + cls;
}

// Minimal XSS-safe text escaping for log table content.
function esc(str) {
  return String(str)
    .replace(/&/g, '&amp;')
    .replace(/</g, '&lt;')
    .replace(/>/g, '&gt;');
}

// ── Resizable log columns ────────────────────────────────────────────────
function initResizableCols() {
  const table = document.getElementById('log-tbl');
  if (!table) return;

  // Fixed layout so explicit widths are respected.
  table.style.tableLayout = 'fixed';

  // Fallback initial widths (px) when offsetWidth is unavailable (e.g. hidden table).
  const defaults = [70, 45, 300, 200];
  const ths = table.querySelectorAll('thead th');

  // Columns 0 (Time) and 1 (Dir) are fixed-width via CSS; only make the rest resizable.
  const FIXED_COLS = 2;

  ths.forEach(function (th, i) {
    th.style.width = (th.offsetWidth || defaults[i]) + 'px';
    if (i < FIXED_COLS) return;   // skip resizer for fixed columns

    const handle = document.createElement('div');
    handle.className = 'col-resizer';
    th.appendChild(handle);

    var startX, startW;

    handle.addEventListener('mousedown', function (e) {
      startX = e.clientX;
      startW = th.offsetWidth;
      handle.classList.add('dragging');
      e.preventDefault();

      function onMove(e) {
        th.style.width = Math.max(40, startW + e.clientX - startX) + 'px';
      }
      function onUp() {
        handle.classList.remove('dragging');
        document.removeEventListener('mousemove', onMove);
        document.removeEventListener('mouseup',   onUp);
      }
      document.addEventListener('mousemove', onMove);
      document.addEventListener('mouseup',   onUp);
    });
  });
}

// ── Boot ─────────────────────────────────────────────────────────────────
wsConnect();
initResizableCols();
