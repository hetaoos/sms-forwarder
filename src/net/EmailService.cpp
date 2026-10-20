#include "net/EmailService.h"

#include <ArduinoJson.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <time.h>

#include <math.h>

// ReadyMail 通过宏裁剪协议实现，必须在 include 之前打开 SMTP。
// ENABLE_DEBUG 不打开：它会把整段 SMTP 会话打到串口，噪音过大。
#define ENABLE_SMTP
#include <ReadyMail.h>

#include "log/LogStore.h"
#include "net/WifiManager.h"
#include "sms/SmsService.h"
#include "time/TimeSync.h"
#include "utils/TlsTurn.h"  // TlsTurn：与 OTA / HTTP 推送共用 TLS 轮次锁

namespace {

// 本通信模组日志出口：全局日志环（串口由 LogStore 统一镜像输出）
LogStore &_log = devLog();

// ---- 网络超时 ----
// 取值原则：网络差时要足够宽容，但必须有硬上界，否则一次阻塞就能把主循环冻死。
// ReadyMail 默认 read 超时高达 120 秒，且认证后会把它重设回去，所以要在
// 连接前、认证后各压一次（见 EmailService::sendOnce）。
constexpr uint32_t kSocketTimeoutMs = cfg::kMailSocketTimeoutMs;

// ---- 全局 SMTP 客户端（不在头文件暴露，避免 ReadyMail 污染其它编译单元）----
WiFiClientSecure s_sslClient;
SMTPClient       s_smtp(s_sslClient);

// ---- HTML 统一样式表（class 版）----
// 与 esp32-sms-forwarding 一致：每个 <div>/<td> 都内联同一串 CSS 会大量重复，
// 这里集中定义一次，各构建函数只输出 class，体积更小、改风格只需动这一处。
// 兼容性说明（这是改用 class 的代价，需以实际收到的邮件为准）：
//   - Gmail 网页/iOS、Apple Mail、多数手机原生客户端：支持 <style> + class
//   - Outlook 桌面版（Word 排版引擎）：认 class，但不支持渐变等部分属性（标题栏会退化成纯色）
//   - 少数客户端会剥掉 <style>：此时正文退化为无样式表格（仍可读）
// 因此仅把「动态值」（卡片最大宽度、状态徽标配色）以内联方式保留。
const char MAIL_CSS[] =
  ".mw{background:#f4f6f8;padding:16px;font-family:-apple-system,Segoe UI,Helvetica,Arial,sans-serif}"
  ".card{margin:0 auto;background:#ffffff;border:1px solid #e3e8ee;border-radius:10px;overflow:hidden}"
  ".hd{background:linear-gradient(135deg,#4f8cff,#2f6fed);color:#ffffff;padding:14px 20px}"
  ".hd span{font-size:17px;font-weight:600}"
  ".blk{padding:16px 20px 4px}"                     /* 带小标题的区块 */
  ".sub{padding:14px 20px 4px}"                     /* 次级分组（顶部留白，避免标题贴住上一块的分隔线） */
  ".rows{padding:16px 20px}"                        /* 只有表格的区块 */
  ".cap{font-size:13px;font-weight:600;color:#2f6fed;margin:0 0 8px}"
  ".tb{width:100%;border-collapse:collapse;font-size:14px;color:#333;line-height:1.5}"
  ".k{padding:8px 0;width:96px;color:#888;vertical-align:top;white-space:nowrap}"
  ".v{padding:8px 0;border-bottom:1px solid #f0f0f0;word-break:break-all}"
  ".kn{padding:8px 0;width:64px;color:#888;vertical-align:top}"
  ".vn{padding:8px 0;border-bottom:1px solid #f0f0f0}"
  ".vl{padding:8px 0;word-break:break-word}"
  ".info{padding:12px 20px 0;border-top:1px solid #f0f0f0;margin-top:12px}"
  ".info .tb{font-size:13px;color:#666}"
  ".ft{padding:10px 20px;background:#fafbfc;color:#aaa;font-size:12px;border-top:1px solid #f0f0f0;margin-top:12px}"
  ".badge{margin:16px 20px 0;border-radius:8px;padding:12px 16px}"
  ".b1{font-size:14px;font-weight:600}"
  ".b2{font-size:13px;color:#666}"
  ".note{padding:16px 20px;font-size:14px;color:#333;line-height:1.6}"
  /* 验证码高亮块：淡红底 + 红框，居中；标签小字、码本身 28px 大字加宽字距，便于一眼抄写 */
  ".code{margin:16px 20px 0;background:#fff4f4;border:1px solid #ffd0d0;border-radius:8px;padding:12px 16px;text-align:center}"
  ".code1{font-size:12px;color:#c0392b;letter-spacing:2px}"
  ".code2{font-size:28px;font-weight:700;color:#d00;letter-spacing:5px;margin-top:2px}"
  ".hl{color:#2f6fed}";

// 设备唯一 ID：完整芯片 eFuse MAC，12 位十六进制（与参考项目一致）
String mailDeviceId() {
  uint64_t mac = ESP.getEfuseMac() >> 24;  // 高 48 位为 MAC
  char buf[13];
  snprintf(buf, sizeof(buf), "%012llx", (unsigned long long)mac);
  return String(buf);
}

// ---- 验证码识别（移植自 esp32-sms-forwarding 的 email.cpp）----

// 验证码关键词表。命中关键词附近的数字串优先作为验证码。
// 子串匹配：中文按字节比较即可（UTF-8 子串不会跨字边界误命中），
// 英文统一转小写后匹配，故表中只写小写形式。
// 注意：长词若包含表内短词则无需单列（如 "verification code" 已被 "code" 覆盖）。
const char *const VERIFY_CODE_KEYWORDS[] = {
    // 中文
    "验证码", "校验码", "确认码", "安全码", "交易码", "支付码", "授权码",
    "登录码", "激活码", "注册码", "动态码", "口令", "密码",
    // 英文
    "code", "otp", "one-time", "onetime",
};

// 关键词回溯窗口（字符数）：覆盖「您的短信验证码为 123456」这类在关键词与数字之间
// 夹了若干说明文字的写法。窗口太大会把「尾号1234…验证码8888」里的 1234 也判成命中。
constexpr int VERIFY_KEYWORD_WINDOW = 12;

// 只看 ASCII 大小写转换：直接用 String::toLowerCase() 会把 UTF-8 的多字节
// （char 为负）传给 tolower()，越界读表属于未定义行为，这里手动避开。
bool windowHasKeyword(const String &window) {
  String lower;
  lower.reserve(window.length());
  for (unsigned int i = 0; i < window.length(); i++) {
    unsigned char c = (unsigned char)window.charAt(i);
    lower += (char)((c >= 'A' && c <= 'Z') ? (c + 32) : c);
  }
  for (size_t i = 0; i < sizeof(VERIFY_CODE_KEYWORDS) / sizeof(VERIFY_CODE_KEYWORDS[0]); i++) {
    if (lower.indexOf(VERIFY_CODE_KEYWORDS[i]) >= 0) return true;
  }
  return false;
}

// 数字串左右边界是否「干净」：用于排除金额（1000.00）、日期（2026-10-01）、
// 编号（ID1234、No1234）这类同样 4~6 位的数字。
// 注意不要把 ':' '：' 之类分隔符号算作脏边界——「验证码:123456」是最常见写法。
bool isCodeBoundary(const String &s, int start, int end) {
  if (start > 0) {
    unsigned char p = (unsigned char)s.charAt(start - 1);
    if (isalnum(p) || p == '.' || p == '/') return false;
  }
  if (end < (int)s.length()) {
    unsigned char c = (unsigned char)s.charAt(end);
    if (isalnum(c)) return false;
    // 小数点/连字符后紧跟数字 → 小数或日期的一部分
    if ((c == '.' || c == '-' || c == '/') && end + 1 < (int)s.length() &&
        isdigit((unsigned char)s.charAt(end + 1))) {
      return false;
    }
  }
  return true;
}

// 取验证码：优先「关键词附近的 4~6 位数字串」；都没有关键词时回退到首个 4~6 位数字串。
// 返回空串表示未识别到。
String findVerifyCode(const String &message) {
  const int n = message.length();
  String fallback;   // 无关键词时的兜底：首个合法数字串
  String byKeyword;  // 关键词命中：最早出现的那个

  int i = 0;
  while (i < n) {
    if (isdigit((unsigned char)message.charAt(i))) {
      int j = i;
      while (j < n && isdigit((unsigned char)message.charAt(j))) j++;
      int len = j - i;
      if (len >= 4 && len <= 6 && isCodeBoundary(message, i, j)) {
        if (fallback.length() == 0) fallback = message.substring(i, j);
        if (byKeyword.length() == 0) {
          int ws = (i > VERIFY_KEYWORD_WINDOW) ? (i - VERIFY_KEYWORD_WINDOW) : 0;
          if (windowHasKeyword(message.substring(ws, i))) byKeyword = message.substring(i, j);
        }
      }
      i = j;
    } else {
      i++;
    }
  }
  return byKeyword.length() > 0 ? byKeyword : fallback;
}

// 扫描某一类括号的全部出现，按「到最近边缘的距离」更新最优候选。
// open/close 为括号字符串（多字节需注意字节长度 openLen/closeLen），其余参数为输入输出的最优候选。
void scanBracket(const String &msg, const char *open, int openLen, const char *close,
                 int closeLen, int &bestDist, int &bestSegStart, int &bestCStart,
                 int &bestCEnd) {
  int n = msg.length();
  int pos = 0;
  while (pos < n) {
    int o = msg.indexOf(open, pos);
    if (o < 0) break;
    int c = msg.indexOf(close, o + openLen);
    if (c < 0) break;
    int contentStart = o + openLen;  // 括号内纯内容起点（跳过整个开括号）
    int contentEnd = c;              // 闭括号起点（不含闭括号）
    if (contentEnd > contentStart) {
      int segStart = o;
      int segEnd = c + closeLen;
      // 到最近边缘（开头或结尾）的距离，越小越优先
      int dist = (segStart < (n - segEnd)) ? segStart : (n - segEnd);
      if (dist < bestDist || (dist == bestDist && segStart < bestSegStart)) {
        bestDist = dist;
        bestSegStart = segStart;
        bestCStart = contentStart;
        bestCEnd = contentEnd;
      }
    }
    pos = c + closeLen;
  }
}

}  // namespace

