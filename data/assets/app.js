/* ================= 短信转发器 · 应用逻辑 =================
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

// ---- 模组状态 / 信号强度展示（仪表盘与短信页共用）----
function modemName(p) { return '模组' + p; }
// 显示名称：有别名时「别名（模组x）」，否则「模组x」；m 可为模组对象或 port 数字
function modemLabel(m) {
  const port = (m && typeof m === 'object') ? m.port : m;
  const base = '模组' + port;
  const alias = (m && typeof m === 'object' && m.alias) ? String(m.alias).trim() : '';
  return alias ? (alias + '（' + base + '）') : base;
}
// 模组型号展示：取「厂商 + 最完整的料号」。CGMM 可能仅前缀（如 ML307A-DSLN），
// 而 CGMR 常含完整料号/后缀（如 -MTSH1S00），故优先使用更长的那一个。
function modemModel(m) {
  const vendor = String(m.vendor || '').trim();
  const model  = String(m.model || '').trim();
  const rev    = String(m.revision || '').trim();
  let full = model;
  if (rev && (model === '' || (rev.length >= model.length && rev.indexOf(model) >= 0))) full = rev;
  const parts = [vendor, full].filter(x => x);
  return parts.length ? parts.join(' ') : '未知型号';
}
// 固件版本：仅当 CGMR 未被型号行覆盖时展示（型号可能是「厂商+完整料号」，如
// CMCC ML307A-DSLN-MTSH1S00，此时固件与型号重复，隐藏以免刷屏）
function modemRevision(m) {
  const rev = String(m.revision || '').trim();
  return (rev && modemModel(m).indexOf(rev) < 0) ? rev : '';
}
// 运营商短代码 -> 中文名（CMCC/CUCC/CTCC/CBN）
function operCn(code) {
  const map = { CMCC: '中国移动', CUCC: '中国联通', CTCC: '中国电信', CBN: '中国广电' };
  return code ? (map[code] || code) : '';
}
function modemStateText(m) {
  if (!m.enabled) return '已禁用';
  switch (m.state) {
    case 'ready': return '已就绪';
    case 'fault': return '故障 / 未连接';
    case 'wait_at': return '等待模组响应';
    case 'wait_cpin': return '等待 SIM 卡';
    case 'bring_rf': return '射频启动中';
    case 'wait_reg': return '等待网络注册';
    case 'config': return '下发模组配置';
    case 'poweron': return '上电中';
    default: return '初始化中';
  }
}
function modemStateClass(m) {
  if (!m.enabled) return 'err';
  if (m.state === 'ready') return 'ok';
  if (m.state === 'fault') return 'err';
  return 'warn';
}
function csqBars(csq) {            // 信号格（0~4）
  if (csq < 0 || csq > 31) return '—';
  const n = Math.max(0, Math.min(4, Math.round(csq / 8)));
  return '▮'.repeat(n) + '▯'.repeat(4 - n);
}
function csqDbm(csq) {             // 信号 dBm
  if (csq < 0 || csq > 31) return '-';
  return (-113 + 2 * csq) + ' dBm';
}
function actText(act) {            // 接入技术（AT+CREG/CEREG 末字段）
  switch (Number(act)) {
    case 0: return 'GSM';
    case 2: return 'UMTS';
    case 3: return 'GSM(compact)';
    case 4: case 5: return 'E-UTRAN';
    case 6: return 'HSDPA';
    case 7: return 'LTE';
    default: return '';
  }
}
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

/* ---------- SSE 实时推送 ----------
 * 连接建在外壳作用域：bootstrap() 只调用一次，切换页面（哈希路由换
 * 内容区）不重建、不断开——同一浏览器内所有页面共享同一条连接。
 * 所有页面组件只读 Alpine.store('live')，不再自行轮询状态接口；
 * REST 接口原样保留给第三方客户端使用。 */
let es = null;
let pollTimer = null;

