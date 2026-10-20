#include "sms/SmsService.h"

#include "log/LogStore.h"

#include <ArduinoJson.h>
#include <Preferences.h>

#include <Arduino.h>

namespace {

// 本通信模组日志出口：全局日志环（串口由 LogStore 统一镜像输出）
LogStore &_log = devLog();

// 把 String 拷进定长 char 缓冲（超长由 snprintf 截断）。
// 空串写入空串——调用方（快照结构体）的默认值已有「-」之类兜底，这里不做替换，
// 由调用方决定「值缺失」时保留默认还是覆盖。
void copyTo(char *dst, size_t n, const String &s) {
  if (!s.length()) return;
  snprintf(dst, n, "%s", s.c_str());
}

}  // namespace

// JSON 字符串转义（运营商/IMEI 等文本字段安全入 JSON）
static String jsonStr(const String &s) {
  String o;
  o.reserve(s.length() + 2);
  for (int i = 0; i < s.length(); i++) {
    char c = s[i];
    if (c == '"' || c == '\\') { o += '\\'; o += c; }
    else if (c == '\n') o += "\\n";
    else if (c == '\r') o += "\\r";
    else o += c;
  }
  return o;
}

// 网络注册状态码 -> 可读文本（AT+CREG/CEREG 第二字段）
static const char *regText(int stat) {
  switch (stat) {
    case 0:  return "未注册";
    case 1:  return "已注册(家网)";
    case 2:  return "搜索中";
    case 3:  return "注册被拒";
    case 4:  return "未知";
    case 5:  return "已注册(漫游)";
    default: return "未知";
  }
}

// 运营商识别（AT+COPS 可能是长名或数字形式）-> 短代码 CMCC/CUCC/CTCC/CBN
static String operCode(const String &raw) {
  String s = raw; s.trim(); s.replace("\"", "");
  // 数字形式 "46000"：MCC 前 3 位 + MNC 余下
  if (s.length() >= 5 && s.charAt(0) >= '0' && s.charAt(0) <= '9') {
    String mcc = s.substring(0, 3), mnc = s.substring(3);
    if (mcc == "460") {
      if (mnc == "00" || mnc == "02" || mnc == "04" || mnc == "07" || mnc == "08") return "CMCC";
      if (mnc == "01" || mnc == "06" || mnc == "09") return "CUCC";
      if (mnc == "03" || mnc == "05" || mnc == "11") return "CTCC";
      if (mnc == "15") return "CBN";
    }
    return s;  // 未知数字，原样返回
  }
  String low = s; low.toLowerCase();
  if (low.indexOf("china mobile") >= 0 || low == "cmcc") return "CMCC";
  if (low.indexOf("unicom") >= 0 || low == "cucc") return "CUCC";
  if (low.indexOf("telecom") >= 0 || low == "ctcc") return "CTCC";
  if (low.indexOf("broadcast") >= 0 || low == "cbn") return "CBN";
  return "";  // 未知运营商
}

// bridge：AT 串口桥引用
SmsService::SmsService(SerialBridge &bridge) : _bridge(bridge) {}

// 载入配置、准备存储、对启用通信模组启动异步初始化（状态机由抽数任务轮询驱动）
void SmsService::begin() {
  _mutex = xSemaphoreCreateMutex();
  _store.begin(cfg::kSmsStoreMax);
  bool hadRecord = loadConfig();
  if (!hadRecord) saveConfig();  // 首次启动落一份默认配置

  // 注册抽数任务轮询回调：每个端口处理完后由泵任务调用，驱动通信模组初始化/心跳。
  // 回调运行于抽数任务上下文、不持桥锁，内部只用 trySend（非阻塞），因此绝不阻塞
  // 抽数任务、主循环或 Web；故障/未连接通信模组只会在自身端口上空转重试。
  _bridge.setPollHandler([this](uint8_t port) { this->pollInit(port); });

  // 对启用通信模组启动初始化（异步：仅置初始态，真正 AT 交互交由 pollInit）
  for (uint8_t p = 1; p <= cfg::kSmsMaxModems; p++) {
    if (_modems[p].enabled) configureModem(p, true);
  }
  _lastSmsId = _store.maxId();
}

// 周期调用（主循环）：长短信重组超时清理 + 来电检测派发；
// 通信模组初始化由抽数任务轮询驱动
void SmsService::tick() {
  _codec.pruneTimeouts(_concat);
  for (uint8_t p = 1; p <= cfg::kSmsMaxModems; p++) pollCall(p);
}

// 判断一行是否为 PDU 十六进制串（偶数长度、全为 hex 字符）
static bool isPduLine(const String &s) {
  if (!s.length() || (s.length() & 1)) return false;
  for (size_t i = 0; i < s.length(); i++) {
    char c = s[i];
    if (!((c >= '0' && c <= '9') || (c >= 'A' && c <= 'F') ||
          (c >= 'a' && c <= 'f')))
      return false;
  }
  return true;
}

// 串口自发数据消费（抽数任务上下文，持锁运行——禁止回调 SerialBridge）。
// 逐行扫描整段（而非只看首行）：一段 pend 可能含多行（+CMT 头 + PDU，或
// +CMT 与其它行同段到达）。只认首行时，只要 +CMT 不在段首（段内混入
// 前一条命令的尾巴/空行/其它 URC），整段会被判为「非短信流量」交回 AT
// 历史——短信静默丢失，这正是日志里出现 +CMT: 记录的原因。
// 参考 esp32-sms-forwarding 的 SimDispatcher：按行路由，+CMT: 置
// waitingPdu，下一行即 PDU（可同段也可跨段）。
bool SmsService::onUnsolicited(uint8_t port, const String &text) {
  if (port < 1 || port > cfg::kSmsMaxModems) return false;
  ModemState &m = _modems[port];

  String t = text;
  t.replace("\r", "");
  bool consumed = false;

  int pos = 0;
  while (pos <= (int)t.length()) {
    int nl = t.indexOf('\n', pos);
    String line = (nl >= 0) ? t.substring(pos, nl) : t.substring(pos);
    pos = (nl >= 0) ? nl + 1 : (int)t.length() + 1;
    line.trim();
    if (!line.length()) continue;  // 跳过空行（段首空行曾导致 +CMT 漏判）

    // 等待中的 PDU 行优先归属上一条 +CMT
    if (m.waitingPdu) {
      m.waitingPdu = false;
      if (isPduLine(line)) {
        handleCmt(port, line);
      } else {
        _log.addf(LogLevel::kWarn, "sms", "通信模组 %u 的 +CMT 后未收到 PDU：%s",
                  (unsigned)port, line.c_str());
      }
      consumed = true;
      continue;
    }

    // 运行时通信模组重启检测：仅在已就绪态下识别开机类 URC，置位让 tick 重新初始化
    if (m.initState == ModemInitState::kReady) {
      if (line == "RDY" || line.startsWith("^STARTUP") || line.startsWith("+PBREADY") ||
          line == "+CPIN: READY") {
        m.needReinit = true;
        consumed = true;
        continue;
      }
    }

    if (line.startsWith("+CMT:")) {
      // PDU 可能在同段后续行，也可能在下一段（头/PDU 跨 flush 时）
      m.waitingPdu = true;
      consumed = true;
      continue;
    }
    // 其它短信相关 URC（CNMI=2,2 模式下正常不会出现）：消费但不处理，
    // 避免通信模组索引通知类流量冲刷 AT 历史
    if (line.startsWith("+CMTI:") || line.startsWith("+CDS:")) {
      consumed = true;
      continue;
    }
    // 来电相关 URC（AT+CLIP=1 已下发）：RING/+CRING 起一轮，+CLIP/+CLCC 带号码，
    // NO CARRIER 等收口。仅处理启用中的通信模组；解析只置状态，派发在 tick。
    if (m.enabled) {
      if (line == "RING" || line.startsWith("+CRING:")) {
        handleCallRing(port);
        consumed = true;
        continue;
      }
      if (line.startsWith("+CLIP:")) {
        handleCallClip(port, line);
        consumed = true;
        continue;
      }
      if (line.startsWith("+CLCC:")) {
        handleCallClcc(port, line);
        consumed = true;
        continue;
      }
      if (line == "NO CARRIER" || line == "BUSY" || line == "NO ANSWER") {
        handleCallEnd(port, line);
        consumed = true;
        continue;
      }
    }
    // 非短信流量：不消费（由调用方决定是否入 AT 历史）
  }
  return consumed;
}