// ---------- 初始化 / 调度 ----------

void EmailService::begin(WifiManager &wifi, SmsService &sms, TimeSync &time) {
  _wifi = &wifi;
  _sms = &sms;
  _time = &time;
  _mutex = xSemaphoreCreateRecursiveMutex();

  // 与 HTTP 推送通道保持一致：跳过证书验证。
  // 嵌入式设备没有可信根证书存储，且多数 SMTP 服务商用自签/长链证书。
  s_sslClient.setInsecure();

  loadConfig();

  // 启动邮件：等 WiFi 连上再发（boot 阶段大概率还没连上）
  _startupPending = true;
  _startupTries = 0;
  _startupWifiMs = 0;
  _startupDueMs = millis() + cfg::kMailBootDelayMs;

  // 定时在线通知：间隔模式从开机起算一个周期；每日模式由 dailyDueLocked()
  // 按本地时间判定（_lastDailyEpoch 已从 NVS 载入，防重启后重复触发）
  if ((_notifyTypes & cfg::kEmailNotifyOnline) &&
      _sched.mode == cfg::kSchedModeInterval) {
    _onlineDueMs = millis() + (uint32_t)_sched.intervalH * 3600000UL;
  }

  _log.addf(LogLevel::kInfo, "mail", "邮件服务就绪（总开关%s，%s）",
            _enabled ? "开" : "关", isConfigured() ? "已配置" : "未配置完整");
}

void EmailService::tick() {
  uint32_t now = millis();

  // ---- 启动邮件：分两段等待 ----
  //   a) WiFi 未连：每 15s 探一次，超过 kMailBootWaitMaxMs 放弃（无网发不出）；
  //   b) WiFi 已连：给通信模组初始化 kMailBootModemWaitMs 宽限，期间通信模组一有结果就
  //      发，到期仍未出结果也照发（正文状态显示未就绪），不因通信模组异常而卡死。
  if (_startupPending && now >= _startupDueMs) {
    if (WiFi.status() != WL_CONNECTED) {
      // ---- a) 等 WiFi ----
      if (++_startupTries >= (uint8_t)(cfg::kMailBootWaitMaxMs / 15000)) {
        _startupPending = false;
        _log.addf(LogLevel::kWarn, "mail", "WiFi 长时间未连接，放弃启动邮件");
      } else {
        _startupDueMs = now + 15000;  // 每 15s 再探一次
      }
    } else {
      // ---- b) 等通信模组初始化（有宽限期，到期照发）----
      if (_startupWifiMs == 0) {
        // WiFi 首次连上：记录时刻并给出通信模组宽限期的截止点
        _startupWifiMs = now;
        _startupDueMs = now + cfg::kMailBootModemWaitMs;
      }
      // 未注入放行钩子时视为满足（只等 WiFi）
      bool gateOk = !_startupGate || _startupGate();
      bool graceOver = now >= _startupWifiMs + cfg::kMailBootModemWaitMs;
      if (gateOk || graceOver) {
        _startupPending = false;
        if (!gateOk && graceOver) {
          _log.addf(LogLevel::kWarn, "mail",
                    "通信模组初始化 %us 内未出结果，按宽限期照发启动邮件",
                    (unsigned)(cfg::kMailBootModemWaitMs / 1000));
        }
        Guard g(_mutex);
        if (!activeLocked()) {
          if (configuredLocked()) {
            _log.addf(LogLevel::kInfo, "mail", "邮件通知已停用，跳过启动邮件");
          }
        } else if (!(_notifyTypes & cfg::kEmailNotifyStartup)) {
          _log.addf(LogLevel::kInfo, "mail", "「设备启动」邮件通知已关闭，跳过");
        } else {
          enqueueStartupLocked();
        }
      }
      // 宽限期内且通信模组未出结果：下个 tick 再看（tick 每 loop 一次，无需改 dueMs）
    }
  }

  // ---- 定时在线通知（总控：notifyTypes 的 ONLINE 位；两种排程模式）----
  {
    Guard g(_mutex);
    bool onlineOn = (_notifyTypes & cfg::kEmailNotifyOnline) != 0;
    if (!onlineOn) {
      _onlineImmediate = false;  // 关闭时丢弃待补发的标记
    } else {
      bool fire = false;
      if (_onlineImmediate) {
        // 保存配置后补发一封（确认配置生效）；每日模式同时记录触发时刻，
        // 避免紧接着的定点判定在同一窗口内重复发送
        _onlineImmediate = false;
        if (_sched.mode == cfg::kSchedModeDaily) markDailyFiredLocked();
        fire = true;
      } else if (_sched.mode == cfg::kSchedModeDaily) {
        fire = dailyDueLocked();
      } else if (now >= _onlineDueMs) {
        _onlineDueMs = now + (uint32_t)_sched.intervalH * 3600000UL;
        fire = true;
      }
      if (fire && activeLocked()) enqueueOnlineLocked();
    }
  }

  // ---- 发送队列 ----
  if (_queueCount == 0) return;
  if (WiFi.status() != WL_CONNECTED) {
    _nextRetryMs = now + 5000;  // 等 WiFi，不消耗尝试次数
    return;
  }
  if (now < _nextRetryMs) return;

  MailJob job;
  {
    Guard g(_mutex);
    if (_queueCount == 0) return;
    job = _queue[0];  // 拷贝队首（String 深拷贝）
    for (uint8_t i = 1; i < _queueCount; i++) _queue[i - 1] = _queue[i];
    _queueCount--;
    _queue[_queueCount] = MailJob{};  // 清空尾槽，及时释放 String 内存
  }

  // HTML 正文在此构建（loop 任务，8KB 栈，正文缓冲走 PSRAM）——入队时只存了原始事件
  String subject, text;
  PsBuffer html;
  if (!buildMail(job, subject, html, text)) return;  // 已被停用/关闭/构建失败：静默丢弃

  bool configErr = false;
  bool ok = sendOnce(subject, html, text, configErr);
  if (ok) return;

  if (configErr) {
    // 认证失败等配置类错误：重试无意义，直接丢弃
    _log.addf(LogLevel::kError, "mail",
              "邮件配置错误（请检查账号与 SMTP 授权码），本次通知已丢弃");
    return;
  }
  job.attempts++;
  if (job.attempts >= cfg::kMailMaxAttempts) {
    _log.addf(LogLevel::kWarn, "mail", "多次重试后仍失败，本次通知已丢弃");
    return;
  }
  // 退避重试：第 2 次前等 2s，第 3 次前等 4s
  _nextRetryMs = millis() + (2000u << (job.attempts - 1));
  Guard g(_mutex);
  requeueFrontLocked(job);
}

