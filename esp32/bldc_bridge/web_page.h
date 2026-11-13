#pragma once
#include <pgmspace.h>

static const char WEB_PAGE[] PROGMEM = R"rawhtml(
<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>BLDC-FOC Control</title>
<style>
  *{box-sizing:border-box;margin:0;padding:0}
  body{font-family:'Segoe UI',system-ui,sans-serif;background:#0f0f13;color:#e0e0e8;min-height:100vh;padding:20px}
  h1{font-size:1.4rem;font-weight:600;color:#7eb8f7;margin-bottom:4px}
  .sub{font-size:.75rem;color:#666;margin-bottom:20px}
  .grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(220px,1fr));gap:12px;margin-bottom:16px}
  .card{background:#1a1a24;border:1px solid #2a2a38;border-radius:10px;padding:16px}
  .card h2{font-size:.7rem;text-transform:uppercase;letter-spacing:.08em;color:#666;margin-bottom:8px}
  .val{font-size:2rem;font-weight:700;color:#e8e8f0;line-height:1}
  .unit{font-size:.8rem;color:#555;margin-top:2px}
  .badge{display:inline-block;padding:3px 10px;border-radius:12px;font-size:.7rem;font-weight:600;text-transform:uppercase;letter-spacing:.05em}
  .ok{background:#1a3a1a;color:#4ade80}.warn{background:#3a2a1a;color:#fb923c}.err{background:#3a1a1a;color:#f87171}
  .ctrl{background:#1a1a24;border:1px solid #2a2a38;border-radius:10px;padding:20px;margin-bottom:16px}
  .ctrl h2{font-size:.7rem;text-transform:uppercase;letter-spacing:.08em;color:#666;margin-bottom:14px}
  .row{display:flex;align-items:center;gap:14px;margin-bottom:12px}
  label{font-size:.8rem;color:#aaa;width:80px;flex-shrink:0}
  input[type=range]{flex:1;accent-color:#7eb8f7;height:4px}
  .num{font-size:.9rem;color:#e0e0e8;width:60px;text-align:right}
  button{padding:10px 24px;border:none;border-radius:8px;font-size:.85rem;font-weight:600;cursor:pointer;transition:opacity .15s}
  .btn-en{background:#2563eb;color:#fff}.btn-dis{background:#7f1d1d;color:#fff}.btn-estop{background:#991b1b;color:#fff;width:100%;font-size:1rem;padding:14px}
  button:hover{opacity:.85}
  .link-bar{display:flex;align-items:center;gap:10px;padding:10px 14px;background:#1a1a24;border:1px solid #2a2a38;border-radius:8px;margin-bottom:16px;font-size:.78rem}
  #log{background:#0d0d12;border:1px solid #222;border-radius:8px;padding:12px;height:120px;overflow-y:auto;font-family:monospace;font-size:.72rem;color:#88a}
  .bar-wrap{background:#111;border-radius:4px;height:8px;overflow:hidden;margin-top:6px}
  .bar{height:100%;border-radius:4px;transition:width .15s}
  .bar-id{background:#3b82f6}.bar-iq{background:#22c55e}.bar-ref{background:#a78bfa}
  #canvas{display:block;width:100%;height:120px;border-radius:8px;background:#0d0d12;margin-top:8px}
</style>
</head>
<body>
<h1>BLDC-FOC Motor Control</h1>
<p class="sub">STM32G431CB + DRV8316CT | ESP32 WiFi Bridge | 24 V / 4 A max</p>

<div class="link-bar">
  <span>Link</span>
  <span id="lnk" class="badge err">OFFLINE</span>
  <span style="margin-left:auto;color:#555" id="age">–</span>
  <span class="badge" id="fault_b" style="display:none">FAULT</span>
</div>

<div class="grid">
  <div class="card">
    <h2>Speed</h2>
    <div class="val" id="rpm_v">–</div>
    <div class="unit">RPM</div>
  </div>
  <div class="card">
    <h2>q-axis current (measured)</h2>
    <div class="val" id="iq_v">–</div>
    <div class="unit">A</div>
    <div class="bar-wrap"><div class="bar bar-iq" id="iq_bar" style="width:0%"></div></div>
  </div>
  <div class="card">
    <h2>q-axis current (reference)</h2>
    <div class="val" id="iqr_v">–</div>
    <div class="unit">A</div>
    <div class="bar-wrap"><div class="bar bar-ref" id="iqr_bar" style="width:0%"></div></div>
  </div>
  <div class="card">
    <h2>d-axis current</h2>
    <div class="val" id="id_v">–</div>
    <div class="unit">A</div>
    <div class="bar-wrap"><div class="bar bar-id" id="id_bar" style="width:0%"></div></div>
  </div>
  <div class="card">
    <h2>Encoder angle</h2>
    <div class="val" id="ang_v">–</div>
    <div class="unit">raw (0–4095)</div>
  </div>
  <div class="card">
    <h2>Uptime</h2>
    <div class="val" id="up_v">–</div>
    <div class="unit">seconds</div>
  </div>
</div>

<div class="ctrl">
  <h2>Control</h2>
  <div class="row">
    <label>iq ref (A)</label>
    <input type="range" id="iq_sl" min="-4000" max="4000" value="0" step="10">
    <span class="num" id="iq_disp">0.00 A</span>
  </div>
  <div class="row">
    <label>RPM limit</label>
    <input type="range" id="rpm_sl" min="0" max="800" value="400" step="10">
    <span class="num" id="rpm_disp">400</span>
  </div>
  <div class="row" style="margin-top:4px">
    <button class="btn-en" onclick="sendEnable()">ENABLE</button>
    <button class="btn-dis" onclick="sendDisable()">DISABLE</button>
    <button class="btn-en" onclick="clearFault()" style="background:#1e40af">CLEAR FAULT</button>
  </div>
  <button class="btn-estop" style="margin-top:12px" onclick="eStop()">&#9679; EMERGENCY STOP</button>
</div>

<div class="card" style="margin-bottom:16px">
  <h2>RPM Trend (last 60 s)</h2>
  <canvas id="canvas"></canvas>
</div>

<div class="card">
  <h2>Log</h2>
  <div id="log"></div>
</div>

<script>
const IQ_MAX = 4.0;
let enabled = false, clear_fault = false;
let last_hb = 0;
let rpmHistory = new Array(300).fill(null);

// Slider labels
document.getElementById('iq_sl').addEventListener('input', e => {
  document.getElementById('iq_disp').textContent = (e.target.value/1000).toFixed(2)+' A';
});
document.getElementById('rpm_sl').addEventListener('input', e => {
  document.getElementById('rpm_disp').textContent = e.target.value;
});

function sendCmd(en, cf) {
  const iq = parseInt(document.getElementById('iq_sl').value);
  const rpm = parseInt(document.getElementById('rpm_sl').value);
  fetch(`/api/set?en=${en?1:0}&cf=${cf?1:0}&iq=${iq}&id=0&rpm=${rpm}`)
    .then(r=>r.json()).then(d=>log('CMD: en='+en+' iq='+(iq/1000).toFixed(2)+'A'));
  last_hb = Date.now();
}

function sendEnable()  { enabled=true;  sendCmd(true, false); }
function sendDisable() { enabled=false; sendCmd(false,false); }
function clearFault()  { sendCmd(enabled, true); }
function eStop()       { enabled=false; document.getElementById('iq_sl').value=0;
                          document.getElementById('iq_disp').textContent='0.00 A';
                          fetch('/api/estop').then(()=>log('ESTOP sent')); }

// Heartbeat: keep sending the current setpoint every 500 ms so the STM32 watchdog doesn't fire
setInterval(()=>{
  if(enabled) sendCmd(true, false);
}, 500);

// Poll telemetry at 10 Hz
setInterval(async ()=>{
  try {
    const r = await fetch('/api/state');
    const d = await r.json();
    update(d);
  } catch(e) {
    setLinkBadge(false, 0);
  }
}, 100);

function update(d) {
  const age = d.link_age_ms;
  const alive = age < 1000;
  setLinkBadge(alive, age);

  document.getElementById('rpm_v').textContent  = d.rpm !== undefined ? d.rpm.toFixed(1) : '–';
  document.getElementById('iq_v').textContent   = d.iq_a !== undefined ? d.iq_a.toFixed(3) : '–';
  document.getElementById('iqr_v').textContent  = d.iq_ref_a !== undefined ? d.iq_ref_a.toFixed(3) : '–';
  document.getElementById('id_v').textContent   = d.id_a !== undefined ? d.id_a.toFixed(3) : '–';
  document.getElementById('ang_v').textContent  = d.angle_raw !== undefined ? d.angle_raw : '–';
  document.getElementById('up_v').textContent   = d.uptime_ms !== undefined ? (d.uptime_ms/1000).toFixed(1) : '–';

  bar('iq_bar',  Math.abs(d.iq_a||0) / IQ_MAX * 100);
  bar('iqr_bar', Math.abs(d.iq_ref_a||0) / IQ_MAX * 100);
  bar('id_bar',  Math.abs(d.id_a||0) / IQ_MAX * 100);

  const fb = document.getElementById('fault_b');
  if(d.flags & 2) { fb.style.display=''; fb.className='badge err'; fb.textContent='FAULT'; }
  else { fb.style.display='none'; }

  // RPM history chart
  rpmHistory.push(d.rpm || 0);
  if(rpmHistory.length > 300) rpmHistory.shift();
  drawChart();
}

function bar(id, pct) {
  document.getElementById(id).style.width = Math.min(100, Math.max(0, pct)) + '%';
}

function setLinkBadge(alive, age_ms) {
  const el = document.getElementById('lnk');
  el.className = 'badge ' + (alive ? 'ok' : 'err');
  el.textContent = alive ? 'ONLINE' : 'OFFLINE';
  document.getElementById('age').textContent = alive ? age_ms+'ms ago' : 'no link';
}

function drawChart() {
  const c = document.getElementById('canvas');
  const ctx = c.getContext('2d');
  c.width = c.offsetWidth; c.height = 120;
  ctx.clearRect(0,0,c.width,c.height);
  const max = 900, n = rpmHistory.length;
  ctx.strokeStyle='#3b82f6'; ctx.lineWidth=1.5; ctx.beginPath();
  for(let i=0;i<n;i++){
    const v=rpmHistory[i];
    if(v===null){ctx.moveTo((i/n)*c.width, c.height/2);continue;}
    const x=(i/n)*c.width, y=c.height-Math.abs(v)/max*c.height;
    i===0?ctx.moveTo(x,y):ctx.lineTo(x,y);
  }
  ctx.stroke();
  // Zero line
  ctx.strokeStyle='#333'; ctx.lineWidth=1; ctx.beginPath();
  ctx.moveTo(0,c.height/2); ctx.lineTo(c.width,c.height/2); ctx.stroke();
}

function log(msg) {
  const el = document.getElementById('log');
  const t = new Date().toISOString().substr(11,8);
  el.innerHTML += `<div>[${t}] ${msg}</div>`;
  el.scrollTop = el.scrollHeight;
  if(el.children.length > 80) el.removeChild(el.firstChild);
}
</script>
</body>
</html>
)rawhtml";