// ---------------- 通信模组管理 ----------------
bool SmsService::modemEnabled(uint8_t port) const {
  if (port < 1 || port > cfg::kSmsMaxModems) return false;
  return _modems[port].enabled;
}

bool SmsService::setModemEnabled(uint8_t port, bool enabled) {
  if (port < 1 || port > cfg::kSmsMaxModems) return false;
  configureModem(port, enabled);
  return saveConfig();
}

bool SmsService::setModemAlias(uint8_t port, const String &alias) {
  if (port < 1 || port > cfg::kSmsMaxModems) return false;
  _modems[port].alias = alias;   // 仅展示用，不触发通信模组状态机
  return saveConfig();
}

String SmsService::modemsJson() const {
  String js = "[";
  for (uint8_t p = 1; p <= cfg::kSmsMaxModems; p++) {
    if (p > 1) js += ",";
    const ModemState &m = _modems[p];
    bool ready = m.initState == ModemInitState::kReady;
    js += "{\"port\":" + String(p) +
          ",\"enabled\":" + (m.enabled ? "true" : "false") +
          ",\"ready\":" + (ready ? "true" : "false") +
          ",\"state\":\"" + initStateName(m.initState) + "\"" +
          ",\"csq\":" + String(m.csq) +
          ",\"oper\":\"" + jsonStr(m.oper) + "\"" +
          ",\"reg\":\"" + jsonStr(m.reg) + "\"" +
          ",\"sim\":" + (m.simReady ? "true" : "false") +
          ",\"act\":" + String(m.act) +
          ",\"imei\":\"" + jsonStr(m.imei) + "\"" +
          ",\"num\":\"" + jsonStr(m.num) + "\"" +
          ",\"vendor\":\"" + jsonStr(m.vendor) + "\"" +
          ",\"model\":\"" + jsonStr(m.model) + "\"" +
          ",\"revision\":\"" + jsonStr(m.revision) + "\"" +
          ",\"operCode\":\"" + jsonStr(m.operCode) + "\"" +
          ",\"alias\":\"" + jsonStr(m.alias) + "\"}";
  }
  js += "]";
  return js;
}

// 单个通信模组的 LCD 简报：状态机阶段给中文短词，就绪态补上运营商与信号强度
String SmsService::modemBrief(uint8_t port) const {
  if (port == 0 || port > cfg::kSmsMaxModems) return String();
  const ModemState &m = _modems[port];
  if (!m.enabled) return String("未启用");

  String s;
  switch (m.initState) {
    case ModemInitState::kReady: s = "就绪"; break;
    case ModemInitState::kFault: s = "故障"; break;
    case ModemInitState::kPowerOn: s = "上电中"; break;
    case ModemInitState::kWaitAt: s = "等 AT"; break;
    case ModemInitState::kWaitCpin: s = "等 SIM"; break;
    case ModemInitState::kBringRf: s = "开射频"; break;
    case ModemInitState::kWaitReg: s = "等注册"; break;
    case ModemInitState::kConfig: s = "配置中"; break;
    default: s = "未知"; break;
  }
  if (m.initState == ModemInitState::kReady) {
    // 优先中文名（CMCC→中国移动），未知短代码才回退到 AT+COPS? 原始值（可能为数字）
    String op = operCn(m.operCode);
    if (op.length() == 0) op = m.oper;
    if (op.length()) { s += " "; s += op; }
    if (m.csq >= 0) { s += " "; s += String(m.csq); }
  }
  return s;
}

// 运营商短代码 -> 中文名（与 EmailService::operCn 同一套映射，
// LCD 菜单直接复用，避免展示层再写一份）
String SmsService::operCn(const String &code) {
  if (code == "CMCC") return "中国移动";
  if (code == "CUCC") return "中国联通";
  if (code == "CTCC") return "中国电信";
  if (code == "CBN") return "中国广电";
  return "";
}

// 单个通信模组的完整状态快照（LCD 菜单「通信模组」页用），不加锁，只读字段
bool SmsService::modemSnapshot(uint8_t port, ModemSnapshot &out) const {
  if (port == 0 || port > cfg::kSmsMaxModems) return false;
  const ModemState &m = _modems[port];

  out.enabled = m.enabled;
  out.ready = m.initState == ModemInitState::kReady;
  out.sim = m.simReady;
  out.csq = m.csq;

  // 状态短词与 modemBrief 同源，保持同一套措辞
  switch (m.initState) {
    case ModemInitState::kReady: snprintf(out.state, sizeof(out.state), "就绪"); break;
    case ModemInitState::kFault: snprintf(out.state, sizeof(out.state), "故障"); break;
    case ModemInitState::kPowerOn: snprintf(out.state, sizeof(out.state), "上电中"); break;
    case ModemInitState::kWaitAt: snprintf(out.state, sizeof(out.state), "等AT"); break;
    case ModemInitState::kWaitCpin: snprintf(out.state, sizeof(out.state), "等SIM"); break;
    case ModemInitState::kBringRf: snprintf(out.state, sizeof(out.state), "开射频"); break;
    case ModemInitState::kWaitReg: snprintf(out.state, sizeof(out.state), "等注册"); break;
    case ModemInitState::kConfig: snprintf(out.state, sizeof(out.state), "配置中"); break;
    default: snprintf(out.state, sizeof(out.state), "未启用"); break;
  }

  copyTo(out.alias, sizeof(out.alias), m.alias);
  copyTo(out.model, sizeof(out.model), m.model);
  copyTo(out.revision, sizeof(out.revision), m.revision);
  copyTo(out.num, sizeof(out.num), m.num);
  copyTo(out.reg, sizeof(out.reg), m.reg);
  copyTo(out.imei, sizeof(out.imei), m.imei);
  // 运营商优先用中文短名，其次短代码，最后才是 AT+COPS? 的长格式名
  String cn = operCn(m.operCode);
  if (cn.length()) copyTo(out.oper, sizeof(out.oper), cn);
  else if (m.operCode.length()) copyTo(out.oper, sizeof(out.oper), m.operCode);
  else copyTo(out.oper, sizeof(out.oper), m.oper);
  return true;
}

// 导出通信模组配置（启用/别名，供备份用）到给定 JSON 对象
void SmsService::exportTo(JsonObject &o) const {
  o["max"] = _max;
  JsonArray arr = o["modems"].to<JsonArray>();
  for (uint8_t p = 1; p <= cfg::kSmsMaxModems; p++) {
    JsonObject m = arr.add<JsonObject>();
    m["port"] = p;
    m["enabled"] = _modems[p].enabled;
    m["alias"] = _modems[p].alias;
  }
}