// ---------- 配置 ----------

bool EmailService::enabled() const {
  Guard g(_mutex);
  return _enabled;
}

bool EmailService::setEnabled(bool on) {
  {
    Guard g(_mutex);
    if (_enabled == on) return true;  // 已是目标态，无需重复落盘
    _enabled = on;
    if (!on) {
      // 停用：丢弃待发队列与挂起的启动/在线邮件，设备立刻进入「零邮件」状态；
      // 已保存的 SMTP 配置与通知类型保留，重新开启后立即恢复
      for (uint8_t i = 0; i < _queueCount; i++) _queue[i] = MailJob{};
      _queueCount = 0;
      _startupPending = false;
      _onlineImmediate = false;
    }
    if (!persistConfig()) return false;
  }
  _log.addf(LogLevel::kInfo, "mail", "邮件通知总开关已%s", on ? "开启" : "停用");
  return true;
}

bool EmailService::isConfigured() const {
  Guard g(_mutex);
  return configuredLocked();
}

bool EmailService::configuredLocked() const {
  return _server.length() > 0 && _user.length() > 0 && _pass.length() > 0 &&
         _sendTo.length() > 0;
}

// ---------------- 配置只读快照（LCD 菜单「邮件通知」页用）----------------
// 全部持锁读一份副本返回：调用方（菜单渲染）不持锁，也就不存在锁序问题。
String EmailService::server() const { Guard g(_mutex); return _server; }
uint16_t EmailService::port() const { Guard g(_mutex); return _port; }
String EmailService::user() const { Guard g(_mutex); return _user; }
String EmailService::sendTo() const { Guard g(_mutex); return _sendTo; }
uint8_t EmailService::queueCount() const { Guard g(_mutex); return _queueCount; }
uint32_t EmailService::notifyTypes() const { Guard g(_mutex); return _notifyTypes; }
String EmailService::scheduleText() const { Guard g(_mutex); return schedText(); }

bool EmailService::notifyEnabled(uint32_t mask) const {
  Guard g(_mutex);
  return (_notifyTypes & mask) != 0;
}

bool EmailService::saveConfig(const String &server, uint16_t port,
                              const String &user, const String &pass,
                              const String &sendTo, uint32_t notifyTypes,
                              const MailSchedule &sched, bool enabled) {
  {
    Guard g(_mutex);
    _server = server;
    _server.trim();
    _port = (port > 0 && port <= 65535) ? port : cfg::kMailSmtpPortDefault;
    _user = user;
    _user.trim();
    // 授权码：空串表示不修改（Web 表单不回显已保存的值）
    if (pass.length() > 0) _pass = pass;
    _sendTo = sendTo;
    _sendTo.trim();
    _enabled = enabled;
    _notifyTypes = notifyTypes & cfg::kEmailNotifyAll;
    // 排程：模式只认两个已知值，字段越界钳制（防御性校验，Web 侧也已钳制）
    _sched.mode = (sched.mode == cfg::kSchedModeDaily) ? cfg::kSchedModeDaily
                                                       : cfg::kSchedModeInterval;
    _sched.hour = (sched.hour <= 23) ? sched.hour : 0;
    _sched.minute = (sched.minute <= 59) ? sched.minute : 0;
    uint16_t ih = sched.intervalH;
    if (ih < cfg::kMailIntervalHMin) ih = cfg::kMailIntervalHDefault;
    if (ih > cfg::kMailOnlineIntervalMaxH) ih = cfg::kMailOnlineIntervalMaxH;
    _sched.intervalH = ih;
    if (!persistConfig()) return false;
    if (!_enabled) {
      // 保存时停用：同样丢弃待发队列与挂起邮件（与 setEnabled(false) 一致）
      for (uint8_t i = 0; i < _queueCount; i++) _queue[i] = MailJob{};
      _queueCount = 0;
      _startupPending = false;
      _onlineImmediate = false;
    }
  }
  // 保存成功后补发一封在线通知，便于确认配置生效与设备在线
  // （与参考项目「保存后立即发送一次在线通知」行为一致）。
  // 间隔模式同时把周期计时推到下一个整周期，避免补发后立刻再触发一次。
  if (_enabled && (_notifyTypes & cfg::kEmailNotifyOnline)) {
    _onlineImmediate = true;
    _onlineDueMs = millis() + (uint32_t)_sched.intervalH * 3600000UL;
  }
  _log.addf(LogLevel::kInfo, "mail",
            "邮件配置已保存（总开关 %s，通知类型掩码 0x%02X，在线通知 %s）",
            _enabled ? "开" : "关", (unsigned)_notifyTypes, schedText().c_str());
  return true;
}

String EmailService::configJson() const {
  Guard g(_mutex);
  JsonDocument doc;
  doc["ok"] = true;
  doc["enabled"] = _enabled;         // SMTP 邮件通知总开关
  doc["configured"] = configuredLocked();
  doc["server"] = _server;
  doc["port"] = _port;
  doc["user"] = _user;
  doc["sendTo"] = _sendTo;
  doc["hasPass"] = _pass.length() > 0;  // 授权码不回显
  doc["notifyTypes"] = _notifyTypes;
  // 排程字段（前端按模式显示间隔小时 或 每日时/分）
  doc["schedMode"] = _sched.mode;
  doc["schedHour"] = _sched.hour;
  doc["schedMinute"] = _sched.minute;
  doc["intervalH"] = _sched.intervalH;
  String out;
  serializeJson(doc, out);
  return out;
}

// 导出完整配置（含授权码，供备份用）到给定 JSON 对象
void EmailService::exportTo(JsonObject &o) const {
  Guard g(_mutex);
  o["enabled"] = _enabled;
  o["server"] = _server;
  o["port"] = _port;
  o["user"] = _user;
  o["pass"] = _pass;            // 备份用，含授权码
  o["sendTo"] = _sendTo;
  o["notifyTypes"] = _notifyTypes;
  o["schedMode"] = _sched.mode;
  o["schedHour"] = _sched.hour;
  o["schedMinute"] = _sched.minute;
  o["intervalH"] = _sched.intervalH;
}

// 从给定 JSON 对象导入配置并落盘（缺失字段保持原值）
bool EmailService::importFrom(const JsonObject &o) {
  {
    Guard g(_mutex);
    _enabled = o["enabled"] | _enabled;
    _server = o["server"] | _server;
    int port = o["port"] | (int)_port;
    _port = (port > 0 && port <= 65535) ? (uint16_t)port : _port;
    _user = o["user"] | _user;
    if (o["pass"].is<const char *>()) _pass = o["pass"].as<String>();
    _sendTo = o["sendTo"] | _sendTo;
    uint32_t types = o["notifyTypes"] | _notifyTypes;
    _notifyTypes = types & cfg::kEmailNotifyAll;
    int mode = o["schedMode"] | (int)_sched.mode;
    _sched.mode = (mode == cfg::kSchedModeDaily) ? cfg::kSchedModeDaily
                                                 : cfg::kSchedModeInterval;
    int hour = o["schedHour"] | (int)_sched.hour;
    _sched.hour = (hour >= 0 && hour <= 23) ? (uint8_t)hour : _sched.hour;
    int minute = o["schedMinute"] | (int)_sched.minute;
    _sched.minute = (minute >= 0 && minute <= 59) ? (uint8_t)minute : _sched.minute;
    int ih = o["intervalH"] | (int)_sched.intervalH;
    if (ih < (int)cfg::kMailIntervalHMin) ih = cfg::kMailIntervalHDefault;
    if (ih > (int)cfg::kMailOnlineIntervalMaxH) ih = cfg::kMailOnlineIntervalMaxH;
    _sched.intervalH = (uint16_t)ih;
  }
  if (!persistConfig()) {
    _log.addf(LogLevel::kError, "mail", "邮件配置导入落盘失败");
    return false;
  }
  _log.addf(LogLevel::kInfo, "mail", "已从备份导入邮件配置");
  return true;
}

