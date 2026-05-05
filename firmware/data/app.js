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
      if      (msg.type === 'status')         handleStatus(msg);
      else if (msg.type === 'log')            appendLog(msg);
      else if (msg.type === 'log_clear')      clearLogTable();
      else if (msg.type === 'replay_window')  handleReplayWindow(msg);
    } catch (_) {}
  };
}

// ── Status update ────────────────────────────────────────────────────────
let wsInitialized = false;

const MODE_NAMES = ['Manual', 'Live', 'Replay'];

/** 87 s = LIVE_FETCH_INTERVAL_S on the firmware side. */
const LIVE_FETCH_INTERVAL_S = 87;

// Enable/disable the manual-values section based on sensor mode.
function updateManualSection(sensor, mode) {
  const div = document.getElementById(sensor + '-manual');
  if (!div) return;
  const disabled = (mode !== 0);
  div.classList.toggle('manual-disabled', disabled);
  div.querySelectorAll('input, button').forEach(el => { el.disabled = disabled; });
}

// Update/hide the live-fetch countdown bar and info line.
function updateLiveFetchBar(age_s, any_live, fetch_ok, lat, lon) {
  const row  = document.getElementById('live-fetch-bar-row');
  const fill = document.getElementById('live-fetch-fill');
  const rem  = document.getElementById('live-fetch-remaining');
  const info = document.getElementById('live-fetch-info');
  if (!row) return;
  if (!any_live) {
    row.style.display  = 'none';
    if (info) info.style.display = 'none';
    return;
  }
  row.style.display = 'flex';
  if (info) info.style.display = 'block';

  if (age_s < 0) {
    // Never fetched yet — show empty bar, label 'waiting'
    if (fill) fill.style.width = '0%';
    if (rem)  rem.textContent  = '...';
    if (info) info.textContent = 'Waiting for first fetch…';
  } else {
    const pct = Math.max(0, (1 - age_s / LIVE_FETCH_INTERVAL_S)) * 100;
    if (fill) fill.style.width = pct.toFixed(1) + '%';
    const secs_left = Math.max(0, LIVE_FETCH_INTERVAL_S - age_s);
    if (rem)  rem.textContent  = secs_left + 's';

    if (info) {
      const lastTime  = new Date(Date.now() - age_s * 1000);
      const timeStr   = lastTime.toLocaleTimeString();
      const statusMark = (fetch_ok === null) ? '' : (fetch_ok ? ' ✓' : ' ✗ failed');
      let html = 'Last fetch: ' + timeStr + statusMark;
      if (lat !== null && lon !== null) {
        const url = 'https://api.open-meteo.com/v1/forecast'
          + '?latitude='  + lat.toFixed(4)
          + '&longitude=' + lon.toFixed(4)
          + '&current=temperature_2m,relative_humidity_2m'
          + ',wind_speed_10m,wind_direction_10m&wind_speed_unit=ms';
        html += ' &nbsp;·&nbsp; <a href="' + url + '" target="_blank" rel="noopener noreferrer">Verify on Open-Meteo ↗</a>';
      }
      info.innerHTML = html;
    }
  }
}