// ---------------- 发送 ----------------
uint32_t SmsService::send(uint8_t port, const String &phone, const String &text,
                          String &err, bool *storedPtr) {
  if (port < 1 || port > cfg::kSmsMaxModems) {
    err = "通信模组不存在";
    return 0;
  }
  ModemState &m = _modems[port];
  if (!m.enabled) {
    err = "通信模组未启用或不存在";
    return 0;
  }
  // 通信模组未就绪（初始化中/故障/未连接）时直接拒绝，避免无谓等待与持锁
  if (m.initState != ModemInitState::kReady) {
    err = "通信模组尚未就绪（初始化中或故障）";
    return 0;
  }
  // 号码清洗：去分隔符，仅保留数字与前导 +
  String num;
  for (size_t i = 0; i < phone.length(); i++) {
    char c = phone[i];
    if (c == ' ' || c == '-' || c == '(' || c == ')' || c == '.') continue;
    num += c;
  }
  size_t digits = (num.length() && num[0] == '+') ? 1 : 0;
  if (num.length() - digits < 3 || num.length() > 20) {
    err = "号码格式不正确";
    return 0;
  }
  for (size_t i = digits; i < num.length(); i++) {
    if (num[i] < '0' || num[i] > '9') {
      err = "号码只能包含数字";
      return 0;
    }
  }
  if (!text.length()) {
    err = "短信内容不能为空";
    return 0;
  }
  if (text.length() > 1000) {
    err = "短信内容过长";
    return 0;
  }

  // 编码（长短信自动拆分）；参考号取本通信模组计数器（单条时忽略）
  SmsPduPart parts[cfg::kSmsConcatMaxParts];
  int n = _codec.encodeSend(num, text, m.csmsRef, parts,
                            cfg::kSmsConcatMaxParts);
  if (n < 0) {
    err = String("内容编码失败（错误码 ") + n + "）";
    return 0;
  }
  // 推进参考号（跳过 0：库要求非零）
  if (++m.csmsRef == 0) m.csmsRef = 1;

  // 逐段发送，任一失败即中止；发送期间持续刷新 lastSendMs 让初始化轮询让行
  m.lastSendMs = millis();
  for (int i = 0; i < n; i++) {
    if (!sendPduPart(port, parts[i], err)) return 0;
    m.lastSendMs = millis();
  }

  // 入库（发出记录）
  SmsMessage msg;
  msg.incoming = false;
  msg.phone = num;
  msg.content = text;
  msg.ts = nowTs();  // 已同步为纪元秒，未同步为开机秒数（前端按 >=1.6e9 区分）
  msg.modem = port;
  bool storedOk = false;
  uint32_t id = _store.add(msg);
  if (id) {
    lock();
    _lastSmsId = id;
    unlock();
    storedOk = true;
    _log.addf(LogLevel::kInfo, "sms", "通信模组 %u 已发送 #%u 至 %s（%d 段）",
              (unsigned)port, (unsigned)id, num.c_str(), n);
  } else {
    // 通信模组侧已成功发出，但记录未入库（userdata 未挂载/写失败）。
    // 不判失败——短信确实已发出；通过 stored 出参告知调用方（参考实现同款语义：
    // 存储失败不影响发送结果），避免「界面报失败但其实已发送成功」的误判。
    _log.addf(LogLevel::kWarn, "sms", "通信模组 %u 已发至 %s，但记录入库失败",
              (unsigned)port, num.c_str());
  }
  if (storedPtr) *storedPtr = storedOk;
  return storedOk ? id : kIdSentUnstored;
}

// ---------------- 串口调试 ----------------
// 经短信通信模组发送 AT 命令：与短信收发共用同一端口锁（bridge.send 内部阻塞取锁），
// 因此控制台命令与 CMGS 发送**串行**，不会出现命令/PDU 交织破坏短信；发送窗口内
// 置 lastSendMs 令 pollInit 让行，避免与心跳 AT 互相干扰。通信模组就绪后自动还原
// CMGF=0 / CNMI=2,2，使控制台任意 AT（如切到文本模式、关直投、关射频）都不会
// 让短信收发永久失效——这是与原"直接 _bridge.send 绕过短信通信模组"最关键的区别。
SerialBridge::Result SmsService::sendAt(uint8_t port, uint32_t baud,
                                        const String &cmd, SerialBridge::Eol eol,
                                        uint32_t timeoutMs, String &err) {
  SerialBridge::Result empty{false, (uint32_t)-1, String()};
  if (port < 1 || port > cfg::kSmsMaxModems) {
    err = "通信模组不存在";
    return empty;
  }
  ModemState &m = _modems[port];
  m.lastSendMs = millis();  // 令初始化/心跳轮询在此期间让行
  // quiet=false：命令与响应记入 AT 历史，供控制台展示
  SerialBridge::Result r = _bridge.send(port, baud, cmd, eol, timeoutMs, false);
  if (!r.ok) {
    err = "invalid port";
    return r;
  }
  // 还原短信关键配置（仅通信模组就绪时）；初始化进行中由状态机负责下发，无需此处干预
  if (m.initState == ModemInitState::kReady) {
    _bridge.send(port, cfg::kUartBaud, "AT+CMGF=0", SerialBridge::kCrlf,
                 cfg::kSmsCmdTimeoutMs, true);
    _bridge.send(port, cfg::kUartBaud, "AT+CNMI=2,2,0,0,0", SerialBridge::kCrlf,
                 cfg::kSmsCmdTimeoutMs, true);
  }
  return r;
}

// ---------------- 内部实现 ----------------
bool SmsService::configureModem(uint8_t port, bool enabled) {
  if (enabled) startInit(port);
  else stopInit(port);
  return true;
}

// 触发通信模组重新初始化：仅启用态有效，置 needReinit 由抽数任务状态机重跑（不清空已发记录）
bool SmsService::reinitPort(uint8_t port) {
  if (port < 1 || port > cfg::kSmsMaxModems) return false;
  if (!_modems[port].enabled) return false;
  _modems[port].needReinit = true;
  _log.addf(LogLevel::kInfo, "sms", "通信模组 %u 请求重新初始化（等待抽数任务重跑）",
            (unsigned)port);
  return true;
}

// 通信模组是否已就绪（启用且初始化状态机到达 kReady）
bool SmsService::modemReady(uint8_t port) const {
  if (port < 1 || port > cfg::kSmsMaxModems) return false;
  return _modems[port].initState == ModemInitState::kReady;
}

// 启用通信模组的初始化是否都已出结果（终态 kReady / kFault，不含中间态）。
// 只读枚举值（单字节读，与其他 const 访问器一致，不加锁），调用方据此
// 决定是否可以继续依赖通信模组状态的动作（如发送启动邮件）
bool SmsService::modemsSettled() const {
  for (uint8_t p = 1; p <= cfg::kSmsMaxModems; p++) {
    if (!_modems[p].enabled) continue;
    switch (_modems[p].initState) {
      // 终态：就绪或故障（故障态会慢速重试，但初始化这一轮已有结论）
      case ModemInitState::kReady:
      case ModemInitState::kFault:
      case ModemInitState::kDown:
        break;
      // 中间态：上电 / 等 AT / 等 SIM / 射频 / 等注册 / 下发配置
      default:
        return false;
    }
  }
  return true;
}