bool EmailService::loadConfig() {
  Preferences prefs;
  String json;
  if (prefs.begin(cfg::kNvsNamespace, true)) {
    json = prefs.getString(cfg::kNvsKeyEmail, "");
    prefs.end();
  }
  if (!json.length()) {
    _log.addf(LogLevel::kInfo, "mail", "NVS 无邮件配置，使用默认值（四类通知全开）");
    return false;
  }
  JsonDocument doc;
  if (deserializeJson(doc, json) != DeserializationError::Ok) {
    _log.addf(LogLevel::kWarn, "mail", "邮件配置解析失败，使用默认值");
    return false;
  }
  Guard g(_mutex);
  // 总开关：NVS 无该字段时默认关闭（新设备出厂不启用邮件，避免未配置就发信）。
  // 已配置的设备此前落盘必有 enabled 字段，不受此默认值影响。
  _enabled = doc["enabled"] | false;
  _server = doc["server"] | "";
  int port = doc["port"] | (int)cfg::kMailSmtpPortDefault;
  _port = (port > 0 && port <= 65535) ? (uint16_t)port : cfg::kMailSmtpPortDefault;
  _user = doc["user"] | "";
  _pass = doc["pass"] | "";
  _sendTo = doc["sendTo"] | "";
  uint32_t types = doc["notifyTypes"] | cfg::kEmailNotifyDefault;
  _notifyTypes = types & cfg::kEmailNotifyAll;

  // 排程：兼容旧配置的 onlineH 字段（旧固件只有「间隔小时」一种模式，
  // onlineH>0 视为间隔模式；为 0 视为关闭——由 notifyTypes 位统一管控后
  // 旧配置迁移为默认周期）
  int mode = doc["schedMode"] | (int)cfg::kSchedModeInterval;
  _sched.mode = (mode == cfg::kSchedModeDaily) ? cfg::kSchedModeDaily
                                               : cfg::kSchedModeInterval;
  int hour = doc["schedHour"] | 9;
  _sched.hour = (hour >= 0 && hour <= 23) ? (uint8_t)hour : 9;
  int minute = doc["schedMinute"] | 0;
  _sched.minute = (minute >= 0 && minute <= 59) ? (uint8_t)minute : 0;
  int ih = cfg::kMailIntervalHDefault;
  if (doc["intervalH"].is<int>()) {
    ih = doc["intervalH"].as<int>();
  } else if (doc["onlineH"].is<int>() && doc["onlineH"].as<int>() > 0) {
    ih = doc["onlineH"].as<int>();  // 旧配置迁移
  }
  if (ih < (int)cfg::kMailIntervalHMin) ih = cfg::kMailIntervalHDefault;
  if (ih > (int)cfg::kMailOnlineIntervalMaxH) ih = cfg::kMailOnlineIntervalMaxH;
  _sched.intervalH = (uint16_t)ih;

  // 每日模式上次触发时刻（防重启后补发当天已发的通知）
  int64_t lastDaily = doc["lastDaily"] | 0;
  _lastDailyEpoch = (lastDaily > 0 && lastDaily < 0xFFFFFFFFLL) ? (uint32_t)lastDaily : 0;

  _log.addf(LogLevel::kInfo, "mail",
            "邮件配置已载入：server=%s port=%u notify=0x%02X 在线通知[%s]",
            _server.c_str(), (unsigned)_port, (unsigned)_notifyTypes,
            schedText().c_str());
  return true;
}

bool EmailService::persistConfig() {
  JsonDocument doc;
  doc["ver"] = 1;
  doc["enabled"] = _enabled;
  doc["server"] = _server;
  doc["port"] = _port;
  doc["user"] = _user;
  doc["pass"] = _pass;
  doc["sendTo"] = _sendTo;
  doc["notifyTypes"] = _notifyTypes;
  doc["schedMode"] = _sched.mode;
  doc["schedHour"] = _sched.hour;
  doc["schedMinute"] = _sched.minute;
  doc["intervalH"] = _sched.intervalH;
  doc["lastDaily"] = _lastDailyEpoch;
  String out;
  serializeJson(doc, out);

  Preferences prefs;
  if (!prefs.begin(cfg::kNvsNamespace, false)) return false;
  size_t written = prefs.putString(cfg::kNvsKeyEmail, out);
  prefs.end();
  return written == out.length();
}

// ---------- 定时在线通知排程 ----------

bool EmailService::dailyDueLocked() {
  // 每日模式依赖本地时间：NTP / SIM 未同步时系统时钟不可信，不触发
  if (!_time || !_time->isSynced()) return false;
  time_t nowT = time(nullptr);
  if (nowT < 100000) return false;  // 时钟明显异常（未同步），跳过

  struct tm t;
  localtime_r(&nowT, &t);
  struct tm trig = t;
  trig.tm_hour = _sched.hour;
  trig.tm_min = _sched.minute;
  trig.tm_sec = 0;
  // mktime 会归一化：触发点已过则得到今天的时刻，未到则得到「今天更晚」的
  // 时刻（大于 now，下方判定不成立），跨天边界（如 23:50 触发 00:10）同样正确
  time_t trigEpoch = mktime(&trig);

  // 仅在触发时刻起的窗口内触发一次。窗口而非精确到分：tick 可能被邮件发送
  // （最长约 45s）或 AT 交互拖过触发分，窗口内的迟到触发仍然有效。
  // 上次触发时刻落盘于 NVS（lastDaily），重启后不会补发当天已发的通知。
  if (nowT >= trigEpoch && nowT < trigEpoch + (time_t)cfg::kMailDailyWindowSec &&
      _lastDailyEpoch < (uint32_t)trigEpoch) {
    markDailyFiredLocked();
    return true;
  }
  return false;
}

void EmailService::markDailyFiredLocked() {
  time_t nowT = time(nullptr);
  if (nowT < 100000) return;  // 时间未同步，不记录
  if (_lastDailyEpoch >= (uint32_t)nowT) return;  // 已有更晚的记录
  _lastDailyEpoch = (uint32_t)nowT;
  persistConfig();  // 落盘（每日最多一次，开销可忽略）
}

String EmailService::schedText() const {
  Guard g(_mutex);
  if (_sched.mode == cfg::kSchedModeDaily) {
    char buf[24];
    snprintf(buf, sizeof(buf), "每日 %02u:%02u", (unsigned)_sched.hour,
             (unsigned)_sched.minute);
    return String(buf);
  }
  return String("每 ") + String(_sched.intervalH) + " 小时";
}

// ---------- 通知入队 ----------

void EmailService::notifySms(const String &sender, const String &message,
                             uint32_t ts, int8_t modem) {
  // 运行于抽数任务上下文：只做浅入队，不构建 HTML（模板拼接约 2-3KB 栈）
  Guard g(_mutex);
  if (!activeLocked()) return;  // 总开关关闭或未配置时直接返回，避免每条短信都打日志
  if (!(_notifyTypes & cfg::kEmailNotifySms)) return;
  if (message.length() == 0) return;

  MailJob job;
  job.kind = kSms;
  job.sender = sender;
  job.message = message;
  job.ts = ts;
  job.modem = modem;
  enqueueLocked(job);
}

void EmailService::notifySim(uint8_t port, bool ready) {
  // 同上：抽数任务上下文，浅入队
  Guard g(_mutex);
  if (!activeLocked()) return;
  if (!(_notifyTypes & cfg::kEmailNotifySim)) {
    _log.addf(LogLevel::kInfo, "mail", "「SIM状态变更」邮件通知已关闭，跳过");
    return;
  }

  MailJob job;
  job.kind = kSim;
  job.port = port;
  job.ready = ready;
  enqueueLocked(job);
}

void EmailService::notifyStartup() {
  _startupPending = true;
  _startupTries = 0;
  _startupWifiMs = 0;
  _startupDueMs = millis() + cfg::kMailBootDelayMs;
}

void EmailService::notifyCall(const String &number, uint32_t ts, int8_t modem) {
  // 运行于 tick 主循环上下文（SmsService 来电派发）：只做浅入队
  Guard g(_mutex);
  if (!activeLocked()) return;
  if (!(_notifyTypes & cfg::kEmailNotifyCall)) return;
  MailJob job;
  job.kind = kCall;
  job.sender = number;  // 复用 sender 字段存主叫号码
  job.ts = ts;
  job.modem = modem;
  enqueueLocked(job);
}

void EmailService::notifyOnline() {
  Guard g(_mutex);
  if (!activeLocked()) return;
  if (!(_notifyTypes & cfg::kEmailNotifyOnline)) return;
  MailJob job;
  job.kind = kOnline;
  enqueueLocked(job);
}

void EmailService::enqueueStartupLocked() {
  MailJob job;
  job.kind = kStartup;
  enqueueLocked(job);
}

