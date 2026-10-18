#pragma once

#include <pgmspace.h>

// 网页模板，占位符由 WebPanel::buildPage() 替换：
//   %SSID%   -> WiFi 热点名
//   %IP%     -> 本机 IP
//   %RSSI%   -> 信号强度（dBm）
//   %UPTIME% -> 运行秒数
//   %HEAP%   -> 剩余堆内存（字节）
//   %COLOR%  -> RGB 颜色名
//   %SWATCH% -> RGB 色块（rgb(r,g,b)）
//   %DISPON% -> 屏幕开关状态（"1"/"0"，注入 JS 变量）
// 内容为单页应用：状态卡片 + RGB 控制 + AT 命令控制台（1s 轮询 /at/hist 增量刷新）
static const char kWebPage[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="zh-CN">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>ESP32-S3 控制台</title>
<style>
  body{font-family:-apple-system,system-ui,sans-serif;background:#f5f6f8;color:#222;margin:0;padding:16px}
  .card{max-width:640px;margin:0 auto 16px;background:#fff;border-radius:12px;padding:20px;box-shadow:0 2px 12px rgba(0,0,0,.08)}
  h1{font-size:18px;margin:0 0 12px}
  .row{display:flex;justify-content:space-between;padding:6px 0;border-bottom:1px solid #eee;font-size:14px}
  .row:last-of-type{border-bottom:none}
  .k{color:#888}
  .v{font-family:ui-monospace,monospace}
  .swatch{display:inline-block;width:12px;height:12px;border-radius:50%;vertical-align:-1px;margin-right:6px;background:%SWATCH%}
  label{font-size:13px;color:#666}
  select,input,button{font-size:14px;padding:8px;border:1px solid #ddd;border-radius:8px;background:#fff}
  .bar{display:flex;flex-wrap:wrap;gap:8px;align-items:center;margin-bottom:10px}
  .cmdline{display:flex;gap:8px}
  .cmdline input{flex:1;min-width:120px;font-family:ui-monospace,monospace}
  button{cursor:pointer;background:#0066ff;color:#fff;border:none}
  button:active{background:#004ecc}
  button.ghost{background:#f0f0f0;color:#333}
  #log{margin-top:12px;height:340px;overflow:auto;background:#1e1f22;color:#dfe;border-radius:8px;padding:10px;font-size:13px}
  .entry{margin-bottom:10px;border-bottom:1px dashed #3a3d42;padding-bottom:8px}
  .meta{color:#7c8b95;font-size:11px;margin-bottom:2px}
  .cmd{color:#7fd1ff;white-space:pre-wrap;word-break:break-all}
  .resp{color:#c8f5c0;margin:2px 0 0;white-space:pre-wrap;word-break:break-all;font-family:ui-monospace,monospace}
  .hint{font-size:12px;color:#999;margin-top:6px}
</style>
</head>
<body>

<div class="card">
  <h1>系统状态</h1>
  <div class="row"><span class="k">SSID</span><span class="v">%SSID%</span></div>
  <div class="row"><span class="k">IP</span><span class="v">%IP%</span></div>
  <div class="row"><span class="k">信号强度</span><span class="v">%RSSI% dBm</span></div>
  <div class="row"><span class="k">运行时间</span><span class="v">%UPTIME% s</span></div>
  <div class="row"><span class="k">剩余堆内存</span><span class="v">%HEAP% B</span></div>
  <div class="row"><span class="k">RGB 灯</span><span class="v"><i class="swatch"></i>%COLOR%</span></div>
  <div class="bar" style="margin-top:12px">
    <button onclick="fetch('/led').then(refresh)">切换 RGB</button>
    <button class="ghost" id="dispBtn" onclick="toggleDisp()">屏幕</button>
  </div>
</div>

<div class="card">
  <h1>WiFi 设置</h1>
  <div class="row"><span class="k">模式</span><span class="v" id="wMode">-</span></div>
  <div class="row"><span class="k">连接</span><span class="v" id="wConn">-</span></div>
  <div class="bar" style="margin-top:12px">
    <button onclick="scanWifi()">扫描附近 WiFi</button>
    <span class="hint" id="scanHint"></span>
  </div>
  <div id="scanList"></div>
  <h1 style="margin-top:16px">已保存的网络（启动时按顺序尝试，最多 5 组）</h1>
  <div id="savedList"></div>
  <div class="cmdline" style="margin-top:12px">
    <input id="wSsid" placeholder="SSID（点击上方扫描结果填入）" autocomplete="off">
    <input id="wPass" type="password" placeholder="密码（开放网络留空）" autocomplete="off">
    <button onclick="saveWifi()">保存并连接</button>
  </div>
  <div class="hint" id="wifiHint">AP 配网模式下网页需要 admin / admin123 登录；新配置保存到 NVS，重启后仍有效。</div>
</div>

<div class="card">
  <h1>串口 AT 命令</h1>
  <div class="bar">
    <label>串口</label>
    <select id="port">
      <option value="1">串口1 (TX17 / RX18)</option>
      <option value="2">串口2 (TX41 / RX40)</option>
    </select>
    <label>波特率</label>
    <select id="baud">
      <option>9600</option><option>19200</option><option>38400</option>
      <option>57600</option><option selected>115200</option><option>230400</option>
      <option>460800</option><option>921600</option>
    </select>
    <label>行尾</label>
    <select id="eol">
      <option value="0">CRLF</option><option value="1">CR</option>
      <option value="2">LF</option><option value="3">无</option>
    </select>
    <label>超时</label>
    <select id="t">
      <option value="300">300ms</option><option value="1000" selected>1s</option>
      <option value="3000">3s</option>
    </select>
  </div>
  <div class="cmdline">
    <input id="cmd" placeholder="输入 AT 命令，回车发送" autocomplete="off">
    <button onclick="sendCmd()">发送</button>
    <button class="ghost" onclick="clearLog()">清空</button>
  </div>
  <div class="hint">↑/↓ 切换本机历史输入；串口自发上报会自动显示。</div>
  <div id="log"></div>
</div>

<script>
let lastId = -1;
let localHist = [];
let localIdx = -1;
let dispOn = %DISPON%;

function syncDispBtn(){
  const b = document.getElementById('dispBtn');
  b.textContent = dispOn ? '关闭屏幕' : '打开屏幕';
}

async function toggleDisp(){
  try {
    const r = await fetch('/disp?on=' + (dispOn ? 0 : 1));
    dispOn = (await r.text()) === 'on';
    syncDispBtn();
  } catch (e) {}
}
syncDispBtn();

function esc(s){return String(s).replace(/&/g,'&amp;').replace(/</g,'&lt;').replace(/>/g,'&gt;');}

function addEntry(e){
  const log = document.getElementById('log');
  const atBottom = log.scrollHeight - log.scrollTop - log.clientHeight < 40;
  const d = document.createElement('div');
  d.className = 'entry';
  let html = '<div class="meta">#' + e.id + ' · 串口' + e.port + '</div>';
  if (e.cmd) html += '<div class="cmd">&gt; ' + esc(e.cmd) + '</div>';
  html += '<pre class="resp">' + (e.resp ? esc(e.resp) : '<i>(无响应)</i>') + '</pre>';
  d.innerHTML = html;
  log.appendChild(d);
  if (atBottom) log.scrollTop = log.scrollHeight;
}

async function sendCmd(){
  const el = document.getElementById('cmd');
  const cmd = el.value;
  if (!cmd.length) return;
  if (localHist[localHist.length-1] !== cmd) localHist.push(cmd);
  localIdx = localHist.length;
  const q = new URLSearchParams({
    port: document.getElementById('port').value,
    baud: document.getElementById('baud').value,
    eol: document.getElementById('eol').value,
    t: document.getElementById('t').value,
    cmd: cmd
  });
  try {
    const r = await fetch('/at/send?' + q.toString());
    const j = await r.json();
    if (j.id !== undefined) { addEntry(j); lastId = Math.max(lastId, j.id); }
  } catch (err) {
    addEntry({id:'-', port:'-', cmd:cmd, resp:'请求失败'});
  }
  el.value = '';
}

async function poll(){
  try {
    const r = await fetch('/at/hist?after=' + lastId + '&n=20');
    const arr = await r.json();
    arr.forEach(e => { addEntry(e); lastId = Math.max(lastId, e.id); });
  } catch (e) {}
  wifiStatus();   // 顺带刷新 WiFi 状态行
}

/* ---------------- WiFi 配网 ---------------- */
// 刷新 WiFi 模式/连接信息
async function wifiStatus(){
  try {
    const r = await fetch('/wifi/status');
    const j = await r.json();
    document.getElementById('wMode').textContent =
      j.mode === 'ap' ? 'AP 配网' : (j.mode === 'sta' ? 'STA 已连接' : '连接中');
    document.getElementById('wConn').textContent =
      j.connected ? (j.ssid + ' · ' + j.ip + ' · ' + j.rssi + 'dBm')
                  : (j.ap ? ('AP: ' + j.apSsid + ' · 192.168.4.1') : '-');
  } catch (e) {}
}

// 扫描附近网络：点击后轮询 /wifi/scan 直至 busy=0
let scanTimer = null;
function scanWifi(){
  document.getElementById('scanHint').textContent = '扫描中...';
  fetch('/wifi/scan').then(r => r.json()).then(j => {
    if (j.busy) {
      if (!scanTimer) scanTimer = setInterval(pollScan, 1200);
      return;
    }
    stopScanTimer();
    renderScan(j.nets || []);
  }).catch(() => stopScanTimer());
}

function pollScan(){
  fetch('/wifi/scan').then(r => r.json()).then(j => {
    if (!j.busy) {
      stopScanTimer();
      renderScan(j.nets || []);
    }
  }).catch(() => stopScanTimer());
}

function stopScanTimer(){
  if (scanTimer) { clearInterval(scanTimer); scanTimer = null; }
  document.getElementById('scanHint').textContent = '';
}

// 渲染扫描结果；点击某条目把 SSID 填入输入框
function renderScan(nets){
  const box = document.getElementById('scanList');
  box.innerHTML = '';
  if (!nets.length) {
    box.innerHTML = '<div class="hint">未发现网络</div>';
    return;
  }
  nets.forEach(n => {
    const d = document.createElement('div');
    d.className = 'entry';
    d.style.cursor = 'pointer';
    d.innerHTML = '<div class="meta">' + esc(n.ssid) + ' · ' + n.rssi +
      ' dBm · ' + (n.secure ? '加密' : '开放') + '</div>';
    d.onclick = () => {
      document.getElementById('wSsid').value = n.ssid;
      document.getElementById('wPass').value = '';
      document.getElementById('wPass').focus();
    };
    box.appendChild(d);
  });
}

// 拉取已保存配置并渲染（含上移/下移/删除）
async function loadSaved(){
  let j;
  try {
    const r = await fetch('/wifi/list');
    j = await r.json();
  } catch (e) { return; }
  const box = document.getElementById('savedList');
  box.innerHTML = '';
  if (!j.count) {
    box.innerHTML = '<div class="hint">暂无保存的网络</div>';
    return;
  }
  j.list.forEach(n => {
    const d = document.createElement('div');
    d.className = 'entry';
    d.innerHTML = '<div class="meta">#' + (n.i + 1) + ' ' + esc(n.ssid) +
      (n.hasPass ? ' · 已存密码' : ' · 开放网络') + '</div>';
    const bar = document.createElement('div');
    bar.className = 'bar';
    bar.style.margin = '6px 0 0';
    bar.innerHTML =
      '<button class="ghost" data-act="up" data-i="' + n.i + '">上移</button>' +
      '<button class="ghost" data-act="down" data-i="' + n.i + '">下移</button>' +
      '<button class="ghost" data-act="del" data-i="' + n.i + '">删除</button>';
    bar.querySelectorAll('button').forEach(b => {
      b.onclick = () => {
        const i = parseInt(b.dataset.i, 10);
        if (b.dataset.act === 'up') wifiMove(i, i - 1);
        else if (b.dataset.act === 'down') wifiMove(i, i + 1);
        else wifiDelete(i);
      };
    });
    d.appendChild(bar);
    box.appendChild(d);
  });
}

async function wifiMove(from, to){
  await fetch('/wifi/move?from=' + from + '&to=' + to, {method:'POST'});
  loadSaved();
}

async function wifiDelete(i){
  await fetch('/wifi/delete?index=' + i, {method:'POST'});
  loadSaved();
}

// 保存并尝试连接（成功后由 wifiStatus 轮询反映状态）
async function saveWifi(){
  const ssid = document.getElementById('wSsid').value.trim();
  const pass = document.getElementById('wPass').value;
  const hint = document.getElementById('wifiHint');
  if (!ssid.length) {
    hint.textContent = '请先填写或选择 SSID';
    return;
  }
  hint.textContent = '正在保存并尝试连接...';
  try {
    const r = await fetch('/wifi/save?ssid=' + encodeURIComponent(ssid) +
                          '&pass=' + encodeURIComponent(pass) +
                          '&connect=1', {method:'POST'});
    hint.textContent = r.ok ? '已保存，连接结果见上方状态' : '保存失败（可能已满 5 组）';
    loadSaved();
  } catch (e) {
    hint.textContent = '请求失败';
  }
}

async function clearLog(){
  await fetch('/at/clear');
  document.getElementById('log').innerHTML = '';
  lastId = -1;
}

function refresh(){ location.reload(); }

document.getElementById('cmd').addEventListener('keydown', function(ev){
  if (ev.key === 'Enter') { sendCmd(); }
  else if (ev.key === 'ArrowUp') {
    if (localIdx > 0) { localIdx--; this.value = localHist[localIdx]; }
    ev.preventDefault();
  } else if (ev.key === 'ArrowDown') {
    if (localIdx < localHist.length - 1) { localIdx++; this.value = localHist[localIdx]; }
    else { localIdx = localHist.length; this.value = ''; }
    ev.preventDefault();
  }
});

poll();
setInterval(poll, 1000);
loadSaved();   // 页面加载即拉取已保存的 WiFi 配置
</script>
</body>
</html>
)rawliteral";