// 设置 SIM 就绪态并检测运行时切换（插卡/拔卡事件）。
// 两类「假切换」必须排除，否则每次开机/通信模组重启都会误报：
//   1. 开机首次就绪：everReady=false（通信模组从未就绪过），属启动流程；
//   2. 通信模组重启后的重新就绪：未通知过拔出（simRemovedNotified=false）。
// 只有「就绪 -> 拔出 -> 重新就绪」这条真实路径会成对发出两个事件。
void SmsService::setSimReady(uint8_t port, bool ready) {
  if (port < 1 || port > cfg::kSmsMaxModems) return;
  ModemState &m = _modems[port];
  if (m.simReady == ready) return;
  m.simReady = ready;

  if (!ready) {
    // 运行时掉卡：仅当通信模组曾就绪（排除开机初始化过程）才通知
    if (m.everReady && !m.simRemovedNotified) {
      m.simRemovedNotified = true;
      _log.addf(LogLevel::kInfo, "sim", "通信模组 %u SIM 卡已拔出", (unsigned)port);
      if (_onSim) _onSim(port, false);
    }
  } else {
    // 重新插卡：仅当之前通知过拔出才通知
    if (m.simRemovedNotified) {
      m.simRemovedNotified = false;
      _log.addf(LogLevel::kInfo, "sim", "通信模组 %u SIM 卡已插入", (unsigned)port);
      if (_onSim) _onSim(port, true);
    }
  }
}

// ---------------- 来电检测（移植自 esp32-sms-forwarding 的 Call）----------------
// 线程模型：handleCallXxx 在抽数任务上下文运行（只置状态，不回调、不发 AT）；
// pollCall / dispatchCall 在 tick 主循环运行（派发通知、主动 AT+CLCC 查询）。
// 全部用 millis() 比较，单核无关 mutual exclusion 问题；跨任务共享的布尔/时刻
// 最多让读侧差一拍，不存在一致性风险（与 _modems 其余字段同策略）。

// RING / +CRING：开始一轮来电。去重窗口内的重复 RING（模组每响铃周期都重发）忽略。
void SmsService::handleCallRing(uint8_t port) {
  if (port < 1 || port > cfg::kSmsMaxModems) return;
  ModemState &m = _modems[port];
  uint32_t now = millis();
  if (now - m.callLastNotifyMs < kCallDedupMs) return;
  m.callPending = true;
  m.callDispatch = false;
  m.callNumber = "";
  m.callClipWaitUntilMs = now + kCallClipWaitMs;
  m.callClccTried = false;
  m.callClccDueMs = now + kCallClccDelayMs;
  _log.addf(LogLevel::kInfo, "call", "通信模组 %u 检测到来电（RING），等待主叫号码",
            (unsigned)port);
}

// 提取一行中第一个引号对之间的内容（+CLIP: "138...",129,... 的号码）
static String firstQuoted(const String &line) {
  int q1 = line.indexOf('"');
  if (q1 < 0) return "";
  int q2 = line.indexOf('"', q1 + 1);
  if (q2 <= q1) return "";
  return line.substring(q1 + 1, q2);
}

// +CLIP: 已带齐一条通知所需的全部信息（谁、何时），无先导 RING 时也按来电处理，
// 而不是丢弃——否则一次响铃指示没被认出就丢掉整通来电。
void SmsService::handleCallClip(uint8_t port, const String &line) {
  if (port < 1 || port > cfg::kSmsMaxModems) return;
  ModemState &m = _modems[port];
  String num = firstQuoted(line);
  uint32_t now = millis();
  if (!m.callPending) {
    if (now - m.callLastNotifyMs < kCallDedupMs) return;
    m.callNumber = num.length() > 0 ? num : String("号码保密");
    m.callDispatch = true;
    _log.addf(LogLevel::kInfo, "call", "通信模组 %u 无先导 RING 的 +CLIP，按来电处理：%s",
              (unsigned)port, m.callNumber.c_str());
    return;
  }
  if (num.length() > 0 || m.callNumber.length() == 0) {
    m.callNumber = num.length() > 0 ? num : String("号码保密");
  }
  m.callPending = false;
  m.callDispatch = true;
}

// 主动上报的 +CLCC:（ML307A + 中国电信的即时号码来源，格式同 AT+CLCC 响应）
void SmsService::handleCallClcc(uint8_t port, const String &line) {
  if (port < 1 || port > cfg::kSmsMaxModems) return;
  ModemState &m = _modems[port];
  String num = parseClccNumber(line);
  if (num.length() == 0) return;
  uint32_t now = millis();
  if (!m.callPending) {
    if (now - m.callLastNotifyMs < kCallDedupMs) return;
    m.callNumber = num;
    m.callDispatch = true;
    _log.addf(LogLevel::kInfo, "call", "通信模组 %u 无先导 RING 的主动 +CLCC，按来电处理：%s",
              (unsigned)port, num.c_str());
    return;
  }
  m.callNumber = num;
  m.callPending = false;
  m.callDispatch = true;
  _log.addf(LogLevel::kInfo, "call", "通信模组 %u 从主动 +CLCC 获取到来电号码：%s",
            (unsigned)port, num.c_str());
}

// 通话结束：号码已拿到就立即派发，不再等满窗口；主叫已走，再等也不会有 +CLCC
void SmsService::handleCallEnd(uint8_t port, const String &line) {
  if (port < 1 || port > cfg::kSmsMaxModems) return;
  ModemState &m = _modems[port];
  if (!m.callPending) return;
  m.callPending = false;
  m.callDispatch = true;
  _log.addf(LogLevel::kInfo, "call", "通信模组 %u 通话结束（%s），以已知号码派发：%s",
            (unsigned)port, line.c_str(),
            m.callNumber.length() ? m.callNumber.c_str() : "未知号码");
}

// tick 驱动：派发、超时收口、主动 AT+CLCC 兜底查询
void SmsService::pollCall(uint8_t port) {
  if (port < 1 || port > cfg::kSmsMaxModems) return;
  ModemState &m = _modems[port];
  if (!m.enabled) return;

  if (m.callDispatch) {
    m.callDispatch = false;
    dispatchCall(port);
    return;
  }
  if (!m.callPending) return;
  uint32_t now = millis();

  // 主动 AT+CLCC 兜底：等 kCallClccDelayMs 仍无 +CLIP/+CLCC 时查一次。
  // 只能发给产生这通来电的那个通信模组——换一个模组查不到对方号码。
  // trySend 非阻塞取端口锁：端口忙（如在发短信）本轮跳过，下轮 tick 再试。
  if (!m.callClccTried && (int32_t)(now - m.callClccDueMs) >= 0) {
    SerialBridge::Result r;
    if (_bridge.trySend(port, cfg::kUartBaud, "AT+CLCC", SerialBridge::kCrlf,
                        kCallClccTimeoutMs, true, r)) {
      m.callClccTried = true;
      String num = parseClccNumber(r.resp);
      if (num.length() > 0) {
        _log.addf(LogLevel::kInfo, "call", "通信模组 %u AT+CLCC 获取到来电号码：%s",
                  (unsigned)port, num.c_str());
        m.callNumber = num;
        m.callPending = false;
        dispatchCall(port);
        return;
      }
      _log.addf(LogLevel::kInfo, "call",
                "通信模组 %u AT+CLCC 未获取到号码（可能已挂断），继续等待",
                (unsigned)port);
    }
  }

  // 等待窗口截止：以已知号码（或「未知号码」）派发
  if ((int32_t)(now - m.callClipWaitUntilMs) >= 0) {
    m.callPending = false;
    dispatchCall(port);
  }
}