function handleStatus(s) {
  let any_live = false;
  if (s.fg) {
    setText('st-fg-temp', s.fg.temp !== undefined ? s.fg.temp.toFixed(1) : '—');
    setText('st-fg-hum',  s.fg.hum  !== undefined ? s.fg.hum.toFixed(0)  : '—');
    if (s.fg.mode !== undefined) {
      setText('st-fg-mode', MODE_NAMES[s.fg.mode] || '—');
      const r = document.querySelector('input[name="fg-mode"][value="' + s.fg.mode + '"]');
      if (r) r.checked = true;
      updateManualSection('fg', s.fg.mode);
      if (s.fg.mode !== 0) any_live = true;
    }
    // Always update slider/input in Live or Replay so the user can see the
    // current served value; in Manual only sync on first connect.
    const fg_mode = (s.fg.mode !== undefined) ? s.fg.mode : 0;
    if (fg_mode !== 0 || !wsInitialized) {
      if (s.fg.temp !== undefined) setSliderInput('fg-temp-sl', 'fg-temp-in', s.fg.temp);
      if (s.fg.hum  !== undefined) setSliderInput('fg-hum-sl',  'fg-hum-in',  s.fg.hum);
    }
  }
  if (s.s200) {
    setText('st-s200-spd', s.s200.spd !== undefined ? s.s200.spd.toFixed(0) : '—');
    setText('st-s200-dir', s.s200.dir !== undefined ? s.s200.dir.toFixed(0) : '—');
    if (s.s200.mode !== undefined) {
      setText('st-s200-mode', MODE_NAMES[s.s200.mode] || '—');
      const r = document.querySelector('input[name="s200-mode"][value="' + s.s200.mode + '"]');
      if (r) r.checked = true;
      updateManualSection('s200', s.s200.mode);
      if (s.s200.mode !== 0) any_live = true;
    }
    const s200_mode = (s.s200.mode !== undefined) ? s.s200.mode : 0;
    if (s200_mode !== 0 || !wsInitialized) {
      if (s.s200.spd  !== undefined) setSliderInput('s200-spd-sl',  's200-spd-in',  s.s200.spd);
      if (s.s200.dir  !== undefined) setSliderInput('s200-dir-sl',  's200-dir-in',  s.s200.dir);
      if (s.s200.heat !== undefined) setSliderInput('s200-heat-sl', 's200-heat-in', s.s200.heat);
    }
  }

  // Live-fetch progress bar.
  updateLiveFetchBar(
    (s.live_fetch_age !== undefined) ? s.live_fetch_age : -1,
    any_live,
    (s.live_fetch_ok  !== undefined) ? !!s.live_fetch_ok : null,
    (s.live && s.live.lat !== undefined) ? s.live.lat : null,
    (s.live && s.live.lon !== undefined) ? s.live.lon : null
  );

  // Replay status.
  if (s.replay) {
    handleReplayStatus(s.replay);
  }

  // On first message: sync all editable controls to the device's current state.
  if (!wsInitialized) {
    wsInitialized = true;
    if (s.fg) {
      if (s.fg.addr !== undefined) {
        const el = document.getElementById('fg-addr');
        if (el) el.value = s.fg.addr;
      }
    }
    if (s.s200) {
      if (s.s200.addr !== undefined) {
        const el = document.getElementById('s200-addr');
        if (el) el.value = s.s200.addr;
      }
    }
    if (s.wifi) {
      const ssidEl = document.getElementById('wifi-ssid');
      if (ssidEl && s.wifi.ssid) ssidEl.value = s.wifi.ssid;
    }
    if (s.live) {
      const latEl = document.getElementById('live-lat');
      const lonEl = document.getElementById('live-lon');
      if (latEl && s.live.lat !== undefined) latEl.value = s.live.lat.toFixed(4);
      if (lonEl && s.live.lon !== undefined) lonEl.value = s.live.lon.toFixed(4);
    }
  }
  if (s.wifi) {
    setText('st-wifi-mode', s.wifi.mode || '—');
    setText('st-wifi-ip',   s.wifi.ip   || '—');
    setText('st-wifi-rssi', s.wifi.rssi !== undefined ? s.wifi.rssi : '—');
  }
  setText('st-time', s.time ? s.time.replace('T', ' ') : '—');
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
function addrConflict() {
  const a = parseInt(document.getElementById('fg-addr').value,   10);
  const b = parseInt(document.getElementById('s200-addr').value, 10);
  return !isNaN(a) && !isNaN(b) && a === b;
}
function onAddrInput() {
  if (!addrConflict()) {
    document.getElementById('fg-addr-err').textContent   = '';
    document.getElementById('s200-addr-err').textContent = '';
  }
}
function postFgAddr() {
  if (addrConflict()) {
    document.getElementById('fg-addr-err').textContent = 'Address already used by S200';
    return;
  }
  const addr = parseInt(document.getElementById('fg-addr').value, 10);
  post('/config/sensor', { sensor: 'fg6485a', addr });
}

function postFgMode(mode) {
  mode = parseInt(mode, 10);
  post('/config/sensor', { sensor: 'fg6485a', mode }).then(r => {
    if (!r) return;
    updateManualSection('fg', mode);
  });
}

function postFgManual() {
  const temp = parseFloat(document.getElementById('fg-temp-in').value);
  const hum  = parseInt(document.getElementById('fg-hum-in').value, 10);
  // Applying manual values always switches the mode to Manual.
  post('/config/sensor', { sensor: 'fg6485a', mode: 0, temp, hum }).then(r => {
    if (!r) return;
    const r0 = document.querySelector('input[name="fg-mode"][value="0"]');
    if (r0) r0.checked = true;
    if (r.temp !== undefined) setSliderInput('fg-temp-sl', 'fg-temp-in', r.temp);
    if (r.hum  !== undefined) setSliderInput('fg-hum-sl',  'fg-hum-in',  r.hum);
  });
}

// ── S200 ─────────────────────────────────────────────────────────────────
function postS200Addr() {
  if (addrConflict()) {
    document.getElementById('s200-addr-err').textContent = 'Address already used by FG6485A';
    return;
  }
  const addr = parseInt(document.getElementById('s200-addr').value, 10);
  post('/config/sensor', { sensor: 's200', addr });
}

function postS200Mode(mode) {
  mode = parseInt(mode, 10);
  post('/config/sensor', { sensor: 's200', mode }).then(r => {
    if (!r) return;
    updateManualSection('s200', mode);
  });
}

function postS200Manual() {
  const spd  = parseInt(document.getElementById('s200-spd-in').value, 10);
  const dir  = parseInt(document.getElementById('s200-dir-in').value, 10);
  const heat = parseFloat(document.getElementById('s200-heat-in').value);
  // Applying manual values always switches the mode to Manual.
  post('/config/sensor', { sensor: 's200', mode: 0, spd, dir, heat }).then(r => {
    if (!r) return;
    const r0 = document.querySelector('input[name="s200-mode"][value="0"]');
    if (r0) r0.checked = true;
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

function postTz() {
  const tz = document.getElementById('tz-posix').value.trim();
  post('/config/tz', { tz }).then(r => {
    if (r && r.tz) document.getElementById('tz-posix').value = r.tz;
  });
}

// ── Location ────────────────────────────────────────────────────────────────────
function postLocation() {
  const lat = parseFloat(document.getElementById('live-lat').value);
  const lon = parseFloat(document.getElementById('live-lon').value);
  if (isNaN(lat) || isNaN(lon)) return;
  post('/config/location', { lat, lon }).then(r => {
    if (!r) return;
    if (r.lat !== undefined) document.getElementById('live-lat').value = r.lat.toFixed(4);
    if (r.lon !== undefined) document.getElementById('live-lon').value = r.lon.toFixed(4);
  });
}

// ── Replay transport ─────────────────────────────────────────────────────
let _replayState = 'idle';

function fmtElapsed(sec) {
  const h = Math.floor(sec / 3600);
  const m = Math.floor((sec % 3600) / 60);
  const s = sec % 60;
  return String(h).padStart(2,'0') + ':' + String(m).padStart(2,'0') + ':' + String(s).padStart(2,'0');
}

function handleReplayStatus(r) {
  _replayState = r.state || 'idle';
  const stateMap = { idle: 'Idle', running: 'Running', paused: 'Paused',
                     done: 'Done (EOF)', error: 'Error' };
  const stEl = document.getElementById('replay-status');
  if (stEl) {
    let txt = 'Replay: ' + (stateMap[_replayState] || _replayState);
    if (r.row_count > 0 && r.row >= 0) txt += '  \u2014  row ' + r.row + ' / ' + r.row_count;
    stEl.textContent = txt;
  }
  const elEl = document.getElementById('replay-elapsed');
  if (elEl) {
    elEl.textContent = (_replayState !== 'idle') ? 'Elapsed: ' + fmtElapsed(r.elapsed_s || 0) : '';
  }
  const win = document.getElementById('replay-window');
  if (win) win.style.display = (_replayState !== 'idle') ? 'block' : 'none';

  const btnStart = document.getElementById('btn-replay-start');
  const btnPrev  = document.getElementById('btn-replay-prev');
  const btnPause = document.getElementById('btn-replay-pause');
  const btnNext  = document.getElementById('btn-replay-next');
  const btnStop  = document.getElementById('btn-replay-stop');
  const running  = (_replayState === 'running');
  const paused   = (_replayState === 'paused');
  const canStart = (_replayState === 'idle' || _replayState === 'done' || _replayState === 'error');
  if (btnStart) btnStart.disabled = !canStart;
  if (btnStop)  btnStop.disabled  = !(_replayState !== 'idle');
  if (btnPause) {
    btnPause.disabled  = !(running || paused);
    btnPause.innerHTML = paused ? '&#9654; Play' : '&#9646;&#9646; Pause';
  }
  const atFirst = ((r.row || 0) <= 0);
  const atLast  = (r.row_count > 0 && (r.row || 0) >= r.row_count - 1);
  if (btnPrev) btnPrev.disabled = !(paused && !atFirst);
  if (btnNext) btnNext.disabled = !(paused && !atLast);
}

function replayCmd(action) {
  fetch('/replay/control', {
    method:  'POST',
    headers: { 'Content-Type': 'application/json' },
    body:    JSON.stringify({ action }),
  }).then(r => r.ok ? r.json() : null).then(r => {
    if (r) handleReplayStatus(r);
  }).catch(() => {});
}

function replayTogglePause() {
  replayCmd(_replayState === 'paused' ? 'play' : 'pause');
}

function handleReplayWindow(msg) {
  function fmtVal(v) { return (v === null || v === undefined) ? '\u2014' : v.toFixed(1); }
  function fillRow(trId, entry, marker) {
    const tr = document.getElementById(trId);
    if (!tr) return;
    if (!entry) { tr.innerHTML = '<td colspan="7">\u2014</td>'; return; }
    tr.innerHTML =
      '<td>' + (entry.ts || '\u2014') + '</td>' +
      '<td>' + (marker ? '&#9658;' : '') + '</td>' +
      '<td>' + fmtVal(entry.fg_temp)   + '</td>' +
      '<td>' + fmtVal(entry.fg_hum)    + '</td>' +
      '<td>' + fmtVal(entry.s200_spd)  + '</td>' +
      '<td>' + fmtVal(entry.s200_dir)  + '</td>' +
      '<td>' + fmtVal(entry.s200_heat) + '</td>';
  }
  fillRow('replay-win-prev', msg.prev, false);
  fillRow('replay-win-curr', msg.curr, true);
  fillRow('replay-win-next', msg.next, false);
}

function uploadReplayFile() {
  const input    = document.getElementById('replay-file');
  const statusEl = document.getElementById('replay-upload-status');
  if (!input || !input.files || !input.files[0]) {
    if (statusEl) statusEl.textContent = 'No file selected.';
    return;
  }
  const file = input.files[0];
  if (statusEl) statusEl.textContent = 'Uploading\u2026';
  fetch('/replay/upload', {
    method:  'POST',
    headers: { 'Content-Type': 'text/csv' },
    body:    file,
  }).then(r => r.ok ? r.json() : null)
    .then(r => {
      if (statusEl) statusEl.textContent =
        r ? ('Uploaded ' + r.size + ' bytes.') : 'Upload failed.';
    })
    .catch(() => { if (statusEl) statusEl.textContent = 'Upload error.'; });
}

// ── Modbus log ───────────────────────────────────────────────────────────
const LOG_MAX = 30;

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
