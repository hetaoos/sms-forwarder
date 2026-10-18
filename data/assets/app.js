/* ================= ESP32-S3 控制台 · 应用逻辑 =================
 * 多页结构：index.html 为外壳（左侧菜单 + 顶栏 + 内容区），
 * 右侧内容按 hash 路由懒加载 pages/<id>.html 片段（Alpine 组件渲染）。
 * 认证：Cookie 会话由浏览器自动携带；401 统一跳转独立登录页。
 * ============================================================== */
'use strict';

/* ---------- 小工具 ---------- */
const $ = (s, r) => (r || document).querySelector(s);

function fmtUptime(s) {
  s = Math.max(0, Math.floor(s || 0));
  const d = Math.floor(s / 86400), h = Math.floor(s % 86400 / 3600),
        m = Math.floor(s % 3600 / 60), x = s % 60;
  return (d ? d + 'd ' : '') + (h || d ? h + 'h ' : '') + (m ? m + 'm ' : '') + x + 's';
}
function fmtBytes(b) {
  b = b || 0;
  return b >= 1048576 ? (b / 1048576).toFixed(2) + ' MB'
       : b >= 1024 ? (b / 1024).toFixed(1) + ' KB' : b + ' B';
}
const fmt = fmtUptime;  // 模板里用的短名
function fmtAgo(ms) {
  if (ms == null || ms >= 0xFFFFFFFF) return '从未';
  ms = Math.floor(ms / 1000);
  if (ms < 60) return ms + 's 前';
  if (ms < 3600) return Math.floor(ms / 60) + 'm 前';
  return Math.floor(ms / 3600) + 'h 前';
}
function escHtml(s) {
  return String(s == null ? '' : s).replace(/[&<>"']/g,
    c => ({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c]));
}

/* ---------- 页面清理回调（切页时停轮询/定时器） ---------- */
let cleanups = [];
function onCleanup(fn) { cleanups.push(fn); }
function runCleanups() { cleanups.forEach(f => { try { f(); } catch (e) {} }); cleanups = []; }

/* ---------- 请求封装：401 -> 登录页 ---------- */
let currentUser = 'admin';
async function apiFetch(path, opts) {
  const r = await fetch(path, opts || {});
  if (r.status === 401) {
    const next = location.pathname + location.hash;
    location.replace('/login.html?next=' + encodeURIComponent(next || '/'));
    throw new Error('unauthorized');
  }
  return r;
}

// POST JSON 请求体（全站 POST 契约：请求/响应均为 JSON）
async function postJson(path, obj) {
  return apiFetch(path, {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify(obj || {})
  });
}

// 读取 JSON 错误响应中的 error 字段；非 JSON 时回退到状态码
async function errText(r) {
  try {
    const j = await r.json();
    return j.error || ('HTTP ' + r.status);
  } catch (e) {
    return 'HTTP ' + r.status;
  }
}

/* ---------- 统一操作反馈 ----------
 * 各页面的写操作（POST/动作类 GET）统一走这两个助手：
 * 失败自动弹右上角红色 toast（5s），调用方只需处理成功分支。
 * 轮询类 GET 不要用（避免定时失败刷屏）。 */
async function apiOp(path, opts, okMsg) {
  let r;
  try {
    r = await apiFetch(path, opts);
  } catch (e) {
    toast('✕ 请求失败：' + (e && e.message ? e.message : e), 'err');
    return null;
  }
  if (!r.ok) {
    toast('✕ ' + await errText(r), 'err');
    return null;
  }
  if (okMsg) toast(okMsg, 'ok');
  return r;
}
async function postOp(path, obj, okMsg) {
  return apiOp(path, {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify(obj || {})
  }, okMsg);
}

function goLogin() {
  const next = location.pathname + location.hash;
  location.replace('/login.html?next=' + encodeURIComponent(next || '/'));
}

/* ---------- Toast / 确认框 ---------- */
// 右上角滑入提示；type: ''|'ok'|'err'|'warn'；duration: 停留 ms（错误默认 5s，其余 3s）
function toast(msg, type, duration) {
  const el = document.createElement('div');
  el.className = 'toast ' + (type || '');
  el.textContent = msg;
  const wrap = $('#toastWrap');
  if (!wrap) return;
  wrap.appendChild(el);
  requestAnimationFrame(() => el.classList.add('show'));
  const ms = duration || (type === 'err' ? 5000 : 3000);
  setTimeout(() => { el.classList.remove('show'); setTimeout(() => el.remove(), 350); }, ms);
}
function confirmDialog(title, text, danger) {
  return new Promise(resolve => {
    const ov = document.createElement('div');
    ov.className = 'modal-mask';
    ov.innerHTML = '<div class="modal"><h3>' + escHtml(title) + '</h3><p>' + escHtml(text) +
      '</p><div class="modal-btns"><button class="btn ghost" data-a="0">取消</button>' +
      '<button class="btn' + (danger ? ' danger' : '') + '" data-a="1">确定</button></div></div>';
    document.body.appendChild(ov);
    ov.addEventListener('click', e => {
      const btn = e.target.closest('button');
      if (!btn && e.target !== ov) return;
      ov.remove();
      resolve(!!btn && btn.dataset.a === '1');
    });
  });
}

/* ---------- 菜单配置（新增页面：加一条 + 放一个 pages/<id>.html） ---------- */
const MENU = [
  { group: '监控', items: [
    { id: 'dashboard', title: '仪表盘', icon: 'dashboard' },
    { id: 'logs', title: '运行日志', icon: 'logs', todo: true },
  ]},
  { group: '设备控制', items: [
    { id: 'serial', title: '串口 AT', icon: 'serial' },
    { id: 'display', title: '屏幕显示', icon: 'display' },
    { id: 'rgb', title: 'RGB 灯', icon: 'rgb' },
  ]},
  { group: '网络', items: [
    { id: 'wifi', title: 'WiFi 设置', icon: 'wifi' },
    { id: 'push', title: '推送通道', icon: 'push', todo: true },
    { id: 'mqtt', title: 'MQTT', icon: 'mqtt', todo: true },
  ]},
  { group: '短信', items: [
    { id: 'sms', title: '短信记录', icon: 'sms', todo: true },
    { id: 'blacklist', title: '黑名单', icon: 'blacklist', todo: true },
  ]},
  { group: '系统', items: [
    { id: 'settings', title: '系统设置', icon: 'settings' },
    { id: 'tools', title: '系统工具', icon: 'tools', todo: true },
  ]},
];

/* 线性图标（内联 SVG，无外部依赖） */
const ICONS = {
  dashboard: '<svg viewBox="0 0 24 24"><rect x="3" y="3" width="7" height="7" rx="1.5"/><rect x="14" y="3" width="7" height="7" rx="1.5"/><rect x="3" y="14" width="7" height="7" rx="1.5"/><rect x="14" y="14" width="7" height="7" rx="1.5"/></svg>',
  serial: '<svg viewBox="0 0 24 24"><rect x="3" y="4" width="18" height="16" rx="2"/><polyline points="7 9 10 12 7 15"/><line x1="13" y1="15" x2="17" y2="15"/></svg>',
  display: '<svg viewBox="0 0 24 24"><rect x="3" y="4" width="18" height="13" rx="2"/><line x1="8" y1="21" x2="16" y2="21"/><line x1="12" y1="17" x2="12" y2="21"/></svg>',
  rgb: '<svg viewBox="0 0 24 24"><circle cx="12" cy="12" r="4"/><path d="M12 2v3M12 19v3M2 12h3M19 12h3M4.9 4.9l2.1 2.1M17 17l2.1 2.1M19.1 4.9L17 7M7 17l-2.1 2.1"/></svg>',
  wifi: '<svg viewBox="0 0 24 24"><path d="M5 12.5a10 10 0 0 1 14 0"/><path d="M8.5 15.8a5.5 5.5 0 0 1 7 0"/><circle cx="12" cy="19" r="1.2"/></svg>',
  push: '<svg viewBox="0 0 24 24"><path d="M21 3L3 10l7 3 3 7 8-17z"/><path d="M10 13l4-4"/></svg>',
  mqtt: '<svg viewBox="0 0 24 24"><circle cx="5" cy="12" r="2.2"/><circle cx="19" cy="6" r="2.2"/><circle cx="19" cy="18" r="2.2"/><path d="M7 11l10-4M7 13l10 4"/></svg>',
  sms: '<svg viewBox="0 0 24 24"><path d="M4 5h16v11H8l-4 3.5V5z"/></svg>',
  blacklist: '<svg viewBox="0 0 24 24"><circle cx="12" cy="12" r="9"/><line x1="6" y1="6" x2="18" y2="18"/></svg>',
  logs: '<svg viewBox="0 0 24 24"><path d="M6 3h8l4 4v14H6z"/><polyline points="14 3 14 7 18 7"/><line x1="9" y1="12" x2="15" y2="12"/><line x1="9" y1="16" x2="15" y2="16"/></svg>',
  settings: '<svg viewBox="0 0 24 24"><line x1="4" y1="7" x2="20" y2="7"/><circle cx="9" cy="7" r="2"/><line x1="4" y1="17" x2="20" y2="17"/><circle cx="15" cy="17" r="2"/></svg>',
  tools: '<svg viewBox="0 0 24 24"><path d="M14.7 6.3a4.5 4.5 0 0 0-6 6L3 18l3 3 5.7-5.7a4.5 4.5 0 0 0 6-6L14 12l-2-2 2.7-3.7z"/></svg>',
};

function findMenu(id) {
  for (const g of MENU) for (const it of g.items) if (it.id === id) return it;
  return null;
}

/* ---------- 外壳渲染 ---------- */
function renderShell() {
  const sb = $('#sidebar');
  let html = '<div class="brand"><span class="logo">S</span>' +
             '<span>ESP32-S3<small>控制台</small></span></div><nav class="menu">';
  for (const g of MENU) {
    html += '<div class="menu-group">' + g.group + '</div>';
    for (const it of g.items) {
      html += '<div class="menu-item" data-id="' + it.id + '">' + ICONS[it.icon] +
              '<span>' + it.title + '</span>' + (it.todo ? '<span class="todo">规划中</span>' : '') +
              '</div>';
    }
  }
  html += '</nav>';
  sb.innerHTML = html;
  sb.addEventListener('click', e => {
    const item = e.target.closest('.menu-item');
    if (!item) return;
    location.hash = '#/' + item.dataset.id;
    closeSidebar();
  });
}

function setActiveMenu(id) {
  document.querySelectorAll('.menu-item').forEach(el =>
    el.classList.toggle('active', el.dataset.id === id));
  $('#pageTitle').textContent = (findMenu(id) || {}).title || '页面';
}
function openSidebar() { $('#sidebar').classList.add('open'); $('#mask').classList.add('show'); }
function closeSidebar() { $('#sidebar').classList.remove('open'); $('#mask').classList.remove('show'); }

async function loadUser() {
  try {
    const r = await fetch('/api/session');
    if (r.ok) { const j = await r.json(); currentUser = j.user || 'admin'; }
  } catch (e) {}
  $('#userChip').innerHTML = '<i></i>' + escHtml(currentUser);
}

async function logout() {
  if (!await confirmDialog('退出登录', '确定要退出当前登录会话吗？')) return;
  await fetch('/api/logout', { method: 'POST' }).catch(() => {});
  goLogin();
}

/* ---------- Hash 路由 ---------- */
async function route() {
  runCleanups();
  const id = (location.hash.replace(/^#\/?/, '').split('?')[0]) || 'dashboard';
  const item = findMenu(id);
  const view = $('#view');
  if (!item) {
    view.innerHTML = '<div class="card empty"><div class="icon">&#128269;</div>' +
      '<h3>页面不存在</h3><p>路径 ' + escHtml(location.hash) + ' 没有对应页面</p></div>';
    return;
  }
  view.innerHTML = '<div class="loading">加载中…</div>';
  let html;
  try {
    const r = await fetch('/pages/' + id + '.html', { cache: 'no-store' });
    if (!r.ok) throw new Error('http ' + r.status);
    html = await r.text();
  } catch (e) {
    view.innerHTML = '<div class="card empty"><div class="icon">&#9888;&#65039;</div>' +
      '<h3>页面加载失败</h3><p>' + escHtml(e.message) + '</p></div>';
    return;
  }
  view.innerHTML = html;
  setActiveMenu(id);
  // 片段内含 x-data 组件（pageXxx 全局函数）时初始化 Alpine 子树
  if (/x-data\s*=/.test(html) && window.Alpine) window.Alpine.initTree(view);
}

/* ---------- 启动 ---------- */
async function bootstrap() {
  try {
    const r = await fetch('/api/session');
    if (!r.ok) { goLogin(); return; }
  } catch (e) { goLogin(); return; }
  renderShell();
  await loadUser();
  $('#logoutBtn').addEventListener('click', logout);
  $('#menuBtn').addEventListener('click', openSidebar);
  $('#mask').addEventListener('click', closeSidebar);
  window.addEventListener('hashchange', route);
  if (!location.hash) location.hash = '#/dashboard';
  route();
}

document.addEventListener('DOMContentLoaded', bootstrap);

/* ================= 页面组件（Alpine x-data） ================= */

/* ---------- 仪表盘 ---------- */
function pageDashboard() {
  return {
    st: {}, stats: [], recent: [],
    token: '',
    async init() {
      this.token = sessionStorage.getItem('apiToken') || '';
      await this.refresh();
      const t = setInterval(() => this.refresh(), 2000);
      onCleanup(() => clearInterval(t));
    },
    async refresh() {
      try {
        const r = await apiFetch('/api/status');
        if (r.ok) this.st = await r.json();
      } catch (e) {}
      try {
        const r = await apiFetch('/api/uart/stats');
        if (r.ok) this.stats = await r.json();
      } catch (e) {}
      try {
        const r = await apiFetch('/api/at/hist?after=-1&n=5');
        if (r.ok) this.recent = (await r.json()).slice().reverse();
      } catch (e) {}
    },
    async toggleDisp() {
      const r = await postOp('/api/disp', { on: !this.st.disp });
      if (r) this.st.disp = (await r.json()).on === true;
    },
    async nextLed() {
      await apiOp('/api/led');
      this.refresh();
    },
    rgbCss() {
      const c = this.st.rgb || [0, 0, 0];
      return 'rgb(' + c[0] + ',' + c[1] + ',' + c[2] + ')';
    },
    go(id) { location.hash = '#/' + id; },
    fmt, fmtBytes,
  };
}

/* ---------- 串口 AT ---------- */
function pageSerial() {
  return {
    port: '1', baud: '115200', eol: '0', tmo: '1000',
    cmd: '', entries: [], lastId: -1, stats: [],
    localHist: [], localIdx: -1,
    async init() {
      try {
        const r = await apiFetch('/api/at/baud');
        if (r.ok) this.baud = String((await r.json()).default || 115200);
      } catch (e) {}
      await this.poll();
      const t = setInterval(() => this.poll(), 1000);
      onCleanup(() => clearInterval(t));
    },
    async poll() {
      try {
        const r = await apiFetch('/api/at/hist?after=' + this.lastId + '&n=30');
        if (r.ok) {
          const arr = await r.json();
          arr.forEach(e => {
            this.entries.push(e);
            this.lastId = Math.max(this.lastId, e.id);
          });
          if (this.entries.length > 300) this.entries.splice(0, this.entries.length - 300);
          this.scrollBottom();
        }
      } catch (e) {}
      try {
        const r = await apiFetch('/api/uart/stats');
        if (r.ok) this.stats = await r.json();
      } catch (e) {}
    },
    scrollBottom() {
      this.$nextTick(() => {
        const box = this.$refs.console;
        if (!box) return;
        const atBottom = box.scrollHeight - box.scrollTop - box.clientHeight < 60;
        if (atBottom) box.scrollTop = box.scrollHeight;
      });
    },
    async send() {
      const cmd = this.cmd;
      if (!cmd.length) return;
      if (this.localHist[this.localHist.length - 1] !== cmd) this.localHist.push(cmd);
      this.localIdx = this.localHist.length;
      const q = new URLSearchParams(
        { port: this.port, baud: this.baud, eol: this.eol, t: this.tmo, cmd: cmd });
      const r = await apiOp('/api/at/send?' + q.toString());  // 失败 toast + 日志留痕
      if (r) {
        const j = await r.json();
        if (j.id !== undefined) {
          this.entries.push(j);
          this.lastId = Math.max(this.lastId, j.id);
          this.scrollBottom();
        }
      } else {
        this.entries.push({ id: '-', port: '-', cmd: cmd, resp: '请求失败，详见右上角提示' });
      }
      this.cmd = '';
    },
    async clear() {
      if (!await confirmDialog('清空历史', '确定清空全部 AT 历史记录吗？')) return;
      const r = await apiOp('/api/at/clear', null, '已清空历史');
      if (!r) return;
      this.entries = [];
      this.lastId = -1;
    },
    async reset() {
      const p = parseInt(this.port, 10);
      if (!await confirmDialog('复位串口', '将关闭并重新打开串口' + p + '，确定？', true)) return;
      const r = await apiOp('/api/at/reset?port=' + p, null, '串口' + p + ' 已复位');
      if (!r) return;
    },
    onKey(ev) {
      if (ev.key === 'Enter') this.send();
      else if (ev.key === 'ArrowUp') {
        if (this.localIdx > 0) { this.localIdx--; this.cmd = this.localHist[this.localIdx]; }
        ev.preventDefault();
      } else if (ev.key === 'ArrowDown') {
        if (this.localIdx < this.localHist.length - 1) { this.localIdx++; this.cmd = this.localHist[this.localIdx]; }
        else { this.localIdx = this.localHist.length; this.cmd = ''; }
        ev.preventDefault();
      }
    },
    portStat(p) { return this.stats[p - 1] || {}; },
    fmt, fmtBytes,
  };
}

/* ---------- WiFi 设置 ---------- */
function pageWifi() {
  return {
    wifi: {}, nets: [], saved: [], scanHint: '', scanning: false,
    ssid: '', pass: '', saving: false, wifiHint: '', wifiIsErr: false,
    scanTimer: null,
    async init() {
      await this.refresh();
      await this.loadSaved();
      const t = setInterval(() => this.refresh(), 3000);
      onCleanup(() => { clearInterval(t); if (this.scanTimer) clearInterval(this.scanTimer); });
    },
    /* 提示区辅助：错误走红底错误条，普通信息走灰字 */
    wifiErr(msg) { this.wifiHint = msg; this.wifiIsErr = true; },
    wifiBusy(msg) { this.wifiHint = msg; this.wifiIsErr = false; },
    async refresh() {
      try {
        const r = await apiFetch('/api/wifi/status');
        if (r.ok) this.wifi = await r.json();
      } catch (e) {}
    },
    async loadSaved() {
      try {
        const r = await apiFetch('/api/wifi/list');
        if (r.ok) { const j = await r.json(); this.saved = j.list || []; }
      } catch (e) {}
    },
    async scan() {
      this.scanning = true;
      this.scanHint = '扫描中…';
      const r = await apiOp('/api/wifi/scan');   // 失败自动 toast
      if (!r) { this.scanHint = ''; this.scanning = false; return; }
      const j = await r.json();
      if (j.busy) {
        if (!this.scanTimer) this.scanTimer = setInterval(() => this.pollScan(), 1200);
        return;
      }
      this.doneScan(j.nets || []);
    },
    async pollScan() {
      const r = await apiOp('/api/wifi/scan');
      if (!r) {
        if (this.scanTimer) { clearInterval(this.scanTimer); this.scanTimer = null; }
        this.scanHint = '';
        return;
      }
      const j = await r.json();
      if (!j.busy) this.doneScan(j.nets || []);
    },
    doneScan(nets) {
      if (this.scanTimer) { clearInterval(this.scanTimer); this.scanTimer = null; }
      this.nets = nets;
      this.scanning = false;
      this.scanHint = nets.length ? '' : '未发现网络';
    },
    pick(ssid) { this.ssid = ssid; this.pass = ''; },
    async save() {
      const ssid = this.ssid.trim();
      if (!ssid.length) { this.wifiErr('请先填写或选择 SSID'); return; }
      this.saving = true;
      this.wifiBusy('正在保存并尝试连接…');
      const r = await postOp('/api/wifi/save',
        { ssid: ssid, pass: this.pass, connect: true });
      this.saving = false;
      if (!r) { this.wifiErr('保存失败，详见右上角提示'); return; }
      this.wifiBusy('已保存，连接结果见上方状态');
      this.pass = '';
      await this.loadSaved();
    },
    async move(from, to) {
      const r = await postOp('/api/wifi/move', { from: from, to: to });
      if (r) this.loadSaved();
    },
    async del(i, ssid) {
      if (!await confirmDialog('删除配置', '确定删除「' + ssid + '』吗？重启后将不再自动连接。', true)) return;
      const r = await postOp('/api/wifi/delete', { index: i });
      if (!r) return;
      this.loadSaved();
      toast('已删除 ' + ssid, 'ok');
    },
    modeText() {
      return this.wifi.mode === 'ap' ? 'AP 配网' : (this.wifi.mode === 'sta' ? 'STA 已连接' : '连接中');
    },
    connText() {
      if (this.wifi.connected) return this.wifi.ssid + ' · ' + this.wifi.ip + ' · ' + this.wifi.rssi + 'dBm';
      if (this.wifi.ap) return 'AP: ' + this.wifi.apSsid + ' · 192.168.4.1';
      return '-';
    },
    fmt,
  };
}

/* ---------- RGB 灯 ---------- */
const RGB_PRESETS = [
  { name: 'OFF', rgb: [0, 0, 0] },
  { name: 'RED', rgb: [255, 0, 0] },
  { name: 'GREEN', rgb: [0, 255, 0] },
  { name: 'BLUE', rgb: [0, 0, 255] },
  { name: 'WHITE', rgb: [255, 255, 255] },
];
function pageRgb() {
  return {
    presets: RGB_PRESETS,
    st: {}, r: 255, g: 0, b: 0, bright: 100, touched: false,
    async init() {
      await this.refresh();
      const t = setInterval(() => this.refresh(), 2000);
      onCleanup(() => clearInterval(t));
    },
    async refresh() {
      try {
        const r = await apiFetch('/api/rgb');
        if (r.ok) {
          const j = await r.json();
          this.st = j;
          if (!this.touched) this.bright = j.bright;  // 拖动中不被轮询回抢
        }
      } catch (e) {}
    },
    currentRgb() { return this.st.rgb || [0, 0, 0]; },
    isOn(p) {
      const c = this.currentRgb();
      return c[0] === p.rgb[0] && c[1] === p.rgb[1] && c[2] === p.rgb[2];
    },
    // 色块按当前亮度衰减不透明度，直观反映实际发光强度
    currentSwatchStyle() {
      return 'background:rgb(' + this.currentRgb().join(',') +
             ');opacity:' + (0.25 + 0.75 * (this.st.bright || 0) / 100);
    },
    swatchStyle(p) {
      return 'background:rgb(' + p.rgb.join(',') +
             ');opacity:' + (0.25 + 0.75 * (this.st.bright || 0) / 100);
    },
    async applyPreset(i) {
      const r = await postOp('/api/rgb', { c: i });
      if (r) this.refresh();
    },
    async next() {
      const r = await apiOp('/api/led');
      if (r) this.refresh();
    },
    async applyRaw() {
      const clamp = v => Math.max(0, Math.min(255, v | 0));
      const r = await postOp('/api/rgb',
        { r: clamp(this.r), g: clamp(this.g), b: clamp(this.b) });
      if (r) this.refresh();
    },
    async off() {
      const r = await postOp('/api/rgb', { c: 0 });
      if (r) this.refresh();
    },
    async applyBright() {
      const r = await postOp('/api/rgb', { bright: this.bright });
      if (r) {
        this.st = await r.json();
        this.bright = this.st.bright;
      }
      this.touched = false;
    },
    previewStyle() {
      const clamp = v => Math.max(0, Math.min(255, v | 0));
      return 'background:rgb(' + clamp(this.r) + ',' + clamp(this.g) + ',' +
             clamp(this.b) + ')';
    },
  };
}

/* ---------- 屏幕显示 ---------- */
function pageDisplay() {
  return {
    st: {}, bright: 80, touched: false,
    async init() {
      await this.refresh();
      const t = setInterval(() => this.refresh(), 2000);
      onCleanup(() => clearInterval(t));
    },
    async refresh() {
      try {
        const r = await apiFetch('/api/disp');
        if (r.ok) {
          const j = await r.json();
          this.st = j;
          if (!this.touched) this.bright = j.bright;  // 用户拖动中不被轮询回抢
        }
      } catch (e) {}
    },
    async toggle() {
      const r = await postOp('/api/disp', { on: !this.st.on });
      if (r) this.st = await r.json();
    },
    async applyBright() {
      const r = await postOp('/api/disp', { bright: this.bright });
      if (r) {
        this.st = await r.json();
        this.bright = this.st.bright;
      }
      this.touched = false;
    },
  };
}

/* ---------- 系统设置 · 管理账号 ---------- */
function pageAuth() {
  return {
    user: '', newUser: '', newPass: '', oldPass: '',
    saving: false, done: false,
    hint: '', isErr: false, errField: '',
    async init() {
      try {
        const r = await apiFetch('/api/auth');
        if (r.ok) { const j = await r.json(); this.user = j.user || ''; this.newUser = this.user; }
      } catch (e) {}
    },
    async save() {
      if (this.done) return;
      this.isErr = false;
      this.hint = '';
      this.errField = '';
      // 整个函数体包在 try 内：任何异常都要给出可见提示，不能静默失败
      // （曾经的教训：校验代码在 try 外抛 TypeError，用户点击毫无反应）
      try {
        const u = (this.newUser || '').trim();
        if (!u.length) { this.failField('newUser', '新用户名不能为空'); return; }
        if (u.length > 32) { this.failField('newUser', '用户名最长 32 字符'); return; }
        if (!this.newPass.length) { this.failField('newPass', '新密码不能为空'); return; }
        if (this.newPass.length > 64) { this.failField('newPass', '密码最长 64 字符'); return; }
        if (!this.oldPass.length) { this.failField('oldPass', '请输入当前密码以验证身份'); return; }
        this.saving = true;
        let r;
        try {
          r = await apiFetch('/api/auth', {
            method: 'POST',
            headers: { 'Content-Type': 'application/json' },
            body: JSON.stringify({ user: u, pass: this.newPass, passOld: this.oldPass })
          });
        } catch (e) {
          this.fail('请求失败：' + (e && e.message ? e.message : e));
          return;
        }
        if (r.status === 403) {          // 当前密码不对：高亮旧密码框并聚焦
          this.failField('oldPass', await errText(r));
          return;
        }
        if (!r.ok) {
          this.fail(await errText(r));
          return;
        }
        // 成功：全部登录态已作废，跳登录页
        this.done = true;
        this.isErr = false;
        this.hint = '已保存，全部登录态已作废，即将跳转登录页…';
        toast('账号已保存，请重新登录', 'ok');
        setTimeout(() => { location.replace('/login.html'); }, 1500);
      } catch (e) {
        console.error('[auth] save failed:', e);
        this.fail('请求失败：' + (e && e.message ? e.message : e));
      }
      this.saving = false;
    },
    // 普通错误：红底错误条 + 红色 toast
    fail(msg) {
      this.isErr = true;
      this.hint = msg;
      toast('✕ ' + msg, 'err');
      this.saving = false;
    },
    // 字段级错误：额外高亮对应输入框并聚焦
    failField(field, msg) {
      this.fail(msg);
      this.errField = field;
      // 聚焦出错字段：优先用 Alpine 的 $nextTick，不可用时退化到 setTimeout，
      // 且任何情况下都不允许抛异常（否则会把真正的错误提示覆盖掉）
      const focusField = () => {
        const el = this.$refs && this.$refs[field];
        if (el && el.focus) el.focus();
      };
      if (typeof this.$nextTick === 'function') this.$nextTick(focusField);
      else setTimeout(focusField, 0);
    },
    // 用户重新输入时清除错误态
    clearErr() {
      if (!this.isErr) return;
      this.isErr = false;
      this.hint = '';
      this.errField = '';
    },
  };
}