// 全局实时状态仓：SSE 事件写入，各页面组件只读绑定
Alpine.store('live', {
  conn: 'connecting',   // connecting | online | reconnecting | offline
  status: {},           // 同 GET /api/status
  stats: [],            // 同 GET /api/uart/stats
  wifi: {},             // 同 GET /api/wifi/status
  saved: [],            // 同 GET /api/wifi/list 的 list
  rgb: {},              // 同 GET /api/rgb
  disp: {},             // 同 GET /api/disp
  at: [],               // AT 日志（全页面共享，300 条上限）
  sms: {},              // 最新一条短信摘要（SSE sms 事件）
  smsports: [],         // 模组配置 [{port,enabled}]（SSE smsports 事件）
  smsstats: {},         // 短信存储统计（SSE smsstats 事件）
  // 追加 AT 记录：按 id 去重（SSE 重推/自身回显/REST 兜底三种来源安全合并），
  // 非数字 id（本地占位）直接入列
  pushAt(entries) {
    if (!Array.isArray(entries) || !entries.length) return;
    let maxId = -1;
    for (const e of this.at) if (typeof e.id === 'number' && e.id > maxId) maxId = e.id;
    for (const e of entries) {
      if (typeof e.id !== 'number' || e.id > maxId) {
        this.at.push(e);
        if (typeof e.id === 'number') maxId = e.id;
      }
    }
    if (this.at.length > 300) this.at.splice(0, this.at.length - 300);
  },
});

function setConn(s) { Alpine.store('live').conn = s; }

// 顶栏连接指示灯文案
function connTitle() {
  const c = Alpine.store('live').conn;
  return { online: 'Real-time push: connected',
           reconnecting: 'Real-time push: reconnecting...',
           connecting: 'Real-time push: connecting...',
           offline: 'Real-time push: disconnected (polling fallback)' }[c] || '';
}

// REST 兜底快照：SSE 建立时补拉一次（断线期记录也借此补齐），
// 同时作为不支持 EventSource 时的定时轮询降级
async function restSnapshot() {
  const live = Alpine.store('live');
  try { const r = await fetch('/api/status'); if (r.ok) live.status = await r.json(); } catch (e) {}
  try { const r = await fetch('/api/uart/stats'); if (r.ok) live.stats = await r.json(); } catch (e) {}
  try { const r = await fetch('/api/wifi/status'); if (r.ok) live.wifi = await r.json(); } catch (e) {}
  try {
    const r = await fetch('/api/wifi/list');
    if (r.ok) { const j = await r.json(); live.saved = j.list || []; }
  } catch (e) {}
  try { const r = await fetch('/api/rgb'); if (r.ok) live.rgb = await r.json(); } catch (e) {}
  try { const r = await fetch('/api/disp'); if (r.ok) live.disp = await r.json(); } catch (e) {}
  try {
    const r = await fetch('/api/at/hist?after=-1&n=50');
    if (r.ok) live.pushAt(await r.json());
  } catch (e) {}
  try {
    const r = await fetch('/api/sms/ports');
    if (r.ok) { const j = await r.json(); live.smsports = j.modems || []; }
  } catch (e) {}
  try {
    const r = await fetch('/api/sms/stats');
    if (r.ok) live.smsstats = await r.json();
  } catch (e) {}
  try {
    const r = await fetch('/api/sms/latest');
    if (r.ok) live.sms = await r.json();
  } catch (e) {}
}

// 降级轮询（SSE 不可用/持续失败时）
function fallbackPolling() {
  if (pollTimer) return;
  pollTimer = setInterval(restSnapshot, 2000);
}