void EmailService::enqueueOnlineLocked() {
  MailJob job;
  job.kind = kOnline;
  enqueueLocked(job);
}

// 由原始事件构建邮件正文（loop 任务上下文，可做重模板拼接）。
// 入队与发送之间配置可能被改动，这里做最终判定：
// 服务未启用/未配置，或该类通知已关闭 -> 返回 false（调用方静默丢弃）。
bool EmailService::buildMail(const MailJob &job, String &subject,
                             PsBuffer &html, String &text) const {
  Guard g(_mutex);
  if (!activeLocked()) {
    if (configuredLocked()) {
      _log.addf(LogLevel::kInfo, "mail", "邮件通知已停用，丢弃待发邮件");
    }
    return false;
  }
  uint32_t bit = 0;
  switch (job.kind) {
    case kSms:     bit = cfg::kEmailNotifySms; break;
    case kSim:     bit = cfg::kEmailNotifySim; break;
    case kStartup: bit = cfg::kEmailNotifyStartup; break;
    case kOnline:  bit = cfg::kEmailNotifyOnline; break;
    case kCall:    bit = cfg::kEmailNotifyCall; break;
    case kTest:    break;  // 测试邮件不受通知类型限制
  }
  if (bit && !(_notifyTypes & bit)) return false;

  switch (job.kind) {
    case kSms:
      buildSmsMail(job.sender, job.message, job.ts, job.modem, subject, html,
                   text);
      break;
    case kSim:
      buildSimMail(job.port, job.ready, subject, html, text);
      break;
    case kStartup:
      buildStartupMail(subject, html, text);
      break;
    case kOnline:
      buildOnlineMail(subject, html, text);
      break;
    case kCall:
      buildCallMail(job.sender, job.ts, job.modem, subject, html, text);
      break;
    case kTest:
      buildTestMail(subject, html, text);
      break;
    default:
      return false;
  }
  if (!html.ok()) {
    // PSRAM 与 DRAM 都分配不出来：正文不完整，宁可不发也不发坏邮件
    _log.addf(LogLevel::kError, "mail", "邮件正文缓冲分配失败，本次通知已丢弃");
    return false;
  }
  return true;
}

// 仅入队后立即返回：构建（含 PSRAM 正文缓冲）与 SMTP 发送都在 tick() 的
// loop 任务里完成。运行于 HTTP（async_tcp）任务时绝不能同步发送——该任务栈
// 不足以支撑 TLS 握手，会栈溢出重启，且长时间阻塞会拖住整个 Web 服务。
bool EmailService::requestTest() {
  Guard g(_mutex);
  if (!activeLocked()) return false;
  MailJob job;
  job.kind = kTest;
  job.attempts = 0;
  enqueueLocked(job);
  _log.addf(LogLevel::kInfo, "mail", "测试邮件已入队，稍后由主循环发送");
  return true;
}

// ---------- 队列 ----------

void EmailService::enqueueLocked(const MailJob &job) {
  if (_queueCount >= cfg::kMailQueueSize) {
    _log.addf(LogLevel::kWarn, "mail", "发送队列已满，丢弃本次通知");
    return;
  }
  _queue[_queueCount++] = job;
}

void EmailService::requeueFrontLocked(const MailJob &job) {
  if (_queueCount >= cfg::kMailQueueSize) return;  // 刚从队首取出，理论上不会满
  for (uint8_t i = _queueCount; i > 0; i--) _queue[i] = _queue[i - 1];
  _queue[0] = job;
  _queueCount++;
}

// ---------- 发送 ----------

bool EmailService::sendOnce(const String &subject, const PsBuffer &html,
                            const String &text, bool &configErr) {
  configErr = false;

  // 快照配置（持锁拷贝后立即释放，避免整个 SMTP 会话期间挡住 Web 保存）
  String server, user, pass, sendTo;
  uint16_t port;
  {
    Guard g(_mutex);
    if (!_enabled) {
      // 运行中被停用（Web 关闭总开关）：待发邮件直接丢弃，不消耗重试
      _log.addf(LogLevel::kInfo, "mail", "邮件通知已停用，丢弃待发邮件");
      configErr = true;
      return false;
    }
    if (!configuredLocked()) {
      _log.addf(LogLevel::kWarn, "mail", "邮件配置不完整，跳过发送");
      configErr = true;  // 配置类问题，重试无意义
      return false;
    }
    server = _server;
    user = _user;
    pass = _pass;
    sendTo = _sendTo;
    port = _port;
  }
  if (html.length() == 0 && text.length() == 0) {
    _log.addf(LogLevel::kWarn, "mail", "邮件正文为空，跳过发送");
    return false;
  }

  auto statusCallback = [](SMTPStatus status) {
    if (status.text.length() > 0) {
      _log.addf(LogLevel::kDebug, "mail", "SMTP：%s", status.text.c_str());
    }
  };

  // 与 OTA 检查/升级、HTTP 推送共用 TLS 轮次锁：任意时刻最多一个 mbedTLS 上下文
  // 在建/在用，避免两个上下文并存撑爆堆（-32512）。OTA 升级期间此处以非阻塞方式
  // 让出（拿不到锁即返回 false，由 App::tick 重试），不会卡死主循环。
  TlsTurn tlsTurn(0);
  if (!tlsTurn.held()) return false;

  // 清理上一次尝试的残留连接状态，确保每次从干净状态开始
  s_smtp.stop();

  if (!s_smtp.connect(server, (uint16_t)port, statusCallback)) {
    _log.addf(LogLevel::kWarn, "mail", "邮件服务器连接失败：%s:%u", server.c_str(),
              (unsigned)port);
    return false;
  }

  // 必须在 connect() 成功、socket 有效之后再设置读写超时：
  // stop() 之后立即调用 setTimeout() 会对一个已失效（fd=0）的句柄执行
  // setsockopt，触发 "Bad file number" (EBADF) 告警（不影响功能，但刷屏）。
  // 连接阶段本身走 WiFiClient 默认的 3s 超时；此处覆盖随后的 TLS 握手与收发阶段。
  s_sslClient.setTimeout(kSocketTimeoutMs);

  if (!s_smtp.authenticate(user, pass, readymail_auth_password)) {
    // 认证失败属配置错误，重试无意义
    _log.addf(LogLevel::kError, "mail", "邮件认证失败（请检查账号与 SMTP 授权码）");
    s_smtp.stop();
    configErr = true;
    return false;
  }

  // 认证完成后库会把 socket 超时改成它自己的 120 秒，这里压回我们的上限
  s_sslClient.setTimeout(kSocketTimeoutMs);

  // HTML 超长时降级为纯文本（编码后体积会膨胀数倍，堆有限）
  bool useHtml = html.length() > 0;
  if (useHtml && html.length() > (size_t)kMaxHtmlChars) {
    _log.addf(LogLevel::kWarn, "mail", "HTML 正文过长（%u 字符），本次仅发送纯文本",
              (unsigned)html.length());
    useHtml = false;
    if (text.length() == 0) {
      s_smtp.stop();
      return false;
    }
  }

  SMTPMessage msg;
  msg.headers.add(rfc822_from, String("SMS Notification <" + user + ">"));
  msg.headers.add(rfc822_to, sendTo);
  msg.headers.add(rfc822_subject, subject);
  // 有 HTML 就只发 HTML（单部分 text/html），否则退回纯文本。
  // PSRAM 缓冲以「静态内容」形式交给库：ReadyMail 直接读指针，不会复制回 DRAM。
  if (useHtml) {
    msg.html.body(html.c_str(), html.length());
  } else {
    msg.text.body(text);
  }
  msg.timestamp = (uint32_t)time(nullptr);

  bool ok = s_smtp.send(msg);
  s_smtp.stop();
  if (ok) {
    _log.addf(LogLevel::kInfo, "mail", "邮件发送成功：%s", subject.c_str());
  } else {
    _log.addf(LogLevel::kWarn, "mail", "邮件发送失败：%s", subject.c_str());
  }
  return ok;
}

// ---------- 模板工具 ----------

String EmailService::escape(const String &s) const {
  String result;
  result.reserve(s.length());
  for (unsigned int i = 0; i < s.length(); i++) {
    char c = s.charAt(i);
    if (c == '&')       result += "&amp;";
    else if (c == '<')  result += "&lt;";
    else if (c == '>')  result += "&gt;";
    else if (c == '"')  result += "&quot;";
    else if (c == '\n') result += "<br>";
    else if (c == '\r') { /* 忽略，避免与 \n 重复 */ }
    else                result += c;
  }
  return result;
}