// 实际派发：黑名单（来电作用域）判定 + 回调。运行于 tick 主循环上下文。
void SmsService::dispatchCall(uint8_t port) {
  if (port < 1 || port > cfg::kSmsMaxModems) return;
  ModemState &m = _modems[port];
  m.callLastNotifyMs = millis();
  String num = m.callNumber.length() > 0 ? m.callNumber : String("未知号码");

  // 黑名单命中：不派发（来电是瞬时事件，模组与 SIM 均无存储，拦了就是拦了）
  if (_blacklist && _blacklist->enabled() && _blacklist->match(num, BlackList::kCall)) {
    String note = _blacklist->lastNote();
    _log.addf(LogLevel::kInfo, "black", "命中黑名单，拦截来电 %s%s%s", num.c_str(),
              note.length() ? "（" : "", note.length() ? (note + "]").c_str() : "");
    return;
  }

  _log.addf(LogLevel::kInfo, "call", "通信模组 %u 来电通知：%s", (unsigned)port,
            num.c_str());
  if (_onCall) _onCall(num, nowTs(), (int8_t)port);
}

// 从 +CLCC: 行 / AT+CLCC 响应中提取主叫号：
// 格式 +CLCC: <id>,<dir>,<stat>,<mode>,<mpty>,"<number>",<type>,...（第 5 字段后首个引号对）
String SmsService::parseClccNumber(const String &text) {
  int pos = 0;
  while (true) {
    int idx = text.indexOf("+CLCC:", pos);
    if (idx < 0) break;
    int c1 = text.indexOf(',', idx + 6);
    if (c1 < 0) break;
    int c2 = text.indexOf(',', c1 + 1);
    if (c2 < 0) break;
    int c3 = text.indexOf(',', c2 + 1);
    if (c3 < 0) break;
    int c4 = text.indexOf(',', c3 + 1);
    if (c4 < 0) break;
    int q1 = text.indexOf('"', c4);
    if (q1 < 0) break;
    int q2 = text.indexOf('"', q1 + 1);
    if (q2 <= q1) break;
    String num = text.substring(q1 + 1, q2);
    if (num.length() > 0) return num;
    pos = idx + 6;
  }
  return "";
}

// 启动异步初始化：仅置初始态与计数，真正的 AT 交互由 pollInit（抽数任务轮询）驱动
void SmsService::startInit(uint8_t port) {
  ModemState &m = _modems[port];
  m.enabled = true;
  m.waitingPdu = false;
  m.needReinit = false;
  m.configStep = 0;
  m.initAttempts = 0;
  m.regAttempts = 0;
  m.failCount = 0;
  // 重置遥测（重新初始化后从空开始采集）
  m.csq = -1; m.oper = ""; m.reg = ""; m.imei = ""; m.act = 0; m.num = "";
  m.vendor = ""; m.model = ""; m.revision = ""; m.operCode = "";
  m.simReady = false; m.nextTeleMs = 0; m.teleStep = 0;
  // 清空来电检测状态（重新初始化：进行中的来电不再派发）
  m.callPending = false; m.callDispatch = false; m.callNumber = "";
  m.callClipWaitUntilMs = 0; m.callLastNotifyMs = 0;
  m.callClccTried = false; m.callClccDueMs = 0;
  // 若配置了 EN 复位引脚，先走硬上下电；否则直接等 AT 就绪
  m.initState = (enPin(port) >= 0) ? ModemInitState::kPowerOn : ModemInitState::kWaitAt;
  m.nextPollMs = millis();  // 下一轮泵立即开始
  _log.addf(LogLevel::kInfo, "sms", "通信模组 %u 已启用，启动异步初始化", (unsigned)port);
}

// 停止初始化：禁用后停留在 kDown，pollInit 不再动作
void SmsService::stopInit(uint8_t port) {
  ModemState &m = _modems[port];
  m.enabled = false;
  m.waitingPdu = false;
  m.needReinit = false;
  m.callPending = false; m.callDispatch = false; m.callNumber = "";
  m.initState = ModemInitState::kDown;
  _log.addf(LogLevel::kInfo, "sms", "通信模组 %u 已禁用", (unsigned)port);
}

// 通信模组对应 EN 复位引脚 / 有效电平（来自 Config；-1 表示不控制）
int8_t SmsService::enPin(uint8_t port) const {
  return port == 1 ? cfg::kModemEnPin1 : (port == 2 ? cfg::kModemEnPin2 : -1);
}
bool SmsService::enActiveLow() const { return cfg::kModemEnActiveLow; }

// 状态机可读名（SSE/JSON 用）
const char *SmsService::initStateName(ModemInitState s) {
  switch (s) {
    case ModemInitState::kDown: return "down";
    case ModemInitState::kPowerOn: return "poweron";
    case ModemInitState::kWaitAt: return "wait_at";
    case ModemInitState::kWaitCpin: return "wait_cpin";
    case ModemInitState::kBringRf: return "bring_rf";
    case ModemInitState::kWaitReg: return "wait_reg";
    case ModemInitState::kConfig: return "config";
    case ModemInitState::kReady: return "ready";
    case ModemInitState::kFault: return "fault";
  }
  return "unknown";
}

// 解析注册状态响应中的 stat（第二字段）；无 stat 或失败返回 -1
int SmsService::parseRegStat(const String &resp, const char *prefix) {
  int idx = resp.indexOf(prefix);
  if (idx < 0) return -1;
  int c1 = resp.indexOf(',', idx);
  if (c1 < 0) return -1;  // 仅 +CEREG: <n> 无 stat，视为不完整
  int c2 = resp.indexOf(',', c1 + 1);
  String statStr = c2 < 0 ? resp.substring(c1 + 1) : resp.substring(c1 + 1, c2);
  return statStr.toInt();
}

