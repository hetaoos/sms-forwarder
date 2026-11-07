/* ================= 短信转发器 · 应用逻辑 =================
 * 多页结构：index.html 为外壳（左侧菜单 + 顶栏 + 内容区），
 * 右侧内容按 hash 路由懒加载 pages/<id>.html 片段（Alpine 组件渲染）。
 * 认证：Cookie 会话由浏览器自动携带；401 统一跳转独立登录页。
 * ============================================================== */
'use strict';

/* ---------- 小工具 ---------- */
const $ = (s, r) => (r || document).querySelector(s);

/* ---------- 主题（深/浅色 + 跟随系统） ----------
 * 模式存 localStorage 'smsf-theme'：'system' | 'light' | 'dark'，默认 'system'。
 * head 内的 early script 已按 localStorage / 系统偏好抢先设置 <html data-theme> 防闪烁，
 * 这里只负责按钮交互、模式切换与系统主题变化的实时跟随。
 * system 模式下若浏览器不支持 matchMedia 或 prefers-color-scheme，回落浅色。 */
const THEME_KEY = 'smsf-theme';
const SEND_PORT_KEY = 'smsf-send-port';   // 发送短信对话框“上次使用的通信模组”
const SEND_PHONE_KEY = 'smsf-send-phone'; // 发送短信对话框“上次输入/接收的对方号码”
const SMS_QUERY_KEY = 'smsf-sms-query';   // 短信列表查询条件（筛选/搜索/每页条数）
const themeMql = window.matchMedia ? window.matchMedia('(prefers-color-scheme: dark)') : null;

function themeMode() {
  let m = 'system';
  try { m = localStorage.getItem(THEME_KEY) || 'system'; } catch (e) {}
  return (m === 'light' || m === 'dark' || m === 'system') ? m : 'system';
}
// 计算实际生效主题：system -> 读系统偏好（不支持则回落浅色）
function effectiveTheme() {
  if (themeMode() === 'system') return (themeMql && themeMql.matches) ? 'dark' : 'light';
  return themeMode();
}
function applyTheme() {
  document.documentElement.setAttribute('data-theme', effectiveTheme());
  updateThemeBtn();
}
function setThemeMode(mode) {
  try { localStorage.setItem(THEME_KEY, mode); } catch (e) {}
  applyTheme();
}
// 三态循环：system -> light -> dark -> system
function cycleTheme() {
  const order = ['system', 'light', 'dark'];
  setThemeMode(order[(order.indexOf(themeMode()) + 1) % order.length]);
}
// 主题按钮图标与文案：system=半明半暗, light=太阳, dark=月亮
function updateThemeBtn() {
  const btn = document.getElementById('themeBtn');
  if (!btn) return;
  const m = themeMode();
  const eff = effectiveTheme();
  const ICON = {
    system: '<svg viewBox="0 0 24 24"><circle cx="12" cy="12" r="4"/><path d="M12 2v2M12 20v2M2 12h2M20 12h2M5 5l1.5 1.5M17.5 17.5L19 19M19 5l-1.5 1.5M6.5 17.5L5 19"/></svg>',
    light:  '<svg viewBox="0 0 24 24"><circle cx="12" cy="12" r="4"/><path d="M12 2v2M12 20v2M4.9 4.9l1.4 1.4M17.7 17.7l1.4 1.4M2 12h2M20 12h2M4.9 19.1l1.4-1.4M17.7 6.3l1.4-1.4"/></svg>',
    dark:   '<svg viewBox="0 0 24 24"><path d="M21 12.8A9 9 0 1 1 11.2 3a7 7 0 0 0 9.8 9.8z"/></svg>',
  };
  const LABEL = {
    system: '主题：跟随系统（当前' + (eff === 'dark' ? '深色' : '浅色') + '）',
    light: '主题：浅色',
    dark:  '主题：深色',
  };
  btn.innerHTML = ICON[m] || ICON.system;
  btn.title = LABEL[m] || LABEL.system;
  btn.setAttribute('aria-pressed', m !== 'system' ? 'true' : 'false');
}
// 跟随系统：系统偏好变化时，若处于 system 模式则实时更新
if (themeMql) {
  const onSysChange = () => { if (themeMode() === 'system') applyTheme(); };
  if (themeMql.addEventListener) themeMql.addEventListener('change', onSysChange);
  else if (themeMql.addListener) themeMql.addListener(onSysChange);  // 旧版 Safari 兜底
}

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

/* ---- 时间格式化（全站统一，各页面不再各写一套）----
 * ts：纪元秒；小于阈值视为「开机后秒数」，显示为开机时长。
 * 时间一律按「设备配置的时区」显示，不用浏览器本地时区：设备装在北京、
 * 运维在海外时看到的仍是设备时间。状态未到达 / 旧固件按默认东八区兜底。 */
function tzOffsetMin() {
  try {
    const s = window.Alpine && Alpine.store('live') && Alpine.store('live').status;
    const v = s && s.tzOffsetMin;
    return (typeof v === 'number' && v >= -720 && v <= 840) ? v : 480;
  } catch (e) { return 480; }
}
// 把 epoch 秒平移到设备时区后再用 UTC getter 取值，彻底绕开浏览器本地时区
function tzDate(ts) { return new Date((ts + tzOffsetMin() * 60) * 1000); }
function fmtDateTime(ts) {
  if (!ts) return '-';
  if (ts < 1600000000) return '开机 ' + fmtUptime(ts);
  const d = tzDate(ts);
  const p = n => (n < 10 ? '0' : '') + n;
  return p(d.getUTCFullYear()) + '-' + p(d.getUTCMonth() + 1) + '-' + p(d.getUTCDate()) + ' ' +
         p(d.getUTCHours()) + ':' + p(d.getUTCMinutes()) + ':' + p(d.getUTCSeconds());
}
// 列表用短时间：MM-DD HH:mm（详情与日志用完整格式）
function fmtDateShort(ts) {
  if (!ts) return '-';
  if (ts < 1600000000) return '开机 ' + fmtUptime(ts);
  const d = tzDate(ts);
  const p = n => (n < 10 ? '0' : '') + n;
  return p(d.getUTCMonth() + 1) + '-' + p(d.getUTCDate()) + ' ' +
         p(d.getUTCHours()) + ':' + p(d.getUTCMinutes());
}
// 数量单位统一：「123 条」/「12 个」等，避免各页面手拼
function fmtCount(n, unit) {
  if (n == null) return '-';
  return n + (unit || ' 条');
}