// 建立 SSE 连接（外壳级单例）
function connectSSE() {
  if (!window.EventSource) {
    setConn('offline');
    toast('Browser does not support real-time push, switched to polling', 'warn');
    fallbackPolling();
    return;
  }
  const live = Alpine.store('live');
  setConn('connecting');
  es = new EventSource('/api/sse');
  const on = (name, fn) => es.addEventListener(name, e => {
    try { fn(JSON.parse(e.data)); } catch (err) {}
  });
  on('status', d => { live.status = d; });
  on('stats', d => { live.stats = d; });
  on('wifi', d => { live.wifi = d; });
  on('saved', d => { live.saved = d.list || []; });
  on('rgb', d => { live.rgb = d; });
  on('disp', d => { live.disp = d; });
  on('at', d => { live.pushAt(d); });
  on('sms', d => { live.sms = d; });
  on('smsports', d => { live.smsports = d.modems || []; });
  on('smsstats', d => { live.smsstats = d; });
  es.onopen = () => {
    setConn('online');
    restSnapshot();   // 兜底快照：补齐建连前数据与断线期记录
  };
  es.onerror = async () => {
    if (es.readyState === EventSource.CLOSED) {
      // 服务端关闭了连接：先探测会话，401 回登录页，否则降级轮询
      try {
        const r = await fetch('/api/session', { cache: 'no-store' });
        if (r.status === 401) { goLogin(); return; }
      } catch (e) {}
      setConn('offline');
      toast('Real-time push connection failed, switched to polling', 'warn');
      fallbackPolling();
    } else {
      setConn('reconnecting');   // 其余情况浏览器会自动重连
    }
  };
}