// 通信模组初始化状态机：由 SerialBridge 抽数任务每端口轮询调用（抽数任务上下文）。
// 全程非阻塞——用 trySend，端口忙（正在发短信）立即跳过；受 nextPollMs 节流，
// 单步 AT 最多持端口锁 ~1.5s。故障/未连接通信模组只在自身端口空转，不波及另一通信模组、
// 主循环或 Web。就绪后周期性心跳探活；检测到运行时重启则回到初始化重新配置。
void SmsService::pollInit(uint8_t port) {
  if (port < 1 || port > cfg::kSmsMaxModems) return;
  ModemState &m = _modems[port];
  if (!m.enabled) {
    m.initState = ModemInitState::kDown;
    return;
  }
  uint32_t now = millis();
  // 发送窗口内让行：避免心跳/初始化 AT 在多段发送的两个段之间（端口锁短暂释放）
  // 插入，破坏正在进行的 CMGS 握手。窗口覆盖整段发送（等 > 5s + PDU 响应 30s），
  // 之后恢复。仅在确有发送记录（lastSendMs != 0）时生效，避免开机 0 值误判。
  if (m.lastSendMs != 0 &&
      (now - m.lastSendMs) < cfg::kSmsCmgsPromptMs + cfg::kSmsSendTimeoutMs + 3000)
    return;
  // 节流：就绪态心跳(30s)与遥测(4s)各有计时，取较早者决定本轮是否动作
  uint32_t nextAt = m.nextPollMs;
  if (m.initState == ModemInitState::kReady && m.nextTeleMs && m.nextTeleMs < nextAt)
    nextAt = m.nextTeleMs;
  if (now < nextAt) return;

  // 运行时重启检测（onUnsolicited 在就绪态置位）
  if (m.needReinit) {
    m.needReinit = false;
    m.initState = ModemInitState::kWaitAt;
    m.initAttempts = 0;
    m.regAttempts = 0;
    m.configStep = 0;
    m.failCount = 0;
    m.nextPollMs = now;
    _log.addf(LogLevel::kWarn, "sms", "通信模组 %u 检测到重启，重新开始初始化",
              (unsigned)port);
  }

  // 非阻塞 AT：端口忙返回 false（本轮跳过）；成功返回 true（resp 可能为空=超时）
  String resp;
  auto at = [&](const char *cmd, uint32_t to, bool quiet = true) -> bool {
    SerialBridge::Result r;
    if (!_bridge.trySend(port, cfg::kUartBaud, cmd, SerialBridge::kCrlf, to, quiet, r))
      return false;  // 端口忙
    resp = r.resp;
    return true;
  };

  switch (m.initState) {
    case ModemInitState::kDown:
      m.initState = ModemInitState::kWaitAt;
      m.initAttempts = 0;
      m.nextPollMs = now;
      return;

    case ModemInitState::kPowerOn: {
      if (enPin(port) < 0) {  // 未配置 EN 引脚，直接等 AT
        m.initState = ModemInitState::kWaitAt;
        m.initAttempts = 0;
        m.nextPollMs = now;
        return;
      }
      // 阶段 0：拉低关机 -> 1.2s；阶段 1：拉高开机 -> 0.5s；阶段 2：进等 AT
      if (m.configStep == 0) {
        pinMode(enPin(port), OUTPUT);
        digitalWrite(enPin(port), enActiveLow() ? LOW : HIGH);
        m.configStep = 1;
        m.nextPollMs = now + 1200;
        return;
      } else if (m.configStep == 1) {
        digitalWrite(enPin(port), enActiveLow() ? HIGH : LOW);
        m.configStep = 2;
        m.nextPollMs = now + 500;
        return;
      } else {
        m.configStep = 0;
        m.initState = ModemInitState::kWaitAt;
        m.initAttempts = 0;
        m.nextPollMs = now;
        return;
      }
    }

    case ModemInitState::kWaitAt: {
      if (!at("AT", cfg::kSmsCmdTimeoutMs)) return;  // 忙：跳过本轮
      if (resp.indexOf("OK") >= 0) {
        m.lastOkMs = now;
        m.failCount = 0;
        m.initState = ModemInitState::kWaitCpin;
        m.initAttempts = 0;
        m.nextPollMs = now + 200;
      } else {
        if (++m.initAttempts >= cfg::kModemAtReadyMax) {
          m.initState = ModemInitState::kFault;  // 转故障态慢速重试
          m.nextPollMs = now + cfg::kModemFaultRetryMs;
        } else {
          m.nextPollMs = now + cfg::kModemAtPollMs;
        }
      }
      return;
    }

    case ModemInitState::kWaitCpin: {
      if (!at("AT+CPIN?", cfg::kSmsCmdTimeoutMs)) return;
      if (resp.indexOf("+CPIN: READY") >= 0) {
        setSimReady(port, true);
        m.initState = ModemInitState::kBringRf;
        m.nextPollMs = now + 200;
      } else if (resp.indexOf("NOT INSERTED") >= 0 || resp.indexOf("+CME ERROR: 10") >= 0) {
        setSimReady(port, false);
        m.initState = ModemInitState::kFault;  // 明确未插卡：慢速重试（也许会插卡）
        m.nextPollMs = now + cfg::kModemFaultRetryMs;
      } else {
        if (++m.initAttempts >= cfg::kModemCpinMax) {
          m.initState = ModemInitState::kFault;
          m.nextPollMs = now + cfg::kModemFaultRetryMs;
        } else {
          m.nextPollMs = now + cfg::kModemCpinPollMs;
        }
      }
      return;
    }

    case ModemInitState::kBringRf: {
      at("AT+CFUN=1", cfg::kSmsCmdTimeoutMs);  // 射频上线（失败不阻断）
      m.initState = ModemInitState::kWaitReg;
      m.regAttempts = 0;
      m.nextPollMs = now + 500;
      return;
    }

    case ModemInitState::kWaitReg: {
      // 优先 CEREG（LTE），无则退回 CREG；端口忙则跳过本轮（不推进计数）
      int stat = -1;
      bool busy = false;
      if (!at("AT+CEREG?", cfg::kSmsCmdTimeoutMs)) {
        busy = true;
      } else {
        stat = parseRegStat(resp, "+CEREG:");
        if (stat < 0) {
          if (at("AT+CREG?", cfg::kSmsCmdTimeoutMs))
            stat = parseRegStat(resp, "+CREG:");
          else
            busy = true;
        }
      }
      if (busy) return;
      if (stat == 1 || stat == 5) {
        m.initState = ModemInitState::kConfig;
        m.configStep = 0;
        m.nextPollMs = now + 200;
      } else {
        if (++m.regAttempts >= cfg::kModemRegMaxAttempts) {
          m.initState = ModemInitState::kFault;
          m.nextPollMs = now + cfg::kModemFaultRetryMs;
        } else {
          m.nextPollMs = now + cfg::kModemRegPollMs;
        }
      }
      return;
    }

    case ModemInitState::kConfig: {
      // 逐条下发（每条 tick 一条，节流），避免一次性持端口锁过久
      static const char *kCmds[] = {
          "AT+COPS=0", "AT+CEREG=3", "AT+CGDCONT=1,\"IP\",\"\"",
          "AT+CNMI=2,2,0,0,0", "AT+CMGF=0", "AT+CLIP=1", "AT+CUSD=1"};
      if (m.configStep == 1) {
        // CEREG=3 失败降级到 =2（部分 2G 通信模组不支持带原因的注册上报）
        if (at("AT+CEREG=3", cfg::kSmsCmdTimeoutMs)) {
          if (resp.indexOf("OK") < 0) at("AT+CEREG=2", cfg::kSmsCmdTimeoutMs);
        } else {
          return;  // 忙
        }
      } else {
        if (!at(kCmds[m.configStep], cfg::kSmsCmdTimeoutMs)) return;  // 忙
      }
      m.configStep++;
      if (m.configStep >= (int)(sizeof(kCmds) / sizeof(kCmds[0]))) {
        m.initState = ModemInitState::kReady;
        m.failCount = 0;
        m.everReady = true;  // 此后 SIM 掉卡/插卡才算「运行时事件」
        m.lastOkMs = now;
        m.nextPollMs = now + cfg::kModemLivenessMs;
        _log.addf(LogLevel::kInfo, "sms", "通信模组 %u 就绪（PDU 模式 + 短信直投已配置）",
                  (unsigned)port);
        // 通知上层（如时间同步立即尝试 SIM NITZ）。回调运行于抽数任务上下文，
        // 仅允许置标记等轻量操作
        if (_onReady) _onReady(port);
      } else {
        m.nextPollMs = now + cfg::kModemConfigStepMs;
      }
      return;
    }

    case ModemInitState::kReady: {
      // 心跳探活（周期 30s）与遥测采集（周期 4s）相互独立：各自计时、各自推进，
      // 端口忙（如正在发送短信）则整轮跳过，下一轮重试。
      bool liveDue = (now >= m.nextPollMs);
      bool teleDue = (now >= m.nextTeleMs);
      if (!liveDue && !teleDue) return;

      // 心跳探活：连续失败判定通信模组掉线/重启，回到初始化重新配置
      if (liveDue) {
        if (!at("AT", cfg::kSmsCmdTimeoutMs)) return;  // 忙（如正在发送短信）跳过
        if (resp.indexOf("OK") >= 0) {
          m.lastOkMs = now;
          m.failCount = 0;
          m.nextPollMs = now + cfg::kModemLivenessMs;
        } else {
          if (++m.failCount >= cfg::kModemLivenessMaxFail) {
            _log.addf(LogLevel::kWarn, "sms",
                      "通信模组 %u 心跳失败，可能掉线/重启，重新初始化", (unsigned)port);
            m.initState = ModemInitState::kWaitAt;
            m.initAttempts = 0;
            m.regAttempts = 0;
            m.configStep = 0;
            m.nextPollMs = now;
          } else {
            m.nextPollMs = now + cfg::kModemLivenessMs;
          }
        }
      }

      // 遥测：每轮推进一步，轮流查询信号 / 运营商 / 注册 / IMEI
      if (teleDue) {
        m.nextTeleMs = now + cfg::kModemTeleMs;
        switch (m.teleStep) {
          case 0: {  // 信号质量 AT+CSQ
            if (at("AT+CSQ", cfg::kSmsCmdTimeoutMs)) {
              int i = resp.indexOf("+CSQ:");
              if (i >= 0) {
                String s = resp.substring(i + 5);
                int c = s.indexOf(',');
                if (c >= 0) s = s.substring(0, c);
                s.trim();
                int v = s.toInt();
                m.csq = (v == 99 || v < 0) ? -1 : (int8_t)v;
              }
            }
            break;
          }
          case 1: {  // 运营商 AT+COPS?
            if (at("AT+COPS?", cfg::kSmsCmdTimeoutMs)) {
              int i = resp.indexOf("+COPS:");
              if (i >= 0) {
                // +COPS: <mode>[,<format>[,<oper>]]；运营商是第三个字段
                // （<format> 为 0/1/2，取错会拿到 "0" 而非真实运营商）
                String seg = resp.substring(i + 6);   // 去掉 "+COPS:"
                int c1 = seg.indexOf(',');
                int c2 = (c1 >= 0) ? seg.indexOf(',', c1 + 1) : -1;
                String op;
                if (c2 >= 0) {
                  int c3 = seg.indexOf(',', c2 + 1);
                  op = seg.substring(c2 + 1, c3 >= 0 ? c3 : seg.length());
                  op.replace("\"", "");
                  op.trim();
                } else {
                  op = "";  // 未注册：无运营商字段
                }
                m.oper = op;
                m.operCode = operCode(m.oper);
              }
            }
            break;
          }
          case 2: {  // 网络注册 + 接入技术（优先 LTE CEREG，否则 CREG）
            int stat = -1;
            if (at("AT+CEREG?", cfg::kSmsCmdTimeoutMs)) stat = parseRegStat(resp, "+CEREG:");
            if (stat < 0 && at("AT+CREG?", cfg::kSmsCmdTimeoutMs)) stat = parseRegStat(resp, "+CREG:");
            m.reg = regText(stat);
            if (stat >= 0) {
              int commas = 0;
              for (int k = 0; k < resp.length(); k++) if (resp[k] == ',') commas++;
              if (commas >= 3) {  // 含 act 字段时才取（否则末段是 stat 非 act）
                int lc = resp.lastIndexOf(',');
                if (lc >= 0) m.act = (uint8_t)resp.substring(lc + 1).toInt();
              }
            }
            break;
          }
          case 3: {  // 通信模组 IMEI AT+CGSN
            if (at("AT+CGSN", cfg::kSmsCmdTimeoutMs)) {
              String s = resp; s.replace("\r", ""); s.replace("\n", "");
              int ok = s.indexOf("OK"); if (ok >= 0) s = s.substring(0, ok);
              s.trim(); m.imei = s;
            }
            break;
          }
          case 4: {  // 本机号码（SIM 号码）AT+CNUM，取第二个引号串（number）
            if (at("AT+CNUM", cfg::kSmsCmdTimeoutMs)) {
              int i = resp.indexOf("+CNUM:");
              if (i >= 0) {
                String seg = resp.substring(i + 6);
                int q1 = seg.indexOf('"'), q2 = (q1 >= 0) ? seg.indexOf('"', q1 + 1) : -1;
                int q3 = (q2 >= 0) ? seg.indexOf('"', q2 + 1) : -1;
                int q4 = (q3 >= 0) ? seg.indexOf('"', q3 + 1) : -1;
                if (q3 >= 0 && q4 > q3) m.num = seg.substring(q3 + 1, q4);
                else m.num = "";
              }
            }
            break;
          }
          case 5: {  // 通信模组厂商 AT+CGMI
            if (at("AT+CGMI", cfg::kSmsCmdTimeoutMs)) {
              String s = resp; s.replace("\r", ""); s.replace("\n", "");
              int ok = s.indexOf("OK"); if (ok >= 0) s = s.substring(0, ok);
              s.trim(); m.vendor = s;
            }
            break;
          }
          case 6: {  // 通信模组型号 AT+CGMM（如 ML307A-DSLN）
            if (at("AT+CGMM", cfg::kSmsCmdTimeoutMs)) {
              String s = resp; s.replace("\r", ""); s.replace("\n", "");
              int ok = s.indexOf("OK"); if (ok >= 0) s = s.substring(0, ok);
              s.trim(); m.model = s;
            }
            break;
          }
          case 7: {  // 通信模组版本/完整料号 AT+CGMR（常含后缀，如 ML307A-DSLN-MTSH1S00 或固件版本）
            if (at("AT+CGMR", cfg::kSmsCmdTimeoutMs)) {
              String s = resp; s.replace("\r", ""); s.replace("\n", "");
              int ok = s.indexOf("OK"); if (ok >= 0) s = s.substring(0, ok);
              s.trim(); m.revision = s;
            }
            break;
          }
          case 8: {  // SIM 卡在位检测 AT+CPIN?（运行时拔卡检测，约每 36s 一轮）
            if (at("AT+CPIN?", cfg::kSmsCmdTimeoutMs)) {
              if (resp.indexOf("+CPIN: READY") >= 0) {
                setSimReady(port, true);
              } else if (resp.indexOf("+CPIN:") >= 0 ||
                         resp.indexOf("NOT INSERTED") >= 0 ||
                         resp.indexOf("+CME ERROR: 10") >= 0) {
                // 掉卡/卡失效：标记未就绪（触发「已拔出」通知）并重新初始化。
                // 状态机经 kWaitCpin 确认后进故障态慢速重试；用户重新插卡后
                // CPIN 恢复 READY，setSimReady 会补发「已插入」通知。
                setSimReady(port, false);
                if (!m.needReinit) {
                  m.needReinit = true;
                  _log.addf(LogLevel::kWarn, "sms", "通信模组 %u SIM 卡疑似拔出，重新初始化",
                            (unsigned)port);
                }
              }
            }
            break;
          }
        }
        m.teleStep = (m.teleStep + 1) % 9;
      }
      return;
    }

    case ModemInitState::kFault: {
      // 慢速重试：探活看是否恢复（故障/未连接通信模组只在自身端口空转）
      if (!at("AT", cfg::kSmsCmdTimeoutMs)) return;
      if (resp.indexOf("OK") >= 0) {
        m.initState = ModemInitState::kWaitAt;
        m.initAttempts = 0;
        m.regAttempts = 0;
        m.configStep = 0;
        m.nextPollMs = now;
      } else {
        m.nextPollMs = now + cfg::kModemFaultRetryMs;
      }
      return;
    }
  }
}