// ---- 通信模组状态 / 信号强度展示（仪表盘与短信页共用）----
function modemName(p) { return '通信模组' + p; }
// 显示名称：有别名时「别名（通信模组x）」，否则「通信模组x」；m 可为通信模组对象或 port 数字
function modemLabel(m) {
  const port = (m && typeof m === 'object') ? m.port : m;
  const base = '通信模组' + port;
  const alias = (m && typeof m === 'object' && m.alias) ? String(m.alias).trim() : '';
  return alias ? (alias + '（' + base + '）') : base;
}
// 通信模组型号展示：取「厂商 + 最完整的料号」。CGMM 可能仅前缀（如 ML307A-DSLN），
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
  if (!m) return '';
  if (!m.enabled) return '已禁用';
  switch (m.state) {
    case 'ready': return '已就绪';
    case 'fault': return '故障 / 未连接';
    case 'wait_at': return '等待通信模组响应';
    case 'wait_cpin': return '等待 SIM 卡';
    case 'bring_rf': return '射频启动中';
    case 'wait_reg': return '等待网络注册';
    case 'config': return '下发通信模组配置';
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
function runCleanups() { cleanups.forEach(f => { try { f(); } catch (e) {} }); cleanups = []; clearDirtyGuard(); }

/* ---------- 未保存修改守卫 ----------
 * 配置页在 init 时注册「是否有未保存修改」的判定函数；切页或关闭页面前提示用户，
 * 避免改了一半的表单被静默丢弃。保存成功后由页面自行清标记。
 * 一个页面可能含多个独立组件（如系统设置页的账号 / 令牌 / 定时重启），
 * 因此守卫为列表语义：任一守卫报告「有修改」即触发提示。 */
let dirtyGuards = [];
function setDirtyGuard(fn) { dirtyGuards.push(fn); }
function clearDirtyGuard() { dirtyGuards = []; }
function hasDirty() {
  return dirtyGuards.some(f => { try { return !!f(); } catch (e) { return false; } });
}
// 配置快照：按指定字段生成稳定字符串（仅取「需点保存才生效」的字段）
function snapOf(obj, keys) {
  const o = {};
  for (const k of keys) o[k] = obj ? obj[k] : undefined;
  return JSON.stringify(o);
}

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

/* ---------- 后端错误文案中文化 ----------
 * 固件侧 error 字段已改为中文输出；此表保留英文键，
 * 用于兼容尚未升级的旧固件，未收录的原文原样返回（便于排查新问题）。 */
const ERR_ZH = {
  'unauthorized': '登录已失效，请重新登录',
  '未授权': '登录已失效，请重新登录',
  'bad credentials': '用户名或密码错误',
  'too many sessions': '登录会话过多，请稍后再试',
  'api token disabled': 'API 访问令牌未启用',
  'not found': '接口不存在',
  'invalid json': '请求格式错误',
  'invalid port': '通信模组序号无效',
  'invalid phone': '手机号格式不正确',
  'invalid index': '序号无效',
  'invalid move': '排序参数无效',
  'invalid preset index': '颜色预设序号无效',
  'missing body': '缺少请求内容',
  'missing num': '缺少号码',
  'missing id or all': '缺少短信 ID',
  'missing enabled or alias': '缺少启用状态或别名',
  'key required': '缺少按键参数',
  'unknown key': '未知的按键',
  'pass required': '请填写密码',
  'ssid required': '请填写 WiFi 名称',
  'record not found': '记录不存在',
  'save failed': '保存失败',
  'save failed (max 5)': '保存失败（最多 5 条配置）',
  'config save failed': '配置保存失败',
  'store disabled': '短信存储未启用',
  'log ring not ready': '日志缓冲区未就绪',
  'at history not ready': 'AT 历史缓冲区未就绪',
};
// 翻译一条后端错误描述；未收录的原文原样返回（便于排查新问题）
function zhError(msg) {
  if (!msg) return '';
  const s = String(msg);
  return ERR_ZH[s] || s;
}

// 读取 JSON 错误响应中的 error 字段；非 JSON 时回退到状态码
async function errText(r) {
  try {
    const j = await r.json();
    return zhError(j.error) || ('请求失败（HTTP ' + r.status + '）');
  } catch (e) {
    return '请求失败（HTTP ' + r.status + '）';
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
// SSE 事件 -> 当前打开页面的回调槽位（页面 init 时注册、cleanup 时清空）
let _logSink = null, _outboxSink = null;
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
  smsports: [],         // 通信模组配置 [{port,enabled}]（SSE smsports 事件）
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

function setConn(s) {
  Alpine.store('live').conn = s;
  renderConnDot();   // 顶栏连接灯（静态元素不走 Alpine 绑定，原因见 index.html 注释）
}

// 顶栏连接指示灯：静态元素若用 Alpine 绑定，会因 start() 早于本脚本执行
// （store 未注册）而初始化失败并永久停留在灰色默认态，故在此 imperative 刷新
function renderConnDot() {
  const el = document.getElementById('connDot');
  if (!el) return;
  el.className = 'conn conn-' + Alpine.store('live').conn;
  el.title = connTitle();
}

// 顶栏连接指示灯文案
function connTitle() {
  const c = Alpine.store('live').conn;
  return { online: '实时推送：已连接',
           reconnecting: '实时推送：正在重连…',
           connecting: '实时推送：正在连接…',
           offline: '实时推送：已断开（已降级为轮询）' }[c] || '';
}

// REST 兜底快照：SSE 建立时补拉一次（断线期记录也借此补齐），
// 同时作为不支持 EventSource 时的定时轮询降级
async function restSnapshot() {
  const live = Alpine.store('live');
  try {
    const r = await fetch('/api/status');
    if (r.ok) { live.status = await r.json(); applyFeatureFlags(live.status); }
  } catch (e) {}
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
    toast('浏览器不支持实时推送，已切换为轮询', 'warn');
    fallbackPolling();
    return;
  }
  const live = Alpine.store('live');
  setConn('connecting');
  es = new EventSource('/api/sse');
  const on = (name, fn) => es.addEventListener(name, e => {
    try { fn(JSON.parse(e.data)); } catch (err) {}
  });
  on('status', d => { live.status = d; applyFeatureFlags(d); });
  on('stats', d => { live.stats = d; });
  on('wifi', d => { live.wifi = d; });
  on('saved', d => { live.saved = d.list || []; });
  on('rgb', d => { live.rgb = d; });
  on('disp', d => { live.disp = d; });
  on('at', d => { live.pushAt(d); });
  on('sms', d => { live.sms = d; });
  on('smsports', d => { live.smsports = d.modems || []; });
  on('smsstats', d => { live.smsstats = d; });
  // 运行日志增量（仿 at）与发件箱变更脏信号：转交给当前打开页的槽位
  on('log', d => { if (_logSink) _logSink(d); });
  on('outbox', () => { if (_outboxSink) _outboxSink(); });
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
      toast('实时推送连接失败，已切换为轮询', 'warn');
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
  const wrap = $('#toastWrap');
  if (!wrap) return;
  type = type || '';
  // 前缀符号已由彩色图标表达，去掉避免重复（兼容旧调用带 ✕/✓/⚠ 的情况）
  msg = String(msg).replace(/^[✕✓⚠]\s*/, '');
  const el = document.createElement('div');
  el.className = 'toast ' + type;
  const ico = type === 'err' ? '✕' : type === 'warn' ? '⚠'
            : type === 'ok' ? '✓' : 'ℹ';
  el.innerHTML = '<span class="toast-ico"></span><span class="toast-msg"></span><span class="toast-bar"></span>';
  el.querySelector('.toast-ico').textContent = ico;
  el.querySelector('.toast-msg').textContent = msg;  // 文本走 textContent，防 XSS
  wrap.appendChild(el);
  const ms = duration || (type === 'err' ? 5000 : 3000);
  requestAnimationFrame(() => {
    el.classList.add('show');
    const bar = el.querySelector('.toast-bar');
    if (bar) { bar.style.transitionDuration = ms + 'ms';
      requestAnimationFrame(() => bar.classList.add('run')); }
  });
  setTimeout(() => { el.classList.remove('show'); setTimeout(() => el.remove(), 350); }, ms);
}
function confirmDialog(title, text, danger) {
  return new Promise(resolve => {
    const prev = document.activeElement;      // 关闭后把焦点还给触发元素
    const ov = document.createElement('div');
    ov.className = 'modal-mask';
    ov.setAttribute('role', 'dialog');
    ov.setAttribute('aria-modal', 'true');
    ov.innerHTML = '<div class="modal"><h3>' + escHtml(title) + '</h3><p>' + escHtml(text) +
      '</p><div class="modal-btns"><button class="btn ghost" data-a="0">取消</button>' +
      '<button class="btn' + (danger ? ' danger' : '') + '" data-a="1">确定</button></div></div>';
    document.body.appendChild(ov);
    const btns = [ov.querySelector('[data-a="0"]'), ov.querySelector('[data-a="1"]')].filter(Boolean);
    const done = v => {
      ov.remove();
      try { if (prev && prev.focus) prev.focus(); } catch (e) {}
      resolve(v);
    };
    // 焦点落在「取消」上：默认动作是「不执行」，更安全
    if (btns[0]) btns[0].focus();
    // 焦点陷阱：Tab 在两个按钮之间循环，不逸出对话框
    ov.addEventListener('keydown', e => {
      if (e.key !== 'Tab' || btns.length < 2) return;
      const i = btns.indexOf(document.activeElement);
      e.preventDefault();
      btns[(i + (e.shiftKey ? -1 : 1) + btns.length) % btns.length].focus();
    });
    ov.addEventListener('click', e => {
      const btn = e.target.closest('button');
      if (!btn && e.target !== ov) return;
      done(!!btn && btn.dataset.a === '1');
    });
  });
}

/* ESC 关闭最上层弹窗：取文档中最后一个可见的 .modal-mask 并模拟点击遮罩。
 * Alpine 弹窗（发送短信 / 详情）由 @click.self 关闭；确认框遮罩等同「取消」。 */
document.addEventListener('keydown', e => {
  if (e.key !== 'Escape' || e.defaultPrevented) return;
  const masks = document.querySelectorAll('.modal-mask');
  for (let i = masks.length - 1; i >= 0; i--) {
    const m = masks[i];
    if (getComputedStyle(m).display === 'none') continue;   // 隐藏的弹窗跳过
    m.click();
    e.preventDefault();
    return;
  }
});

/* ---------- 菜单配置（新增页面：加一条 + 放一个 pages/<id>.html） ---------- */
const MENU = [
  { group: '总览', items: [
    { id: 'dashboard', title: '仪表盘', icon: 'dashboard' },
  ]},
  { group: '短信', items: [
    { id: 'sms', title: '短信', icon: 'sms' },
    { id: 'outbox', title: '发件箱', icon: 'outbox' },
    { id: 'admin', title: '管理员', icon: 'admin' },
    { id: 'blacklist', title: '黑名单', icon: 'blacklist' },
    { id: 'modem', title: '通信模组', icon: 'modem' },
  ]},
  { group: '设备控制', items: [
    { id: 'display', title: '屏幕显示', icon: 'display' },
    { id: 'rgb', title: 'RGB 灯', icon: 'rgb' },
    { id: 'serial', title: '模组调试', icon: 'serial' },
  ]},
  { group: '网络', items: [
    { id: 'wifi', title: 'WiFi 设置', icon: 'wifi' },
    { id: 'push', title: '推送通道', icon: 'push' },
    { id: 'email', title: '邮件通知', icon: 'email' },
    { id: 'mqtt', title: 'MQTT', icon: 'mqtt' },
  ]},
  { group: '系统', items: [
    { id: 'settings', title: '系统设置', icon: 'settings' },
    { id: 'tools', title: '系统工具', icon: 'tools' },
    { id: 'logs', title: '运行日志', icon: 'logs' },
  ]},
];

/* 线性图标（内联 SVG，无外部依赖） */
const ICONS = {
  dashboard: '<svg viewBox="0 0 24 24"><rect x="3" y="3" width="7" height="7" rx="1.5"/><rect x="14" y="3" width="7" height="7" rx="1.5"/><rect x="3" y="14" width="7" height="7" rx="1.5"/><rect x="14" y="14" width="7" height="7" rx="1.5"/></svg>',
  serial: '<svg viewBox="0 0 24 24"><rect x="3" y="4" width="18" height="16" rx="2"/><polyline points="7 9 10 12 7 15"/><line x1="13" y1="15" x2="17" y2="15"/></svg>',
  display: '<svg viewBox="0 0 24 24"><rect x="3" y="4" width="18" height="13" rx="2"/><line x1="8" y1="21" x2="16" y2="21"/><line x1="12" y1="17" x2="12" y2="21"/></svg>',
  rgb: '<svg viewBox="0 0 24 24"><circle cx="12" cy="12" r="4"/><path d="M12 2v3M12 19v3M2 12h3M19 12h3M4.9 4.9l2.1 2.1M17 17l2.1 2.1M19.1 4.9L17 7M7 17l-2.1 2.1"/></svg>',
  wifi: '<svg viewBox="0 0 24 24"><path d="M5 12.5a10 10 0 0 1 14 0"/><path d="M8.5 15.8a5.5 5.5 0 0 1 7 0"/><circle cx="12" cy="19" r="1.2"/></svg>',
  email: '<svg viewBox="0 0 24 24"><rect x="3" y="5" width="18" height="14" rx="2"/><polyline points="3 7 12 13 21 7"/></svg>',
  push: '<svg viewBox="0 0 24 24"><path d="M21 3L3 10l7 3 3 7 8-17z"/><path d="M10 13l4-4"/></svg>',
  mqtt: '<svg viewBox="0 0 24 24"><circle cx="5" cy="12" r="2.2"/><circle cx="19" cy="6" r="2.2"/><circle cx="19" cy="18" r="2.2"/><path d="M7 11l10-4M7 13l10 4"/></svg>',
  sms: '<svg viewBox="0 0 24 24"><path d="M4 5h16v11H8l-4 3.5V5z"/></svg>',
  outbox: '<svg viewBox="0 0 24 24"><path d="M3 13h8V4H3z"/><path d="M13 8h7l-2 5h-5z"/><path d="M3 13v6h18v-6"/><path d="M12 4l5 4"/></svg>',
  modem: '<svg viewBox="0 0 24 24"><rect x="3" y="14" width="3" height="7" rx="1"/><rect x="8.5" y="10" width="3" height="11" rx="1"/><rect x="14" y="6" width="3" height="15" rx="1"/><rect x="19.5" y="3" width="3" height="18" rx="1"/></svg>',
  blacklist: '<svg viewBox="0 0 24 24"><circle cx="12" cy="12" r="9"/><line x1="6" y1="6" x2="18" y2="18"/></svg>',
  admin: '<svg viewBox="0 0 24 24"><circle cx="12" cy="8" r="4"/><path d="M4 21c0-4 4-6 8-6s8 2 8 6"/></svg>',
  logs: '<svg viewBox="0 0 24 24"><path d="M6 3h8l4 4v14H6z"/><polyline points="14 3 14 7 18 7"/><line x1="9" y1="12" x2="15" y2="12"/><line x1="9" y1="16" x2="15" y2="16"/></svg>',
  settings: '<svg viewBox="0 0 24 24"><line x1="4" y1="7" x2="20" y2="7"/><circle cx="9" cy="7" r="2"/><line x1="4" y1="17" x2="20" y2="17"/><circle cx="15" cy="17" r="2"/></svg>',
  tools: '<svg viewBox="0 0 24 24"><path d="M14.7 6.3a4.5 4.5 0 0 0-6 6L3 18l3 3 5.7-5.7a4.5 4.5 0 0 0 6-6L14 12l-2-2 2.7-3.7z"/></svg>',
};

function findMenu(id) {
  for (const g of MENU) for (const it of g.items) if (it.id === id) return it;
  return null;
}

/* 编译期功能开关：由 /api/status 的 compiled 字段驱动菜单显隐。
   ENABLE_MQTT=0 时后端返回 mqtt.compiled=false，MQTT 菜单与配置页入口不再出现；
   字段缺省（旧固件）按已编译处理，避免误隐藏。用 style.display 而非 hidden 属性，
   防止 .menu-item 的 display 规则盖掉浏览器默认 [hidden] 样式。 */
function applyFeatureFlags(s) {
  const mqttCompiled = !s || !s.mqtt || s.mqtt.compiled !== false;
  const btn = document.querySelector('.menu-item[data-id="mqtt"]');
  if (btn) btn.style.display = mqttCompiled ? '' : 'none';
  if (!mqttCompiled && location.hash === '#/mqtt') location.hash = '#/dashboard';
}

/* ---------- 外壳渲染 ---------- */
function renderShell() {
  const sb = $('#sidebar');
  let html = '<div class="brand"><span class="logo">S</span>' +
             '<span>短信转发器</span></div><nav class="menu">';
  for (const g of MENU) {
    html += '<div class="menu-group">' + g.group + '</div>';
    for (const it of g.items) {
      // hidden 项仍留在 MENU 里（findMenu 能找到、hash 可直达），只是不进侧栏；
      // 目前无此类页面，预留给后续需要「直达但不展示」的调试页。
      if (it.hidden) continue;
      html += '<button type="button" class="menu-item" data-id="' + escHtml(it.id) + '"' +
              ' title="' + escHtml(it.title) + '">' + ICONS[it.icon] +
              '<span>' + escHtml(it.title) + '</span></button>';
    }
  }
  // 侧栏底部：只放源码仓库入口（不随菜单滚动，所有页面可见）。
  // AP 配网模式无外网时链接打不开，但地址仍可直接看到 / 复制。
  html += '</nav>' +
    '<div class="side-foot">' +
      '<div class="side-label">源码仓库</div>' +
      '<a href="https://cnb.cool/null/sms-forwarder" target="_blank"' +
        ' rel="noopener noreferrer" title="打开源码仓库（新标签页）">' +
        '<svg viewBox="0 0 24 24" aria-hidden="true">' +
          '<path d="M14 4h6v6M20 4l-9 9"/>' +
          '<path d="M18 14v5a1 1 0 0 1-1 1H5a1 1 0 0 1-1-1V7a1 1 0 0 1 1-1h5"/>' +
        '</svg><span>cnb.cool/null/sms-forwarder</span>' +
      '</a>' +
    '</div>';
  sb.innerHTML = html;
  sb.addEventListener('click', e => {
    if (e.target.closest('.side-foot a')) { closeSidebar(); return; }
    const item = e.target.closest('.menu-item');
    if (!item) return;
    location.hash = '#/' + item.dataset.id;
    closeSidebar();
  });
}

function setActiveMenu(id) {
  document.querySelectorAll('.menu-item').forEach(el =>
    el.classList.toggle('active', el.dataset.id === id));
  const cur = document.querySelector('.menu-item.active');
  if (cur) cur.setAttribute('aria-current', 'page');
  else document.querySelectorAll('.menu-item').forEach(el => el.removeAttribute('aria-current'));
  $('#pageTitle').textContent = (findMenu(id) || {}).title || '页面';
}
function openSidebar() { $('#sidebar').classList.add('open'); $('#mask').classList.add('show'); }
function closeSidebar() { $('#sidebar').classList.remove('open'); $('#mask').classList.remove('show'); }

async function loadUser() {
  try {
    const r = await fetch('/api/session');
    if (r.ok) { const j = await r.json(); currentUser = j.user || 'admin'; }
  } catch (e) {}
  $('#userChip').textContent = currentUser;
}

async function logout() {
  if (!await confirmDialog('退出登录', '确定要退出当前登录会话吗？')) return;
  closeSSE();                                    // 先断推送连接
  await fetch('/api/logout', { method: 'POST' }).catch(() => {});
  goLogin();
}

/* ---------- Hash 路由 ---------- */
let currentPageId = '';       // 当前已加载的页面（未保存守卫取消时用于回退 hash）
async function route() {
  // 离开前检查未保存修改：用户确认才放行，否则退回原页面
  if (hasDirty()) {
    const ok = await confirmDialog('未保存的修改',
      '当前页面有未保存的修改，离开将丢失。确定离开吗？', true);
    if (!ok) {
      if (currentPageId) history.replaceState(null, '', '#/' + currentPageId);
      return;
    }
  }
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
  currentPageId = id;
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
  // 右上角用户名：点击前往系统设置（管理账号 / 修改密码）
  $('#userChip').addEventListener('click', () => { location.hash = '#/settings'; });
  $('#menuBtn').addEventListener('click', openSidebar);
  $('#mask').addEventListener('click', closeSidebar);
  // 主题：按钮交互 + 同步图标/文案（data-theme 已由 head early script 抢先设置）
  $('#themeBtn').addEventListener('click', cycleTheme);
  // 页面内任意位置触发退出登录（如系统设置页的入口，移动端顶栏按钮已隐藏）
  document.addEventListener('click', e => {
    const t = e.target && e.target.closest ? e.target.closest('[data-action="logout"]') : null;
    if (t) { e.preventDefault(); logout(); }
  });
  applyTheme();
  window.addEventListener('hashchange', route);
  // 关闭/刷新页面时同样提示未保存修改（浏览器原生提示框）
  window.addEventListener('beforeunload', e => {
    if (!hasDirty()) return;
    e.preventDefault();
    e.returnValue = '';
  });
  if (!location.hash) location.hash = '#/dashboard';
  connectSSE();     // SSE 连接（外壳级单例，跨页面共享）
  renderConnDot();  // 连接灯初态；后续状态变化由 setConn 驱动
  route();
}

document.addEventListener('DOMContentLoaded', bootstrap);

/* ================= 页面组件（Alpine x-data） ================= */

/* ---------- 仪表盘 ---------- */
function pageDashboard() {
  return {
    // 全部数据来自 SSE 实时仓（外壳级共享），本页不做任何轮询
    get st() { return Alpine.store('live').status; },
    get stats() { return Alpine.store('live').stats; },
    get modems() { return Alpine.store('live').smsports; },  // 通信模组状态（含信号/运营商，由 SSE 实时更新）
    get recent() {
      const at = Alpine.store('live').at;
      return at.slice(-5).reverse();
    },
    get smsstats() { return Alpine.store('live').smsstats; },
    get lastSms() { return Alpine.store('live').sms; },
    // MQTT 推送连接状态（取自 SSE status.mqtt，实时更新）
    get mqtt() {
      return Alpine.store('live').status.mqtt ||
        { enabled: false, configured: false, connected: false, error: '' };
    },
    mqttConnClass() {
      const m = this.mqtt;
      if (!m.enabled) return 'err';
      if (!m.configured) return 'warn';
      return m.connected ? 'ok' : 'err';
    },
    mqttConnText() {
      const m = this.mqtt;
      if (!m.enabled) return '已停用';
      if (!m.configured) return '未配置';
      return m.connected ? '已连接' : (m.error ? '连接失败' : '连接中');
    },
    // 时间格式化：全站统一（纪元秒 -> "YYYY-MM-DD HH:mm:ss"；否则按开机时长）
    fmtTs(ts) { return fmtDateTime(ts); },
    timeSrcText() {
      return { ntp: 'NTP', sim: 'SIM NITZ', nvs: '上次保存', none: '未同步' }[this.st.timeSrc] || '未同步';
    },
    timeSrcClass() {
      return { ntp: 'ok', sim: 'warn', nvs: 'info', none: 'err' }[this.st.timeSrc] || 'err';
    },
    calibText() {
      switch (this.st.timeSrc) {
        case 'ntp': return 'SNTP 每 24h 自动校准';
        case 'sim': return '通信模组 NITZ，NTP 恢复后覆盖';
        case 'nvs': return '重启恢复值，等待 NTP / SIM 校准';
        default: return '等待 WiFi 连接或通信模组就绪';
      }
    },
    init() {
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
    // 按模组编号取通信模组对象（modems 来自 SSE smsports 事件）
    modemOf(p) { return (this.modems || []).find(m => m.port === p) || null; },
    // 通信模组信号值（主值行）：禁用 / 初始化中 / dBm
    modemDbm(p) {
      const m = this.modemOf(p);
      if (!m) return '-';
      if (!m.enabled) return '已禁用';
      if (!m.ready) return '初始化中';
      return csqDbm(m.csq);
    },
    fmt, fmtBytes,
  };
}

/* ---------- 定时重启（系统保活） ---------- */
function pageSched() {
  return {
    enabled: false,
    mode: 1,            // 1=固定间隔，0=每日定点
    daily: '04:00',
    intervalH: 24,
    weekdays: 0,        // 位掩码 bit0=周日…bit6=周六；0=每天
    saving: false,
    loaded: false,
    _snap: '',            // 已保存配置快照（用于未保存检测）
    // 星期选项（展示顺序：周一→周日，对应位见 bit）
    days: [
      { bit: 1, name: '周一' }, { bit: 2, name: '周二' }, { bit: 3, name: '周三' },
      { bit: 4, name: '周四' }, { bit: 5, name: '周五' }, { bit: 6, name: '周六' },
      { bit: 0, name: '周日' },
    ],

    init() {
      if (this._sync()) return;
      // 状态尚未到达，短轮询直到 st.sched 出现
      const t = setInterval(() => {
        if (this._sync()) clearInterval(t);
      }, 500);
      setTimeout(() => clearInterval(t), 8000);
    },
    _sync() {
      const s = Alpine.store('live').status.sched;
      if (!s) return false;
      this.enabled = !!s.enabled;
      this.mode = s.mode;
      this.daily = s.daily || '04:00';
      this.intervalH = s.intervalH || 24;
      this.weekdays = s.weekdays || 0;
      this.loaded = true;
      this.markClean();
      setDirtyGuard(() => this.dirty);
      return true;
    },
    dayOn(bit) { return (this.weekdays & (1 << bit)) !== 0; },
    toggleDay(bit) { this.weekdays = this.weekdays ^ (1 << bit); },

    // 未保存检测
    markClean() {
      this._snap = JSON.stringify({
        e: this.enabled, m: this.mode, d: this.daily, h: this.intervalH, w: this.weekdays,
      });
    },
    get dirty() {
      const now = JSON.stringify({
        e: this.enabled, m: this.mode, d: this.daily, h: this.intervalH, w: this.weekdays,
      });
      return !!this._snap && this._snap !== now;
    },

    async save() {
      if (!this.loaded || this.saving) return;
      // 校验：固定间隔模式限制 1~720 小时
      if (Number(this.mode) === 1) {
        const h = Number(this.intervalH);
        if (!(h >= 1 && h <= 720)) { toast('间隔小时需在 1~720 之间', 'err'); return; }
      }
      this.saving = true;
      try {
        // 独立接口：仅保存配置，不重启设备（定时重启按新计划即时生效）
        const r = await postOp('/api/sched', {
          enabled: !!this.enabled,
          mode: this.mode,
          daily: this.daily,
          intervalH: Number(this.intervalH) || 24,
          weekdays: this.weekdays,
        }, '定时重启配置已保存');
        if (r) this.markClean();
      } finally {
        this.saving = false;
      }
    },
  };
}

/* ---------- 系统设置：设备时区 ---------- */
// UTC 偏移选项：-12:00 ~ +14:00 每 30 分钟一档，另补 3 个 45 分钟时区。
const TZ_ALIAS = {
  0: 'UTC', 60: '中欧（柏林 / 巴黎）', 330: '印度', 345: '尼泊尔',
  480: '北京 / 新加坡 / 香港', 525: '澳中西部 Eucla', 540: '东京 / 首尔',
  570: '缅甸', 600: '悉尼', 765: '查塔姆群岛', '-300': '美东（纽约）',
  '-480': '美西（洛杉矶）', '-720': 'UTC-12', 840: 'UTC+14',
};
function tzLabel(m) {
  const a = Math.abs(m);
  const s = 'UTC' + (m >= 0 ? '+' : '-') +
            String(Math.floor(a / 60)).padStart(2, '0') + ':' + String(a % 60).padStart(2, '0');
  return s + (TZ_ALIAS[m] ? ' · ' + TZ_ALIAS[m] : '');
}
function tzOptions() {
  const v = [];
  for (let m = -720; m <= 840; m += 30) v.push(m);
  [345, 525, 765].forEach(m => { if (!v.includes(m)) v.push(m); });
  v.sort((a, b) => a - b);
  return v.map(m => ({ v: m, label: tzLabel(m) }));
}
function pageTimezone() {
  return {
    loaded: false, saving: false, tzOffsetMin: 480, tzText: '', time: '', timeSynced: false,
    options: tzOptions(), _snap: '',
    async init() {
      try {
        const r = await apiFetch('/api/timezone');
        if (r.ok) this._sync(await r.json());
      } catch (e) {}
      this.loaded = true;      // 请求失败也放行，沿用默认东八区
      this.syncSelect();
      setDirtyGuard(() => this.dirty);
    },
    // <option> 由 x-for 动态生成时，x-model 初始化可能早于选项插入，
    // 首屏下拉框会停在第一项而不是当前时区；DOM 就绪后按当前值对齐一次。
    syncSelect() {
      this.$nextTick(() => {
        const el = this.$refs.tzSelect;
        if (el) el.value = String(this.tzOffsetMin);
      });
    },
    _sync(j) {
      this.tzOffsetMin = (typeof j.tzOffsetMin === 'number') ? j.tzOffsetMin : 480;
      this.tzText = j.tzText || '';
      this.time = j.time || '';
      this.timeSynced = !!j.timeSynced;
      this.loaded = true;
      this.markClean();
      this.syncSelect();
    },
    markClean() { this._snap = JSON.stringify({ t: this.tzOffsetMin }); },
    get dirty() { return !!this._snap && this._snap !== JSON.stringify({ t: this.tzOffsetMin }); },
    async save() {
      if (!this.loaded || this.saving) return;
      this.saving = true;
      try {
        const r = await postOp('/api/timezone', { tzOffsetMin: Number(this.tzOffsetMin) }, '时区已保存');
        if (!r) return;
        this._sync(await r.json());
        // 立刻让列表时间按新时区渲染（SSE 下一次 status 快照也会带来新值）
        const st = Alpine.store('live').status;
        st.tzOffsetMin = this.tzOffsetMin;
        st.tzText = this.tzText;
      } finally { this.saving = false; }
    },
  };
}

/* ---------- 模组调试（AT 命令） ---------- */
function pageSerial() {
  return {
    port: '1', baud: '115200', eol: '0', tmo: '1000',
    cmd: '', localHist: [], localIdx: -1,
    filter: 'all',             // 类型过滤：all / 0(用户) / 1(后台) / 2(上报)
    portFilter: 'all',         // 模组过滤：all / 1(模组1) / 2(模组2)
    autoScroll: true,          // 自动滚动到最新（用户上移滚动条较多时自动取消，默认开启）
    atStats: {},               // GET /api/at/stats（存储位置/容量等）
    // 日志与统计来自 SSE 实时仓（与仪表盘共享，切页不丢日志）
    get entries() {
      const all = Alpine.store('live').at;
      const byType = this.filter === 'all'
        ? all : all.filter(e => String(e.type) === this.filter);
      if (this.portFilter === 'all') return byType;
      return byType.filter(e => String(e.port) === this.portFilter);
    },
    get stats() { return Alpine.store('live').stats; },
    async init() {
      try {
        const r = await apiFetch('/api/at/baud');
        if (r.ok) this.baud = String((await r.json()).default || 115200);
      } catch (e) {}
      this.loadAtStats();
      // 仓库为空（SSE 尚未送达）时兜底拉一次历史
      if (!Alpine.store('live').at.length) restSnapshot();
      // 监听滚动条：用户上移较多时自动取消「自动滚动」
      const box = this.$refs.console;
      if (box) box.addEventListener('scroll', () => this.onConsoleScroll());
    },
    // AT 历史存储统计（PSRAM 落点 / 容量），与日志页对称
    async loadAtStats() {
      try {
        const r = await apiFetch('/api/at/stats');
        if (r.ok) this.atStats = await r.json();
      } catch (e) {}
    },
    scrollBottom() {
      if (!this.autoScroll) return;   // 未开启（用户上移较多已自动取消）则不滚动
      this.$nextTick(() => {
        const box = this.$refs.console;
        if (!box) return;
        // 开启即强制滚到最新：不检查 atBottom，否则新增高条目（长 AT 响应）会把距底
        // 撑过阈值而误判为「已离开底部」，导致不再滚动。取消逻辑交由 onConsoleScroll。
        box.scrollTop = box.scrollHeight;
      });
    },
    // 列表变化（SSE 新记录 / 过滤切换）时，若开启自动滚动则滚到底部（由 x-effect 驱动）
    onEntriesChanged() {
      const arr = this.entries;
      void arr.length;
      const last = arr[arr.length - 1];
      void (last ? last.id : 0);   // 依赖末条 id：缓冲满时替换旧记录也能触发
      this.scrollBottom();
    },
    // 用户拖动滚动条：离底超过阈值即取消自动滚动；滚回底部则恢复
    onConsoleScroll() {
      const box = this.$refs.console;
      if (!box) return;
      const atBottom = box.scrollHeight - box.scrollTop - box.clientHeight < 60;
      this.autoScroll = atBottom;
    },
    // 手动勾选「自动滚动」时立即滚到最新
    onAutoScrollChange() {
      if (this.autoScroll) {
        const box = this.$refs.console;
        if (box) box.scrollTop = box.scrollHeight;
      }
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
          { id: '-', port: this.port, cmd: cmd, resp: '请求失败，详见右上角提示' });
      }
      this.cmd = '';
    },
    async clear() {
      if (!await confirmDialog('清空历史', '确定清空全部 AT 历史记录吗？')) return;
      const r = await apiOp('/api/at/clear', null, '已清空历史');
      if (!r) return;
      Alpine.store('live').at = [];
      this.loadAtStats();
    },
    async reset() {
      const p = parseInt(this.port, 10);
      if (!await confirmDialog('复位模组', '将复位模组' + p + ' 的串口连接，确定？', true)) return;
      const r = await apiOp('/api/at/reset?port=' + p, null, '模组' + p + ' 已复位');
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
    // 记录类型文案：0 用户命令 / 1 后台命令 / 2 主动上报
    typeLabel(t) {
      if (t === 1) return '后台';
      if (t === 2) return '上报';
      return '用户';
    },
    typeBadge(t) {
      if (t === 1) return 'bg';
      if (t === 2) return 'urc';
      return 'cmd';
    },
    // 存储位置文案：外部 PSRAM / 回落到内部内存（容量较小，功能不受影响）
    atStoreText() {
      if (!this.atStats.max) return '-';
      return this.atStats.psram ? '外部 PSRAM' : '内部内存';
    },
    atStoreHint() {
      if (this.atStats.psram) return '';
      return '未使用外部 PSRAM，AT 历史容量较小，功能不受影响';
    },
    // 时间格式化：全站统一（纪元秒 -> "YYYY-MM-DD HH:mm:ss"；否则按开机时长）
    fmtTs(ts) { return fmtDateTime(ts); },
    fmt, fmtBytes,
  };
}

/* ---------- WiFi 设置 ---------- */
function pageWifi() {
  return {
    nets: [], scanHint: '', scanning: false,
    ssid: '', pass: '', saving: false, wifiHint: '', wifiIsErr: false,
    scanTimer: null,
    // AP 配网热点密码（运行时可改）
    apPass: '', apSecure: false, apSaving: false, apHint: '', apIsErr: false,
    // 状态与已保存列表来自 SSE 实时仓
    get wifi() { return Alpine.store('live').wifi; },
    get saved() { return Alpine.store('live').saved; },
    async init() {
      // 仓库为空（SSE 尚未送达）时兜底拉一次
      const live = Alpine.store('live');
      if (!Object.keys(live.wifi).length) this.refresh();
      if (!live.saved.length) this.loadSaved();
      await this.loadAp();
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
    // ---- AP 配网热点密码 ----
    async loadAp() {
      try {
        const r = await apiFetch('/api/wifi/appass');
        if (!r.ok) return;
        const j = await r.json();
        this.apSecure = !!j.secure;
        this.apPass = j.pass || '';
        this.apHint = '';
      } catch (e) {}
    },
    apErr(msg) { this.apHint = msg; this.apIsErr = true; },
    apBusy(msg) { this.apHint = msg; this.apIsErr = false; },
    async saveAp() {
      const p = this.apPass || '';
      if (p.length && (p.length < 8 || p.length > 63)) {
        this.apErr('热点密码需 8~63 字符（留空 = 开放热点）');
        return;
      }
      this.apSaving = true;
      this.apBusy('正在保存…');
      const r = await postOp('/api/wifi/appass', { pass: this.apPass });
      this.apSaving = false;
      if (!r) { this.apErr('保存失败，详见右上角提示'); return; }
      const j = await r.json().catch(() => ({}));
      this.apSecure = !!j.secure;
      this.apBusy(this.apSecure ? '已设为加密热点' : '已设为开放热点');
      toast('AP 热点密码已更新', 'ok');
    },
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
      toast('WiFi 配置已保存', 'ok');
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
      // SSE 的 rgb 事件到达时回写亮度滑块：另一个浏览器页（或第三方客户端调用
      // /api/rgb）改了亮度后，本页无需刷新即可跟上；本页正在拖动时不回抢。
      this.$watch('$store.live.rgb', j => {
        if (!j || !Object.keys(j).length) return;
        if (!this.touched && typeof j.bright === 'number') this.bright = j.bright;
      });
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
      if (r) { this.refresh(); toast('已切换预设', 'ok'); }
    },
    async next() {
      const r = await apiOp('/api/led');
      if (r) { this.refresh(); toast('已切换颜色', 'ok'); }
    },
    async applyRaw() {
      const clamp = v => Math.max(0, Math.min(255, v | 0));
      const r = await postOp('/api/rgb',
        { r: clamp(this.r), g: clamp(this.g), b: clamp(this.b) });
      if (r) { this.refresh(); toast('已应用自定义颜色', 'ok'); }
    },
    async off() {
      const r = await postOp('/api/rgb', { c: 0 });
      if (r) { this.refresh(); toast('RGB 已关闭', 'ok'); }
    },
    async applyBright() {
      const r = await postOp('/api/rgb', { bright: this.bright });
      if (r) {
        Alpine.store('live').rgb = await r.json();
        this.bright = this.st.bright;
        toast('亮度已保存', 'ok');
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
    autoOffSec: 0, tAuto: false,   // 无操作自动熄屏（秒，0 = 关闭）+ 拖动中标记
    idleSec: 10, tIdle: false,     // 无操作自动退回状态页（秒，0 = 关闭）+ 拖动中标记
    live: true,            // LCD 实时画面是否轮询刷新
    screenUrl: '',         // 预览 <img> 的 src（带缓存破除数）
    timer: null,
    // 状态来自 SSE 实时仓
    get st() { return Alpine.store('live').disp; },
    init() {
      const live = Alpine.store('live');
      if (typeof live.disp.bright === 'number') this.bright = live.disp.bright;
      this.syncTimers(live.disp);
      // 仓库为空（SSE 尚未送达）时兜底拉一次
      if (!Object.keys(live.disp).length) this.refresh();
      // SSE 的 disp 事件到达时回写本地控件：设备端菜单调亮度/熄屏、或另一个
      // 浏览器页操作后，本页无需刷新即可跟上。正在拖动的那个滑块不回抢。
      this.$watch('$store.live.disp', j => {
        if (!j || !Object.keys(j).length) return;
        if (!this.touched && typeof j.bright === 'number') this.bright = j.bright;
        this.syncTimers(j);
      });
      this.startLive();    // 进入页面即开始 LCD 实时预览轮询
    },
    // 从状态对象回显两项自动行为（拖动中不回抢，与亮度滑块同一套路）
    syncTimers(j) {
      if (!j) return;
      if (!this.tAuto && typeof j.autoOffSec === 'number') this.autoOffSec = j.autoOffSec;
      if (!this.tIdle && typeof j.idleSec === 'number') this.idleSec = j.idleSec;
    },
    // 秒值人性化显示：0 -> 关闭；<60 -> N 秒；<3600 -> N 分 [N 秒]；其余 -> N 小时
    secText(v) {
      v = Number(v) || 0;
      if (!v) return '关闭';
      if (v < 60) return v + ' 秒';
      if (v < 3600) {
        const m = Math.floor(v / 60), s = v % 60;
        return s ? (m + ' 分 ' + s + ' 秒') : (m + ' 分钟');
      }
      return (v / 3600) + ' 小时';
    },
    // LCD 实时画面：每 ~700ms 重新抓取一帧（约 1.4fps），缓存破除数避免命中旧图
    startLive() {
      this.live = true;
      this.refreshScreen();
    },
    refreshScreen() {
      if (!this.live) return;
      this.screenUrl = '/api/screen?t=' + Date.now();
      this.timer = setTimeout(() => this.refreshScreen(), 700);
    },
    toggleLive() {
      if (this.live) {
        this.live = false;
        if (this.timer) { clearTimeout(this.timer); this.timer = null; }
      } else {
        this.startLive();
      }
    },
    async refresh() {
      try {
        const r = await apiFetch('/api/disp');
        if (r.ok) {
          const j = await r.json();
          Alpine.store('live').disp = j;
          if (!this.touched) this.bright = j.bright;  // 用户拖动中不被回抢
          this.syncTimers(j);
        }
      } catch (e) {}
    },
    async toggle() {
      const r = await postOp('/api/disp', { on: !this.st.on });
      if (r) {
        Alpine.store('live').disp = await r.json();
        toast(this.st.on ? '屏幕已关闭' : '屏幕已打开', 'ok');
      }
    },
    async applyBright() {
      const r = await postOp('/api/disp', { bright: this.bright });
      if (r) {
        Alpine.store('live').disp = await r.json();
        this.bright = this.st.bright;
        toast('亮度已保存', 'ok');
      }
      this.touched = false;
    },
    // 自动行为：松开滑块才提交，服务端钳制后回显实际值（落 NVS，重启后保持）
    async applyIdle() {
      const r = await postOp('/api/disp', { idleSec: this.idleSec });
      if (r) {
        const j = await r.json();
        this.tIdle = false;
        Alpine.store('live').disp = j;
        this.syncTimers(j);
        toast('自动返回已保存', 'ok');
      }
      this.tIdle = false;
    },
    async applyAutoOff() {
      const r = await postOp('/api/disp', { autoOffSec: this.autoOffSec });
      if (r) {
        const j = await r.json();
        this.tAuto = false;
        Alpine.store('live').disp = j;
        this.syncTimers(j);
        toast('自动关屏已保存', 'ok');
      }
      this.tAuto = false;
    },
    // 远程按键：等同于设备物理 4 键，驱动屏幕菜单（与本地按键同一处理路径）
    async pressKey(k) {
      try { await postOp('/api/key', { key: k }); } catch (e) {}
    },
  };
}

/* ---------- 系统设置 · 管理账号 ---------- */
function pageAuth() {
  return {
    user: '', newUser: '', newPass: '', oldPass: '',
    saving: false, done: false,
    hint: '', isErr: false, errField: '',
    _snap: '',            // 已加载值快照（用于未保存检测）
    async init() {
      try {
        const r = await apiFetch('/api/auth');
        if (r.ok) { const j = await r.json(); this.user = j.user || ''; this.newUser = this.user; }
      } catch (e) {}
      this.markClean();
      setDirtyGuard(() => this.dirty);
    },
    // 未保存检测：新用户名 / 新密码 / 当前密码任一被改动即视为未保存
    markClean() { this._snap = JSON.stringify({ u: this.newUser, p: '', o: '' }); },
    get dirty() {
      const now = JSON.stringify({ u: this.newUser, p: this.newPass, o: this.oldPass });
      return !!this._snap && this._snap !== now;
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

/* ---------- 系统设置 · API 访问令牌 ---------- */
function pageApiToken() {
  return {
    enabled: false, token: '', tokenShown: false,
    loaded: false, saving: false,
    _snap: '',            // 已保存开关状态快照（用于未保存检测）
    // 拉取当前令牌配置（开关 + 令牌），启用后据此刷新显示
    async refresh() {
      try {
        const r = await apiFetch('/api/token');
        if (r.ok) {
          const j = await r.json();
          this.enabled = !!j.enabled;
          this.token = j.token || '';
        }
      } catch (e) {}
    },
    async init() {
      await this.refresh();
      this.loaded = true;
      this.markClean();
      setDirtyGuard(() => this.dirty);
    },
    // 未保存检测：仅开关状态需要保存
    markClean() { this._snap = String(this.enabled); },
    get dirty() { return !!this._snap && this._snap !== String(this.enabled); },
    // 令牌掩码：仅 reset 后瞬时持有 raw 时可显示掩码；平时只提示已隐藏
    get tokenMasked() {
      const t = this.token || '';
      if (t.length) return this.tokenShown ? t : (t.slice(0, 6) + '••••••••••' + t.slice(-4));
      return this.enabled ? '已隐藏（点「重置令牌」可见一次）' : '';
    },
    // 示例命令：默认演示 challenge→exchange 换票据（纯 HTTP 下不暴露 raw 令牌）；
    // 若刚点过「显示完整」（持有 raw），则一并填入真实值，下方兼容行才可用。
    get curlCmd() {
      const origin = location.origin || 'http://设备IP';
      const raw = (this.tokenShown && this.token) ? this.token : '<RAW_TOKEN>';
      return [
        '# 推荐：challenge → exchange 换短时效票据（不暴露 raw 令牌）',
        'CH=$(curl -s ' + origin + '/api/token/challenge)',
        'NONCE=$(echo "$CH" | jq -r .nonce); TS=$(echo "$CH" | jq -r .ts)',
        'V=$(printf %s "' + raw + '" | sha256sum | cut -d" " -f1)',
        'PROOF=$(printf "%s:%s:%s" "$V" "$TS" "$NONCE" | sha256sum | cut -d" " -f1)',
        'TICKET=$(curl -s -X POST ' + origin + '/api/token/exchange -H "Content-Type: application/json" \\',
        '  -d "{\\"v\\":2,\\"nonce\\":\\"$NONCE\\",\\"ts\\":$TS,\\"proof\\":\\"$PROOF\\"}" | jq -r .ticket)',
        'curl -H "Authorization: Bearer $TICKET" ' + origin + '/api/status',
        '',
        '# 兼容（默认开启）：直接带 raw 令牌（明文传输，建议迁移到上方）',
        'curl -H "Authorization: Bearer ' + raw + '" ' + origin + '/api/status'
      ].join('\n');
    },
    async copyCmd() {
      const txt = this.curlCmd;
      // 设备多为 http 明文来源，navigator.clipboard 不可用，回落到 execCommand
      try {
        if (navigator.clipboard && window.isSecureContext) {
          await navigator.clipboard.writeText(txt);
        } else {
          const ta = document.createElement('textarea');
          ta.value = txt;
          ta.style.position = 'fixed';
          ta.style.opacity = '0';
          document.body.appendChild(ta);
          ta.select();
          if (!document.execCommand('copy')) throw new Error('execCommand failed');
          ta.remove();
        }
        toast('已复制示例命令', 'ok');
      } catch (e) {
        toast('✕ 复制失败，请手动选择文本', 'err');
      }
    },
    async save() {
      if (this.saving || !this.loaded) return;
      this.saving = true;
      try {
        const r = await apiFetch('/api/token', {
          method: 'POST',
          headers: { 'Content-Type': 'application/json' },
          body: JSON.stringify({ enabled: this.enabled })
        });
        if (!r.ok) toast('✕ 保存失败（' + r.status + '）', 'err');
        else { await this.refresh(); this.markClean(); toast('已保存', 'ok'); }
      } catch (e) {
        toast('✕ 请求失败', 'err');
      }
      this.saving = false;
    },
    async resetToken() {
      if (!this.enabled) return;
      try {
        const r = await apiFetch('/api/token/reset', { method: 'POST' });
        if (r.ok) {
          const j = await r.json();
          this.token = j.token || '';
          this.tokenShown = true;
          toast('API 令牌已重置', 'ok');
        } else {
          toast('✕ 重置失败（' + r.status + '）', 'err');
        }
      } catch (e) {
        toast('✕ 请求失败', 'err');
      }
    },
  };
}

/* ---------- 短信 ----------
 * 收发 / 列表 / 删除 + 通信模组启用管理；新短信经 SSE sms 事件推送，首页无筛选时增量并入（免整页重拉） */
function pageSms() {
  return {
    modems: [],               // [{port,enabled,alias,ready}]
    sendPort: 0, phone: '', content: '', sending: false,
    sendHint: '', sendErr: false, sendOpen: false,
    q: '', portFilter: -1, page: 1, pageSize: 10,
    total: 0, messages: [], stats: {}, loading: false, lastId: 0,
    storeErr: '',
    detail: null,

    init() {
      // 恢复上次保存的查询条件（筛选/搜索/每页条数），避免重新打开页面丢失
      this.loadQuery();
      // 新短信：SSE sms 事件只给最新一条（含完整正文）。首页且未筛选时优先纯前端增量并入，
      // 免去整页重拉；多条/断档或处于筛选/翻页态时再整页对账，避免漏推或分页错乱。
      this.$watch('$store.live.sms', (d) => {
        const id = d && d.id;
        if (!id) return;
        if (this.q || this.portFilter > 0) { this.reload(); return; }   // 筛选态须整页重拉
        if (id <= this.lastId) return;                                  // 旧事件或重复
        if (this.page !== 1) { this.total = (this.total || 0) + 1; this.lastId = id; return; }  // 非首页只更新计数
        if (id === this.lastId + 1) {                                   // 单条新增：增量并入
          this.messages = [d, ...this.messages];
          if (this.messages.length > this.pageSize) this.messages.pop();
          this.total = (this.total || 0) + 1;
          this.lastId = id;
        } else {
          this.reload();                                                // 多条/断档：整页对账
        }
      });
      this.$watch('$store.live.smsports', v => this.applyPorts(v));
      // 查询条件变化时落盘，重开页面可还原
      this.$watch('q', () => this.saveQuery());
      this.$watch('portFilter', () => this.saveQuery());
      this.$watch('pageSize', () => this.saveQuery());
      const live = Alpine.store('live');
      if (live.smsports.length) this.applyPorts(live.smsports);
      else this.loadPorts();
      this.reload();
      this.loadStats();
    },

    /* ---- 通信模组 ---- */
    // 默认通信模组：上次使用的（仍可用则优先）否则第一个启用的
    defaultSendPort() {
      const enabled = this.enabledPorts();
      if (!enabled.length) return 0;
      let saved = 0;
      try { saved = parseInt(localStorage.getItem(SEND_PORT_KEY) || '0', 10) || 0; } catch (e) {}
      if (saved && enabled.find(m => m.port === saved)) return saved;
      return enabled[0].port;
    },
    applyPorts(list) {
      this.modems = list || [];
      // 当前选择的通信模组被禁用时，回落到上次使用 / 第一个启用的通信模组
      const cur = this.modems.find(m => m.port === this.sendPort);
      if (!cur || !cur.enabled) {
        this.sendPort = this.defaultSendPort();
      }
    },
    async loadPorts() {
      try {
        const r = await apiFetch('/api/sms/ports');
        if (r.ok) this.applyPorts((await r.json()).modems || []);
      } catch (e) {}
    },
    enabledPorts() { return this.modems.filter(m => m.enabled); },

    /* ---- 发送（对话框）：与发件箱页同款 ---- */
    async openSend() {
      // 打开时刷新一次通信模组状态，保证 alias / ready 与页面中显示的一致
      await this.loadPorts();
      // 默认选中：上次使用的通信模组（仍可用）或第一个启用的通信模组
      this.sendPort = this.defaultSendPort();
      // 恢复上次保存的对方号码，避免重复输入
      try { this.phone = localStorage.getItem(SEND_PHONE_KEY) || ''; } catch (e) {}
      this.sendHint = ''; this.sendErr = false;
      this.sendOpen = true;
      // 打开后聚焦号码输入框，便于直接输入
      this.$nextTick(() => {
        const el = this.$refs && this.$refs.phoneInput;
        if (el && el.focus) el.focus();
      });
    },
    closeSend() { this.sendOpen = false; },
    savePhone() {
      try { localStorage.setItem(SEND_PHONE_KEY, this.phone || ''); } catch (e) {}
    },
    clearPhone() {
      this.phone = '';
      try { localStorage.removeItem(SEND_PHONE_KEY); } catch (e) {}
    },
    // POST /api/sms/send 只负责把短信登记到持久化发件箱并立即返回 started，
    // 实际 CMGS 由 sms_send worker 执行，投递结果去「发件箱」页看，故不再轮询
    // GET /api/sms/send（该接口保留兼容）。与 pageOutbox().send() 行为一致。
    async send() {
      const phone = this.phone.trim();
      if (!phone) { this.sendFail('请输入对方号码'); return; }
      if (!this.content.length) { this.sendFail('请输入短信内容'); return; }
      if (!this.sendPort) { this.sendFail('没有可用的通信模组，请先在「通信模组」中启用'); return; }
      this.sending = true; this.sendErr = false; this.sendHint = '正在登记…';
      let r;
      try {
        r = await postJson('/api/sms/send', {
          port: this.sendPort, phone: phone, content: this.content
        });
      } catch (e) {
        this.sendFail('请求失败：' + (e && e.message ? e.message : e));
        return;
      }
      if (!r.ok) {
        this.sendFail('提交失败：' + await errText(r));
        return;
      }
      this.sending = false;
      this.sendOpen = false;
      this.content = '';
      this.savePhone();
      try { localStorage.setItem(SEND_PORT_KEY, String(this.sendPort)); } catch (e) {}
      toast('已加入发件箱，等待通信模组发送', 'ok');
      this.reload();
      this.loadStats();
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
          this.lastId = this.messages.length ? this.messages[0].id : 0;
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
              : ('短信记录存储不可用（' + (zhError(j.error) || '未知原因') +
                 '）：收发不受影响，但记录不会保存，请检查串口日志中的 [fs]/[sms] 输出');
        }
      } catch (e) {}
    },
    search() { this.page = 1; this.reload(); },
    // 查询条件本地持久化：重开页面自动还原（仅 q / portFilter / pageSize，page 归 1）
    loadQuery() {
      try {
        const s = JSON.parse(localStorage.getItem(SMS_QUERY_KEY) || 'null');
        if (s && typeof s === 'object') {
          if (typeof s.q === 'string') this.q = s.q;
          if (Number.isFinite(s.portFilter)) this.portFilter = s.portFilter;
          if ([10, 20, 50].includes(s.pageSize)) this.pageSize = s.pageSize;
        }
      } catch (e) {}
    },
    saveQuery() {
      try {
        localStorage.setItem(SMS_QUERY_KEY, JSON.stringify({
          q: this.q || '', portFilter: this.portFilter, pageSize: this.pageSize,
        }));
      } catch (e) {}
    },
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

    // 把正文里的网址识别成可点击链接（新窗口打开）；先转义防 XSS。
    // 匹配范围：http(s):// 完整网址、www. 开头、以及裸域名（example.com / sub.example.co.uk 等，
    // 末段为字母 TLD，故不误伤纯数字 1.2.3、IP 192.168.0.1；结尾标点自动留在链接外）。
    linkify(text) {
      if (!text) return '';
      const esc = s => s.replace(/[&<>"']/g, c =>
        ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' }[c]));
      const safe = esc(text);
      const re = /(https?:\/\/[^\s<>"']+|www\.[^\s<>"']+|[a-z0-9]+(?:[.-][a-z0-9]+)*\.[a-z]{2,}(?:\/[^\s<>"'()]*)?)/gi;
      return safe.replace(re, (m) => {
        const href = /^https?:\/\//i.test(m) ? m : 'http://' + m;
        return '<a href="' + href + '" target="_blank" rel="noopener noreferrer">' + m + '</a>';
      });
    },

    fmtTs(ts) { return fmtDateTime(ts); },
    // 列表用短时间：MM-DD HH:mm（跨年也看不出，详情里有完整时间）
    fmtShort(ts) { return fmtDateShort(ts); },
    preview(s) { return s.length > 80 ? s.slice(0, 80) + '…' : s; },
    // 按通信模组号取显示名（含别名）：记录 / 详情里通信模组号字段为 modem，需回查别名
    modemLabelOf(port) {
      const m = this.modems.find(x => x.port === port);
      return modemLabel(m || { port: port });
    },
  };
}

/* ---------- 管理员短信远程控制（独立页面，支持多个号码）----------
 * 配置经 /api/sms/admin 读写：GET 读 {enabled,phones}，POST {enabled,phones} 写。
 * 与短信页解耦后独立成页，菜单置于「黑名单」之前。 */
function pageAdmin() {
  return {
    admins: [], adminEnabled: false, adminSaving: false, adminHint: '', adminErr: false,
    loaded: false,
    _snap: '',               // 已保存号码列表快照（用于未保存检测）

    init() {
      this.loadAdmin();
      this.$watch('admins', () => { this.adminErr = false; });
    },

    async loadAdmin() {
      try {
        const r = await apiFetch('/api/sms/admin');
        if (r.ok) {
          const data = await r.json();
          this.admins = (data.phones || []).map(p => (p || '').toString());
          this.adminEnabled = !!data.enabled;
          this.loaded = true;
          this.markClean();
          setDirtyGuard(() => this.dirty);
        }
      } catch (e) {}
    },

    // 未保存检测：号码列表变化即视为未保存（启用开关随保存一并写入）
    markClean() { this._snap = JSON.stringify(this.admins); },
    get dirty() {
      return !!this._snap && this._snap !== JSON.stringify(this.admins);
    },

    // 保存前校验：号码非空、格式合法（数字与 + 号，最多 20 位）
    validate() {
      for (let i = 0; i < this.admins.length; i++) {
        const p = (this.admins[i] || '').trim();
        if (!p) return '第 ' + (i + 1) + ' 个号码为空（请填写或删除该行）';
        if (!/^\+?[0-9]{3,20}$/.test(p))
          return '第 ' + (i + 1) + ' 个号码格式不正确：' + p;
      }
      return '';
    },

    async saveAdmin() {
      const bad = this.validate();
      if (bad) {
        this.adminErr = true;
        this.adminHint = bad;
        toast(bad, 'err');
        return;
      }
      this.adminSaving = true;
      this.adminErr = false;
      this.adminHint = '';
      // 丢弃空条目、去重（前端体验），后端再严格校验格式与上限
      const phones = [...new Set(this.admins.map(p => (p || '').trim()).filter(Boolean))];
      const r = await apiOp('/api/sms/admin', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ enabled: this.adminEnabled, phones }),
      }, this.adminEnabled
          ? (phones.length ? '管理员远程控制已开启' : '已开启（未设置号码，暂不会生效）')
          : '已关闭短信远程控制');
      this.adminSaving = false;
      if (r && r.phones) {
        this.admins = r.phones.map(p => (p || '').toString());
        this.adminEnabled = !!r.enabled;
        this.markClean();
      } else if (!r) {
        this.adminErr = true;
        this.adminHint = '保存失败（号码格式错误或超出上限）';
      }
    },
  };
}

/* ---------- 通信模组（查看 + 设置：状态 / 信号 / 运营商 / 注册 / 号码） ---------- */
function pageModem() {
  return {
    modems: [],
    aliasDraft: {},            // 各通信模组别名草稿（按 port 暂存输入）
    loaded: false,
    err: '',                 // 校验错误（定时查询表单）
    _snapPoll: '',            // 定时查询日程快照
    _snapSafety: '',          // 兜底轮询周期快照
    init() {
      // SSE 推送通信模组状态（含遥测）时同步
      this.$watch('$store.live.smsports', v => this.applyPorts(v));
      const live = Alpine.store('live');
      if (live.smsports.length) this.applyPorts(live.smsports);
      else this.loadPorts();
      this.loadQueryStatus();
      this.loadStore();
      this.loadPoll();
      this.$watch('poll', () => { this.err = ''; });
    },
    applyPorts(list) {
      this.modems = list || [];
      // 用已保存别名回填草稿（仅当草稿为空，避免覆盖用户输入）
      this.modems.forEach(m => {
        if (this.aliasDraft[m.port] === undefined) this.aliasDraft[m.port] = m.alias || '';
      });
      this.syncPingPort();
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
                             '通信模组' + p + (enabled ? ' 已启用' : ' 已禁用'));
      if (r) this.applyPorts((await r.json()).modems || []);
    },
    async reinitPort(p) {
      const r = await postOp('/api/sms/reinit', { port: p }, '通信模组' + p + ' 重新初始化');
      if (r) this.applyPorts((await r.json()).modems || []);
    },
    enabledPorts() { return this.modems.filter(m => m.enabled); },

    // ---- 网络连通性测试（PING）----
    pingBusy: false,
    pingResult: '',
    pingOk: false,
    pingPort: 0,
    pingHost: '',
    _pollTimer: null,
    // 默认选中第一个已启用模组；端口列表刷新后若当前选择失效则回落
    syncPingPort() {
      const valid = this.modems.some(m => m.port === this.pingPort && m.enabled);
      if (!valid) {
        const e = this.enabledPorts()[0];
        this.pingPort = e ? e.port : 0;
      }
    },
    async startPing() {
      if (!this.pingPort) { toast('请选择通信模组', 'err'); return; }
      if (this.pingBusy) return;
      const r = await postOp('/api/sms/ping',
                             { port: this.pingPort, host: this.pingHost || '' }, null);
      if (!r) return;
      this.pingBusy = true;
      this.pingResult = '';
      this.pollPing();
    },
    // 轮询 /api/sms/ping 直到 busy=false（超时上限 ~60s）
    pollPing() {
      clearTimeout(this._pollTimer);
      let count = 0;
      const tick = async () => {
        try {
          const r = await apiFetch('/api/sms/ping', { cache: 'no-store' });
          if (r.ok) {
            const j = await r.json();
            if (!j.busy) {
              this.pingBusy = false;
              this.pingOk = !!j.ok;
              this.pingResult = j.report || '';
              return;
            }
          }
        } catch (e) {}
        if (++count > 60) {   // 安全上限：约 60s
          this.pingBusy = false;
          this.pingOk = false;
          this.pingResult = 'PING 超时（后端未返回结果）';
          return;
        }
        this._pollTimer = setTimeout(tick, 1000);
      };
      tick();
    },

    // ---- 主动查询短信（通信模组存储兜底）----
    queryStatus: { storeMode: true, deleteAfterRead: true, ports: [] },
    queryPort: 0,             // 0 = 全部模组
    queryBusy: false,
    clearBusy: 0,             // 正在清空 SIM 的模组号（0 = 无）
    _qTimer: null,
    // 短信接收模式：true = 存储模式（CNMI=2,1，配合主动查询）
    storeMode: true,
    async loadStore() {
      try {
        const r = await apiFetch('/api/sms/store');
        if (r.ok) this.storeMode = !!(await r.json()).store;
      } catch (e) {}
    },
    async saveStore() {
      const r = await postOp('/api/sms/store', { store: !!this.storeMode },
                             '短信接收模式已切换');
      if (!r) {
        await this.loadStore();   // 保存失败：回滚开关
        return;
      }
      this.loadQueryStatus();
    },
    async loadQueryStatus() {
      try {
        const r = await apiFetch('/api/sms/query', { cache: 'no-store' });
        if (r.ok) this.queryStatus = await r.json();
      } catch (e) {}
    },
    queryPorts() { return (this.queryStatus.ports || []).filter(p => p.enabled); },
    // 兜底轮询周期文案（后端实时值）
    safetyText() {
      const sec = this.queryStatus.safetySec || 60;
      return sec < 120 ? (sec + ' 秒') : (Math.round(sec / 60) + ' 分钟');
    },
    queryStateText(q) {
      if (q.running) return '查询中';
      if (q.pending) return '待查询';
      return '空闲';
    },
    // 触发查询：port=0 表示所有启用模组；查询由后端任务异步执行
    async startQuery(port) {
      if (this.queryBusy) return;
      if (port && !this.queryPort) return;
      const r = await postOp('/api/sms/query', port ? { port: port } : {}, '已触发查询');
      if (!r) return;
      this.queryBusy = true;
      this.pollQuery();
    },
    // 清空单个模组 SIM 上的全部短信（危险操作，需二次确认）
    async startClear(port) {
      if (this.queryBusy || this.clearBusy) return;
      const label = modemLabel({ port: port });
      if (!await confirmDialog('清空 SIM 短信',
          '确定清空 ' + label + ' SIM 上的全部短信吗？包含尚未读回/转发的短信，操作不可恢复！', true)) return;
      const r = await postOp('/api/sms/clear', { port: port }, '已开始清空 ' + label + ' 的 SIM 短信');
      if (!r) return;
      this.clearBusy = port;
      this.queryBusy = true;
      this.pollQuery();
    },
    clearStateClass(q) {
      if (q.clearBusy) return 'warn';
      if (!q.clearLastMs) return 'info';
      return q.clearLastOk ? 'ok' : 'err';
    },
    clearStateText(q) {
      if (q.clearBusy) return '清空中';
      if (!q.clearLastMs) return '未执行';
      return q.clearLastOk ? '已清空' : '清空失败';
    },
    clearResultText(q) {
      if (!q.clearLastMs) return '';
      return q.clearLastOk
        ? '上次清空成功，删除 ' + (q.clearLastDeleted || 0) + ' 条短信'
        : '上次清空失败：' + (q.clearLastErr || '未知原因') + '（可查看 AT 历史）';
    },
    // 轮询查询/清空状态直到无端口在执行（普通查询 ~30s，清空 SIM 放宽到 ~90s）
    pollQuery() {
      clearTimeout(this._qTimer);
      let count = 0;
      const tick = async () => {
        await this.loadQueryStatus();
        const busy = (this.queryStatus.ports || []).some(
          p => p.enabled && (p.running || p.clearBusy));
        if (!busy && count > 0) {
          this.queryBusy = false;
          if (this.clearBusy) {
            const q = (this.queryStatus.ports || []).find(p => p.port === this.clearBusy);
            if (q && q.clearLastMs) {
              toast((q.clearLastOk ? '✓ ' : '✕ ') + this.clearResultText(q),
                    q.clearLastOk ? 'ok' : 'err');
            }
            this.clearBusy = 0;
          }
          return;
        }
        if (++count > (this.clearBusy ? 90 : 30)) {
          this.queryBusy = false; this.clearBusy = 0; return;
        }
        this._qTimer = setTimeout(tick, 1000);
      };
      tick();
    },

    // ---- 定时主动查询 ----
    poll: { enabled: false, mode: 1, intervalH: 24, daily: '04:00', safetySec: 60 },
    pollSaving: false,
    async loadPoll() {
      try {
        const r = await apiFetch('/api/sms/poll');
        if (r.ok) {
          const j = await r.json();
          this.poll.enabled = !!j.enabled;
          this.poll.mode = j.mode;
          this.poll.intervalH = j.intervalH || 24;
          this.poll.safetySec = j.safetySec || 60;
          const hh = String(j.hour == null ? 4 : j.hour).padStart(2, '0');
          const mm = String(j.minute == null ? 0 : j.minute).padStart(2, '0');
          this.poll.daily = hh + ':' + mm;
          this.loaded = true;
          this.markClean();
          setDirtyGuard(() => this.dirty);
        }
      } catch (e) {}
    },

    // ---- 未保存检测：定时日程与兜底周期各自独立 ----
    markCleanPoll() {
      const p = this.poll;
      this._snapPoll = JSON.stringify({ e: p.enabled, m: p.mode, h: p.intervalH, d: p.daily });
    },
    markCleanSafety() { this._snapSafety = String(this.poll.safetySec); },
    markClean() { this.markCleanPoll(); this.markCleanSafety(); },
    get dirtySchedule() {
      const p = this.poll;
      const now = JSON.stringify({ e: p.enabled, m: p.mode, h: p.intervalH, d: p.daily });
      return !!this._snapPoll && this._snapPoll !== now;
    },
    get dirtySafety() {
      return !!this._snapSafety && this._snapSafety !== String(this.poll.safetySec);
    },
    get dirty() { return this.dirtySchedule || this.dirtySafety; },

    // 仅保存兜底轮询周期（不影响定时查询日程）
    async saveSafety() {
      if (this.pollSaving) return;
      const sec = Number(this.poll.safetySec);
      if (!(sec >= 30 && sec <= 3600)) {
        this.err = '兜底轮询周期需在 30~3600 秒之间';
        toast(this.err, 'err');
        return;
      }
      this.pollSaving = true;
      try {
        const r = await postOp('/api/sms/poll',
                               { safetySec: sec },
                               '兜底轮询周期已保存');
        if (r) { this.poll.safetySec = sec; this.markCleanSafety(); }
      } finally {
        this.pollSaving = false;
      }
    },

    // 定时查询表单校验
    validate() {
      if (Number(this.poll.mode) === 1) {
        const h = Number(this.poll.intervalH);
        if (!(h >= 1 && h <= 720)) return '间隔小时需在 1~720 之间';
      } else {
        if (!/^\d{1,2}:\d{2}$/.test(String(this.poll.daily || '')))
          return '每日时间格式应为 HH:MM';
      }
      return '';
    },

    async savePoll() {
      if (this.pollSaving) return;
      const bad = this.validate();
      this.err = bad;
      if (bad) { toast(bad, 'err'); return; }
      const parts = String(this.poll.daily || '00:00').split(':');
      const hh = parseInt(parts[0], 10) || 0;
      const mm = parseInt(parts[1], 10) || 0;
      this.pollSaving = true;
      try {
        const r = await postOp('/api/sms/poll', {
          enabled: !!this.poll.enabled,
          mode: Number(this.poll.mode),
          intervalH: Number(this.poll.intervalH) || 24,
          hour: hh,
          minute: mm,
          safetySec: Number(this.poll.safetySec) || 60,
        }, '定时查询配置已保存');
        if (r) { this.markCleanPoll(); this.markCleanSafety(); }
      } finally {
        this.pollSaving = false;
      }
    },
  };
}

/* ---------- 邮件通知（SMTP）----------
 * 配置经 /api/email 读写；通知类型为位掩码（与固件 cfg::kEmailNotify* 一致）：
 *   bit0 = 设备启动完成  bit1 = SIM 状态变更
 *   bit2 = 收到短信      bit3 = 定时在线通知
 * 定时在线通知支持两种排程（bit3 为总控）：固定间隔（intervalH 小时）
 * 与每日定点（schedHour:schedMinute，依赖时间同步）。
 * 授权码不回显：保存时留空表示不修改已保存的值。
 */
function pageEmail() {
  const TYPES = [
    { bit: 4, label: '收到短信', desc: '每收到一条短信发送一封（验证码高亮）' },
    { bit: 2, label: 'SIM 状态变更', desc: '运行时插卡 / 拔卡各一封' },
    { bit: 1, label: '设备启动完成', desc: '开机并就绪后发送设备信息' },
    { bit: 8, label: '定时在线通知', desc: '按排程确认设备仍在线' },
    { bit: 16, label: '来电提醒', desc: '模组检测到 +CLIP/+CLCC 来电时发送一封' },
  ];
  return {
    cfg: { server: '', port: 465, user: '', pass: '', sendTo: '', notifyTypes: 15,
           enabled: false, schedMode: '1', schedHour: 9, schedMinute: 0, intervalH: 24 },
    configured: false,
    hasPass: false,
    saving: false,
    testing: false,
    types: TYPES,
    loading: false,           // 首屏加载中
    loaded: false,
    err: '',                 // 校验错误
    _snap: '',               // 已保存快照（用于未保存检测）

    init() {
      this.load();
      this.$watch('cfg', () => { this.err = ''; });
    },

    async load() {
      this.loading = true;
      try {
        const r = await apiFetch('/api/email');
        if (r.ok) {
          const j = await r.json();
          this.cfg.server = j.server || '';
          this.cfg.port = j.port || 465;
          this.cfg.user = j.user || '';
          this.cfg.sendTo = j.sendTo || '';
          this.cfg.notifyTypes = j.notifyTypes ?? 0;
          this.cfg.enabled = j.enabled ?? true;
          // schedMode 统一存字符串，与 <input type="radio" value=".."> 精确匹配
          this.cfg.schedMode = String(j.schedMode ?? 1);
          this.cfg.schedHour = j.schedHour ?? 9;
          this.cfg.schedMinute = j.schedMinute ?? 0;
          this.cfg.intervalH = j.intervalH || 24;
          this.cfg.pass = '';
          this.configured = !!j.configured;
          this.hasPass = !!j.hasPass;
          this.loaded = true;
          this.markClean();
          setDirtyGuard(() => this.dirty);
        }
      } catch (e) {} finally { this.loading = false; }
    },

    // 未保存检测：只比对「需点保存才生效」的字段（总开关即时生效，不计入）
    markClean() {
      const c = this.cfg;
      this._snap = JSON.stringify({
        s: c.server, p: c.port, u: c.user, w: c.sendTo, n: c.notifyTypes,
        m: c.schedMode, h: c.schedHour, mi: c.schedMinute, i: c.intervalH,
      });
    },
    get dirty() {
      const c = this.cfg;
      const now = JSON.stringify({
        s: c.server, p: c.port, u: c.user, w: c.sendTo, n: c.notifyTypes,
        m: c.schedMode, h: c.schedHour, mi: c.schedMinute, i: c.intervalH,
      });
      return !!this._snap && this._snap !== now;
    },

    // 总开关：只翻转启用位（不动其余配置），停用时设备丢弃待发邮件
    async toggleEnabled() {
      const on = !!this.cfg.enabled;
      const r = await postOp('/api/email/enabled', { enabled: on },
                             on ? '邮件通知已启用' : '邮件通知已停用');
      if (r) {
        const j = await r.json();
        this.configured = !!j.configured;
        this.hasPass = !!j.hasPass;
      } else {
        this.cfg.enabled = !on;  // 失败回滚，避免界面与设备不一致
      }
    },
    isOn(bit) { return (this.cfg.notifyTypes & bit) !== 0; },
    toggle(bit) {
      this.cfg.notifyTypes = this.isOn(bit)
        ? (this.cfg.notifyTypes & ~bit) : (this.cfg.notifyTypes | bit);
    },
    onlineOn() { return this.isOn(8); },
    isInterval() { return Number(this.cfg.schedMode) === 1; },

    // 保存前校验：启用邮件时服务器 / 账号 / 收件地址 / 授权码必须齐全
    validate() {
      const c = this.cfg;
      if (!c.enabled) return '';   // 已停用时允许留空（配置可稍后再填）
      if (!(c.server || '').trim()) return '请填写 SMTP 服务器地址';
      const port = Number(c.port);
      if (!port || port < 1 || port > 65535) return 'SMTP 端口需在 1~65535 之间';
      if (!(c.user || '').trim()) return '请填写发件邮箱（同时作为登录账号）';
      if (!this.hasPass && !(c.pass || '').trim()) return '请填写 SMTP 授权码';
      if (!(c.sendTo || '').trim()) return '请填写收件邮箱';
      if (this.onlineOn() && !this.isInterval()) {
        const h = Number(c.schedHour), mi = Number(c.schedMinute);
        if (!(h >= 0 && h <= 23)) return '定时小时需在 0~23 之间';
        if (!(mi >= 0 && mi <= 59)) return '定时分钟需在 0~59 之间';
      }
      if (this.onlineOn() && this.isInterval()) {
        const ih = Number(c.intervalH);
        if (!(ih >= 1 && ih <= 720)) return '间隔小时需在 1~720 之间';
      }
      return '';
    },

    async save() {
      const bad = this.validate();
      this.err = bad;
      if (bad) { toast(bad, 'err'); return; }
      this.saving = true;
      try {
        const r = await postOp('/api/email', {
          server: (this.cfg.server || '').trim(),
          port: Number(this.cfg.port) || 465,
          user: (this.cfg.user || '').trim(),
          pass: this.cfg.pass || '',       // 空 = 不修改已保存授权码
          sendTo: (this.cfg.sendTo || '').trim(),
          enabled: !!this.cfg.enabled,
          notifyTypes: this.cfg.notifyTypes,
          schedMode: this.isInterval() ? 1 : 0,
          schedHour: Math.min(23, Math.max(0, Number(this.cfg.schedHour) || 0)),
          schedMinute: Math.min(59, Math.max(0, Number(this.cfg.schedMinute) || 0)),
          intervalH: Math.min(720, Math.max(1, Number(this.cfg.intervalH) || 24)),
        }, '邮件配置已保存');
        if (r) {
          const j = await r.json();
          this.configured = !!j.configured;
          this.hasPass = !!j.hasPass;
          this.cfg.pass = '';
          this.markClean();
        }
      } finally {
        this.saving = false;
      }
    },
    async test() {
      this.testing = true;
      try {
        // 只入队：实际发送在设备主循环完成（HTTP 任务栈不足以跑 SMTP/TLS）
        await postOp('/api/email/test', {}, '测试邮件已加入发送队列，稍后发出，请查收邮箱');
      } finally {
        this.testing = false;
      }
    },
  };
}

/* ---------- MQTT 推送 ----------
 * 配置经 /api/mqtt 读写；通知类型为位掩码（与固件 cfg::kMqttNotify* 一致）：
 *   1=启动 2=SIM 4=短信 8=上线 65536=日志（低 4 位与邮件 kEmailNotify* 同义）。
 * 总开关经 /api/mqtt/enabled；测试发布经 /api/mqtt/test（只入队，主循环补发）。
 * 交互与「邮件通知」页对齐：cfg 直接持有字段，isOn/toggle 管理通知类型位。 */
function pageMqtt() {
  // 通知类型位定义（与固件 cfg::kMqttNotify* 保持一致；新增类型在此补一行 + 固件加一位）
  const TYPES = [
    { bit: 1,     label: '设备启动完成', desc: '开机并就绪后发布设备信息' },
    { bit: 2,     label: 'SIM 状态变更', desc: '运行时插卡 / 拔卡各一条' },
    { bit: 4,     label: '收到短信', desc: '每收到一条短信发布一条' },
    { bit: 8,     label: '设备上线（重连）', desc: '每次（重）连接发布一次' },
    { bit: 16,    label: '来电提醒', desc: '来电事件发布到 .../call 主题' },
    { bit: 65536, label: '运行日志实时推送', desc: '运行日志按 20 条/秒限流推送（默认关闭）' },
  ];
  return {
    cfg: { scheme: 'mqtt', server: '', user: '', pass: '', clientId: '',
           topicPrefix: 'smsf', keepAlive: 60,
           subscribeEnabled: true, notifyTypes: 15, enabled: false },
    configured: false,
    hasPass: false,
    saving: false,
    testing: false,
    connected: false,        // MQTT 实时连接状态（load 兜底，SSE 优先）
    types: TYPES,
    loading: false,           // 首屏加载中
    loaded: false,
    err: '',                 // 校验错误
    _snap: '',               // 已保存快照（用于未保存检测）

    init() {
      this.load();
      this.$watch('cfg', () => { this.err = ''; });
    },
    // 实时连接状态：优先取 SSE status.mqtt（实时推送），否则用 load() 兜底值
    get live() {
      const s = Alpine.store('live').status;
      return (s && s.mqtt) ? s.mqtt : null;
    },
    // 设备 ID（来自状态接口，= 芯片 MAC 高 48 位，与 MQTT ClientID / 主题前缀一致）
    get deviceId() {
      const s = Alpine.store('live').status;
      return (s && s.deviceId) ? s.deviceId : '';
    },
    connEnabled() { const l = this.live; return l ? l.enabled : this.cfg.enabled; },
    connConfigured() { const l = this.live; return l ? l.configured : this.configured; },
    connConnected() { const l = this.live; return l ? l.connected : this.connected; },
    connError() { const l = this.live; return (l && l.error) ? l.error : ''; },
    connClass() {
      if (!this.connEnabled()) return 'err';
      if (!this.connConfigured()) return 'warn';
      return this.connConnected() ? 'ok' : 'err';
    },
    connText() {
      if (!this.connEnabled()) return '已停用';
      if (!this.connConfigured()) return '未配置';
      return this.connConnected() ? '已连接' : (this.connError() ? '连接失败' : '连接中');
    },
    async load() {
      this.loading = true;
      try {
        const r = await apiFetch('/api/mqtt');
        if (r.ok) {
          const j = await r.json();
          this.cfg.server = j.server || '';
          this.cfg.scheme = j.scheme || 'mqtt';
          this.cfg.user = j.user || '';
          this.cfg.clientId = j.clientId || '';
          this.cfg.topicPrefix = j.topicPrefix || 'smsf';
          this.cfg.keepAlive = j.keepAlive || 60;
          this.cfg.subscribeEnabled = j.subscribeEnabled !== false;
          this.cfg.notifyTypes = j.notifyTypes ?? 15;   // 缺省：除日志外全开
          this.cfg.enabled = !!j.enabled;
          this.cfg.pass = '';
          this.configured = !!j.configured;
          this.hasPass = !!j.hasPass;
          this.connected = !!j.connected;   // 兜底值，实时连接状态优先取 SSE
          this.loaded = true;
          this.markClean();
          setDirtyGuard(() => this.dirty);
        }
      } catch (e) {} finally { this.loading = false; }
    },

    // 未保存检测：只比对「需点保存才生效」的字段（总开关即时生效，不计入）
    markClean() {
      const c = this.cfg;
      this._snap = JSON.stringify({
        sc: c.scheme, sv: c.server, u: c.user, ci: c.clientId, tp: c.topicPrefix,
        ka: c.keepAlive, se: c.subscribeEnabled, n: c.notifyTypes,
      });
    },
    get dirty() {
      const c = this.cfg;
      const now = JSON.stringify({
        sc: c.scheme, sv: c.server, u: c.user, ci: c.clientId, tp: c.topicPrefix,
        ka: c.keepAlive, se: c.subscribeEnabled, n: c.notifyTypes,
      });
      return !!this._snap && this._snap !== now;
    },
    // 总开关：只翻转启用位（不动其余配置），停用时销毁客户端停止连接
    async toggleEnabled() {
      const on = !!this.cfg.enabled;
      const r = await postOp('/api/mqtt/enabled', { enabled: on },
                             on ? 'MQTT 已启用' : 'MQTT 已停用');
      if (r) {
        const j = await r.json();
        this.configured = !!j.configured;
        this.hasPass = !!j.hasPass;
      } else {
        this.cfg.enabled = !on;   // 失败回滚，避免界面与设备不一致
      }
    },
    isOn(bit) { return (this.cfg.notifyTypes & bit) !== 0; },
    toggle(bit) {
      this.cfg.notifyTypes = this.isOn(bit)
        ? (this.cfg.notifyTypes & ~bit) : (this.cfg.notifyTypes | bit);
    },
    // 保存前校验：启用 MQTT 时服务器地址必填；若带端口则需在 1~65535
    validate() {
      const c = this.cfg;
      if (!c.enabled) return '';   // 已停用时允许留空
      const sv = (c.server || '').trim();
      if (!sv) return '请填写 Broker 服务器地址（地址:端口）';
      const m = sv.match(/:(\d{1,5})$/);
      if (m) {
        const port = Number(m[1]);
        if (!port || port < 1 || port > 65535) return 'Broker 端口需在 1~65535 之间';
      }
      const ka = Number(c.keepAlive);
      if (!(ka >= 1 && ka <= 3600)) return '保活间隔需在 1~3600 秒之间';
      return '';
    },

    async save() {
      const bad = this.validate();
      this.err = bad;
      if (bad) { toast(bad, 'err'); return; }
      this.saving = true;
      try {
        const r = await postOp('/api/mqtt', {
          enabled: !!this.cfg.enabled,
          scheme: (this.cfg.scheme || 'mqtt').trim(),
          server: (this.cfg.server || '').trim(),
          user: (this.cfg.user || '').trim(),
          pass: this.cfg.pass || '',       // 空 = 不修改已保存密码
          clientId: (this.cfg.clientId || '').trim(),
          topicPrefix: (this.cfg.topicPrefix || 'smsf').trim(),
          keepAlive: Math.min(3600, Math.max(1, Number(this.cfg.keepAlive) || 60)),
          subscribeEnabled: !!this.cfg.subscribeEnabled,
          notifyTypes: this.cfg.notifyTypes,
        }, 'MQTT 配置已保存');
        if (r) {
          const j = await r.json();
          this.configured = !!j.configured;
          this.hasPass = !!j.hasPass;
          this.cfg.pass = '';              // 清空密码框（已保存则不回显）
          this.markClean();
        }
      } finally { this.saving = false; }
    },
    // 测试：只入队，实际发布在设备主循环完成（HTTP 任务栈不足以跑 MQTT 握手）
    async test() {
      this.testing = true;
      try {
        await postOp('/api/mqtt/test', {}, '测试消息已加入发布队列，请检查订阅端');
      } finally { this.testing = false; }
    },
  };
}

/* ---------- 推送通道 ----------
 * 配置经 /api/push 读写；策略：0=广播（全部通道都发）/ 1=故障转移（按顺序首个成功即停）。
 * 总开关经 /api/push/enabled；测试经 /api/push/test（只入队，主循环补发）。
 * 通道类型与字段 schema 经 /api/push/types 获取，前端据此动态渲染每个通道的表单；
 * 密钥字段（secretField）在配置接口中不回显，仅返回 hasKeyX 标记。 */
function pagePush() {
  return {
    loaded: false,
    loading: false,           // 首屏加载中
    err: '',                 // 校验错误（表单级提示）
    cfg: { enabled: false, strategy: 0, remark: '' },
    channels: [],
    types: [],
    saving: false,
    testing: false,
    _snap: '',               // 已保存配置快照（用于未保存检测）

    init() {
      this.load();
      // 任何编辑后清掉上一次的校验提示，避免过期错误误导
      this.$watch('channels', () => { this.err = ''; });
      this.$watch('cfg.strategy', () => { this.err = ''; });
      this.$watch('cfg.remark', () => { this.err = ''; });
    },

    async load() {
      this.loading = true;
      try {
        const r = await apiFetch('/api/push/types');
        if (r.ok) { const j = await r.json(); this.types = j.types || []; }
      } catch (e) {}
      try {
        const r = await apiFetch('/api/push');
        if (r.ok) {
          const j = await r.json();
          this.cfg.enabled = !!j.enabled;
          this.cfg.strategy = Number(j.strategy) || 0;
          this.cfg.remark = j.remark || '';
          this.channels = (j.channels || []).map(c => ({
            enabled: !!c.enabled, type: c.type, name: c.name || '',
            url: c.url || '', key1: '', key2: c.key2 || '', key3: c.key3 || '',
            key4: c.key4 || '', key5: c.key5 || '', customBody: c.customBody || '',
            retryOnFail: !!c.retryOnFail,
            // SMS 备份通道的发出模组：-1（自动）在表单里显示为空
            modemSlot: (c.modemSlot === undefined || c.modemSlot === null ||
                        Number(c.modemSlot) < 0) ? '' : String(Number(c.modemSlot)),
            valid: !!c.valid,
            hasKey1: !!c.hasKey1, hasKey2: !!c.hasKey2, hasKey3: !!c.hasKey3,
            hasKey4: !!c.hasKey4, hasKey5: !!c.hasKey5,
          }));
          this.loaded = true;
          this.markClean();
          setDirtyGuard(() => this.dirty);
        }
      } catch (e) {} finally { this.loading = false; }
    },

    // 未保存检测：只比对「需点保存才生效」的部分（总开关为即时生效，不计入）
    markClean() { this._snap = JSON.stringify({ s: this.cfg.strategy, r: this.cfg.remark, c: this.channels }); },
    get dirty() {
      return !!this._snap && this._snap !== JSON.stringify({ s: this.cfg.strategy, r: this.cfg.remark, c: this.channels });
    },

    typeMeta(t) {
      return this.types.find(x => x.type === Number(t)) ||
        { label: '未知', required: [], optional: [], secretField: '', body: false };
    },
    fieldsFor(t) {
      const m = this.typeMeta(t);
      const set = [];
      const add = n => { if (n && n !== 'name' && !set.includes(n)) set.push(n); };
      (m.required || []).forEach(add);
      (m.optional || []).forEach(add);
      return set.map(n => ({
        name: n,
        required: (m.required || []).includes(n),
        secret: m.secretField === n,
      }));
    },
    fieldLabel(n) {
      const map = {
        url: 'URL / 端点', key1: '密钥 / Token', key2: '参数2（域名 / 频道 / 服务器）',
        key3: '参数3（收件人 / ClientID）', key4: '参数4（发件人）',
        key5: '参数5（标题模板）', customBody: '自定义请求体 / HTML',
        modemSlot: '发出模组（SMS 备份专用）',
      };
      return map[n] || n;
    },
    fieldPlaceholder(f, ch) {
      if (f.secret) {
        const hk = 'hasKey' + f.name.slice(3);
        return ch[hk] ? '已设置，留空保持不变' : '必填（不回显）';
      }
      if (f.name === 'url') {
        // SMS 备份通道（type=12）的 url 是目标手机号，不是 URL
        return Number(ch.type) === 12 ? '目标手机号，如 13800138000' : 'https://...';
      }
      if (f.name === 'customBody') return '使用 {from}{message}{carrier}{remark} 等占位符，见下方说明表';
      if (f.name === 'modemSlot') return '留空 = 自动挑就绪模组；可填 1 或 2';
      return '';
    },
    isSecretPlaceholder(f, ch) {
      const hk = 'hasKey' + f.name.slice(3);
      return f.secret && ch[hk] && !ch[f.name];
    },
    // 必填项缺失 → 输入框红框（未保存前即可见）
    fieldInvalid(ch, f) {
      if (!f.required || !ch.enabled) return false;
      if ((ch[f.name] || '').toString().trim()) return false;
      if (f.secret && ch['hasKey' + f.name.slice(3)]) return false;
      return true;
    },

    onTypeChange(i) {
      // 切换类型后重置字段与掩码标记，避免残留误导
      const ch = this.channels[i];
      ch.key1 = ch.key2 = ch.key3 = ch.key4 = ch.key5 = ch.customBody = '';
      ch.modemSlot = '';
      ch.hasKey1 = ch.hasKey2 = ch.hasKey3 = ch.hasKey4 = ch.hasKey5 = false;
    },

    addChannel() {
      if (this.channels.length >= 10) { toast('最多 10 个通道', 'warn'); return; }
      const first = this.types.length ? this.types[0].type : 1;
      this.channels.push({
        enabled: true, type: first, name: '', url: '', key1: '', key2: '', key3: '',
        key4: '', key5: '', customBody: '', retryOnFail: false, modemSlot: '',
        valid: false, hasKey1: false, hasKey2: false, hasKey3: false, hasKey4: false, hasKey5: false,
      });
    },
    removeChannel(i) { this.channels.splice(i, 1); },

    async toggleEnabled() {
      const on = !!this.cfg.enabled;
      const r = await postOp('/api/push/enabled', { enabled: on },
        on ? '推送已启用' : '推送已停用');
      if (r) { const j = await r.json(); this.cfg.enabled = !!j.enabled; }
      else { this.cfg.enabled = !on; }
    },

    // 保存前校验：启用的通道必填项不能为空（密钥项已保存视为已填，留空即不修改）
    validate() {
      for (let i = 0; i < this.channels.length; i++) {
        const ch = this.channels[i];
        if (!ch.enabled) continue;
        const m = this.typeMeta(ch.type);
        for (const f of (m.required || [])) {
          if ((ch[f] || '').toString().trim()) continue;
          if (m.secretField === f && ch['hasKey' + f.slice(3)]) continue;
          return '第 ' + (i + 1) + ' 个通道缺少必填项「' + this.fieldLabel(f) + '」';
        }
      }
      return '';
    },

    async save() {
      const bad = this.validate();
      this.err = bad;
      if (bad) { toast(bad, 'err'); return; }
      this.saving = true;
      try {
        const payload = {
          enabled: !!this.cfg.enabled,
          strategy: Number(this.cfg.strategy) || 0,
          remark: (this.cfg.remark || '').trim(),
          channels: this.channels.map(c => ({
            enabled: !!c.enabled, type: Number(c.type) || 1, name: (c.name || '').trim(),
            url: (c.url || '').trim(), key1: c.key1 || '', key2: c.key2 || '',
            key3: c.key3 || '', key4: c.key4 || '', key5: c.key5 || '',
            customBody: c.customBody || '', retryOnFail: !!c.retryOnFail,
            modemSlot: (c.modemSlot || '').toString().trim(),
          })),
        };
        const r = await postOp('/api/push', payload, '推送配置已保存');
        if (r) {
          const j = await r.json();
          this.cfg.enabled = !!j.enabled;
          this.cfg.strategy = Number(j.strategy) || 0;
          if (typeof j.remark !== 'undefined') this.cfg.remark = j.remark || '';
          const chs = j.channels || [];
          for (let i = 0; i < this.channels.length; i++) {
            const src = chs[i] || {};
            this.channels[i].valid = !!src.valid;
            if (typeof src.hasKey1 !== 'undefined') this.channels[i].hasKey1 = !!src.hasKey1;
            if (typeof src.hasKey2 !== 'undefined') this.channels[i].hasKey2 = !!src.hasKey2;
            if (typeof src.hasKey3 !== 'undefined') this.channels[i].hasKey3 = !!src.hasKey3;
            if (typeof src.hasKey4 !== 'undefined') this.channels[i].hasKey4 = !!src.hasKey4;
            if (typeof src.hasKey5 !== 'undefined') this.channels[i].hasKey5 = !!src.hasKey5;
            // 清空密钥输入（已保存不回显）
            const sec = this.typeMeta(this.channels[i].type).secretField;
            if (sec) this.channels[i][sec] = '';
          }
          this.markClean();     // 已保存，解除未保存标记
        }
      } finally { this.saving = false; }
    },

    async test() {
      this.testing = true;
      try {
        await postOp('/api/push/test', {}, '测试推送已加入队列，稍后于各通道发出');
      } finally { this.testing = false; }
    },
  };
}

/* ---------- 运行日志（PSRAM 环形缓冲：分页 + 级别/关键字过滤 + 实时追加） ----------
 * 后端经 SSE `log` 事件增量推送新记录（仿 at 历史），前端 applyTail 并入；
 * SSE 不可用时回退 5s 轮询（GET 不走 apiOp，失败不弹 toast，避免离线刷屏）。 */
/* ---------- 发件箱（持久化不丢消息转发队列）----------
 * 数据来自 GET /api/outbox；每条记录含各通道投递回执。
 * 后端经 SSE `outbox` 事件推「脏信号」即时刷新；SSE 不可用时回退 5s 轮询。 */
function pageOutbox() {
  const CH = {
    pending:   ['待投',      '#8a8f98'],
    sending:   ['发送中',    '#c8860d'],
    ok:        ['成功',      '#17994a'],
    submitted: ['已入队',    '#17994a'],
    failed:    ['失败待重试', '#c8860d'],
    skipped:   ['已跳过',    '#8a8f98'],
    dead:      ['已放弃',    '#d63a2f'],
  };
  return {
    records: [], total: 0, pending: 0, failed: 0, done: 0,
    page: 1, pageSize: 20, loading: false, auto: true, enabled: true, timer: null,
    // 发送短信弹窗
    modems: [], sendPort: 0, phone: '', content: '',
    sending: false, sendOpen: false, sendHint: '', sendErr: false,

    init() {
      this.reload();
      this.loadPorts();
      // SSE 驱动：收到 outbox 脏信号即刷新当前视图；SSE 不可用时（conn=offline）回退 5s 轮询
      _outboxSink = () => { if (this.auto && !document.hidden) this.reload(true); };
      this.timer = setInterval(() => {
        if (this.auto && !document.hidden && Alpine.store('live').conn === 'offline')
          this.reload(true);
      }, 5000);
      onCleanup(() => {
        if (this.timer) { clearInterval(this.timer); this.timer = null; }
        _outboxSink = null;
      });
    },

    async reload(silent) {
      if (!silent) this.loading = true;
      try {
        const r = await apiFetch('/api/outbox?page=' + this.page + '&pageSize=' + this.pageSize);
        if (!r.ok) return;
        const j = await r.json();
        this.enabled = !!j.enabled;
        this.total = j.total || 0;
        this.pending = j.pending || 0;
        this.failed = j.failed || 0;
        this.done = j.done || 0;
        this.records = j.records || [];
        if (this.page > this.pageCount()) { this.page = this.pageCount(); this.reload(true); }
      } catch (e) {} finally { this.loading = false; }
    },

    // POST 操作统一走 JSON 契约，成功后静默刷新一次
    async op(path, body, okMsg) {
      let r;
      try { r = await postJson(path, body || {}); }
      catch (e) { toast('✕ 请求失败：' + (e && e.message ? e.message : e), 'err'); return; }
      if (r.ok) { toast(okMsg, 'ok'); this.reload(true); }
      else toast('✕ ' + await errText(r), 'err');
    },

    retry(id)     { this.op('/api/outbox/retry', { id: id || 0 }, '已重新排队'); },
    del(id)       { this.op('/api/outbox/delete', { id: id }, '已删除该记录'); },
    clearDone()   { this.op('/api/outbox/clear', {}, '已完成回执已清空'); },
    goto(p)       { this.page = Math.max(1, Math.min(this.pageCount(), p)); this.reload(); },
    pageCount()   { return Math.max(1, Math.ceil(this.total / this.pageSize)); },
    // sms: 通道的 submitted 表示「结果未知」，也允许手动重试（用户自行确认是否重复）
    hasFailed(r) {
      return (r.channels || []).some(c => c.s === 'failed' || c.s === 'dead' ||
        (c.s === 'submitted' && String(c.k || '').startsWith('sms:')));
    },
    chText(s, k) {
      if (s === 'submitted' && String(k || '').startsWith('sms:')) return '结果未知';
      return (CH[s] || ['未知', '#8a8f98'])[0];
    },
    chStyle(s) {
      const c = (CH[s] || ['未知', '#8a8f98'])[1];
      return 'color:' + c + ';border-color:' + c;
    },

    /* ---- 发送短信（复用 POST /api/sms/send，该接口已改为登记到发件箱） ---- */
    async loadPorts() {
      try {
        const r = await apiFetch('/api/sms/ports');
        if (r.ok) this.modems = (await r.json()).modems || [];
      } catch (e) {}
    },
    enabledPorts() { return this.modems.filter(m => m.enabled); },
    defaultSendPort() {
      const enabled = this.enabledPorts();
      if (!enabled.length) return 0;
      let saved = 0;
      try { saved = parseInt(localStorage.getItem(SEND_PORT_KEY) || '0', 10) || 0; } catch (e) {}
      if (saved && enabled.find(m => m.port === saved)) return saved;
      return enabled[0].port;
    },
    async openSend() {
      await this.loadPorts();
      this.sendPort = this.defaultSendPort();
      try { this.phone = localStorage.getItem(SEND_PHONE_KEY) || ''; } catch (e) {}
      this.sendHint = ''; this.sendErr = false; this.sendOpen = true;
      this.$nextTick(() => {
        const el = this.$refs && this.$refs.phoneInput;
        if (el && el.focus) el.focus();
      });
    },
    closeSend() { this.sendOpen = false; },
    savePhone() {
      try { localStorage.setItem(SEND_PHONE_KEY, this.phone || ''); } catch (e) {}
    },
    clearPhone() {
      this.phone = '';
      try { localStorage.removeItem(SEND_PHONE_KEY); } catch (e) {}
    },
    sendFail(msg) { this.sending = false; this.sendHint = msg; this.sendErr = true; },
    async send() {
      const phone = this.phone.trim();
      if (!phone) { this.sendFail('请输入对方号码'); return; }
      if (!this.content.length) { this.sendFail('请输入短信内容'); return; }
      if (!this.sendPort) { this.sendFail('没有可用的通信模组，请先在「通信模组」中启用'); return; }
      this.sending = true; this.sendErr = false; this.sendHint = '正在登记…';
      let r;
      try {
        r = await postJson('/api/sms/send', {
          port: this.sendPort, phone: phone, content: this.content
        });
      } catch (e) {
        this.sendFail('请求失败：' + (e && e.message ? e.message : e));
        return;
      }
      if (!r.ok) {
        this.sendFail('提交失败：' + await errText(r));
        return;
      }
      // 发件箱页不轮询 GET /api/sms/send：记录状态由 5s 自动刷新呈现
      this.sending = false;
      this.sendOpen = false;
      this.content = '';
      this.savePhone();
      try { localStorage.setItem(SEND_PORT_KEY, String(this.sendPort)); } catch (e) {}
      toast('已加入发件箱，等待通信模组发送', 'ok');
      this.reload(true);
    },

    fmtTs(ts) {
      if (!ts || ts < 1600000000) return '时间未同步';
      return fmtDateTime(ts);   // 统一按设备时区格式化
    },
  };
}

function pageLogs() {
  const LEVELS = { 0: '调试', 1: '信息', 2: '警告', 3: '错误' };
  return {
    logs: [],                 // 当前页记录（按 ID 倒序，最新在前）
    lastId: 0,                // 已知最新记录 ID（增量拉取基线：只取比它大的）
    total: 0, page: 1, pageSize: 20,
    level: '',                // 最低级别过滤：'' 全部 / 1 信息 / 2 警告 / 3 错误
    q: '',                    // 关键字（匹配 tag/msg）
    stats: {},                // GET /api/logs/stats
    statsErr: '',             // 缓冲不可用提示（stats.ok=false）
    loading: false,
    auto: true,               // 自动刷新开关
    timer: null,

    init() {
      this.loadStats();
      this.reload();
      // SSE 驱动：收到 log 事件即增量追加（applyTail）；SSE 不可用时（conn=offline）回退 5s 轮询
      _logSink = (d) => this.applyTail(d.logs, d.total);
      this.timer = setInterval(() => {
        if (this.auto && !document.hidden && Alpine.store('live').conn === 'offline')
          this.tick();
      }, 5000);
      onCleanup(() => {
        if (this.timer) { clearInterval(this.timer); this.timer = null; }
        _logSink = null;
      });
    },
    // 增量拉取：只取 ID 大于 lastId 的新记录并插入列表顶部（最新在前），
    // 已在看其它页时不改动可见列表，仅同步计数与基线
    // 增量并入新日志（REST 拉取与 SSE 事件共用）：got 为升序新记录数组，total 为匹配总数
    applyTail(got, total) {
      if (!this.lastId) return;   // 还没基线（REST 尚未拉取），等下次 reload 补齐
      if (typeof total === 'number' && this.level === '' && !this.q) {
        this.total = total;                       // 仅在无过滤时同步全局条数
        if (this.stats.max) this.stats.total = total;
      }
      // 只追加比当前基线更新的；SSE 可能重发已展示记录（连接/REST 时序窗口），由 id 去重
      const fresh = (got || []).filter(e => e.id > this.lastId);
      if (!fresh.length) return;
      const box = this.$refs.box;
      const prevTop = box ? box.scrollTop : 0;
      const prevH = box ? box.scrollHeight : 0;
      if (this.page === 1) {
        this.logs = fresh.concat(this.logs);                 // fresh 升序，新记录在前
        if (this.logs.length > this.pageSize) this.logs = this.logs.slice(0, this.pageSize);
      }
      this.lastId = fresh[fresh.length - 1].id;
      this.$nextTick(() => {
        if (box && prevTop > 0) box.scrollTop = prevTop + (box.scrollHeight - prevH);
      });
    },
    async tick() {
      // 还没有基线（视图为空）：静默整页重拉一次拿到基线，后续即走增量
      if (!this.lastId) { this.reload(true); return; }
      const limit = 100;
      try {
        const q = new URLSearchParams({ after: this.lastId, limit: limit });
        if (this.level !== '') q.set('level', this.level);
        if (this.q) q.set('q', this.q);
        const r = await apiFetch('/api/logs?' + q.toString());
        if (!r.ok) return;
        const j = await r.json();
        this.applyTail(j.logs, j.total);
        // 基线推进：取回的条数未满 limit 且服务端给出最新 ID 时，直接推进到最新，避免漏推
        if (j.last) this.lastId = j.last;
      } catch (e) {}
    },
    // silent=true：不切 loading 态（轮询兜底用，避免空列表时「加载中…」闪烁）
    async reload(silent) {
      if (!silent) this.loading = true;
      try {
        const q = new URLSearchParams({ page: this.page, pageSize: this.pageSize });
        if (this.level !== '') q.set('level', this.level);
        if (this.q) q.set('q', this.q);
        const r = await apiFetch('/api/logs?' + q.toString());
        if (r.ok) {
          const j = await r.json();
          this.logs = j.logs || [];
          this.total = j.total || 0;
          // 清空/筛选后当前页越界：自动回到最后一页并重新拉取
          const pc = Math.max(1, Math.ceil(this.total / this.pageSize));
          if (this.page > pc) { this.page = pc; if (!silent) this.loading = false; return this.reload(silent); }
          // 基线 = 当前可见的最新一条（列表按 ID 倒序）
          this.lastId = this.logs.length ? this.logs[0].id : 0;
        }
      } catch (e) {}
      if (!silent) this.loading = false;
    },
    async loadStats() {
      try {
        const r = await apiFetch('/api/logs/stats');
        if (r.ok) {
          const j = await r.json();
          this.stats = j;
          this.statsErr = j.ok ? ''
              : ('日志缓冲不可用（' + (zhError(j.error) || '未知原因') + '）：请检查串口输出中的 [log] 日志');
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
    async clearAll() {
      if (!await confirmDialog('清空日志', '确定清空全部运行日志吗？', true)) return;
      const r = await apiOp('/api/logs', { method: 'DELETE' }, '日志已清空');
      if (r) { this.page = 1; this.reload(); this.loadStats(); }
    },
    // 存储位置文案：外部 PSRAM / 回落到内部内存（容量较小，功能不受影响）
    storeText() {
      if (!this.stats.max) return '-';
      return this.stats.psram ? '外部 PSRAM' : '内部内存';
    },
    storeHint() {
      if (this.stats.psram) return '';
      return '未使用外部 PSRAM，日志容量较小，功能不受影响';
    },
    levelText(l) { return LEVELS[l] || '信息'; },
    levelClass(l) { return l >= 3 ? 'err' : l === 2 ? 'warn' : l === 0 ? '' : 'info'; },
    msgClass(l) { return l >= 3 ? 'lv-err' : l === 2 ? 'lv-warn' : ''; },
    // 时间格式化与全站一致（纪元秒 -> "YYYY-MM-DD HH:mm:ss"；否则开机时长）
    fmtTs(ts) { return fmtDateTime(ts); },
  };
}

/* ---------- 黑名单 ----------
 * 配置经 /api/blacklist 读写；总开关经 /api/blacklist/enabled；
 * 测试匹配经 /api/blacklist/test（忽略总开关，便于调试）。
 * 匹配模式 mode: 0 精确 / 1 前缀 / 2 通配符；作用域 scope: 位1=短信 位2=来电。 */
function pageBlacklist() {
  return {
    cfg: { enabled: false, action: 0, list: [] },
    saving: false,
    testing: false,
    testNum: '',
    testRes: null,
    loading: false,           // 首屏加载中
    loaded: false,
    err: '',                 // 校验错误
    _snap: '',               // 已保存快照（用于未保存检测）

    init() {
      this.load();
      this.$watch('cfg.list', () => { this.err = ''; });
      this.$watch('cfg.action', () => { this.err = ''; });
    },

    // 规整后端条目为前端可用的数字字段（mode / scope）
    _norm(e) {
      return {
        pattern: e.pattern || '',
        mode: Number(e.mode) || 0,
        scope: Number(e.scope) || 1,
        note: e.note || '',
        enabled: e.enabled !== false,
      };
    },

    async load() {
      this.loading = true;
      try {
        const r = await apiFetch('/api/blacklist');
        if (r.ok) {
          const j = await r.json();
          this.cfg.enabled = !!j.enabled;
          this.cfg.action = j.action ?? 0;
          this.cfg.list = (j.list || []).map(e => this._norm(e));
          this.loaded = true;
          this.markClean();
          setDirtyGuard(() => this.dirty);
        }
      } catch (e) {} finally { this.loading = false; }
    },

    // 未保存检测：只比对「需点保存才生效」的部分（总开关即时生效，不计入）
    markClean() { this._snap = JSON.stringify({ a: this.cfg.action, l: this.cfg.list }); },
    get dirty() {
      return !!this._snap && this._snap !== JSON.stringify({ a: this.cfg.action, l: this.cfg.list });
    },

    // 总开关：只翻转启用位（不动规则与动作）
    async toggleEnabled() {
      const on = !!this.cfg.enabled;
      const r = await postOp('/api/blacklist/enabled', { enabled: on },
                            on ? '黑名单已启用' : '黑名单已停用');
      if (r) { const j = await r.json(); this.cfg.enabled = !!j.enabled; }
      else { this.cfg.enabled = !on; }   // 失败回滚，避免界面与设备不一致
    },

    // 作用域位：短信(1) / 来电(2)；允许全部取消（保存后后端回落为短信）
    setScope(rule, bit, on) {
      rule.scope = on ? (rule.scope | bit) : (rule.scope & ~bit);
    },

    addRule() {
      if (this.cfg.list.length >= 100) { toast('已达规则上限 100 条', 'warn'); return; }
      this.cfg.list.push({ pattern: '', mode: 0, scope: 1, note: '', enabled: true });
    },
    delRule(i) { this.cfg.list.splice(i, 1); },

    // 启用中的规则号码为空 → 红框提示
    ruleInvalid(r) {
      return !!(r.enabled && !(r.pattern || '').toString().trim());
    },

    // 保存前校验：启用的规则必须有号码（且不能只填空白）
    validate() {
      for (let i = 0; i < this.cfg.list.length; i++) {
        const e = this.cfg.list[i];
        if (!e.enabled) continue;
        if (!(e.pattern || '').toString().trim())
          return '第 ' + (i + 1) + ' 条规则的号码为空（停用该规则或填写号码）';
      }
      return '';
    },

    async save() {
      const bad = this.validate();
      this.err = bad;
      if (bad) { toast(bad, 'err'); return; }
      this.saving = true;
      try {
        const r = await postOp('/api/blacklist', {
          enabled: !!this.cfg.enabled,
          action: Number(this.cfg.action) || 0,
          list: this.cfg.list.map(e => ({
            pattern: (e.pattern || '').trim(),
            mode: Number(e.mode) || 0,
            scope: Number(e.scope) || 1,
            note: (e.note || '').trim(),
            enabled: e.enabled !== false,
          })),
        }, '黑名单已保存');
        if (r) {
          const j = await r.json();
          this.cfg.enabled = !!j.enabled;
          this.cfg.action = j.action ?? 0;
          this.cfg.list = (j.list || []).map(e => this._norm(e));
          this.markClean();
        }
      } finally { this.saving = false; }
    },

    async test() {
      const num = (this.testNum || '').trim();
      if (!num) { toast('请输入待测试号码', 'warn'); return; }
      this.testing = true;
      try {
        const r = await apiFetch('/api/blacklist/test?num=' + encodeURIComponent(num));
        if (r.ok) this.testRes = await r.json();
      } catch (e) {}
      finally { this.testing = false; }
    },
  };
}

/* ---------- 系统工具：固件 OTA 升级与本地刷写 ---------- */
function pageTools() {
  return {
    otaAvailable: false,       // 分区表是否支持 OTA 双槽
    curVer: '',                // 当前固件版本（Release tag）
    latestVer: '',             // 远端最新版本（检查后填充）
    otaState: 0,               // OtaState：0空闲 1检查 2下载 3写入 4成功 5失败 6烧录FS
    otaProgress: 0,            // 0~100
    otaMessage: '',            // 失败原因 / 当前阶段说明
    firmwareFile: null,
    lfsFile: null,
    pending: false,            // 已发起操作、尚未观察到非空闲状态（避免轮询提前结束）
    _polling: false,
    pollCount: 0,

    busy() {
      return this.otaState === 1 || this.otaState === 2 || this.otaState === 3 || this.otaState === 6;
    },
    stateText() {
      return ({
        0: '空闲', 1: '检查更新中…', 2: '下载固件中…', 3: '写入固件中…',
        4: '升级成功', 5: '失败', 6: '烧录 Web UI 中…',
      })[this.otaState] || '未知';
    },
    statusLine() {
      if (this.busy()) return this.stateText() + ' ' + this.otaProgress + '%';
      if (this.otaState === 4) return '升级成功，设备重启中…';
      if (this.otaState === 5) return '失败：' + (this.otaMessage || '未知原因');
      if (this.latestVer && this.latestVer !== this.curVer) return '发现新版本：' + this.latestVer;
      if (this.otaMessage) return this.otaMessage;
      return '已是最新版本' + (this.curVer ? '（' + this.curVer + '）' : '');
    },

    // ---------- 任务看门狗 ----------
    wdt: {},                 // GET /api/status 的 wdt 子对象
    wdtTask: 'loopTask',     // 自检时模拟卡死的任务
    wdtTimeoutSec: 10,       // 自检临时超时（秒）；60 = 沿用默认
    wdtBusy: false,

    async loadWdt() {
      try {
        const r = await fetch('/api/status', { cache: 'no-store' });
        if (!r.ok) return;
        const s = await r.json();
        this.wdt = s.wdt || {};
      } catch (e) {}
    },
    wdtText() {
      const w = this.wdt || {};
      if (!w.wdtCount) return '暂无记录';
      return '累计 ' + w.wdtCount + ' 次 · 卡死任务 ' + (w.lastWdtTask || '未能定位') +
             ' · 开机 ' + (w.lastWdtUptimeSec || 0) + ' 秒' +
             (w.lastWdtVersion ? ' · 版本 ' + w.lastWdtVersion : '');
    },
    async wdtSelfTest() {
      if (this.wdtBusy || !this.wdt.enabled) return;
      if (typeof confirmDialog === 'function' &&
          !(await confirmDialog('确认看门狗自检',
              '所选任务将停止喂狗，设备约 ' + this.wdtTimeoutSec + ' 秒后自动重启。期间短信与网页服务中断，是否继续？', true))) return;
      this.wdtBusy = true;
      const r = await postOp('/api/wdt/selftest',
        { task: this.wdtTask, timeoutMs: Number(this.wdtTimeoutSec) * 1000 },
        '✓ 自检已启动，设备将自动重启');
      if (!r) this.wdtBusy = false;
    },

    init() { this.loadStatus(); this.loadCoredumpInfo(); this.loadWdt(); },
    async loadStatus() {
      try {
        const r = await fetch('/api/ota/status', { cache: 'no-store' });
        if (!r.ok) return;
        const s = await r.json();
        this.otaAvailable = !!s.available;
        this.curVer = s.currentVersion || '';
        this.latestVer = s.latestVersion || '';
        this.otaState = s.state || 0;
        this.otaProgress = s.progress || 0;
        this.otaMessage = s.message || '';
      } catch (e) {}
    },

    async checkUpdate() {
      if (this.busy() || !this.otaAvailable) return;
      this.pending = true;
      const r = await apiOp('/api/ota/version');   // 失败自动 toast
      if (!r) { this.pending = false; return; }
      this.startPolling();
    },
    async doUpgrade() {
      if (!this.latestVer || this.busy() || !this.otaAvailable) return;
      if (typeof confirmDialog === 'function' &&
          !(await confirmDialog('确认升级', '确认升级到 ' + this.latestVer + '？升级过程中设备会重启。'))) return;
      this.pending = true;
      const r = await apiOp('/api/ota/start', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ tag: this.latestVer }),
      });
      if (!r) { this.pending = false; return; }
      this.startPolling();
    },
    async uploadFirmware() {
      if (!this.firmwareFile || this.busy() || !this.otaAvailable) return;
      const fd = new FormData();
      fd.append('file', this.firmwareFile);
      this.pending = true;
      const r = await apiOp('/api/ota/upload', { method: 'POST', body: fd });
      if (!r) { await this.loadStatus(); this.pending = false; return; }
      this.startPolling();
    },
    async uploadLfs() {
      if (!this.lfsFile || this.busy() || !this.otaAvailable) return;
      const fd = new FormData();
      fd.append('file', this.lfsFile);
      this.pending = true;
      const r = await apiOp('/api/ota/upload-fs', { method: 'POST', body: fd });
      if (!r) { await this.loadStatus(); this.pending = false; return; }
      this.startPolling();
    },

    // 轮询 OTA 状态：每 1s 拉一次 /api/ota/status，直到终态或回到空闲
    startPolling() {
      if (this._polling) return;
      this._polling = true;
      this.pollCount = 0;
      const tick = async () => {
        await this.loadStatus();
        this.pollCount++;
        if (this.otaState === 4) { this._polling = false; toast('升级成功，设备重启中…', 'ok'); return; }
        if (this.otaState === 5) { this._polling = false; toast('升级失败：' + (this.otaMessage || ''), 'err'); return; }
        if (this.otaState !== 0) this.pending = false;               // 操作已真正开始
        if (this.otaState === 0 && !this.pending) { this._polling = false; return; }
        if (this.pollCount > 180) { this._polling = false; return; } // 安全上限 ~3 分钟
        setTimeout(tick, 1000);
      };
      tick();
    },

    onFirmware(e) { this.firmwareFile = e.target.files[0] || null; },
    onLfs(e) { this.lfsFile = e.target.files[0] || null; },

    // ---------- 配置导入导出 ----------
    importFile: null,    // 待导入的 JSON 文件
    importing: false,    // 导入进行中（避免重复点击）
    resetWarn: false,     // 已发送出厂复位指令

    // 导出配置：直接触发浏览器下载（GET /api/config/export 返回附件）
    exportConfig() {
      window.location.href = '/api/config/export';
    },

    // 选择文件
    onImportFile(e) { this.importFile = e.target.files[0] || null; },

    // 导入配置：读文件内容 -> POST 原始 JSON 到 /api/config/import
    async importConfig() {
      if (!this.importFile || this.importing) return;
      if (typeof confirmDialog === 'function' &&
          !(await confirmDialog('确认导入', '导入配置将覆盖当前 WiFi、账号、邮件与短信设置，并自动重启设备。是否继续？', true))) {
        return;
      }
      this.importing = true;
      try {
        const text = await this.importFile.text();
        const r = await fetch('/api/config/import', {
          method: 'POST',
          headers: { 'Content-Type': 'application/json' },
          body: text,
        });
        let data = {};
        try { data = await r.json(); } catch (e) {}
        if (!r.ok) {
          toast('✕ 导入失败：' + (zhError(data.error) || ('请求失败（HTTP ' + r.status + '）')), 'err', 6000);
          this.importing = false;
          return;
        }
        toast('✓ ' + (data.message || '导入成功，设备即将重启'), 'ok');
        // 成功后设备会在约 2 秒后重启，无需后续处理
      } catch (e) {
        toast('✕ 导入失败：' + (e && e.message ? e.message : e), 'err', 6000);
        this.importing = false;
      }
    },

    // ---------- 崩溃记录（coredump） ----------
    // panic 时 ESP-IDF 把内存快照写入 coredump 分区，这里只做查询 / 导出 / 清除
    cdHas: false,       // 分区中是否有未导出的崩溃快照
    cdLoaded: false,    // 信息是否查询过（区分「暂无」与「未查询」）
    cdBusy: false,      // 导出 / 清除进行中
    cdSize: 0,          // 快照字节数
    cdTime: 0,          // 崩溃时间（纪元秒，0 = 未知）
    cdVersion: '',      // 崩溃时固件版本（Release tag）
    cdElfUrl: '',       // 对应调试符号下载地址（版本已知时非空）

    cdTimeText() {
      if (!this.cdTime) return '时间未知';
      return fmtDateTime(this.cdTime);   // 统一按设备时区格式化
    },
    cdSizeText() {
      const b = this.cdSize;
      if (b >= 1048576) return (b / 1048576).toFixed(2) + ' MB';
      if (b >= 1024) return (b / 1024).toFixed(1) + ' KB';
      return b + ' B';
    },

    // 查询崩溃记录信息（页面初始化与清除后各调一次）
    async loadCoredumpInfo() {
      try {
        const r = await fetch('/api/tools/coredump/info', { cache: 'no-store' });
        if (!r.ok) return;
        const d = await r.json();
        this.cdHas = !!d.hasCoredump;
        this.cdSize = d.size || 0;
        this.cdTime = d.crashTime || 0;
        this.cdVersion = d.crashVersion || '';
        this.cdElfUrl = d.elfUrl || '';
        this.cdLoaded = true;
      } catch (e) {}
    },

    // 导出 coredump.bin：GET 附件下载（与导出配置同路线，页面不跳走）
    exportCoredump() {
      window.location.href = '/api/tools/coredump/export';
    },

    // 清除分区快照（不可恢复），成功后刷新信息
    async clearCoredump() {
      if (!(await confirmDialog('清除崩溃记录',
            '将擦除 coredump 分区中的崩溃快照，且无法恢复。确定继续？', true)))
        return;
      this.cdBusy = true;
      const r = await postOp('/api/tools/coredump/clear', {}, 'coredump 已清除');
      this.cdBusy = false;
      if (r) await this.loadCoredumpInfo();
    },

    // ---------- 系统维护：重启 / 出厂复位 ----------
    async restart() {
      if (!(await confirmDialog('重启设备', '确定要重启设备吗？重启过程中短信转发会短暂中断。')))
        return;
      await postOp('/api/system/restart', {}, '设备即将重启');
    },
    async reset() {
      if (!(await confirmDialog('出厂复位',
            '将清除全部配置（WiFi / 账号 / 短信等）与用户数据（短信记录 / 日志 / AT 历史），设备回到首次启动状态（AP 配网模式）。此操作不可恢复！',
            true)))
        return;
      const r = await postOp('/api/system/reset', {}, '已恢复出厂设置');
      if (r) this.resetWarn = true;
    },
  };
}