void EmailService::appendEscaped(PsBuffer &out, const String &s) const {
  for (unsigned int i = 0; i < s.length(); i++) {
    char c = s.charAt(i);
    if (c == '&')       out.append("&amp;");
    else if (c == '<')  out.append("&lt;");
    else if (c == '>')  out.append("&gt;");
    else if (c == '"')  out.append("&quot;");
    else if (c == '\n') out.append("<br>");
    else if (c == '\r') { /* 忽略，避免与 \n 重复 */ }
    else                out.append(c);
  }
}

void EmailService::appendRow(PsBuffer &out, const String &key,
                             const String &val) const {
  out.append("<tr><td class=\"k\">");
  appendEscaped(out, key);
  out.append("</td><td class=\"v\">");
  appendEscaped(out, val);
  out.append("</td></tr>");
}

void EmailService::appendLinkRow(PsBuffer &out, const String &key,
                                 const String &url) const {
  if (url.length() == 0) {
    appendRow(out, key, String("未知"));
    return;
  }
  out.append("<tr><td class=\"k\">");
  appendEscaped(out, key);
  out.append("</td><td class=\"v\"><a href=\"");
  appendEscaped(out, url);
  out.append("\" style=\"color:#2f6fed;text-decoration:underline;word-break:break-all\">");
  appendEscaped(out, url);
  out.append("</a></td></tr>");
}

void EmailService::appendTableOpen(PsBuffer &out) const {
  out.append("<div class=\"blk\"><table class=\"tb\">");
}

void EmailService::appendTableClose(PsBuffer &out) const {
  out.append("</table></div>");
}

void EmailService::appendSectionOpen(PsBuffer &out,
                                     const String &caption) const {
  out.append("<div class=\"sub\"><div class=\"cap\">");
  appendEscaped(out, caption);
  out.append("</div><table class=\"tb\">");
}

void EmailService::appendSectionClose(PsBuffer &out) const {
  out.append("</table></div>");
}

void EmailService::appendBadge(PsBuffer &out, const String &main,
                               const String &sub, bool ok) const {
  // 徽标配色是动态值（正常/异常），仍用内联
  out.append("<div class=\"badge\" style=\"background:");
  out.append(ok ? "#e6f7ec" : "#fff4e6");
  out.append(";border:1px solid ");
  out.append(ok ? "#bfe9cd" : "#ffd9a8");
  out.append(";\"><span class=\"b1\" style=\"color:");
  out.append(ok ? "#1a8a4a" : "#c97a00");
  out.append(";\">");
  appendEscaped(out, main);
  out.append("</span>");
  if (sub.length() > 0) {
    out.append(" <span class=\"b2\">");
    appendEscaped(out, sub);
    out.append("</span>");
  }
  out.append("</div>");
}

void EmailService::appendNote(PsBuffer &out, const String &note) const {
  out.append("<div class=\"note\">");
  appendEscaped(out, note);
  out.append("</div>");
}

void EmailService::buildHtml(PsBuffer &out, const String &title,
                             const PsBuffer &inner, int maxWidth) const {
  // 样式表随每封邮件一起发出：邮件正文是片段，没有 <head> 可放 <style>，只能贴在正文开头。
  // MAIL_CSS 常驻 flash（内存映射），直接按指针追加，不必先拷成 String。
  out.append("<style>");
  out.append(MAIL_CSS);
  out.append("</style>");
  out.append("<div class=\"mw\">");
  // 卡片宽度按邮件类型可变，动态值保留内联
  out.append("<div class=\"card\" style=\"max-width:");
  out.append(String(maxWidth));
  out.append("px\">");
  out.append("<div class=\"hd\"><span>");
  out.append(title);  // 可带 emoji，调用方保证已转义
  out.append("</span></div>");
  out.append(inner);
  appendInfoBlock(out);
  out.append("<div class=\"ft\" style=\"display:flex;justify-content:space-between;align-items:center\">"
             "<span>SMS Forwarder · 短信转发通知</span>"
             "<span style=\"text-align:right;flex:1\">已运行 ");
  out.append(formatUptime(millis()));
  out.append("</span></div>");
  out.append("</div></div>");
}

// ---------- 模板辅助 ----------

void EmailService::appendInfoBlock(PsBuffer &out) const {
  out.append("<div class=\"info\"><table class=\"tb\">");
  appendRow(out, String("系统时间"),
            (_time && _time->isSynced()) ? _time->dateStr() : String("未同步"));

  // 每个启用的通信模组一行「本机号码 N」；一个号码都取不到时才回退到设备 IP。
  // 每条通知都应能看出「什么时候、哪台设备」，因此统一在这里追加。
  // 号码行先攒进独立缓冲：只有确实拿到号码才并入，否则回退 IP 行。
  bool hasNum = false;
  JsonDocument doc;
  if (_sms && deserializeJson(doc, _sms->modemsJson()) == DeserializationError::Ok &&
      doc.is<JsonArray>()) {
    int enabled = 0;
    for (JsonObject m : doc.as<JsonArray>()) {
      if (m["enabled"] | false) enabled++;
    }
    bool multi = enabled > 1;
    PsBuffer nums;
    for (JsonObject m : doc.as<JsonArray>()) {
      if (!(m["enabled"] | false)) continue;
      String num = m["num"] | "";
      if (num.length() == 0 || num == "未知") continue;
      int port = m["port"] | 0;
      appendRow(nums,
                multi ? (String("本机号码") + String(port)) : String("本机号码"),
                num);
      hasNum = true;
    }
    if (hasNum && nums.ok()) out.append(nums);
    else hasNum = false;
  }
  if (!hasNum && _wifi) {
    appendRow(out, String("设备IP"), _wifi->ip());  // 无号码时用 IP 标识设备
  }
  out.append("</table></div>");
}

void EmailService::appendDeviceRows(PsBuffer &out) const {
  if (_wifi) {
    appendLinkRow(out, String("设备地址"), String("http://") + _wifi->ip());
    appendRow(out, String("IP地址"), _wifi->ip());
  }
  appendRow(out, String("MAC地址"), WiFi.macAddress());
  appendRow(out, String("设备名称"), String("SMS-Forwarder-") + mailDeviceId());
  appendRow(out, String("WiFi信号"), String(WiFi.RSSI()) + " dBm");
  appendRow(out, String("运行时长"), formatUptime(millis()));
  appendChipRows(out);
}

void EmailService::appendChipRows(PsBuffer &out) const {
  // 芯片型号与片内温度：与运行无关的硬件信息，排查型号差异/过热时有用。
  // 部分芯片或 SDK 未使能温度传感器会返回 NaN，此时展示「不支持」，不打断整封邮件。
  String chipModel = ESP.getChipModel();
  String mdl = (chipModel.length() > 0)
                   ? (chipModel + " rev " + String((int)ESP.getChipRevision()))
                   : String("未知");
  appendRow(out, String("芯片型号"), mdl);

  float tempC = temperatureRead();
  if (isnan(tempC)) {
    appendRow(out, String("设备温度"), String("不支持"));
  } else {
    char buf[16];
    snprintf(buf, sizeof(buf), "%.1f °C", tempC);
    appendRow(out, String("设备温度"), String(buf));
  }
}