// 发送一段 PDU：AT+CMGS=<len> -> 等 '>' -> 发 PDU(内嵌 0x1A) -> 等 +CMGS/OK。
// 握手细节（等 > 5s / 响应 30s / +CMGS 确认即成功）在 SerialBridge::sendSmsPdu，
// 流程与 esp32-sms-forwarding 一致：通信模组回包慢或只回 +CMGS 时不会误报失败。
bool SmsService::sendPduPart(uint8_t port, const SmsPduPart &part, String &err) {
  if (part.tpduLen <= 0) {
    err = "PDU 长度异常";
    return false;
  }
  auto r = _bridge.sendSmsPdu(port, cfg::kUartBaud, part.tpduLen, part.hex, err);
  if (!r.ok) {
    if (!err.length()) err = "invalid port";
    return false;
  }
  return err.length() == 0;  // sendSmsPdu 用 err 表达失败原因（含通信模组响应）
}

// 处理一条收到的 PDU
void SmsService::handleCmt(uint8_t port, const String &pduHex) {
  String sender, ts, text;
  int concat[3] = {0, 0, 0};
  if (!_codec.decode(pduHex, sender, ts, text, concat)) {
    _log.addf(LogLevel::kWarn, "sms", "通信模组 %u 的 PDU 解码失败（%u 字符）",
              (unsigned)port, (unsigned)pduHex.length());
    return;
  }

  uint32_t epoch = SmsCodec::sctsToEpoch(ts);
  if (concat[2] > 0) {
    // 长短信：喂入重组槽位，未收齐直接返回
    String full, fullTs;
    if (!_codec.reassemble(_concat, sender, ts, text, concat[0], concat[1],
                           concat[2], full, fullTs)) {
      return;
    }
    text = full;
    ts = fullTs;
    epoch = SmsCodec::sctsToEpoch(ts);
    _log.addf(LogLevel::kInfo, "sms", "通信模组 %u 长短信已拼接（%u 段）",
              (unsigned)port, (unsigned)concat[2]);
  }

  // 黑名单拦截：在落盘与转发之前判定，使「静默丢弃」真正不入库、不推送。
  bool blocked = false;
  if (_blacklist && _blacklist->enabled()) {
    blocked = _blacklist->match(sender, BlackList::kSms);
  }
  if (blocked) {
    String note = _blacklist->lastNote();
    _log.addf(LogLevel::kInfo, "black", "命中黑名单，来自 %s（%s）%s",
              sender.c_str(),
              _blacklist->action() == BlackList::kDrop ? "静默丢弃" : "仅记录不推送",
              note.length() ? ("[" + note + "]").c_str() : "");
    if (_blacklist->action() == BlackList::kDrop) return;  // 不入库、不推送
  }

  SmsMessage msg;
  msg.incoming = true;
  msg.phone = sender;
  msg.content = text;
  msg.ts = epoch ? epoch : nowTs();
  msg.modem = port;
  uint32_t id = _store.add(msg);
  if (id) {
    lock();
    _lastSmsId = id;
    unlock();
    _log.addf(LogLevel::kInfo, "sms", "通信模组 %u 收到短信 #%u，来自 %s（%u 字节）",
              (unsigned)port, (unsigned)id, sender.c_str(),
              (unsigned)text.length());
  } else {
    // 解码成功但未入库：留痕便于区分「没收到」与「收到没存上」
    _log.addf(LogLevel::kWarn, "sms", "通信模组 %u 收到来自 %s 的短信，但入库失败",
              (unsigned)port, sender.c_str());
  }
  // 通知上层（邮件通知等）。命中黑名单（仅记录模式）时跳过，不向任何通道转发；
  // 静默丢弃已在上面 return。回调运行于抽数任务上下文，只允许轻量操作。
  if (_onSms && !blocked) _onSms(sender, text, msg.ts, (int8_t)port);
}