// 关闭 SSE 连接（仅登出/会话失效时调用）
function closeSSE() {
  if (es) { es.close(); es = null; }
  if (pollTimer) { clearInterval(pollTimer); pollTimer = null; }
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
  { group: '总览', items: [
    { id: 'dashboard', title: '仪表盘', icon: 'dashboard' },
  ]},
  { group: '短信', items: [
    { id: 'sms', title: '短信', icon: 'sms' },
    { id: 'modem', title: '4G 模组', icon: 'modem' },
    { id: 'serial', title: '短信调试', icon: 'serial' },
    { id: 'blacklist', title: '黑名单', icon: 'blacklist', todo: true },
  ]},
  { group: '监控', items: [
    { id: 'logs', title: '运行日志', icon: 'logs', todo: true },
  ]},
  { group: '设备控制', items: [
    { id: 'display', title: '屏幕显示', icon: 'display' },
    { id: 'rgb', title: 'RGB 灯', icon: 'rgb' },
  ]},
  { group: '网络', items: [
    { id: 'wifi', title: 'WiFi 设置', icon: 'wifi' },
    { id: 'push', title: '推送通道', icon: 'push', todo: true },
    { id: 'mqtt', title: 'MQTT', icon: 'mqtt', todo: true },
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
  modem: '<svg viewBox="0 0 24 24"><rect x="3" y="14" width="3" height="7" rx="1"/><rect x="8.5" y="10" width="3" height="11" rx="1"/><rect x="14" y="6" width="3" height="15" rx="1"/><rect x="19.5" y="3" width="3" height="18" rx="1"/></svg>',
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
             '<span>短信转发器</span></div><nav class="menu">';
  for (const g of MENU) {
    html += '<div class="menu-group">' + g.group + '</div>';
    for (const it of g.items) {
      if (it.hidden) continue;  // 隐藏项（如 AT 调试）不进主菜单，仅可经直达链接访问
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
  closeSSE();                                    // 先断推送连接
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
  connectSSE();     // SSE 连接（外壳级单例，跨页面共享）
  route();
}

document.addEventListener('DOMContentLoaded', bootstrap);

/* ================= 页面组件（Alpine x-data） ================= */

/* ---------- 仪表盘 ---------- */
function pageDashboard() {
  return {
    token: '',                // 持久化 API 令牌（GET /api/token 拉取，默认掩码展示）
    tokenShown: false,        // 是否显示完整令牌
    // 全部数据来自 SSE 实时仓（外壳级共享），本页不做任何轮询
    get st() { return Alpine.store('live').status; },
    get stats() { return Alpine.store('live').stats; },
    get modems() { return Alpine.store('live').smsports; },  // 模组状态（含信号/运营商，由 SSE 实时更新）
    get recent() {
      const at = Alpine.store('live').at;
      return at.slice(-5).reverse();
    },
    get smsstats() { return Alpine.store('live').smsstats; },
    get lastSms() { return Alpine.store('live').sms; },
    // 时间格式化：纪元秒 -> "YYYY-MM-DD HH:mm:ss"；否则按开机时长
    fmtTs(ts) {
      if (!ts) return '-';
      if (ts >= 1600000000) {
        const d = new Date(ts * 1000);
        const p = n => (n < 10 ? '0' : '') + n;
        return p(d.getFullYear()) + '-' + p(d.getMonth() + 1) + '-' + p(d.getDate()) + ' ' +
               p(d.getHours()) + ':' + p(d.getMinutes()) + ':' + p(d.getSeconds());
      }
      return '开机 ' + fmtUptime(ts);
    },
    timeSrcText() {
      return { ntp: 'NTP', sim: 'SIM NITZ', nvs: 'NVS 恢复', none: '未同步' }[this.st.timeSrc] || '未同步';
    },
    timeSrcClass() {
      return { ntp: 'ok', sim: 'warn', nvs: 'info', none: 'err' }[this.st.timeSrc] || 'err';
    },
    calibText() {
      switch (this.st.timeSrc) {
        case 'ntp': return 'SNTP 每 24h 自动校准';
        case 'sim': return '模组 NITZ，NTP 恢复后覆盖';
        case 'nvs': return '重启恢复值，等待 NTP / SIM 校准';
        default: return '等待 WiFi 连接或模组就绪';
      }
    },
    // 令牌默认掩码：前 6 + 中间 10 个点 + 后 4；点按后才显示完整
    get tokenMasked() {
      const t = this.token || '';
      if (!t.length) return '';
      if (this.tokenShown) return t;
      return t.slice(0, 6) + '••••••••••' + t.slice(-4);
    },
    async loadToken() {
      try {
        const r = await apiFetch('/api/token');
        if (r.ok) this.token = (await r.json()).token || '';
      } catch (e) {}
    },
    async resetToken() {
      const r = await postOp('/api/token', {}, 'API 令牌已重置');
      if (r) { this.token = (await r.json()).token || ''; this.tokenShown = true; }
    },
    init() {
      this.loadToken();
      // 仓库还是空（SSE 尚未送达）时兜底拉一次
      const live = Alpine.store('live');
      if (!Object.keys(live.status).length) restSnapshot();
    },
    async toggleDisp() {
      const r = await postOp('/api/disp', { on: !this.st.disp });
      if (r) this.st.disp = (await r.json()).on === true;
    },
    async nextLed() {
      await apiOp('/api/led');   // 新状态由 SSE rgb 事件推送
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
    cmd: '', localHist: [], localIdx: -1,
    // 日志与统计来自 SSE 实时仓（与仪表盘共享，切页不丢日志）
    get entries() { return Alpine.store('live').at; },
    get stats() { return Alpine.store('live').stats; },
    async init() {
      try {
        const r = await apiFetch('/api/at/baud');
        if (r.ok) this.baud = String((await r.json()).default || 115200);
      } catch (e) {}
      // 仓库为空（SSE 尚未送达）时兜底拉一次历史
      if (!Alpine.store('live').at.length) restSnapshot();
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
      const r = await apiOp('/api/at/send?' + q.toString());  // 失败 toast
      if (r) {
        const j = await r.json();
        if (j.id !== undefined) {
          // 立即回显；SSE 的 at 事件到达同一 id 时会自动去重
          Alpine.store('live').pushAt([j]);
          this.scrollBottom();
        }
      } else {
        Alpine.store('live').at.push(
          { id: '-', port: '-', cmd: cmd, resp: '请求失败，详见右上角提示' });
      }
      this.cmd = '';
    },
    async clear() {
      if (!await confirmDialog('清空历史', '确定清空全部 AT 历史记录吗？')) return;
      const r = await apiOp('/api/at/clear', null, '已清空历史');
      if (!r) return;
      Alpine.store('live').at = [];
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
    nets: [], scanHint: '', scanning: false,
    ssid: '', pass: '', saving: false, wifiHint: '', wifiIsErr: false,
    scanTimer: null,
    // 状态与已保存列表来自 SSE 实时仓
    get wifi() { return Alpine.store('live').wifi; },
    get saved() { return Alpine.store('live').saved; },
    async init() {
      // 仓库为空（SSE 尚未送达）时兜底拉一次
      const live = Alpine.store('live');
      if (!Object.keys(live.wifi).length) this.refresh();
      if (!live.saved.length) this.loadSaved();
      // 扫描轮询计时器随切页清理
      onCleanup(() => { if (this.scanTimer) { clearInterval(this.scanTimer); this.scanTimer = null; } });
    },
    /* 提示区辅助：错误走红底错误条，普通信息走灰字 */
    wifiErr(msg) { this.wifiHint = msg; this.wifiIsErr = true; },
    wifiBusy(msg) { this.wifiHint = msg; this.wifiIsErr = false; },
    async refresh() {
      try {
        const r = await apiFetch('/api/wifi/status');
        if (r.ok) Alpine.store('live').wifi = await r.json();
      } catch (e) {}
    },
    async loadSaved() {
      try {
        const r = await apiFetch('/api/wifi/list');
        if (r.ok) { const j = await r.json(); Alpine.store('live').saved = j.list || []; }
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
    r: 255, g: 0, b: 0, bright: 100, touched: false,
    // 状态来自 SSE 实时仓
    get st() { return Alpine.store('live').rgb; },
    async init() {
      const live = Alpine.store('live');
      if (typeof live.rgb.bright === 'number') this.bright = live.rgb.bright;
      // 仓库为空（SSE 尚未送达）时兜底拉一次
      if (!Object.keys(live.rgb).length) this.refresh();
    },
    async refresh() {
      try {
        const r = await apiFetch('/api/rgb');
        if (r.ok) {
          const j = await r.json();
          Alpine.store('live').rgb = j;
          if (!this.touched) this.bright = j.bright;  // 拖动中不被回抢
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
        Alpine.store('live').rgb = await r.json();
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
    bright: 80, touched: false,
    // 状态来自 SSE 实时仓
    get st() { return Alpine.store('live').disp; },
    async init() {
      const live = Alpine.store('live');
      if (typeof live.disp.bright === 'number') this.bright = live.disp.bright;
      // 仓库为空（SSE 尚未送达）时兜底拉一次
      if (!Object.keys(live.disp).length) this.refresh();
    },
    async refresh() {
      try {
        const r = await apiFetch('/api/disp');
        if (r.ok) {
          const j = await r.json();
          Alpine.store('live').disp = j;
          if (!this.touched) this.bright = j.bright;  // 用户拖动中不被回抢
        }
      } catch (e) {}
    },
    async toggle() {
      const r = await postOp('/api/disp', { on: !this.st.on });
      if (r) Alpine.store('live').disp = await r.json();
    },
    async applyBright() {
      const r = await postOp('/api/disp', { bright: this.bright });
      if (r) {
        Alpine.store('live').disp = await r.json();
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

/* ---------- 短信 ----------
 * 收发 / 列表 / 删除 + 模组启用管理；新短信经 SSE sms 事件实时刷新列表 */
function pageSms() {
  return {
    modems: [],               // [{port,enabled}]
    sendPort: 1, phone: '', content: '', sending: false,
    sendHint: '', sendErr: false, sendOpen: false,
    q: '', portFilter: -1, page: 1, pageSize: 10,
    total: 0, messages: [], stats: {}, loading: false,
    storeErr: '',
    detail: null,

    init() {
      // SSE 驱动：新短信 -> 刷新列表（无筛选时）；模组配置 -> 同步开关
      this.$watch('$store.live.sms', () => { if (!this.q) this.reload(); });
      this.$watch('$store.live.smsports', v => this.applyPorts(v));
      const live = Alpine.store('live');
      if (live.smsports.length) this.applyPorts(live.smsports);
      else this.loadPorts();
      this.reload();
      this.loadStats();
    },

    /* ---- 模组 ---- */
    applyPorts(list) {
      this.modems = list || [];
      // 当前选择的模组被禁用时，切到第一个启用的模组
      const cur = this.modems.find(m => m.port === this.sendPort);
      if (!cur || !cur.enabled) {
        const first = this.modems.find(m => m.enabled);
        this.sendPort = first ? first.port : 0;
      }
    },
    async loadPorts() {
      try {
        const r = await apiFetch('/api/sms/ports');
        if (r.ok) this.applyPorts((await r.json()).modems || []);
      } catch (e) {}
    },
    enabledPorts() { return this.modems.filter(m => m.enabled); },

    /* ---- 发送（对话框） ---- */
    openSend() {
      // 默认选中第一个启用的模组
      if (!this.enabledPorts().find(m => m.port === this.sendPort)) {
        const first = this.enabledPorts()[0];
        this.sendPort = first ? first.port : 0;
      }
      this.sendHint = ''; this.sendErr = false;
      this.sendOpen = true;
    },
    closeSend() { this.sendOpen = false; },
    async send() {
      const phone = this.phone.trim();
      if (!phone) { this.sendFail('请输入对方号码'); return; }
      if (!this.content.length) { this.sendFail('请输入短信内容'); return; }
      if (!this.sendPort) { this.sendFail('没有可用的模组，请先在「4G 模组」中启用'); return; }
      this.sending = true;
      this.sendHint = '正在发送…';
      this.sendErr = false;
      let r;
      try {
        // 带 120s 超时（单段握手最坏 5s 等 > + 30s 等响应，长短信多段累加；
        // 参考项目无此前端超时——掐断请求时服务端仍在发送，模组已把短信发出，
        // 会造成「界面报失败但其实发送成功」的误判）
        const ctrl = new AbortController();
        const timer = setTimeout(() => ctrl.abort(), 120000);
        try {
          r = await apiFetch('/api/sms/send', {
            method: 'POST',
            headers: { 'Content-Type': 'application/json' },
            body: JSON.stringify({ port: this.sendPort, phone: phone, content: this.content }),
            signal: ctrl.signal,
          });
        } finally { clearTimeout(timer); }
      } catch (e) {
        this.sending = false;
        this.sendFail('请求失败 / 超时：' + (e && e.message ? e.message : e));
        return;
      }
      this.sending = false;
      if (r.ok) {
        this.phone = '';
        this.content = '';
        this.sendOpen = false;
        this.reload();
        this.loadStats();
        // 模组已发出但记录未入库：后端返回 warn，提示但不判失败
        let warn = '';
        try { warn = (await r.json()).warn || ''; } catch (e) {}
        if (warn) toast('发送成功，但' + warn, 'warn');
      } else {
        // 精确回显：HTTP 状态码 + 后端错误原因（如「模组尚未就绪」「号码格式不正确」）
        const msg = await errText(r);
        this.sendFail('发送失败（HTTP ' + r.status + '）：' + msg);
      }
    },
    sendFail(msg) { this.sendHint = msg; this.sendErr = true; this.sending = false; },

    /* ---- 列表 ---- */
    async reload() {
      this.loading = true;
      try {
        const q = new URLSearchParams({ page: this.page, pageSize: this.pageSize });
        if (this.q) q.set('q', this.q);
        if (this.portFilter > 0) q.set('port', this.portFilter);
        const r = await apiFetch('/api/sms?' + q.toString());
        if (r.ok) {
          const j = await r.json();
          this.messages = j.messages || [];
          this.total = j.total || 0;
          // 删除/清空后当前页越界：自动回到最后一页并重新拉取
          const pc = Math.max(1, Math.ceil(this.total / this.pageSize));
          if (this.page > pc) { this.page = pc; this.loading = false; return this.reload(); }
        }
      } catch (e) {}
      this.loading = false;
    },
    async loadStats() {
      try {
        const r = await apiFetch('/api/sms/stats');
        if (r.ok) {
          const j = await r.json();
          this.stats = j;
          // 存储不可用时在页面顶部常驻提示（否则只会看到「暂无记录」）
          this.storeErr = j.ok ? ''
              : ('短信记录存储不可用（' + (j.error || 'unknown') +
                 '）：收发不受影响，但记录不会保存，请检查串口日志中的 [fs]/[sms] 输出');
        }
      } catch (e) {}
    },
    search() { this.page = 1; this.reload(); },
    changePageSize() { this.page = 1; this.reload(); },
    gotoPage(p) { if (p < 1 || p > this.pageCount) return; this.page = p; this.reload(); },
    get pageCount() { return Math.max(1, Math.ceil(this.total / this.pageSize)); },
    get pages() {
      const out = [];
      const pc = this.pageCount;
      let start = Math.max(1, this.page - 2);
      let end = Math.min(pc, start + 4);
      start = Math.max(1, end - 4);
      for (let i = start; i <= end; i++) out.push(i);
      return out;
    },
    async del(id) {
      if (!await confirmDialog('删除记录', '确定删除这条短信记录吗？', true)) return;
      const r = await apiOp('/api/sms?id=' + id, { method: 'DELETE' }, '已删除');
      if (r) { this.reload(); this.loadStats(); }
    },
    async clearAll() {
      if (!await confirmDialog('清空记录', '确定清空全部短信记录吗？此操作不可恢复！', true)) return;
      const r = await apiOp('/api/sms?all=1', { method: 'DELETE' }, '已清空');
      if (r) { this.page = 1; this.reload(); this.loadStats(); }
    },
    showDetail(m) { this.detail = m; },
    fmtTs(ts) {
      return ts >= 1600000000 ? new Date(ts * 1000).toLocaleString()
                              : '开机 ' + fmtUptime(ts);
    },
    // 列表用短时间：MM-DD HH:mm（跨年也看不出，详情里有完整时间）
    fmtShort(ts) {
      if (ts < 1600000000) return '开机 ' + fmtUptime(ts);
      const d = new Date(ts * 1000);
      const p = n => (n < 10 ? '0' : '') + n;
      return p(d.getMonth() + 1) + '-' + p(d.getDate()) + ' ' + p(d.getHours()) + ':' + p(d.getMinutes());
    },
    preview(s) { return s.length > 80 ? s.slice(0, 80) + '…' : s; },
    // 按模组号取显示名（含别名）：记录 / 详情里模组号字段为 modem，需回查别名
    modemLabelOf(port) {
      const m = this.modems.find(x => x.port === port);
      return modemLabel(m || { port: port });
    },
  };
}

/* ---------- 4G 模组（查看 + 设置：状态 / 信号 / 运营商 / 注册 / 号码） ---------- */
function pageModem() {
  return {
    modems: [],
    aliasDraft: {},            // 各模组别名草稿（按 port 暂存输入）
    init() {
      // SSE 推送模组状态（含遥测）时同步
      this.$watch('$store.live.smsports', v => this.applyPorts(v));
      const live = Alpine.store('live');
      if (live.smsports.length) this.applyPorts(live.smsports);
      else this.loadPorts();
    },
    applyPorts(list) {
      this.modems = list || [];
      // 用已保存别名回填草稿（仅当草稿为空，避免覆盖用户输入）
      this.modems.forEach(m => {
        if (this.aliasDraft[m.port] === undefined) this.aliasDraft[m.port] = m.alias || '';
      });
    },
    async loadPorts() {
      try {
        const r = await apiFetch('/api/sms/ports');
        if (r.ok) this.applyPorts((await r.json()).modems || []);
      } catch (e) {}
    },
    async saveAlias(p) {
      const a = (this.aliasDraft[p] ?? '').trim();
      const r = await postOp('/api/sms/ports', { port: p, alias: a }, '别名已保存');
      if (r) {
        this.applyPorts((await r.json()).modems || []);
        this.aliasDraft[p] = a;
      }
    },
    async togglePort(p, enabled) {
      const r = await postOp('/api/sms/ports', { port: p, enabled: enabled },
                             '模组' + p + (enabled ? ' 已启用' : ' 已禁用'));
      if (r) this.applyPorts((await r.json()).modems || []);
    },
    async reinitPort(p) {
      const r = await postOp('/api/sms/reinit', { port: p }, '模组' + p + ' 重新初始化');
      if (r) this.applyPorts((await r.json()).modems || []);
    },
    enabledPorts() { return this.modems.filter(m => m.enabled); },
  };
}