void EmailService::appendModemGroupSection(PsBuffer &out) const {
  JsonDocument doc;
  if (_sms && deserializeJson(doc, _sms->modemsJson()) == DeserializationError::Ok &&
      doc.is<JsonArray>()) {
    int enabled = 0;
    for (JsonObject m : doc.as<JsonArray>()) {
      if (m["enabled"] | false) enabled++;
    }
    bool multi = enabled > 1;

    // 按通信模组分组：把「通信模组信息」与「SIM/信号」合并到同一个分组输出，
    // 每个启用的通信模组一个分组；多通信模组时分组标题带序号（通信模组 1 / 通信模组 2），
    // 单通信模组时不加序号（通信模组），与参考项目风格一致。
    bool any = false;
    for (JsonObject m : doc.as<JsonArray>()) {
      if (!(m["enabled"] | false)) continue;
      any = true;
      int port = m["port"] | 0;
      appendSectionOpen(out,
                        multi ? (String("通信模组 ") + String(port)) : String("通信模组"));
      String vendor = m["vendor"] | "";
      String model = m["model"] | "";
      String revision = m["revision"] | "";
      appendRow(out, String("制造商"), vendor.length() > 0 ? vendor : String("未知"));
      appendRow(out, String("通信模组型号"), model.length() > 0 ? model : String("未知"));
      appendRow(out, String("通信模组固件"),
                revision.length() > 0 ? revision : String("未知"));
      String imei = m["imei"] | "";
      appendRow(out, String("IMEI"), imei.length() > 0 ? imei : String("未知"));
      String num = m["num"] | "";
      appendRow(out, String("本机号码"),
                (num.length() > 0 && num != "未知") ? num : String("未知"));
      appendRow(out, String("SIM状态"),
                (m["sim"] | false) ? String("已就绪") : String("未就绪"));
      String oper = m["oper"] | "";
      String operCode = m["operCode"] | "";
      if (operCode.length() > 0) {
        String cn = operCn(operCode);
        appendRow(out, String("运营商"),
                  cn.length() > 0 ? (operCode + " · " + cn) : operCode);
      } else {
        appendRow(out, String("运营商"),
                  oper.length() > 0 ? oper : String("未知"));
      }
      appendRow(out, String("信号强度"), signalText(m["csq"] | -1));
      appendSectionClose(out);
    }
    if (any) return;
  }
  appendSectionOpen(out, String("通信模组"));
  appendRow(out, String("通信模组型号"), String("未启用任何通信模组"));
  appendSectionClose(out);
}

void EmailService::appendSimRows(PsBuffer &out) const {
  bool any = false;
  JsonDocument doc;
  if (_sms && deserializeJson(doc, _sms->modemsJson()) == DeserializationError::Ok &&
      doc.is<JsonArray>()) {
    int enabled = 0;
    for (JsonObject m : doc.as<JsonArray>()) {
      if (m["enabled"] | false) enabled++;
    }
    bool multi = enabled > 1;
    for (JsonObject m : doc.as<JsonArray>()) {
      if (!(m["enabled"] | false)) continue;
      any = true;
      int port = m["port"] | 0;
      String p = multi ? (String("通信模组") + String(port) + " ") : String("");
      appendRow(out, p + "SIM状态",
                (m["sim"] | false) ? String("已就绪") : String("未就绪"));
      String operCode = m["operCode"] | "";
      String oper = m["oper"] | "";
      if (operCode.length() > 0) {
        String cn = operCn(operCode);
        appendRow(out, p + "运营商",
                  cn.length() > 0 ? (operCode + " · " + cn) : operCode);
      } else {
        appendRow(out, p + "运营商",
                  oper.length() > 0 ? oper : String("未知"));
      }
      appendRow(out, p + "信号强度", signalText(m["csq"] | -1));
    }
  }
  if (!any) {
    appendRow(out, String("SIM状态"), String("未启用任何通信模组"));
  }
}

bool EmailService::simOverallReady() const {
  JsonDocument doc;
  if (_sms && deserializeJson(doc, _sms->modemsJson()) == DeserializationError::Ok &&
      doc.is<JsonArray>()) {
    int enabled = 0;
    for (JsonObject m : doc.as<JsonArray>()) {
      if (!(m["enabled"] | false)) continue;
      enabled++;
      if (m["sim"] | false) return true;
    }
    if (enabled == 0) return true;  // 没接通信模组时设备本身算健康（不因无 SIM 报警）
  }
  return true;
}

String EmailService::simOverallStateText() const {
  JsonDocument doc;
  if (_sms && deserializeJson(doc, _sms->modemsJson()) == DeserializationError::Ok &&
      doc.is<JsonArray>()) {
    int enabled = 0;
    bool anyReady = false;
    for (JsonObject m : doc.as<JsonArray>()) {
      if (!(m["enabled"] | false)) continue;
      enabled++;
      if (m["sim"] | false) anyReady = true;
    }
    if (enabled == 0) return "未启用通信模组";
    return anyReady ? "已就绪" : "未就绪";
  }
  return "未知";
}

String EmailService::simStateText(uint8_t port) const {
  JsonDocument doc;
  if (_sms && deserializeJson(doc, _sms->modemsJson()) == DeserializationError::Ok &&
      doc.is<JsonArray>()) {
    for (JsonObject m : doc.as<JsonArray>()) {
      if ((m["port"] | 0) != (int)port) continue;
      if (!(m["enabled"] | false)) return "通信模组未启用";
      return (m["sim"] | false) ? "已就绪" : "未就绪";
    }
  }
  return "未知";
}

String EmailService::modemLabelOf(int8_t modem) const {
  if (modem < 1 || modem > cfg::kSmsMaxModems) return "";
  JsonDocument doc;
  if (_sms && deserializeJson(doc, _sms->modemsJson()) == DeserializationError::Ok &&
      doc.is<JsonArray>()) {
    for (JsonObject m : doc.as<JsonArray>()) {
      if ((m["port"] | 0) != (int)modem) continue;
      String alias = m["alias"] | "";
      alias.trim();
      if (alias.length() > 0) return alias;
      break;
    }
  }
  return String("通信模组") + String(modem);
}

String EmailService::signalText(int csq) {
  if (csq < 0 || csq > 31) return "未知";
  return String(csq) + "/31（约 " + (-113 + 2 * csq) + " dBm）";
}

String EmailService::operCn(const String &code) {
  if (code == "CMCC") return "中国移动";
  if (code == "CUCC") return "中国联通";
  if (code == "CTCC") return "中国电信";
  if (code == "CBN") return "中国广电";
  return "";
}

String EmailService::formatTs(uint32_t ts) {
  // 短信时间戳统一为纪元秒；小于 1.6e9 说明设备尚未同步时间（开机秒数）
  if (ts < 1600000000u) return "时间未同步";
  time_t t = (time_t)ts;
  struct tm tmInfo;
  localtime_r(&t, &tmInfo);
  char buf[32];
  strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tmInfo);
  return String(buf);
}

String EmailService::formatUptime(uint32_t ms) {
  // 移植自 esp32-sms-forwarding 的 MsgContext::formatUptime
  uint32_t seconds = ms / 1000;
  uint32_t minutes = seconds / 60;
  uint32_t hours = minutes / 60;
  uint32_t days = hours / 24;

  if (ms < 60000UL) return "不足1分钟";
  if (ms < 3600000UL) return String(minutes) + "分钟";
  if (ms < 86400000UL) return String(hours) + "小时" + String(minutes % 60) + "分";
  return String(days) + "天" + String(hours % 24) + "小时";
}

// ---------- 短信元数据解析 ----------

void EmailService::parseSmsMeta(const String &message, String &senderName,
                                String &verifyCode) {
  senderName = "";
  verifyCode = "";

  int bestDist = 0x7FFFFFFF;
  int bestSegStart = 0x7FFFFFFF;
  int bestCStart = -1, bestCEnd = -1;
  scanBracket(message, "【", 3, "】", 3, bestDist, bestSegStart, bestCStart, bestCEnd);
  scanBracket(message, "[[", 2, "]]", 2, bestDist, bestSegStart, bestCStart, bestCEnd);
  scanBracket(message, "[", 1, "]", 1, bestDist, bestSegStart, bestCStart, bestCEnd);
  if (bestCStart >= 0 && bestCEnd > bestCStart) {
    senderName = message.substring(bestCStart, bestCEnd);
    senderName.trim();
  }

  verifyCode = findVerifyCode(message);
}

// ---------- 各事件邮件 ----------

void EmailService::buildStartupMail(String &subject, PsBuffer &html,
                                    String &text) const {
  bool ok = simOverallReady();
  PsBuffer inner;
  appendBadge(inner, ok ? "设备启动完成" : "设备已启动（SIM 未就绪）",
              simOverallStateText(), ok);
  appendSectionOpen(inner, String("📡 设备信息"));
  appendDeviceRows(inner);
  appendSectionClose(inner);
  appendModemGroupSection(inner);

  subject = "设备启动完成";
  buildHtml(html, String("🚀 设备启动完成"), inner, 600);

  text = "设备启动完成\n";
  text += "系统时间: " + ((_time && _time->isSynced()) ? _time->dateStr()
                                                       : String("未同步")) +
          "\n";
  if (_wifi) text += "IP地址: " + _wifi->ip() + "\n";
  text += "运行时长: " + formatUptime(millis()) + "\n";
  text += "SIM状态: " + simOverallStateText() + "\n";
}