// 最新一条短信的摘要 JSON
String SmsService::latestJson() {
  SmsMessage m;
  if (!_store.detail(_lastSmsId, m)) return "{}";
  return String("{\"id\":") + m.id + ",\"incoming\":" +
         (m.incoming ? "true" : "false") + ",\"phone\":\"" + m.phone +
         "\",\"ts\":" + m.ts + ",\"modem\":" + m.modem + "}";
}

// 载入 NVS 配置
bool SmsService::loadConfig() {
  // 默认：两个通信模组都启用（本设备即为双通信模组桥接设计）
  for (uint8_t p = 1; p <= cfg::kSmsMaxModems; p++) {
    _modems[p].enabled = true;
    _modems[p].csmsRef = 1;
  }
  _max = cfg::kSmsStoreMax;

  Preferences prefs;
  String json;
  if (prefs.begin(cfg::kNvsNamespace, true)) {
    json = prefs.getString(cfg::kNvsKeySms, "");
    prefs.end();
  }
  if (!json.length()) return false;

  JsonDocument doc;
  if (deserializeJson(doc, json) != DeserializationError::Ok) return false;
  if (doc["max"].is<int>()) {
    _max = (uint16_t)constrain(doc["max"].as<int>(), 10, 1000);
  }
  for (JsonObject o : doc["modems"].as<JsonArray>()) {
    int port = o["port"] | 0;
    if (port >= 1 && port <= cfg::kSmsMaxModems) {
      _modems[port].enabled = o["enabled"] | true;
      _modems[port].alias = o["alias"] | "";
    }
  }
  _log.addf(LogLevel::kInfo, "sms", "短信配置已载入：上限 %u 条，通信模组=[%d,%d]",
            (unsigned)_max, (int)_modems[1].enabled, (int)_modems[2].enabled);
  return true;
}

// 从给定 JSON 对象导入通信模组配置并落盘
bool SmsService::importFrom(const JsonObject &o) {
  if (o["max"].is<int>()) {
    _max = (uint16_t)constrain(o["max"].as<int>(), 10, 1000);
  }
  for (JsonObject m : o["modems"].as<JsonArray>()) {
    int port = m["port"] | 0;
    if (port >= 1 && port <= cfg::kSmsMaxModems) {
      _modems[port].enabled = m["enabled"] | _modems[port].enabled;
      _modems[port].alias = m["alias"] | _modems[port].alias;
    }
  }
  if (!saveConfig()) {
    _log.addf(LogLevel::kError, "sms", "短信配置导入落盘失败");
    return false;
  }
  _log.addf(LogLevel::kInfo, "sms", "已从备份导入短信配置");
  return true;
}

// 保存 NVS 配置
bool SmsService::saveConfig() {
  JsonDocument doc;
  doc["ver"] = 1;
  doc["max"] = _max;
  JsonArray arr = doc["modems"].to<JsonArray>();
  for (uint8_t p = 1; p <= cfg::kSmsMaxModems; p++) {
    JsonObject o = arr.add<JsonObject>();
    o["port"] = p;
    o["enabled"] = _modems[p].enabled;
    o["alias"] = _modems[p].alias;
  }
  String out;
  serializeJson(doc, out);

  Preferences prefs;
  if (!prefs.begin(cfg::kNvsNamespace, false)) return false;
  size_t written = prefs.putString(cfg::kNvsKeySms, out);
  prefs.end();
  return written == out.length();
}