void EmailService::buildOnlineMail(String &subject, PsBuffer &html,
                                   String &text) const {
  bool ok = simOverallReady();
  PsBuffer inner;
  appendBadge(inner, String("设备在线"), simOverallStateText(), ok);
  appendSectionOpen(inner, String("📡 设备信息"));
  appendDeviceRows(inner);
  appendSectionClose(inner);
  appendModemGroupSection(inner);
  appendNote(inner, String("💡 此邮件由「定时通知」自动发送，用于确认设备仍在线，并非设备刚刚重启。"));

  subject = "设备在线通知";
  buildHtml(html, String("📡 设备在线通知"), inner, 600);

  text = "设备在线通知（定时通知自动发送）\n";
  text += "系统时间: " + ((_time && _time->isSynced()) ? _time->dateStr()
                                                       : String("未同步")) +
          "\n";
  if (_wifi) text += "IP地址: " + _wifi->ip() + "\n";
  text += "运行时长: " + formatUptime(millis()) + "\n";
  text += "SIM状态: " + simOverallStateText() + "\n";
}

void EmailService::buildSimMail(uint8_t port, bool ready, String &subject,
                                PsBuffer &html, String &text) const {
  PsBuffer inner;
  appendBadge(inner, ready ? "SIM 卡已插入" : "SIM 卡已拔出", simStateText(port),
              ready);

  // 事件说明里带上通信模组标识；再把所有通信模组的状态都列出来
  appendTableOpen(inner);
  appendRow(inner, String("事件说明"),
            String("通信模组 ") + String(port) + (ready ? " 的 SIM 卡已插入并就绪"
                                                    : " 的 SIM 卡已拔出或失效"));
  appendSimRows(inner);
  appendTableClose(inner);

  subject = ready ? "SIM 卡已插入" : "SIM 卡已拔出";
  buildHtml(html, ready ? "🔔 SIM 卡已插入" : "🔔 SIM 卡已拔出", inner);

  text = subject + "\n";
  text += "通信模组: " + String(port) + "\n";
  text += "SIM状态: " + simStateText(port) + "\n";
}

void EmailService::buildSmsMail(const String &sender, const String &message,
                                uint32_t ts, int8_t modem, String &subject,
                                PsBuffer &html, String &text) const {
  String senderName, verifyCode;
  parseSmsMeta(message, senderName, verifyCode);

  // 运营商类短信等场景可能没有 SCTS 时间戳，回退到设备本地时间
  String tsText = formatTs(ts);
  if (tsText == "时间未同步" && _time && _time->isSynced()) {
    tsText = _time->dateStr();
  }

  // 标题：验证码优先——收件箱列表里能一眼看到验证码，不必点开正文
  if (verifyCode.length() > 0) {
    subject = "验证码: " + verifyCode + " 来自 " +
              (senderName.length() > 0 ? senderName : sender);
  } else if (senderName.length() > 0) {
    subject = "来自 " + senderName + " 的短信";
  } else {
    subject = "来自 " + sender + " 的短信";
  }

  PsBuffer inner;

  // 验证码高亮块（红色大字号，一眼可抄）
  if (verifyCode.length() > 0) {
    inner.append("<div class=\"code\"><div class=\"code1\">验证码</div>");
    inner.append("<div class=\"code2\">");
    appendEscaped(inner, verifyCode);
    inner.append("</div></div>");
  }

  inner.append("<div class=\"rows\"><table class=\"tb\">");
  inner.append("<tr><td class=\"kn\">发件人</td><td class=\"vn\">");
  appendEscaped(inner, sender);
  if (senderName.length() > 0) {
    inner.append(" <span class=\"hl\">(");
    appendEscaped(inner, senderName);
    inner.append(")</span>");
  }
  inner.append("</td></tr>");
  inner.append("<tr><td class=\"kn\">时间</td><td class=\"vn\">");
  appendEscaped(inner, tsText);
  inner.append("</td></tr>");
  String mlabel = modemLabelOf(modem);
  if (mlabel.length() > 0) {
    inner.append("<tr><td class=\"kn\">接收通信模组</td><td class=\"vn\">");
    appendEscaped(inner, mlabel);
    inner.append("</td></tr>");
  }
  inner.append("<tr><td class=\"kn\">内容</td><td class=\"vl\">");
  appendEscaped(inner, message);
  inner.append("</td></tr>");
  inner.append("</table></div>");

  buildHtml(html, String("📱 ") + escape(subject), inner, 600);

  // 纯文本兜底：HTML 不可用（或超长降级）时仍能读到完整信息
  text = "发件人: " + sender;
  if (senderName.length() > 0) {
    text += "(" + senderName + ")";
  }
  text += "\n时间: " + tsText;
  if (mlabel.length() > 0) {
    text += "\n接收通信模组: " + mlabel;
  }
  text += "\n内容:\n" + message;
}

// 来电提醒邮件：卡片展示主叫号码 / 时间 / 接入通信模组。
// 号码可能为「未知号码」（模组不发 +CLIP/+CLCC 且主动查询也无果）或
// 「号码保密」（主叫方开通了号码隐藏），均为正常取值，不做特殊处理。
void EmailService::buildCallMail(const String &number, uint32_t ts, int8_t modem,
                                 String &subject, PsBuffer &html,
                                 String &text) const {
  subject = "来电: " + number;

  String tsText = formatTs(ts);
  if (tsText == "时间未同步" && _time && _time->isSynced()) {
    tsText = _time->dateStr();
  }

  PsBuffer inner;
  appendBadge(inner, String("📞 ") + escape(number), String("检测到来电"),
              number != "未知号码");
  inner.append("<div class=\"rows\"><table class=\"tb\">");
  inner.append("<tr><td class=\"kn\">主叫号码</td><td class=\"vn\">");
  appendEscaped(inner, number);
  inner.append("</td></tr>");
  inner.append("<tr><td class=\"kn\">时间</td><td class=\"vn\">");
  appendEscaped(inner, tsText);
  inner.append("</td></tr>");
  String mlabel = modemLabelOf(modem);
  if (mlabel.length() > 0) {
    inner.append("<tr><td class=\"kn\">接入通信模组</td><td class=\"vn\">");
    appendEscaped(inner, mlabel);
    inner.append("</td></tr>");
  }
  inner.append("</table></div>");

  buildHtml(html, String("📞 来电提醒"), inner, 600);

  text = "主叫号码: " + number;
  text += "\n时间: " + tsText;
  if (mlabel.length() > 0) {
    text += "\n接入通信模组: " + mlabel;
  }
}

void EmailService::buildTestMail(String &subject, PsBuffer &html,
                                 String &text) const {
  subject = "测试邮件";
  PsBuffer inner;
  appendBadge(inner, String("这是一封测试邮件"), String("SMTP 邮件通知配置正确"),
              true);

  // 通知类型清单
  String types;
  if (_notifyTypes & cfg::kEmailNotifySms) types += "收到短信 / ";
  if (_notifyTypes & cfg::kEmailNotifySim) types += "SIM状态变更 / ";
  if (_notifyTypes & cfg::kEmailNotifyStartup) types += "设备启动 / ";
  if (_notifyTypes & cfg::kEmailNotifyOnline) types += "定时在线通知 / ";
  if (_notifyTypes & cfg::kEmailNotifyCall) types += "来电提醒 / ";
  if (types.length() >= 3) types = types.substring(0, types.length() - 3);
  if (types.length() == 0) types = "无（全部关闭）";

  appendSectionOpen(inner, String("📮 配置信息"));
  appendRow(inner, String("发件账号"), _user);
  appendRow(inner, String("收件邮箱"), _sendTo);
  appendRow(inner, String("SMTP 服务器"), _server + ":" + String(_port));
  appendRow(inner, String("通知类型"), types);
  if (_notifyTypes & cfg::kEmailNotifyOnline) {
    appendRow(inner, String("在线通知排程"), schedText());
  }
  appendSectionClose(inner);

  appendNote(inner, String("💡 收到本邮件说明 SMTP 配置可用。后续短信、SIM 状态变更、设备启动与定时在线通知都将按上述类型发送。"));

  buildHtml(html, String("📮 测试邮件"), inner, 600);

  text = "这是一封测试邮件\n";
  text += "发件账号: " + _user + "\n";
  text += "收件邮箱: " + _sendTo + "\n";
  text += "SMTP 服务器: " + _server + ":" + String(_port) + "\n";
  text += "通知类型: " + types + "\n";
}
