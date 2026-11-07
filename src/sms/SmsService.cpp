#include "sms/SmsService.h"

#include "app/TaskWatchdog.h"  // 发送 / 查询常驻任务注册 TWDT 并喂狗
#include "log/LogStore.h"

#include <ArduinoJson.h>
#include <Preferences.h>

#include <Arduino.h>
#include <esp_system.h>              // esp_restart()：管理员 RESET 命令
#include <freertos/FreeRTOS.h>       // xTaskCreate / vTaskDelay
#include <freertos/task.h>

#include <algorithm>                 // std::sort：长短信分段排序
#include <time.h>                    // localtime_r / mktime：每日定点排程

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

// 32 位 FNV-1a 哈希：短信正文指纹（去重键的一部分）
uint32_t textHash(const String &s) {
  uint32_t h = 2166136261u;
  for (size_t i = 0; i < s.length(); i++) {
    h ^= (uint8_t)s[i];
    h *= 16777619u;
  }
  return h;
}

// 解析 AT+CPMS="SM" 响应里的第一个 <used>（SIM 存储已用条数）；无有效值返回 -1
int parseCpmsUsed(const String &resp) {
  int p = resp.indexOf("+CPMS:");
  if (p < 0) return -1;
  int j = p + 6;
  while (j < (int)resp.length() &&
         (resp[j] == ' ' || resp[j] == '\r' || resp[j] == '\n')) j++;
  int v = 0;
  bool num = false;
  while (j < (int)resp.length() && resp[j] >= '0' && resp[j] <= '9') {
    v = v * 10 + (resp[j] - '0');
    j++;
    num = true;
  }
  return num ? v : -1;
}

// 解析 AT+CMGF? 响应：0 = PDU 模式，1 = TEXT 模式，-1 = 无有效响应
int parseCmgfMode(const String &resp) {
  int p = resp.indexOf("+CMGF:");
  if (p < 0) return -1;
  int j = p + 6;
  while (j < (int)resp.length() &&
         (resp[j] == ' ' || resp[j] == '\r' || resp[j] == '\n' || resp[j] == '\t')) j++;
  if (j >= (int)resp.length() || resp[j] < '0' || resp[j] > '9') return -1;
  return resp[j] - '0';
}

// 把响应压成一行短片段，供日志与 Web 诊断展示（限制长度，避免占内存/刷屏）
String briefResp(const String &resp) {
  String s = resp.substring(0, 100);
  s.replace("\r", " ");
  s.replace("\n", " ");
  s.trim();
  return s;
}

}  // namespace

// 网络注册状态码 -> 可读文本
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

// 注册状态解析结果：stat = 网络注册状态码；act = 接入技术（仅 CEREG 有）
struct RegInfo {
  int stat = -1;
  int act  = -1;
};

// 严格整数判断：整串均为十进制数字且落在 [lo,hi] 内
static bool regIntIn(const String &s, int lo, int hi) {
  if (!s.length()) return false;
  long v = 0;
  for (int i = 0; i < (int)s.length(); i++) {
    char c = s[i];
    if (c < '0' || c > '9') return false;
    v = v * 10 + (c - '0');
    if (v > hi) return false;
  }
  return v >= lo;
}

// 解析一行 +CEREG:/+CREG: 中的 stat 与 act。
// 同一前缀有两种行格式，字段位置不同：
//   查询响应 AT+CEREG?  -> "+CEREG: <n>,<stat>[,<tac>,<ci>,<act>]"  stat 第 2 段
//   主动上报 URC        -> "+CEREG: <stat>[,<tac>,<ci>,<act>]"        stat 第 1 段
// 早期实现一律取第 2 段：解析 URC 时把 <tac> 当成 stat（十六进制串 -> 0），
// 于是 regText(0) 得到「未注册」，而 act 恰好取到末段 LTE(7)，
// 出现「未注册 · LTE」这种自相矛盾的显示。本函数按字段个数与取值范围
// （stat 0..5、act 0..7、tac/ci 为十六进制串）自动判别格式。
// 成功返回 true；act 可能为 -1（该行未给出接入技术）。
static bool parseRegLine(const String &line, const char *prefix, RegInfo &out) {
  int idx = line.indexOf(prefix);
  if (idx < 0) return false;
  String rest = line.substring(idx + (int)strlen(prefix));
  rest.trim();
  if (!rest.length()) return false;

  String f[8];
  int n = 0, start = 0;
  while (n < 8) {
    int c = rest.indexOf(',', start);
    String seg = (c < 0) ? rest.substring(start) : rest.substring(start, c);
    seg.trim();
    seg.replace("\"", "");
    f[n++] = seg;
    if (c < 0) break;
    start = c + 1;
  }
  if (!n) return false;

  int stat = -1, act = -1;
  if (n == 1) {
    // URC 精简格式：只有 stat
    if (!regIntIn(f[0], 0, 5)) return false;
    stat = f[0].toInt();
  } else if (n == 2) {
    // 查询响应（<n>,<stat>）
    if (!regIntIn(f[1], 0, 5)) return false;
    stat = f[1].toInt();
  } else {
    // 多字段：URC(<stat>,<tac>,<ci>,<act>[,<ct>,<rc>]) 或
    // 查询响应(<n>,<stat>,<tac>,<ci>,<act>[,...])。
    // 用取值范围区分：tac/ci 是十六进制串，不是 0..7 的小整数。
    bool urcOk = regIntIn(f[0], 0, 5) && (n < 4 || regIntIn(f[3], 0, 7));
    bool qryOk = regIntIn(f[1], 0, 5) && (n < 5 || regIntIn(f[4], 0, 7));
    if (urcOk && !qryOk) {
      stat = f[0].toInt();
      if (n >= 4) act = f[3].toInt();
    } else if (qryOk) {
      stat = f[1].toInt();
      if (n >= 5) act = f[4].toInt();
    } else {
      return false;
    }
  }
  out.stat = stat;
  out.act = act;
  return true;
}

// 从整段 AT 响应中解析注册状态：逐行尝试，取最后一个可解析的 +prefix 行
// （命令响应位于响应窗口末尾，窗口内混入的 URC 排在前面）。
static bool parseRegResp(const String &resp, const char *prefix, RegInfo &out) {
  bool ok = false;
  int pos = 0;
  while (pos <= (int)resp.length()) {
    int nl = resp.indexOf('\n', pos);
    String line = (nl >= 0) ? resp.substring(pos, nl) : resp.substring(pos);
    pos = (nl >= 0) ? nl + 1 : (int)resp.length() + 1;
    line.trim();
    if (!line.startsWith(prefix)) continue;
    RegInfo ri;
    if (parseRegLine(line, prefix, ri)) {
      out = ri;
      ok = true;
    }
  }
  return ok;
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
  _codecMutex = xSemaphoreCreateMutex();
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

  // 统一发送队列 + 专职 worker：所有发送入口只入队，worker 串行执行阻塞式 CMGS
  // 握手，完成事件在 tick 主循环上下文回调。worker 自创建即注册任务看门狗，
  // 并在空闲等待与握手循环里喂狗，长耗时不会被误判为卡死。
  _sendQ = xQueueCreate(kSendQueueLen, sizeof(SendJob *));
  _doneQ = xQueueCreate(kDoneQueueLen, sizeof(DoneEvent *));
  if (_sendQ && _doneQ) {
    xTaskCreate(&SmsService::sendTaskEntry, "sms_send", 8192, this, 4, &_sendTask);
  } else {
    if (_sendQ) vQueueDelete(_sendQ);
    if (_doneQ) vQueueDelete(_doneQ);
    _sendQ = _doneQ = nullptr;
    _log.addf(LogLevel::kWarn, "sms", "发送队列创建失败，统一发送不可用");
  }

  _lastSmsId = _store.maxId();
  _pollBaseMs = millis();  // 间隔模式以「开机」为基准
  _safetyLastMs = millis();  // 兜底轮询以「开机」为基准（首个周期后才触发）

  // 主动查询任务（通信模组存储兜底）：独立任务执行阻塞式 AT 查询，
  // 不占用抽数任务（URC 解析）与主循环的时间片；优先级低于发送任务。
  if (_codecMutex) {
    if (xTaskCreate(&SmsService::queryTaskEntry, "sms_query", 8192, this, 3, &_queryTask) != pdPASS) {
      _log.addf(LogLevel::kError, "sms", "主动查询任务创建失败，短信只能等 +CMTI 后手动查询");
    } else {
      _log.addf(LogLevel::kInfo, "sms", "主动查询任务已启动");
    }
  } else {
    _log.addf(LogLevel::kWarn, "sms", "编解码互斥量创建失败，主动查询不可用");
  }

  // 开机即排一轮全量查询：清掉上次运行遗留（已读但未删）的 SIM 副本，
  // 避免 SIM 已满时还要等一个兜底轮询周期才恢复收信。模组未就绪会保留标记，
  // 由查询任务在就绪后自动补跑。
  requestQueryAll();
}

// 周期调用（主循环）：长短信重组超时清理 + 来电检测派发；
// 通信模组初始化由抽数任务轮询驱动。先消费发送完成事件（在 loop 上下文触发
// done 回调，保证可安全访问 Web/MQTT 等共享资源），再跑常规 tick 逻辑。
void SmsService::tick() {
  if (_doneQ) {
    DoneEvent *e = nullptr;
    while (xQueueReceive(_doneQ, &e, 0) == pdTRUE) {
      if (e->done) e->done(e->id, e->stored, e->err);
      delete e;
    }
  }
  _codec.pruneTimeouts(_concat);
  for (uint8_t p = 1; p <= cfg::kSmsMaxModems; p++) pollCall(p);
  pollAdminCmd();  // 管理员短信命令（入队于抽数任务，执行于主循环）
  pollSchedule();  // 定时主动查询：只判断到期并置待查标记（不做阻塞 AT）
}

// 统一发送入口：入队一个 SendJob（不阻塞）。队列满返回 false。
bool SmsService::enqueueSend(uint8_t port, const String &phone, const String &content,
                             SendDoneFn done) {
  if (!_sendQ) return false;   // 队列未创建（极端内存不足）：退化，调用方须自行拒绝
  SendJob *j = new SendJob{port, phone, content, std::move(done)};
  if (xQueueSend(_sendQ, &j, 0) != pdTRUE) {
    delete j;
    return false;   // 队列满
  }
  return true;
}

// worker 任务入口
void SmsService::sendTaskEntry(void *arg) {
  static_cast<SmsService *>(arg)->sendTask();
}

// 专职发送任务：串行取出 SendJob，执行阻塞式 CMGS 握手（send 内部持端口锁），
// 完成后把结果打包成 DoneEvent 投到 _doneQ 由 tick 主循环回调。
// 已订阅 TWDT：空闲等待与 CMGS 握手（最坏 5s 提示符 + 30s 响应）都会喂狗，
// 因此单条发送耗时长不会误触发整机复位，只有真正卡死才触发。
void SmsService::sendTask() {
  TaskWatchdog::registerCurrent("sms_send");
  SendJob *j = nullptr;
  while (true) {
    // 有限超时等待而非 portMAX_DELAY：空闲期间也必须周期性喂狗。
    if (xQueueReceive(_sendQ, &j, pdMS_TO_TICKS(cfg::kWdtFeedGapMs)) != pdTRUE) {
      TaskWatchdog::feed();
      continue;
    }
    TaskWatchdog::feed();
    String err;
    bool stored = true;
    uint32_t id = send(j->port, j->phone, j->content, err, &stored);
    if (j->done) {
      DoneEvent *e = new DoneEvent{std::move(j->done), id, stored, err};
      if (xQueueSend(_doneQ, &e, 0) != pdTRUE) delete e;  // 完成队列满：丢弃事件（极少）
    }
    delete j;
    j = nullptr;
  }
}

// 一行是否全为 hex 字符（不要求偶数长度：长 PDU 可能被模组分行）
static bool isHexOnly(const String &s) {
  if (!s.length()) return false;
  for (size_t i = 0; i < s.length(); i++) {
    char c = s[i];
    if (!((c >= '0' && c <= '9') || (c >= 'A' && c <= 'F') ||
          (c >= 'a' && c <= 'f')))
      return false;
  }
  return true;
}

// 判断一行是否为完整的 PDU 十六进制串（偶数长度、全为 hex 字符）
static bool isPduLine(const String &s) {
  return !(s.length() & 1) && isHexOnly(s);
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
    m.lastActivityMs = millis();   // 任意模组上报都算"活跃"，用于抑制心跳探活

    // 等待中的 PDU 行优先归属上一条 +CMT。只有纯 hex 行才消费；其它行必须
    // 继续按行路由——否则恰好排在 +CMT 之后的 +CMTI 会被吞掉，存储模式下
    // queryPending 永远不置位，短信留在 SIM 里再也不被读回。
    if (m.waitingPdu) {
      if (millis() - m.waitingPduAtMs > cfg::kSmsPduWaitMs) {
        m.waitingPdu = false;  // 超时自清，避免标志永久卡住
      } else if (isPduLine(line)) {
        m.waitingPdu = false;
        handleCmt(port, line);
        consumed = true;
        continue;
      } else {
        m.waitingPdu = false;
        _log.addf(LogLevel::kWarn, "sms",
                  "通信模组 %u 的 +CMT 后未收到 PDU，按普通上报继续处理：%s",
                  (unsigned)port, line.c_str());
      }
    }

    // 运行时通信模组重启检测：仅在已就绪态下识别开机类 URC，置位让 tick 重新初始化。
    // 注意：+CPIN: READY 已移出此处——SIM 插拔由下方 +CPIN: URC 处理，避免误判重启。
    if (m.initState == ModemInitState::kReady) {
      if (line == "RDY" || line.startsWith("^STARTUP") || line.startsWith("+PBREADY")) {
        m.needReinit = true;
        consumed = true;
        continue;
      }
    }

    // 注册状态 URC（AT+CEREG=3 / AT+CREG=1 已开启主动上报）：仅在就绪态驱动展示与运营商刷新，
    // 替代周期性 AT+CEREG?/CREG? 查询。模组状态变化即上报，无需轮询。
    if (m.initState == ModemInitState::kReady) {
      const char *regPfx = nullptr;
      if (line.startsWith("+CEREG:"))     regPfx = "+CEREG:";
      else if (line.startsWith("+CREG:"))  regPfx = "+CREG:";
      RegInfo ri;
      if (regPfx && parseRegLine(line, regPfx, ri)) {
        int stat = ri.stat;
        if (stat != m.lastRegStat) {        // 任意注册状态变化都刷新运营商（家网/漫游/搜网切换）
          m.operRefreshPending = true;
          m.lastRegStat = stat;
        }
        m.reg = regText(stat);
        if (ri.act >= 0) m.act = (uint8_t)ri.act;  // 该行未给出 act 时保留原值
        consumed = true;
        continue;
      }
    }
    // SIM 卡状态 URC（+CPIN: 在插拔/状态变化时主动上报）：替代周期性 AT+CPIN? 查询。
    if (line.startsWith("+CPIN:")) {
      if (line.indexOf("READY") >= 0) {
        setSimReady(port, true);
      } else {  // NOT INSERTED / SIM PIN / +CME ERROR 10 等：掉卡或失效
        setSimReady(port, false);
        if (!m.needReinit) {
          m.needReinit = true;
          _log.addf(LogLevel::kWarn, "sms", "通信模组 %u SIM 卡疑似拔出，重新初始化",
                    (unsigned)port);
        }
      }
      consumed = true;
      continue;
    }

    if (line.startsWith("+CMT:")) {
      // PDU 可能在同段后续行，也可能在下一段（头/PDU 跨 flush 时）
      m.waitingPdu = true;
      m.waitingPduAtMs = millis();
      { uint32_t u = millis() + cfg::kSmsRecvPauseMs; if (u > m.cmdPauseUntil) m.cmdPauseUntil = u; }  // 收短信延长暂停窗口
      consumed = true;
      continue;
    }
    // +CMTI:（CNMI=2,1 存储模式）：通信模组已把新短信写进自身存储并给出索引。
    // 本机收不到 +CMT 直投，只能靠主动查询把它读回——置待查标记，由查询任务处理。
    // 查询进行中同样置位：查询期间到达的新短信本轮未必读到，留给下一轮。
    if (line.startsWith("+CMTI:")) {
      m.queryPending = true;
      m.queryRetryAfterMs = 0;   // 新短信优先，不继承上次补删失败的重试节流
      { uint32_t u = millis() + cfg::kSmsRecvPauseMs; if (u > m.cmdPauseUntil) m.cmdPauseUntil = u; }  // 与 +CMT 一致，令后台 AT 让行
      // +CMTI 形如 +CMTI: "SM",3 —— 末尾数字即该短信在模组存储中的索引，
      // 记下来可直接 AT+CMGR 读回，不必依赖 AT+CMGL 列表（部分模组不支持该命令）
      int ci = line.lastIndexOf(',');
      int cidx = 0;
      if (ci >= 0) {
        for (int k = ci + 1; k < (int)line.length(); k++) {
          char c = line[k];
          if (c >= '0' && c <= '9') cidx = cidx * 10 + (c - '0');
          else if (cidx > 0) break;
        }
      }
      if (cidx > 0) {
        bool dup = false;
        for (uint8_t k = 0; k < m.queryIndexCount; k++) {
          if (m.queryIndexes[k] == cidx) { dup = true; break; }
        }
        if (!dup) {
          // 多条 +CMTI 不能互相覆盖（被挤掉的索引会永久留在 SIM 里）；缓存满时
          // 挤掉最旧的一个，它仍会被 AT+CMGL=0/1 全量列表捞回。
          if (m.queryIndexCount >= ModemState::kQueryIndexSlots) {
            for (uint8_t k = 1; k < ModemState::kQueryIndexSlots; k++) {
              m.queryIndexes[k - 1] = m.queryIndexes[k];
            }
            m.queryIndexCount = ModemState::kQueryIndexSlots - 1;
          }
          m.queryIndexes[m.queryIndexCount++] = cidx;
        }
      }
      _log.addf(LogLevel::kInfo, "sms",
                "通信模组 %u 收到 +CMTI 索引通知（索引 %d，待查 %u 个），已置待查标记",
                (unsigned)port, cidx, (unsigned)m.queryIndexCount);
      consumed = true;
      continue;
    }
    // +CDS:（状态报告）：消费但不处理，避免通信模组索引类流量冲刷 AT 历史
    if (line.startsWith("+CDS:")) {
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
  JsonDocument doc;
  JsonArray arr = doc.to<JsonArray>();
  for (uint8_t p = 1; p <= cfg::kSmsMaxModems; p++) {
    const ModemState &m = _modems[p];
    bool ready = m.initState == ModemInitState::kReady;
    JsonObject o = arr.add<JsonObject>();
    o["port"] = p;
    o["enabled"] = m.enabled;
    o["ready"] = ready;
    o["state"] = initStateName(m.initState);
    o["csq"] = m.csq;
    o["oper"] = m.oper;   // 直接赋值，ArduinoJson 自动转义
    o["reg"] = m.reg;
    o["sim"] = m.simReady;
    o["act"] = m.act;
    o["imei"] = m.imei;
    o["num"] = m.num;
    o["vendor"] = m.vendor;
    o["model"] = m.model;
    o["revision"] = m.revision;
    o["operCode"] = m.operCode;
    o["alias"] = m.alias;
  }
  String out;
  serializeJson(doc, out);
  return out;
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
  o["adminEnabled"] = _adminEnabled;
  o["cnmiStore"] = _cnmiStore;
  o["deleteAfterRead"] = _deleteAfterRead;
  JsonObject po = o["poll"].to<JsonObject>();
  po["enabled"] = _pollEnabled;
  po["mode"] = _pollMode;
  po["intervalH"] = _pollIntervalH;
  po["hour"] = _pollDailyMin / 60;
  po["minute"] = _pollDailyMin % 60;
  po["safetySec"] = (uint32_t)(_safetyPollMs / 1000);
  JsonArray ap = o["adminPhones"].to<JsonArray>();
  for (uint8_t i = 0; i < _adminPhoneCount; i++) ap.add(_adminPhones[i]);
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

  // 逐段发送，任一失败即中止；发送期间持续延长 cmdPauseUntil 令后台指令让行
  { uint32_t u = millis() + cfg::kSmsSendPauseMs; if (u > m.cmdPauseUntil) m.cmdPauseUntil = u; }
  for (int i = 0; i < n; i++) {
    if (!sendPduPart(port, parts[i], err)) return 0;
    { uint32_t u = millis() + cfg::kSmsSendPauseMs; if (u > m.cmdPauseUntil) m.cmdPauseUntil = u; }
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

// ---------------- 模组调试 ----------------
// 经短信通信模组发送 AT 命令：与短信收发共用同一端口锁（bridge.send 内部阻塞取锁），
// 因此控制台命令与 CMGS 发送**串行**，不会出现命令/PDU 交织破坏短信；发送窗口内
// 发送前延长 cmdPauseUntil 令 pollInit 让行，避免与后台查询 AT 互相干扰。通信模组就绪后自动还原
// CMGF=0 / CNMI（存储或直投模式），使控制台任意 AT（如切到文本模式、关直投、关射频）都不会
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
  { uint32_t u = millis() + cfg::kSmsSendPauseMs; if (u > m.cmdPauseUntil) m.cmdPauseUntil = u; }  // 用户命令期间令后台指令让行
  // 用户主动命令（Web 控制台）：以 kCmd 类型记入 AT 历史，供控制台展示
  SerialBridge::Result r = _bridge.send(port, baud, cmd, eol, timeoutMs,
                                        AtHistory::Type::kCmd);
  if (!r.ok) {
    err = "通信模组序号无效";
    return r;
  }
  // 还原短信关键配置（仅通信模组就绪时）；初始化进行中由状态机负责下发，无需此处干预。
  // 必须确认 OK：用户刚发过 AT+CMGF=1 / AT+CNMI=... 时若还原失败，模组会停在
  // TEXT 模式或未开 +CMTI 上报，存储模式的 AT+CMGR 就读不到 PDU。
  if (m.initState == ModemInitState::kReady) {
    const char *restore[2] = {"AT+CMGF=0", cnmiCmd()};
    for (uint8_t i = 0; i < 2; i++) {
      SerialBridge::Result rr = _bridge.send(port, cfg::kUartBaud, restore[i],
                                             SerialBridge::kCrlf, cfg::kSmsCmdTimeoutMs,
                                             AtHistory::Type::kBg);
      if (rr.ok && rr.resp.indexOf("OK") >= 0 && rr.resp.indexOf("ERROR") < 0) {
        if (i == 0) { m.cmgfChecked = false; m.cmgfOk = false; }  // 查询任务再复核一次
      } else {
        _log.addf(LogLevel::kWarn, "sms",
                  "通信模组 %u 还原 %s 未确认（响应：%s），短信收发可能异常",
                  (unsigned)port, restore[i], briefResp(rr.resp).c_str());
      }
    }
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

// 是否处于去重窗口内（上次派发后 kCallDedupMs 内）。
// callLastNotifyMs==0 表示「本次开机从未派发过」——不可当作 window 计算，
// 否则开机前 10 秒内的来电会被 now-0 误判为窗口内而整通丢弃。
static bool inCallDedupWindow(uint32_t lastNotifyMs, uint32_t now) {
  return lastNotifyMs != 0 && (now - lastNotifyMs) < SmsService::kCallDedupMs;
}

// RING / +CRING：开始一轮来电。去重窗口内的重复 RING（模组每响铃周期都重发）忽略。
void SmsService::handleCallRing(uint8_t port) {
  if (port < 1 || port > cfg::kSmsMaxModems) return;
  ModemState &m = _modems[port];
  uint32_t now = millis();
  if (inCallDedupWindow(m.callLastNotifyMs, now)) return;
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
    if (inCallDedupWindow(m.callLastNotifyMs, now)) return;
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
    if (inCallDedupWindow(m.callLastNotifyMs, now)) return;
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
                        kCallClccTimeoutMs, AtHistory::Type::kBg, r)) {
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
    } else {
      // 端口忙（如在发短信）：500ms 后重试，避免每轮 tick 空转
      m.callClccDueMs = now + 500;
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
    if (note.length()) {
      _log.addf(LogLevel::kInfo, "black", "命中黑名单，拦截来电 %s（%s）", num.c_str(),
                note.c_str());
    } else {
      _log.addf(LogLevel::kInfo, "black", "命中黑名单，拦截来电 %s", num.c_str());
    }
    return;
  }

  _log.addf(LogLevel::kInfo, "call", "通信模组 %u 来电通知：%s", (unsigned)port,
            num.c_str());
  if (_onCall) _onCall(num, nowTs(), (int8_t)port);
}

// ---------------- 管理员短信远程控制（移植自 esp32-sms-forwarding）----------------

// 号码归一化后精确相等（剥 +86 前缀、去空白）；任一为空返回 false
bool SmsService::phonesEqual(const String &a, const String &b) {
  if (!a.length() || !b.length()) return false;
  String x = a, y = b;
  x.trim(); y.trim();
  if (x.startsWith("+86")) x = x.substring(3);
  if (y.startsWith("+86")) y = y.substring(3);
  return x.length() > 0 && x == y;
}

// sender 是否命中管理员列表。持锁遍历：列表很小（<=kAdminPhoneMax 条短串），
// 临界区极短；与 setAdminPhones（HTTP 任务）的写互斥，避免读到半替换状态。
bool SmsService::isAdmin(const String &sender) {
  if (!_adminPhoneCount) return false;
  bool hit = false;
  lock();
  for (uint8_t i = 0; i < _adminPhoneCount; i++) {
    if (phonesEqual(sender, _adminPhones[i])) {
      hit = true;
      break;
    }
  }
  unlock();
  return hit;
}

// 校验单条管理员号码格式（与发送号码同一套规则：数字 + 可选前导 +，
// 可含空格与常见分隔符，3-20 位）；空串视为「未填写」，由调用方决定去留
static bool adminPhoneValid(const String &p) {
  if (p.length() > 20) return false;
  size_t digits = (p.length() && p[0] == '+') ? 1 : 0;
  if (p.length() - digits < 3) return false;
  for (size_t i = 0; i < p.length(); i++) {
    char c = p[i];
    if (i >= digits && !(c >= '0' && c <= '9') && c != ' ' && c != '-' &&
        c != '(' && c != ')' && c != '.') {
      return false;
    }
  }
  return true;
}

// HELP 命令回复：命令清单与参数说明（纯文本，长短信自动拆分）
static String adminHelpText() {
  String s;
  s += "管理员命令清单：\n";
  s += "SMS:<号码>:<内容>  从本卡代发短信（内容可含任意字符，含冒号）\n";
  s += "RESET  设备重启（先回复确认，2秒后执行）\n";
  s += "PING [主机]  经通信模组做网络连通性测试（默认 8.8.8.8），并回附系统状态\n";
  s += "HELP  显示本帮助\n";
  s += "注：命令在收到它的卡上执行并回复；号码匹配忽略+86；黑名单优先。";
  return s;
}

String SmsService::adminPhonesJson() const {
  JsonDocument doc;
  JsonArray arr = doc.to<JsonArray>();
  lock();
  for (uint8_t i = 0; i < _adminPhoneCount; i++) {
    arr.add(_adminPhones[i]);   // ArduinoJson 自动转义
  }
  unlock();
  String out;
  serializeJson(doc, out);
  return out;
}

bool SmsService::setAdminPhones(const std::vector<String> &phones) {
  // 先全部校验 + 归一化去重（不动运行态），任一条不合法整体拒绝
  String norm[cfg::kAdminPhoneMax];
  uint8_t n = 0;
  for (const String &raw : phones) {
    String p = raw;
    p.trim();
    if (!p.length()) continue;  // 空条目忽略（前端允许留空行）
    if (!adminPhoneValid(p)) {
      _log.addf(LogLevel::kWarn, "sms", "管理员号码格式不合法：%s", p.c_str());
      return false;
    }
    bool dup = false;
    for (uint8_t i = 0; i < n; i++) {
      if (phonesEqual(p, norm[i])) {
        dup = true;
        break;
      }
    }
    if (dup) continue;
    if (n >= cfg::kAdminPhoneMax) {
      _log.addf(LogLevel::kWarn, "sms", "管理员号码超出上限（%u 条）",
                (unsigned)cfg::kAdminPhoneMax);
      return false;
    }
    norm[n++] = p;
  }

  lock();
  for (uint8_t i = 0; i < n; i++) _adminPhones[i] = norm[i];
  _adminPhoneCount = n;
  unlock();

  if (!saveConfig()) {
    _log.addf(LogLevel::kError, "sms", "管理员号码保存失败");
    return false;
  }
  if (n) {
    String all;
    for (uint8_t i = 0; i < n; i++) {
      if (i) all += ", ";
      all += _adminPhones[i];
    }
    _log.addf(LogLevel::kInfo, "sms", "管理员号码已设置（%u 条）：%s", (unsigned)n,
              all.c_str());
  } else {
    _log.addf(LogLevel::kInfo, "sms", "管理员号码已清空，短信远程控制已关闭");
  }
  return true;
}

// 总开关置位并持久化。开启后还需配置了管理员号码才会真正生效
//（_adminPhoneCount > 0）；仅置开关不改变号码列表。写盘失败返回 false。
bool SmsService::setAdminEnabled(bool on) {
  _adminEnabled = on;
  if (!saveConfig()) {
    _log.addf(LogLevel::kError, "sms", "管理员总开关保存失败");
    return false;
  }
  _log.addf(LogLevel::kInfo, "sms", "管理员短信远程控制总开关已%s",
            on ? "开启" : "关闭");
  return true;
}

// 系统状态文本（PING 回复附带）；复用 App 注入的 _statusFn
String SmsService::systemStatusText() const {
  return _statusFn ? _statusFn() : String();
}

// PING 任务上下文（独立 FreeRTOS 任务，避免阻塞主循环/看门狗）
struct PingTaskCtx {
  SmsService *self;
  uint8_t    port;
  String     host;
  String     from;
  String     status;  // 主循环上下文预先抓好的系统状态（避免任务内再调 collectStatus 与主循环竞争）
};

// 经通信模组执行一次 ICMP ping（AT+CGPING，覆盖 SIMCOM/UNISOC 等常见 Cat.1 模组）。
// 先尽力激活 PDP 上下文（AT+CGACT=1,1），再发 AT+CGPING；所有 AT 经 sendAt 发出，
// 发完自动还原短信模式（CMGF/CNMI），不影响短信收发。host 空时回退 8.8.8.8。
bool SmsService::pingHost(uint8_t port, const String &hostIn, String &report) {
  if (port < 1 || port > cfg::kSmsMaxModems) {
    report = "PING 失败：通信模组不存在";
    return false;
  }
  ModemState &m = _modems[port];
  if (m.initState != ModemInitState::kReady) {
    report = "PING 失败：通信模组未就绪（" + String(initStateName(m.initState)) + "）";
    return false;
  }
  String host = hostIn;
  host.trim();
  host.replace("\"", "");          // 去除可能混入的引号，避免 AT 注入
  if (!host.length()) host = "8.8.8.8";

  String err;
  // 1) 尽力激活 PDP 上下文（已激活则很快返回 OK）；失败不阻断，交给 MPING 自行报错
  static constexpr uint32_t kCgactTimeoutMs = 15000;
  sendAt(port, cfg::kUartBaud, "AT+CGACT=1,1", SerialBridge::kCrlf,
         kCgactTimeoutMs, err);
  // 2) 发起 ping：AT+MPING=<host>,<count>,<timeout>,<size>,<ttl>
  //    参数范围（AT+MPING=?）：count(1-60) timeout(1-65535) size(1-1400) ttl(1-15)
  static constexpr int kMpingCount = 4;
  static constexpr int kMpingTimeoutMs = 10000;
  static constexpr int kMpingSize = 32;
  static constexpr int kMpingTtl = 15;  // 受模组 ttl 上限(1-15)约束
  String cmd = "AT+MPING=\"" + host + "\"," + String(kMpingCount) + "," +
               String(kMpingTimeoutMs) + "," + String(kMpingSize) + "," +
               String(kMpingTtl);
  SerialBridge::Result r = sendAt(port, cfg::kUartBaud, cmd,
                                  SerialBridge::kCrlf, cfg::kSmsSendTimeoutMs, err);
  const String &resp = r.resp;

  // 无回显：可能模块不支持 / 数据未开通 / 上下文失败
  if (resp.indexOf("+MPING:") < 0) {
    String reason;
    if (resp.indexOf("ERROR") >= 0) reason = "模块返回 ERROR";
    else if (resp.indexOf("NO CARRIER") >= 0) reason = "NO CARRIER";
    else if (resp.indexOf("OK") >= 0) reason = "模块无 MPING 回显（可能不支持）";
    else reason = "无响应";
    report = "PING " + host + " 失败：" + reason;
    String raw = resp;
    raw.replace("\r", " ");
    raw.replace("\n", " ");
    if (raw.length() > 80) raw = raw.substring(0, 80) + "…";
    if (raw.length()) report += "（原始：" + raw + "）";
    return false;
  }

  // 容错解析 +MPING:（不同模组格式略有差异）：
  //  - 汇总行（UNISOC/部分模组）：<sent>,<recv>,<lost>,<min>,<avg>,<max>
  //  - 逐跳行（SIMCOM）：<idx>,<result>,<rtt>，result==0 视为成功
  static const String kMpingPrefix = "+MPING:";
  int sent = 0, recv = 0, lost = 0;
  long rttMin = -1, rttAvg = 0, rttMax = -1;
  bool haveSummary = false;

  int pos = 0;
  while (true) {
    int idx = resp.indexOf(kMpingPrefix, pos);
    if (idx < 0) break;
    int eol = resp.indexOf('\n', idx);
    if (eol < 0) eol = resp.length();
    String line = resp.substring(idx + kMpingPrefix.length(), eol);  // 跳过 "+MPING:"
    line.trim();
    int commas = 0;
    for (size_t i = 0; i < line.length(); i++) if (line[i] == ',') commas++;
    if (commas >= 5 && !haveSummary) {
      long v[6] = {0, 0, 0, 0, 0, 0};
      int f = 0, s = 0;
      for (int i = 0; i <= commas && f < 6; i++) {
        int c = line.indexOf(',', s);
        String tok = (c < 0) ? line.substring(s) : line.substring(s, c);
        tok.trim();
        v[f++] = tok.toInt();
        s = (c < 0) ? (int)line.length() + 1 : c + 1;
      }
      sent = (int)v[0]; recv = (int)v[1]; lost = (int)v[2];
      rttMin = v[3]; rttAvg = v[4]; rttMax = v[5];
      haveSummary = true;
    } else {
      int c1 = line.indexOf(',');
      int c2 = (c1 >= 0) ? line.indexOf(',', c1 + 1) : -1;
      if (c1 >= 0) {
        long res = (c2 >= 0) ? line.substring(c1 + 1, c2).toInt() : 0;
        long rtt = (c2 >= 0) ? line.substring(c2 + 1).toInt() : 0;
        sent++;
        if (res == 0 && rtt > 0) {
          recv++;
          rttAvg += rtt;  // 复用为累计和，下方求平均
          if (rttMin < 0 || rtt < rttMin) rttMin = rtt;
          if (rttMax < 0 || rtt > rttMax) rttMax = rtt;
        }
      }
    }
    pos = eol + 1;
  }

  if (recv > 0) {
    if (haveSummary) {
      report = "PING " + host + " 成功：发送 " + String(sent) + " 接收 " + String(recv) +
               " 丢失 " + String(lost) + "，平均 RTT " + String(rttAvg) + " ms";
    } else {
      long avg = recv ? rttAvg / recv : 0;
      report = "PING " + host + " 成功：接收 " + String(recv) + " / " + String(sent) +
               "，平均 RTT " + String(avg) + " ms";
    }
    if (rttMin >= 0) report += "（最小 " + String(rttMin) + " / 最大 " + String(rttMax) + "）";
    return true;
  }
  report = "PING " + host + " 失败：未收到回包";
  String raw = resp;
  raw.replace("\r", " ");
  raw.replace("\n", " ");
  if (raw.length() > 80) raw = raw.substring(0, 80) + "…";
  if (raw.length()) report += "（原始：" + raw + "）";
  return false;
}
// 返回 false = 不是有效命令（SMS: 格式错误），调用方按普通短信继续处理；
// 队列被占用（上一条尚未执行）时丢弃新的并记日志，但仍算已消费（返回 true）。
bool SmsService::enqueueAdminCmd(uint8_t port, const String &sender,
                                 const String &cmd) {
  if (port < 1 || port > cfg::kSmsMaxModems) return false;

  AdminCmd job;
  job.port = port;
  job.from = sender;  // 结果回复给发命令的那个管理员
  // 关键字大小写不敏感（短信常被自动大写），仅用于分支判定；
  // SMS 的内容原样取自 cmd，保持大小写
  String key = cmd;
  key.toUpperCase();
  if (key.startsWith("SMS:")) {
    // SMS:<号码>:<内容> —— 冒号分隔，内容可含任意字符（含后续冒号）
    int fc = cmd.indexOf(':');
    int sc = cmd.indexOf(':', fc + 1);
    if (sc <= fc + 1) {
      // 格式错误：不当命令消费，由调用方按普通短信处理（入库 + 转发），
      // 便于管理员在记录里发现自己笔误
      _log.addf(LogLevel::kWarn, "sms", "通信模组 %u 管理员 SMS 命令格式错误，按普通短信处理：%s",
                (unsigned)port, cmd.c_str());
      return false;
    }
    job.kind = AdminCmd::kSms;
    job.phone = cmd.substring(fc + 1, sc);
    job.content = cmd.substring(sc + 1);
    job.phone.trim();
    job.content.trim();
  } else if (key == "RESET") {
    job.kind = AdminCmd::kReset;
  } else if (key == "PING") {
    job.kind = AdminCmd::kPing;
    // 可选目标主机：PING / PING 8.8.8.8 / PING:8.8.8.8（空 = 8.8.8.8）
    if (cmd.length() > 4) {
      char d = cmd[4];
      if (d == ' ' || d == ':') {
        String h = cmd.substring(5);
        h.trim();
        if (h.length()) job.content = h;
      }
    }
  } else if (key == "HELP") {
    job.kind = AdminCmd::kHelp;
  } else {  // 兜底（handleCmt 已过滤，正常不会到此）
    job.kind = AdminCmd::kReset;
  }

  lock();
  bool ok = !_adminCmdPending;
  if (ok) {
    _adminCmd = job;
    _adminCmdPending = true;
  }
  unlock();
  if (!ok) {
    _log.addf(LogLevel::kWarn, "sms", "通信模组 %u 管理员命令被丢弃（上一条尚未执行）",
              (unsigned)port);
  }
  return true;
}

// 执行队首命令（tick 主循环）。取出即清位，执行失败也有回复兜底。
void SmsService::pollAdminCmd() {
  AdminCmd job;
  {
    lock();
    if (!_adminCmdPending) {
      unlock();
      return;
    }
    job = _adminCmd;
    _adminCmdPending = false;
    unlock();
  }

  if (job.kind == AdminCmd::kReset) {
    _log.addf(LogLevel::kInfo, "sms", "通信模组 %u 执行管理员命令（来自 %s）：设备重启",
              (unsigned)job.port, job.from.c_str());
    replyToAdmin(job.port, job.from, "远程命令完成：设备正在重启");
    // 与 Web 重启同款：起低优先级任务延时 2 秒重启，留时间把回复发出去
    xTaskCreate(
        [](void *) {
          vTaskDelay(pdMS_TO_TICKS(2000));
          esp_restart();
          vTaskDelete(nullptr);
        },
        "sms_rst", 2048, nullptr, 5, nullptr);
    return;
  }

  if (job.kind == AdminCmd::kHelp) {
    _log.addf(LogLevel::kInfo, "sms", "通信模组 %u 执行管理员命令（来自 %s）：HELP",
              (unsigned)job.port, job.from.c_str());
    replyToAdmin(job.port, job.from, adminHelpText());
    return;
  }

  if (job.kind == AdminCmd::kPing) {
    _log.addf(LogLevel::kInfo, "sms", "通信模组 %u 执行管理员命令（来自 %s）：PING %s",
              (unsigned)job.port, job.from.c_str(),
              job.content.length() ? job.content.c_str() : "8.8.8.8");
    // ping 可能阻塞较久（CGACT + CGPING 各数十秒），放独立任务跑，避免阻塞主循环/
    // 看门狗；完成后从同一模组回复（含系统状态），sendAt 已自动还原短信模式。
    uint8_t port = job.port;
    String host = job.content;
    String from = job.from;
    // 系统状态在主循环上下文抓取（collectStatus 与主循环 LCD 渲染共享，勿在任务内调用）
    String status = systemStatusText();
    PingTaskCtx *ctx = new PingTaskCtx{this, port, host, from, status};
    if (!ctx) {
      replyToAdmin(port, from, "PING 失败：内存不足");
      return;
    }
    xTaskCreate(
        [](void *arg) {
          PingTaskCtx *c = static_cast<PingTaskCtx *>(arg);
          String report;
          c->self->pingHost(c->port, c->host, report);
          String reply = report;
          if (c->status.length()) reply += "\n---\n" + c->status;
          c->self->replyToAdmin(c->port, c->from, reply);
          delete c;
          vTaskDelete(nullptr);
        },
        "sms_ping", 5120, ctx, 5, nullptr);
    return;
  }

  // 管理员代发：入队统一发送队列（不阻塞主循环），完成后回复执行结果
  uint8_t aport = job.port;
  String afrom = job.from, aphone = job.phone, acontent = job.content;
  enqueueSend(aport, aphone, acontent,
      [this, aport, afrom, aphone](uint32_t id, bool stored, const String &err) {
        if (id != 0) {
          _log.addf(LogLevel::kInfo, "sms",
              "通信模组 %u 管理员代发成功：%s -> %s（#%u）", (unsigned)aport,
              afrom.c_str(), aphone.c_str(), (unsigned)id);
          replyToAdmin(aport, afrom, "远程命令完成：已向 " + aphone + " 发出短信");
        } else {
          _log.addf(LogLevel::kWarn, "sms", "通信模组 %u 管理员代发失败：%s -> %s：%s",
              (unsigned)aport, afrom.c_str(), aphone.c_str(), err.c_str());
          replyToAdmin(aport, afrom, "远程命令失败：" + err);
        }
      });
}

// 执行结果回复（从收到命令的同一模组发出，管理员看到的是熟悉号码）。
// 回复失败只记日志——命令本身的结果已由日志与短信记录体现。
// 经统一发送队列（不阻塞调用方任务：主循环或 PING 任务）。
void SmsService::replyToAdmin(uint8_t port, const String &to, const String &text) {
  if (!to.length()) return;
  enqueueSend(port, to, text,
      [this](uint32_t id, bool stored, const String &err) {
        if (id != 0) {
          _log.addf(LogLevel::kInfo, "sms", "管理员命令结果已回复（记录 #%u）",
                    (unsigned)id);
        } else {
          _log.addf(LogLevel::kWarn, "sms", "管理员命令结果回复失败：%s", err.c_str());
        }
      });
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
  // 重置遥测（重新初始化后从空开始采集；静态信息将在 kReady 时一次性重查并缓存）
  m.csq = -1; m.oper = ""; m.reg = ""; m.imei = ""; m.act = 0; m.num = "";
  m.vendor = ""; m.model = ""; m.revision = ""; m.operCode = "";
  m.simReady = false;
  m.identityQueried = false; m.idStep = 0; m.nextIdMs = 0;
  m.nextCsqMs = 0; m.nextOpsMs = 0; m.cmdPauseUntil = 0;
  m.lastRegStat = -1; m.operRefreshPending = false; m.regSynced = false;
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
  // 暂停其他后台指令窗口：短信发送前标记、每收到短信延长（见 sendSms/sendAt/onUnsolicited/handleCmt）。
  // 窗口内 pollInit 整体让行——心跳/遥测(CSQ/COPS)/重初始化均暂停，避免与短信收发交织。
  // cmdPauseUntil 为绝对时刻，0 表示无暂停（开机初值为 0，不会误触发）。
  if (now < m.cmdPauseUntil) return;
  // 节流：就绪态心跳(30s)与遥测(4s)各有计时，取较早者决定本轮是否动作
  uint32_t nextAt = m.nextPollMs;
  if (m.initState == ModemInitState::kReady) {
    if (m.nextIdMs   && m.nextIdMs   < nextAt) nextAt = m.nextIdMs;
    if (m.nextCsqMs  && m.nextCsqMs  < nextAt) nextAt = m.nextCsqMs;
    if (m.nextOpsMs  && m.nextOpsMs  < nextAt) nextAt = m.nextOpsMs;
  }
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
  // 初始化/心跳等内部流量统一以 kBg 类型记入 AT 历史（控制台可见但仍与用户命令区分）
  String resp;
  auto at = [&](const char *cmd, uint32_t to,
               AtHistory::Type type = AtHistory::Type::kBg) -> bool {
    SerialBridge::Result r;
    if (!_bridge.trySend(port, cfg::kUartBaud, cmd, SerialBridge::kCrlf, to, type, r))
      return false;  // 端口忙
    resp = r.resp;
    m.lastActivityMs = now;  // 任意模组响应都算"活跃"，用于抑制心跳探活
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
        m.lastActivityMs = now;
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
        RegInfo ri;
        if (parseRegResp(resp, "+CEREG:", ri)) {
          stat = ri.stat;
        } else {
          // CEREG 无可解析结果：退回 CREG（端口忙则跳过本轮，不推进计数）
          if (!at("AT+CREG?", cfg::kSmsCmdTimeoutMs)) {
            busy = true;
          } else if (parseRegResp(resp, "+CREG:", ri)) {
            stat = ri.stat;
          }
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
      // CMGF=0 必须先于 CNMI：部分模组在 TEXT 模式下会拒绝 CNMI 参数。
      // 这两条直接决定「+CMTI 上报后能否用 AT+CMGR 读回 PDU」，必须确认 OK。
      static const char *kCmds[] = {
          "AT+COPS=0", "AT+CEREG=3", "AT+CGDCONT=1,\"IP\",\"\"",
          "AT+CMGF=0",
          nullptr,  // 占位：CNMI 随配置动态生成（存储 / 直投模式）
          "AT+CLIP=1", "AT+CUSD=1", "AT+CREG=1"};
      if (m.configStep == 0) m.configFailCount = 0;
      if (m.configStep == 1) {
        // CEREG=3 失败降级到 =2（部分 2G 通信模组不支持带原因的注册上报）
        if (at("AT+CEREG=3", cfg::kSmsCmdTimeoutMs)) {
          if (resp.indexOf("OK") < 0) at("AT+CEREG=2", cfg::kSmsCmdTimeoutMs);
        } else {
          return;  // 忙
        }
      } else if (m.configStep == 3 || m.configStep == 4) {
        // CMGF=0 / CNMI：原实现只判断「端口锁是否取到」，resp 为 ERROR 甚至空
        // 也照样推进到就绪，模组可能停在 TEXT 模式或未开启 +CMTI 上报——
        // 表现为「串口能看到 +CMTI，短信却始终不入库」。此处校验并重试本步。
        const char *keyCmd = (m.configStep == 3) ? "AT+CMGF=0" : cnmiCmd();
        if (!at(keyCmd, cfg::kSmsCmdTimeoutMs)) return;  // 忙
        if (resp.indexOf("OK") < 0 || resp.indexOf("ERROR") >= 0) {
          if (++m.configFailCount <= 3) {
            _log.addf(LogLevel::kWarn, "sms",
                      "通信模组 %u 配置 %s 未确认（第 %u 次，响应：%s）",
                      (unsigned)port, keyCmd, (unsigned)m.configFailCount,
                      briefResp(resp).c_str());
            m.nextPollMs = now + cfg::kModemConfigStepMs;
            return;  // 停在本步重试，不推进状态机
          }
          _log.addf(LogLevel::kError, "sms",
                    "通信模组 %u 配置 %s 连续失败，跳过该步（短信收发可能异常）",
                    (unsigned)port, keyCmd);
          m.configFailCount = 0;
        } else {
          m.configFailCount = 0;
        }
      } else {
        if (!at(kCmds[m.configStep], cfg::kSmsCmdTimeoutMs)) return;  // 忙
      }
      m.configStep++;
      if (m.configStep >= (int)(sizeof(kCmds) / sizeof(kCmds[0]))) {
        m.initState = ModemInitState::kReady;
        m.failCount = 0;
        m.cmgfChecked = false;  // 重新就绪后由首次查询再复核一次实际模式
        m.cmgfOk = false;
        m.everReady = true;  // 此后 SIM 掉卡/插卡才算「运行时事件」
        m.lastActivityMs = now;
        m.nextPollMs = now + cfg::kModemLivenessMs;
        // 启动一次性静态采集（IMEI/号码/厂商/型号/版本/运营商），并初始化动态遥测计时。
        // 注册/CPIN 改由 URC 主动上报（见 onUnsolicited），不再周期性轮询。
        m.identityQueried = false;
        m.idStep = 0;
        m.nextIdMs = now;
        m.nextCsqMs = now;
        m.nextOpsMs = now + 6000;     // 运营商慢刷兜底（注册跃迁 URC 也会触发）
        // 注册状态初值不在此硬置；就绪后由一次性 AT+CEREG? 同步（见下方遥测），避免误判
        m.operRefreshPending = false;
        _log.addf(LogLevel::kInfo, "sms", "通信模组 %u 就绪（PDU 模式 + 短信%s已配置）",
                  (unsigned)port, _cnmiStore ? "存储" : "直投");
        // 通知上层（如时间同步立即尝试 SIM NITZ）。回调运行于抽数任务上下文，
        // 仅允许置标记等轻量操作
        if (_onReady) _onReady(port);
      } else {
        m.nextPollMs = now + cfg::kModemConfigStepMs;
      }
      return;
    }

    case ModemInitState::kReady: {
      // 心跳探活（周期 30s）与遥测采集相互独立：各自计时；端口忙（如正在发送短信）则跳过本轮。
      bool liveDue = (now >= m.nextPollMs);
      if (!liveDue && m.identityQueried &&
          now < m.nextCsqMs && now < m.nextOpsMs)
        return;

      // 心跳探活：仅当 kModemLivenessMs 内无任何「模组响应 / URC 上报」(lastActivityMs 过期)才发 AT；
      // 平时 CSQ 轮询(15s)/注册·CPIN URC/任意命令响应都会刷新 lastActivityMs，心跳 AT 几乎不发。
      // 连续失败判定通信模组掉线/重启，回到初始化重新配置。
      if (now - m.lastActivityMs >= cfg::kModemLivenessMs) {
        if (!at("AT", cfg::kSmsCmdTimeoutMs)) return;  // 忙（如正在发送短信）跳过
        if (resp.indexOf("OK") >= 0) {
          m.lastActivityMs = now;     // 探活成功也算活动
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
      } else {
        // 近期有活动，跳过心跳；nextPollMs 顺延到活动过期时刻，避免空转唤醒
        m.nextPollMs = m.lastActivityMs + cfg::kModemLivenessMs;
      }

      // 一次性静态采集（IMEI/号码/厂商/型号/版本/运营商）：就绪(reinit)后仅执行一次，
      // 写入 ModemState 永久缓存，不再进入周期轮询。分 tick 逐条下发（kModemIdStepMs 节流）。
      if (!m.identityQueried) {
        if (now >= m.nextIdMs) {
          m.nextIdMs = now + cfg::kModemIdStepMs;
          switch (m.idStep) {
            case 0: {  // 通信模组 IMEI AT+CGSN
              if (at("AT+CGSN", cfg::kSmsCmdTimeoutMs)) {
                String s = resp; s.replace("\r", ""); s.replace("\n", "");
                int ok = s.indexOf("OK"); if (ok >= 0) s = s.substring(0, ok);
                s.trim(); m.imei = s;
              }
              break;
            }
            case 1: {  // 本机号码（SIM 号码）AT+CNUM，取第二个引号串（number）
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
            case 2: {  // 通信模组厂商 AT+CGMI
              if (at("AT+CGMI", cfg::kSmsCmdTimeoutMs)) {
                String s = resp; s.replace("\r", ""); s.replace("\n", "");
                int ok = s.indexOf("OK"); if (ok >= 0) s = s.substring(0, ok);
                s.trim(); m.vendor = s;
              }
              break;
            }
            case 3: {  // 通信模组型号 AT+CGMM（如 ML307A-DSLN）
              if (at("AT+CGMM", cfg::kSmsCmdTimeoutMs)) {
                String s = resp; s.replace("\r", ""); s.replace("\n", "");
                int ok = s.indexOf("OK"); if (ok >= 0) s = s.substring(0, ok);
                s.trim(); m.model = s;
              }
              break;
            }
            case 4: {  // 通信模组版本/完整料号 AT+CGMR（常含后缀如 -MTSH1S00 或固件版本）
              if (at("AT+CGMR", cfg::kSmsCmdTimeoutMs)) {
                String s = resp; s.replace("\r", ""); s.replace("\n", "");
                int ok = s.indexOf("OK"); if (ok >= 0) s = s.substring(0, ok);
                s.trim(); m.revision = s;
              }
              break;
            }
            case 5: {  // 运营商 AT+COPS?（首次就绪采集；之后仅在注册跃迁/慢刷时刷新）
              if (at("AT+COPS?", cfg::kSmsCmdTimeoutMs)) {
                int i = resp.indexOf("+COPS:");
                if (i >= 0) {
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
          }
          m.idStep++;
          if (m.idStep >= 6) m.identityQueried = true;
        }
        return;  // 静态采集完成前不跑动态轮询，避免重复/竞争
      }

      // 初始注册状态同步（一次性）：设置 CEREG=3/CREG=1 后模组不会主动回送当前状态，
      // 注册 URC 仅在状态变化时上报；故显式查询一次填充 m.reg/m.act，之后由 URC 增量维护。
      // 端口忙（at 返回 false）则保持 regSynced=false，下个 tick 重试。
      if (!m.regSynced) {
        RegInfo ri;
        bool ok = false;
        if (at("AT+CEREG?", cfg::kSmsCmdTimeoutMs)) {
          ok = parseRegResp(resp, "+CEREG:", ri);
          if (!ok && at("AT+CREG?", cfg::kSmsCmdTimeoutMs))
            ok = parseRegResp(resp, "+CREG:", ri);
        }
        if (ok) {
          m.lastRegStat = ri.stat;
          m.reg = regText(ri.stat);
          if (ri.act >= 0) m.act = (uint8_t)ri.act;
          m.regSynced = true;
        }
      }

      // 动态遥测：仅轮询真正变化的数据，各查询独立计时；每 tick 至多一条（避免持锁过久）。
      // 信号质量 AT+CSQ
      if (now >= m.nextCsqMs) {
        m.nextCsqMs = now + cfg::kTeleCsqMs;
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
        return;
      }
      // 网络注册状态：已由 onUnsolicited 处理 +CEREG:/+CREG: URC 主动上报并刷新 m.reg/act，
      // 不再周期性轮询 AT+CEREG?/CREG?（仅在注册状态变化时更新，省去固定查询）。
      // 运营商 AT+COPS?：注册跃迁触发刷新，或慢刷兜底（kTeleOpsMs）。端口忙则保持 pending 重试。
      if (m.operRefreshPending || now >= m.nextOpsMs) {
        if (at("AT+COPS?", cfg::kSmsCmdTimeoutMs)) {
          m.nextOpsMs = now + cfg::kTeleOpsMs;
          m.operRefreshPending = false;
          int i = resp.indexOf("+COPS:");
          if (i >= 0) {
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
        return;
      }
      // SIM 卡在位：已由 onUnsolicited 处理 +CPIN: URC 主动上报并调用 setSimReady，
      // 不再周期性轮询 AT+CPIN?（拔卡时模组主动上报，必要时由心跳探活兜底重初始化）。
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
    if (!err.length()) err = "通信模组序号无效";
    return false;
  }
  return err.length() == 0;  // sendSmsPdu 用 err 表达失败原因（含通信模组响应）
}

// 处理一条收到的 PDU（解码 + 重组 + 落盘）
// 线程模型：+CMT 直投走抽数任务上下文，通信模组存储查询走查询任务上下文——两者
// 都调用本函数。pdulib 解码实例与长短信重组槽位都是共享状态，故用 _codecMutex
// 串行化（跨任务安全）。
SmsService::CmtOutcome SmsService::handleCmt(uint8_t port, const String &pduHex) {
  if (!_codecMutex) return CmtOutcome::kKeep;
  xSemaphoreTake(_codecMutex, portMAX_DELAY);
  String sender, ts, text;
  int concat[3] = {0, 0, 0};
  bool ok = _codec.decode(pduHex, sender, ts, text, concat);
  CmtOutcome out = ok ? processDecodedLocked(port, sender, ts, text, concat)
                      : CmtOutcome::kKeep;
  xSemaphoreGive(_codecMutex);
  if (!ok) {
    _log.addf(LogLevel::kWarn, "sms", "通信模组 %u 的 PDU 解码失败（%u 字符）",
              (unsigned)port, (unsigned)pduHex.length());
  }
  return out;
}

// 处理一条已解码的短信（长短信分段在此重组）；调用方须持有 _codecMutex
SmsService::CmtOutcome SmsService::processDecodedLocked(uint8_t port,
                                                        const String &sender,
                                                        const String &ts,
                                                        const String &text,
                                                        const int *concat) {
  ModemState &m = _modems[port];
  { uint32_t u = millis() + cfg::kSmsRecvPauseMs; if (u > m.cmdPauseUntil) m.cmdPauseUntil = u; }  // 收短信处理延长暂停窗口

  String fullText = text;
  String fullTs = ts;
  uint32_t epoch = SmsCodec::sctsToEpoch(ts);
  if (concat[2] > 0) {
    // 长短信：喂入重组槽位，未收齐则保留（模组侧副本不删，等下一段到齐）
    String full, fts;
    if (!_codec.reassemble(_concat, sender, ts, text, concat[0], concat[1],
                           concat[2], full, fts)) {
      return CmtOutcome::kKeep;
    }
    fullText = full;
    fullTs = fts;
    epoch = SmsCodec::sctsToEpoch(fts);
    _log.addf(LogLevel::kInfo, "sms", "通信模组 %u 长短信已拼接（%u 段）",
              (unsigned)port, (unsigned)concat[2]);
  }

  // 去重：TTL 内相同的（发送方 + 时间戳 + 正文指纹）视为已处理过——
  // 模组侧删除失败时下一轮会再次读到同一条，靠这里避免重复落盘/重复推送。
  String dkey = sender + "|" + fullTs + "|" + (unsigned)fullText.length() + "|" +
                (unsigned)textHash(fullText);
  if (dedupHit(dkey)) {
    _log.addf(LogLevel::kInfo, "sms", "通信模组 %u 的短信重复（已处理过），跳过落盘：来自 %s",
              (unsigned)port, sender.c_str());
    return CmtOutcome::kDelete;
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
    if (_blacklist->action() == BlackList::kDrop) {
      dedupAdd(dkey);
      return CmtOutcome::kDelete;  // 不入库、不推送
    }
  }

  // 管理员短信远程控制：命中命令则转 tick 执行，不落盘、不转发。
  // 顺序与旧实现一致：总开关 + 黑名单优先于管理员判定（管理员把自己拉黑则命令失效，
  // 这是用户的显式选择）；总开关关闭或号码列表为空时，所有短信按普通短信处理。
  if (_adminEnabled && _adminPhoneCount > 0 && isAdmin(sender)) {
    String cmd = fullText;
    cmd.trim();
    String key = cmd;
    key.toUpperCase();
    if (key.startsWith("SMS:") || key == "RESET" || key == "PING" || key == "HELP") {
      _log.addf(LogLevel::kInfo, "sms", "通信模组 %u 收到管理员命令（来自 %s）：%s",
                (unsigned)port, sender.c_str(), cmd.c_str());
      // 有效命令（含队列忙被丢弃）一律消费，不再入库/转发；
      // 仅 SMS: 格式错误时返回 false，按普通短信继续处理（便于发现笔误）
      if (enqueueAdminCmd(port, sender, cmd)) {
        dedupAdd(dkey);  // 命令已消费：模组侧副本删除失败时也不应重复执行
        return CmtOutcome::kDelete;
      }
    }
  }

  SmsMessage msg;
  msg.incoming = true;
  msg.phone = sender;
  msg.content = fullText;
  msg.ts = epoch ? epoch : nowTs();
  msg.modem = port;
  uint32_t id = _store.add(msg);
  if (!id) {
    // 解码成功但未入库：留痕便于区分「没收到」与「收到没存上」
    _log.addf(LogLevel::kWarn, "sms", "通信模组 %u 收到来自 %s 的短信，但入库失败",
              (unsigned)port, sender.c_str());
    return CmtOutcome::kKeep;  // 落盘失败：保留模组侧副本，下轮查询重试
  }
  lock();
  _lastSmsId = id;
  unlock();
  dedupAdd(dkey);
  _log.addf(LogLevel::kInfo, "sms", "通信模组 %u 收到短信 #%u，来自 %s（%u 字节）",
            (unsigned)port, (unsigned)id, sender.c_str(),
            (unsigned)fullText.length());
  // 通知上层（邮件通知等）。命中黑名单（仅记录模式）时跳过，不向任何通道转发；
  // 静默丢弃已在上面返回。回调运行于抽数任务/查询任务上下文，只允许轻量操作。
  if (_onSms && !blocked) _onSms(sender, fullText, msg.ts, (int8_t)port);
  return CmtOutcome::kDelete;
}

// ---------------- 主动查询任务（通信模组存储兜底）----------------
// 通信模组以 CNMI=2,1 接收时短信落在通信模组侧，本机收不到 +CMT 直投。
// 查询任务把模组存储里的短信读回本机：列索引 -> 逐条读 PDU -> 复用收信处理链
// （解码 / 长短信重组 / 黑名单 / 管理员命令 / 落盘 / 推送）-> 处理成功后删除
// 模组侧副本。任何一步失败都不删除内容，下轮查询重试（配合去重表避免重复落盘）。

// 查询任务入口
void SmsService::queryTaskEntry(void *arg) {
  static_cast<SmsService *>(arg)->queryTask();
}

// 查询任务主循环：轮询各端口的待查标记（+CMTI / 定时 / 手动触发都会置位）
void SmsService::queryTask() {
  TaskWatchdog::registerCurrent("sms_query");
  for (;;) {
    TaskWatchdog::feed();  // 每轮查询（含阻塞式 AT + 文件落盘）前续心跳
    // 接收模式切换后热下发 CNMI（阻塞 AT 只在本任务里做）
    if (_cnmiDirty) {
      _cnmiDirty = false;
      for (uint8_t p = 1; p <= cfg::kSmsMaxModems; p++) {
        ModemState &m = _modems[p];
        if (!m.enabled || m.initState != ModemInitState::kReady) continue;
        _bridge.send(p, cfg::kUartBaud, cnmiCmd(), SerialBridge::kCrlf,
                     cfg::kSmsCmdTimeoutMs, AtHistory::Type::kBg);
        _log.addf(LogLevel::kInfo, "sms", "通信模组 %u 已下发 %s", (unsigned)p, cnmiCmd());
      }
    }
    for (uint8_t p = 1; p <= cfg::kSmsMaxModems; p++) {
      ModemState &m = _modems[p];
      // 手动清空 SIM（Web 按钮）：阻塞式 AT 只在本任务执行；未就绪时保留标记，
      // 清空期间跳过同端口的普通查询，避免 CMGL/CMGR 与 CMGD 交织。
      if (m.clearPending) {
        if (m.enabled && m.initState == ModemInitState::kReady) {
          m.clearPending = false;
          m.clearRunning = true;
          clearPortSms(p);
          m.clearRunning = false;
        }
        continue;
      }
      if (!m.queryPending) continue;
      // 补删重试节流（防 AT 风暴）；差值比较兼容 millis() 回绕
      if (m.queryRetryAfterMs && (int32_t)(m.queryRetryAfterMs - millis()) > 0) continue;
      if (!m.enabled || m.initState != ModemInitState::kReady) continue;  // 未就绪：保留标记待下次
      m.queryPending = false;
      m.queryRunning = true;
      bool ok = queryPort(p);
      m.queryRunning = false;
      if (!ok) m.queryPending = true;  // 查询失败：下轮重试
    }
    vTaskDelay(pdMS_TO_TICKS(cfg::kSmsQueryPollMs));
  }
}

// 查询一个通信模组的存储短信；返回 false = 查询失败（AT 无响应/协议异常）
bool SmsService::queryPort(uint8_t port) {
  ModemState &m = _modems[port];
  // 查询期间暂停后台指令（心跳 / 遥测），避免 AT 互相交织
  { uint32_t u = millis() + cfg::kSmsRecvPauseMs; if (u > m.cmdPauseUntil) m.cmdPauseUntil = u; }

  // 0) 复核 PDU 模式。初始化或用户在控制台发 AT 都可能把 CMGF 改回 TEXT，
  //    此时 AT+CMGR 返回文本正文，extractPduHex 取不到 PDU —— 这是「+CMTI 可见
  //    但短信不入库」最常见的根因。仅每轮初始化后首次查询、或读失败后复查。
  auto confirmPduMode = [&](bool force) {
    if (m.cmgfChecked && !force) return m.cmgfOk;
    SerialBridge::Result cr = _bridge.send(port, cfg::kUartBaud, "AT+CMGF?",
                                           SerialBridge::kCrlf, cfg::kSmsCmdTimeoutMs,
                                           AtHistory::Type::kBg);
    int mode = parseCmgfMode(cr.resp);
    m.cmgfOk = (mode == 0);
    m.cmgfChecked = true;
    if (!m.cmgfOk) {
      _log.addf(LogLevel::kWarn, "sms",
                "通信模组 %u 不处于 PDU 模式（AT+CMGF? = %d，响应：%s），重新下发 AT+CMGF=0",
                (unsigned)port, mode, briefResp(cr.resp).c_str());
      SerialBridge::Result fix = _bridge.send(port, cfg::kUartBaud, "AT+CMGF=0",
                                              SerialBridge::kCrlf, cfg::kSmsCmdTimeoutMs,
                                              AtHistory::Type::kBg);
      m.cmgfOk = (fix.ok && fix.resp.indexOf("OK") >= 0 && fix.resp.indexOf("ERROR") < 0);
      m.lastQueryErr = String("CMGF 非 PDU 模式，已尝试纠正：") + briefResp(cr.resp);
    }
    return m.cmgfOk;
  };
  confirmPduMode(false);

  // 1) 存储占用（仅用于日志；失败不阻断后续查询）
  SerialBridge::Result cp = _bridge.send(port, cfg::kUartBaud, "AT+CPMS=\"SM\"",
                                         SerialBridge::kCrlf, cfg::kSmsQueryTimeoutMs,
                                         AtHistory::Type::kBg);
  if (cp.ok && cp.resp.indexOf("+CPMS:") >= 0) {
    _log.addf(LogLevel::kDebug, "sms", "通信模组 %u 短信存储占用：%s",
              (unsigned)port, cp.resp.substring(cp.resp.indexOf("+CPMS:")).c_str());
  }

  // 2) 索引来源：+CMTI 优先，直接 AT+CMGR；AT+CMGL 只作补充扫描。
  //    部分 Cat.1 模组不响应 CMGL，原实现每轮固定发两条 8s 超时的 CMGL，
  //    既长时间独占端口，又容易把残留响应串进下一条命令。
  //    CMTI 索引读失败会保留并退避重试，连续失败达阈值才改走 CMGL 全量扫描自愈。
  auto listOk = [](const SerialBridge::Result &r) {
    return r.ok && r.resp.indexOf("OK") >= 0 && r.resp.indexOf("ERROR") < 0;
  };
  std::vector<int> unread, read, cmti, idxs;
  for (uint8_t k = 0; k < m.queryIndexCount; k++) cmti.push_back(m.queryIndexes[k]);
  const bool cmtiOnly = !cmti.empty() && m.queryFailStreak < cfg::kSmsQueryFailMax;
  String gl0Resp, gl1Resp;
  if (!cmtiOnly) {
    if (m.queryFailStreak >= cfg::kSmsQueryFailMax) {
      _log.addf(LogLevel::kWarn, "sms",
                "通信模组 %u 连续 %u 轮读回失败，改用 AT+CMGL 全量扫描自愈",
                (unsigned)port, (unsigned)m.queryFailStreak);
      cmti.clear();             // 交给 CMGL 把 SIM 上的残留（含已读）重新捞回
      m.queryIndexCount = 0;    // 后续新到达的 +CMTI 从 0 开始重新累积
      m.queryFailStreak = 0;
      m.lastQueryErr = String("连续读回失败，已转 AT+CMGL 全量扫描");
    }
    SerialBridge::Result gl0 = _bridge.send(port, cfg::kUartBaud, "AT+CMGL=0",
                                            SerialBridge::kCrlf, cfg::kSmsQueryListTimeoutMs,
                                            AtHistory::Type::kBg);
    SerialBridge::Result gl1 = _bridge.send(port, cfg::kUartBaud, "AT+CMGL=1",
                                            SerialBridge::kCrlf, cfg::kSmsQueryListTimeoutMs,
                                            AtHistory::Type::kBg);
    gl0Resp = gl0.resp;
    gl1Resp = gl1.resp;
    bool have0 = listOk(gl0), have1 = listOk(gl1);
    if (!have0 && !have1 && cmti.empty()) {
      m.lastQueryErr = String("CMGL 无响应：") + briefResp(gl0.resp) + " / " +
                       briefResp(gl1.resp);
      _log.addf(LogLevel::kWarn, "sms", "通信模组 %u 查询短信列表失败（未读/已读均无响应）",
                (unsigned)port);
      m.queryRetryAfterMs = millis() + 5000UL;  // 失败退避，避免 250ms 一轮的 AT 风暴
      return false;
    }
    if (have0) parseSmsIndexes(gl0.resp, unread);
    if (have1) parseSmsIndexes(gl1.resp, read);
  }
  for (int v : unread) idxs.push_back(v);
  for (int v : read) idxs.push_back(v);
  for (int v : cmti) idxs.push_back(v);
  std::sort(idxs.begin(), idxs.end());
  idxs.erase(std::unique(idxs.begin(), idxs.end()), idxs.end());
  if (idxs.empty()) {
    m.queryLastFound = 0;
    m.queryLastHandled = 0;
    m.queryLastDeleted = 0;
    _queryLastMs = millis();
    // 插桩：索引为空时打印模组原始响应（区分「不支持该命令」与「确实无短信」），节流 5 分钟一次
    if (millis() - m.queryLogMs >= 300000) {
      m.queryLogMs = millis();
      _log.addf(LogLevel::kInfo, "sms",
                "通信模组 %u AT+CMGL=0/1 未解析到索引（响应前 100 字：%s / %s）",
                (unsigned)port, briefResp(gl0Resp).c_str(), briefResp(gl1Resp).c_str());
    }
    return true;  // 无存量：正常结束
  }
  _log.addf(LogLevel::kInfo, "sms",
            "通信模组 %u 待读取短信索引 %u 个（未读 %u / 已读 %u / +CMTI %u）",
            (unsigned)port, (unsigned)idxs.size(), (unsigned)unread.size(),
            (unsigned)read.size(), (unsigned)cmti.size());

  // 3) 逐条读出 PDU 并解码（解码需持有编解码锁：pdulib 实例非线程安全）
  struct Stored {
    int    idx = 0;
    String sender, ts, text;
    int    concat[3] = {0, 0, 0};
  };
  std::vector<Stored> msgs;
  std::vector<int> failed;  // 本轮没能成功读回并解码的索引
  bool truncated = false, modeFixed = false;

  // 长短信分组键：与 SmsCodec::reassemble / StuckGroup 用的 (发送方|ref|总段数)
  // 一致，加上端口隔离；0 表示非长短信，无需登记
  auto partGroupKey = [&](const String &gkey) -> uint32_t {
    return gkey.length() ? textHash(String((unsigned)port) + "|" + gkey) : 0u;
  };
  // 登记「本轮读回的这一分段存在 SIM 的哪个索引」。分段可能分多轮到达，
  // 只有登记下来，拼接成功时才能把前几轮的副本一起删掉。
  auto notePartIdx = [&](uint32_t key, int idx) {
    if (!_deleteAfterRead || !key || idx <= 0) return;
    uint32_t now = millis();
    for (auto &r : m.partIdx) {
      if (r.port && now - r.ms >= cfg::kSmsDedupTtlMs) r.port = 0;
    }
    for (auto &r : m.partIdx) {
      if (r.port == port && r.key == key && r.idx == (uint16_t)idx) {
        r.ms = now;   // 同一分段被已读列表反复捞回，只刷新时刻
        return;
      }
    }
    int dst = -1;
    for (uint8_t i = 0; i < ModemState::kPartIdxSlots; i++) {
      if (!m.partIdx[i].port) { dst = i; break; }
    }
    if (dst < 0) {  // 表满：覆盖最旧的一项
      dst = 0;
      for (uint8_t i = 1; i < ModemState::kPartIdxSlots; i++) {
        if (m.partIdx[i].ms < m.partIdx[dst].ms) dst = i;
      }
    }
    m.partIdx[dst].key = key;
    m.partIdx[dst].ms = now;
    m.partIdx[dst].idx = (uint16_t)idx;
    m.partIdx[dst].port = port;
  };
  for (size_t ii = 0; ii < idxs.size(); ii++) {
    int idx = idxs[ii];
    if (msgs.size() >= cfg::kSmsQueryMaxMsgs) {
      truncated = true;
      _log.addf(LogLevel::kWarn, "sms", "通信模组 %u 存量短信超过 %u 条，本轮仅处理前 %u 条",
                (unsigned)port, (unsigned)cfg::kSmsQueryMaxMsgs, (unsigned)msgs.size());
      for (size_t jj = ii; jj < idxs.size(); jj++) failed.push_back(idxs[jj]);
      break;
    }
    char cmd[24];
    snprintf(cmd, sizeof(cmd), "AT+CMGR=%d", idx);
    SerialBridge::Result gr = _bridge.send(port, cfg::kUartBaud, cmd, SerialBridge::kCrlf,
                                           cfg::kSmsQueryTimeoutMs, AtHistory::Type::kBg);
    if (!gr.ok) {
      _log.addf(LogLevel::kWarn, "sms", "通信模组 %u AT+CMGR=%d 发送失败", (unsigned)port, idx);
      failed.push_back(idx);
      continue;
    }
    m.lastCmgrResp = briefResp(gr.resp);  // 保留原始响应，Web 侧无需接 USB 也能取证
    String pdu = extractPduHex(gr.resp);
    if (!pdu.length()) {
      // 取不到 PDU 多因模组停在 TEXT 模式：立即复核并纠正一次，本轮保留索引重试
      if (!modeFixed) {
        modeFixed = true;
        m.cmgfChecked = false;
        confirmPduMode(true);
      }
      m.lastQueryErr = String("AT+CMGR=") + idx + " 未取到 PDU：" + m.lastCmgrResp;
      _log.addf(LogLevel::kWarn, "sms", "通信模组 %u %s",
                (unsigned)port, m.lastQueryErr.c_str());
      failed.push_back(idx);
      continue;
    }
    Stored s;
    s.idx = idx;
    xSemaphoreTake(_codecMutex, portMAX_DELAY);
    bool ok = _codec.decode(pdu, s.sender, s.ts, s.text, s.concat);
    xSemaphoreGive(_codecMutex);
    if (!ok) {
      m.lastQueryErr = String("索引 ") + idx + " PDU 解码失败：" + pdu.substring(0, 60);
      _log.addf(LogLevel::kWarn, "sms", "通信模组 %u %s",
                (unsigned)port, m.lastQueryErr.c_str());
      failed.push_back(idx);
      continue;
    }
    // 长短信分段：记住 SIM 索引，拼接成功后连同前几轮残留的分段一起补删
    if (s.concat[2] > 1) {
      String gk = s.sender + "|" + (unsigned)s.concat[0] + "|" + (unsigned)s.concat[2];
      notePartIdx(partGroupKey(gk), s.idx);
    }
    msgs.push_back(s);
  }

  // 3b) 只有成功读回并解码的 +CMTI 索引才从队列移除；失败的一律保留并退避重试。
  //     原实现在 CMGR 之前就清空队列，一次失败即永久丢失索引，SIM 副本再无人读。
  //     查询期间新到达的 +CMTI 也必须保留，否则会被本轮重建覆盖掉。
  std::vector<int> current, keep;
  for (uint8_t k = 0; k < m.queryIndexCount; k++) current.push_back(m.queryIndexes[k]);
  for (int idx : current) {
    if (std::find(cmti.begin(), cmti.end(), idx) == cmti.end()) keep.push_back(idx);
  }
  for (int idx : cmti) {
    if (std::find(failed.begin(), failed.end(), idx) != failed.end()) keep.push_back(idx);
  }
  std::sort(keep.begin(), keep.end());
  keep.erase(std::unique(keep.begin(), keep.end()), keep.end());
  uint8_t kept = 0;
  for (int idx : keep) {
    if (kept >= ModemState::kQueryIndexSlots) break;
    m.queryIndexes[kept++] = idx;
  }
  m.queryIndexCount = kept;
  if (failed.empty()) {
    m.queryFailStreak = 0;
    if (!modeFixed) m.lastQueryErr = String();  // 正常一轮：清掉历史故障信息
  } else if (m.queryFailStreak < 255) {
    m.queryFailStreak++;
  }
  m.queryLastFound = (uint32_t)msgs.size();

  // 4) 分组：长短信按（发送方 + 参考号）归组，其余各自成组
  std::vector<std::vector<int>> groups;
  for (size_t i = 0; i < msgs.size(); i++) {
    int g = -1;
    if (msgs[i].concat[2] > 1) {
      for (size_t j = 0; j < groups.size(); j++) {
        const Stored &h = msgs[groups[j][0]];
        if (h.sender == msgs[i].sender && h.concat[0] == msgs[i].concat[0]) { g = (int)j; break; }
      }
    }
    if (g < 0) {
      groups.push_back(std::vector<int>());
      g = (int)groups.size() - 1;
    }
    groups[g].push_back((int)i);
  }

  // 5) 逐组处理：按分段序号升序喂入收信处理链，长短信在最后一段到齐时才落盘。
  //    处理成功的组立刻逐条 AT+CMGD 删除 SIM 侧副本；删除失败会随下轮的
  //    「已读」列表被重新捞回来补删（去重表保证不会重复落盘 / 重复推送）。
  //    残缺长短信（缺失分段已不在 SIM 上）永远收不齐，等过滞留窗口后收敛为
  //    「不完整长短信」入库再删除，避免僵尸分段无限占满 SIM。
  uint32_t handled = 0, deleted = 0, delFailed = 0;
  uint8_t delFailStreak = 0;  // 连续失败时不再逐条重试，避免模组异常时长时间占端口
  auto deleteStored = [&](int idx) {
    char del[24];
    const uint8_t attempts = delFailStreak >= 2 ? 1 : 2;
    String lastResp;
    for (uint8_t a = 0; a < attempts; a++) {
      // 首次用标准写法；部分 Cat.1 模组要求显式 <delflag>，重试时补 ",0"
      if (a == 0) snprintf(del, sizeof(del), "AT+CMGD=%d", idx);
      else snprintf(del, sizeof(del), "AT+CMGD=%d,0", idx);
      SerialBridge::Result dr = _bridge.send(port, cfg::kUartBaud, del, SerialBridge::kCrlf,
                                             cfg::kSmsQueryTimeoutMs, AtHistory::Type::kBg);
      if (dr.ok && dr.resp.indexOf("OK") >= 0 && dr.resp.indexOf("ERROR") < 0) {
        delFailStreak = 0;
        return true;
      }
      lastResp = dr.resp.substring(0, 60);
      lastResp.replace("\r", " ");
      lastResp.replace("\n", " ");
      if (a + 1 < attempts) vTaskDelay(pdMS_TO_TICKS(100));
    }
    delFailStreak++;
    _log.addf(LogLevel::kWarn, "sms",
              "通信模组 %u 删除短信 %d 失败（已入库，下轮查询继续补删；响应：%s）",
              (unsigned)port, idx,
              lastResp.length() ? lastResp.c_str() : "无响应");
    return false;
  };
  auto groupKeyOf = [&](const std::vector<int> &grp) {
    const Stored &h = msgs[grp[0]];
    if (h.concat[2] <= 1) return String();
    return String(h.sender + "|" + (unsigned)h.concat[0] + "|" + (unsigned)h.concat[2]);
  };
  auto findStuck = [&](const String &key) -> ModemState::StuckGroup * {
    if (!key.length()) return nullptr;
    for (auto &g : m.stuckGroups) if (g.key == key) return &g;
    return nullptr;
  };
  auto clearStuck = [&](const String &key) {
    if (!key.length()) return;
    for (auto &g : m.stuckGroups) {
      if (g.key == key) { g.key = String(); g.firstMs = 0; g.saved = false; }
    }
  };
  auto addStuck = [&](const String &key) {
    if (!key.length() || findStuck(key)) return;
    for (auto &g : m.stuckGroups) {
      if (!g.key.length()) { g.key = key; g.firstMs = millis(); g.saved = false; return; }
    }
    ModemState::StuckGroup *oldest = &m.stuckGroups[0];  // 表满：覆盖最早的一组
    for (auto &g : m.stuckGroups) if (g.firstMs < oldest->firstMs) oldest = &g;
    oldest->key = key;
    oldest->firstMs = millis();
    oldest->saved = false;
  };
  // 收敛后清掉重组槽里的同组残留，避免后续同 ref 的新长短信被旧分段污染
  auto clearConcatSlots = [&](const Stored &h) {
    xSemaphoreTake(_codecMutex, portMAX_DELAY);
    for (uint8_t k = 0; k < cfg::kSmsConcatSlots; k++) {
      SmsConcatSlot &sl = _concat[k];
      if (sl.inUse && sl.sender == h.sender && sl.ref == (uint16_t)h.concat[0] &&
          sl.total == (uint8_t)h.concat[2]) sl.inUse = false;
    }
    xSemaphoreGive(_codecMutex);
  };
  // 取出并清除同一条长短信的全部已登记分段索引
  auto takePartIdx = [&](uint32_t key, std::vector<int> &out) {
    if (!key) return;
    for (auto &r : m.partIdx) {
      if (r.port == port && r.key == key && r.idx) {
        out.push_back((int)r.idx);
        r.port = 0;
      }
    }
  };
  // 删本轮分组 + 前几轮读回的分段副本。长短信由多个 +CMTI 分批上报时，
  // 只有最后一段落在本轮 groups 里；若不补删先前分段，它们会被 AT+CMGL=1
  // 反复捞回，滞留窗口到期后按「残缺长短信」二次入库（表现为短信重复）。
  auto deleteGroup = [&](const std::vector<int> &grp, uint32_t key) {
    if (!_deleteAfterRead) return false;  // 未删：保留滞留标记，避免重复入库
    std::vector<int> all;
    for (int i : grp) all.push_back(msgs[i].idx);
    takePartIdx(key, all);
    std::sort(all.begin(), all.end());
    all.erase(std::unique(all.begin(), all.end()), all.end());
    if (key && all.size() > grp.size()) {
      _log.addf(LogLevel::kInfo, "sms",
                "通信模组 %u 长短信拼接完成，连同前几轮残留共删除 %u 个分段副本",
                (unsigned)port, (unsigned)all.size());
    }
    bool allOk = true;
    for (int idx : all) {
      if (deleteStored(idx)) deleted++;
      else {
        delFailed++;
        allOk = false;
        if (key) notePartIdx(key, idx);  // 删除失败：保留登记，下轮补删
      }
    }
    return allOk;
  };

  for (auto &grp : groups) {
    std::sort(grp.begin(), grp.end(), [&](int a, int b) {
      return msgs[a].concat[1] < msgs[b].concat[1];
    });
    const Stored &h = msgs[grp[0]];
    const String gkey = groupKeyOf(grp);
    const uint32_t pkey = partGroupKey(gkey);
    ModemState::StuckGroup *st = findStuck(gkey);

    // 已按「不完整长短信」入库：本轮只补删 SIM 副本，不再重复落盘 / 推送
    if (st && st->saved) {
      if (deleteGroup(grp, pkey)) clearStuck(gkey);
      continue;
    }

    CmtOutcome last = CmtOutcome::kKeep;
    for (int i : grp) {
      Stored &s = msgs[i];
      xSemaphoreTake(_codecMutex, portMAX_DELAY);
      last = processDecodedLocked(port, s.sender, s.ts, s.text, s.concat);
      xSemaphoreGive(_codecMutex);
    }
    // 整组可删的条件：序号最大的一段处理成功（长短信只有最后一段到齐才会落盘）
    if (last == CmtOutcome::kDelete) {
      clearStuck(gkey);
      handled++;
      deleteGroup(grp, pkey);
      continue;
    }

    // 未收齐：普通短信（落盘失败等）保留副本下轮重试；长短信进入滞留跟踪，
    // 给 +CMTI 分批上报的分段留出到达窗口，超时仍残缺才收敛。
    if (!gkey.length()) continue;
    addStuck(gkey);
    st = findStuck(gkey);
    if (!st || millis() - st->firstMs < cfg::kSmsConcatStuckMs) continue;

    const int totalSegs = h.concat[2] > (int)cfg::kSmsConcatMaxParts
                              ? (int)cfg::kSmsConcatMaxParts : h.concat[2];
    String partial;
    for (int seg = 1; seg <= totalSegs; seg++) {
      String segText;
      bool found = false;
      for (int i : grp) {
        if (msgs[i].concat[1] == seg) { segText = msgs[i].text; found = true; break; }
      }
      partial += found ? segText : (String("[缺失分段") + seg + "]");
    }
    String content = String("[长短信不完整 ") + (unsigned)grp.size() + "/" +
                     (unsigned)h.concat[2] + " 段]" + partial;
    uint32_t epoch = SmsCodec::sctsToEpoch(h.ts);
    SmsMessage msg;
    msg.incoming = true;
    msg.phone = h.sender;
    msg.content = content;
    msg.ts = epoch ? epoch : nowTs();
    msg.modem = port;
    uint32_t id = _store.add(msg);
    if (!id) {
      _log.addf(LogLevel::kWarn, "sms",
                "通信模组 %u 残缺长短信 ref=%u 入库失败，保留 SIM 副本下轮重试",
                (unsigned)port, (unsigned)h.concat[0]);
      continue;
    }
    lock(); _lastSmsId = id; unlock();
    if (_onSms) _onSms(h.sender, content, msg.ts, (int8_t)port);
    _log.addf(LogLevel::kWarn, "sms",
              "通信模组 %u 长短信 ref=%u 等待超时仍未收齐（%u/%u 段），已按不完整内容入库 #%u 并删除 SIM 副本",
              (unsigned)port, (unsigned)h.concat[0], (unsigned)grp.size(),
              (unsigned)h.concat[2], (unsigned)id);
    st->saved = true;  // 删除失败时下一轮只补删，不重复入库
    clearConcatSlots(h);
    handled++;
    if (deleteGroup(grp, pkey)) clearStuck(gkey);
  }
  m.queryLastHandled = handled;
  m.queryLastDeleted = deleted;
  _queryLastMs = millis();
  // 删除失败 / 单轮没处理完 / 索引读回失败：排下一轮。读回失败按连续轮次指数
  // 退避（3s 起，上限 60s），避免模组异常时每 250ms 刷一轮 AT 风暴。
  uint32_t waitMs = delFailed ? 5000UL : 0UL;
  if (!failed.empty()) {
    uint32_t backoff = 3000UL;
    for (uint8_t i = 1; i < m.queryFailStreak && i < 6; i++) backoff *= 2;
    if (backoff > 60000UL) backoff = 60000UL;
    if (backoff > waitMs) waitMs = backoff;
  }
  if (delFailed || (truncated && deleted > 0) || !failed.empty()) {
    m.queryPending = true;
    m.queryRetryAfterMs = waitMs ? millis() + waitMs : 0;
  } else {
    m.queryRetryAfterMs = 0;
  }
  _log.addf(LogLevel::kInfo, "sms",
            "通信模组 %u 查询完成：存量 %u 条，处理 %u 条，删除 %u 条，读回失败 %u 条%s",
            (unsigned)port, (unsigned)m.queryLastFound, (unsigned)handled,
            (unsigned)deleted, (unsigned)failed.size(),
            delFailed ? "（部分删除失败，已排队补删）" :
            (!failed.empty() ? "（索引已保留，稍后重试）" : ""));
  return true;
}

// 手动清空 SIM：仅置标记，真正的 AT+CMGD 由查询任务执行（不阻塞 Web 任务）
bool SmsService::requestClear(uint8_t port) {
  if (port < 1 || port > cfg::kSmsMaxModems) return false;
  ModemState &m = _modems[port];
  if (!m.enabled || m.initState != ModemInitState::kReady) return false;
  if (m.clearPending || m.clearRunning) return false;
  m.clearPending = true;
  return true;
}

// 清空一个通信模组 SIM 上的全部短信（查询任务上下文，允许阻塞式 AT）。
// 首选 3GPP 标准 AT+CMGD=1,4（DelFlag=4 删除该存储区全部短信）；不支持批量删除
// 的模组用 AT+CMGL 列出全部索引逐条 AT+CMGD 兜底。前后各查一次 CPMS，以
// 「清空后 used == 0」作为成功判据，避免只看 OK 就误报成功。
void SmsService::clearPortSms(uint8_t port) {
  ModemState &m = _modems[port];
  { uint32_t u = millis() + cfg::kSmsRecvPauseMs; if (u > m.cmdPauseUntil) m.cmdPauseUntil = u; }

  auto cpmsUsed = [&]() {
    SerialBridge::Result r = _bridge.send(port, cfg::kUartBaud, "AT+CPMS=\"SM\"",
                                          SerialBridge::kCrlf, cfg::kSmsQueryTimeoutMs,
                                          AtHistory::Type::kBg);
    return parseCpmsUsed(r.resp);
  };
  auto atOk = [](const SerialBridge::Result &r) {
    return r.ok && r.resp.indexOf("OK") >= 0 && r.resp.indexOf("ERROR") < 0;
  };

  const int before = cpmsUsed();
  SerialBridge::Result all = _bridge.send(port, cfg::kUartBaud, "AT+CMGD=1,4",
                                          SerialBridge::kCrlf, cfg::kSmsQueryTimeoutMs,
                                          AtHistory::Type::kBg);
  const bool allOk = atOk(all);
  int after = cpmsUsed();
  uint32_t removedByIndex = 0;

  // 批量删除不被支持 / 仍有残留：列出全部索引逐条删除兜底
  if (!allOk || after > 0) {
    std::vector<int> idxs;
    SerialBridge::Result gl = _bridge.send(port, cfg::kUartBaud, "AT+CMGL=4",
                                           SerialBridge::kCrlf, cfg::kSmsQueryListTimeoutMs,
                                           AtHistory::Type::kBg);
    if (gl.ok) parseSmsIndexes(gl.resp, idxs);
    if (idxs.empty()) {
      // 部分模组不支持 CMGL=4：按四种状态分别列（0 未读 / 1 已读 / 2 待发 / 3 已发）
      const char *lists[] = {"AT+CMGL=0", "AT+CMGL=1", "AT+CMGL=2", "AT+CMGL=3"};
      for (const char *cmd : lists) {
        SerialBridge::Result r = _bridge.send(port, cfg::kUartBaud, cmd, SerialBridge::kCrlf,
                                              cfg::kSmsQueryListTimeoutMs,
                                              AtHistory::Type::kBg);
        if (r.ok) parseSmsIndexes(r.resp, idxs);
      }
    }
    std::sort(idxs.begin(), idxs.end());
    idxs.erase(std::unique(idxs.begin(), idxs.end()), idxs.end());
    for (int idx : idxs) {
      char del[24];
      snprintf(del, sizeof(del), "AT+CMGD=%d", idx);
      SerialBridge::Result dr = _bridge.send(port, cfg::kUartBaud, del, SerialBridge::kCrlf,
                                             cfg::kSmsQueryTimeoutMs, AtHistory::Type::kBg);
      if (!atOk(dr)) {
        snprintf(del, sizeof(del), "AT+CMGD=%d,0", idx);
        dr = _bridge.send(port, cfg::kUartBaud, del, SerialBridge::kCrlf,
                          cfg::kSmsQueryTimeoutMs, AtHistory::Type::kBg);
      }
      if (atOk(dr)) removedByIndex++;
    }
    if (!idxs.empty()) after = cpmsUsed();
  }

  const bool ok = (after == 0) || (after < 0 && allOk);
  uint32_t deleted = 0;
  if (before > 0 && after >= 0 && before > after) deleted = (uint32_t)(before - after);
  else if (removedByIndex) deleted = removedByIndex;
  else if (ok && before > 0) deleted = (uint32_t)before;  // CPMS 无法复核时按清空前计数
  m.clearLastOk = ok;
  m.clearLastDeleted = deleted;
  m.clearLastMs = millis();
  m.clearLastErr = ok ? String()
                      : (after > 0 ? String("仍有 ") + after + " 条未能删除"
                                   : String("模组未确认删除结果"));
  _log.addf(ok ? LogLevel::kInfo : LogLevel::kWarn, "sms",
            "通信模组 %u 清空 SIM 短信：清空前 %d 条，清空后 %d 条%s",
            (unsigned)port, before, after, ok ? "" : "（未完成，请查看 AT 历史）");
}

// 解析 AT+CMGL 响应中的短信索引（+CMGL: <idx>,...）
void SmsService::parseSmsIndexes(const String &resp, std::vector<int> &out) {
  int pos = 0;
  for (;;) {
    int i = resp.indexOf("+CMGL:", pos);
    if (i < 0) break;
    int j = i + 6;
    while (j < (int)resp.length() && resp[j] == ' ') j++;
    int idx = 0;
    bool num = false;
    while (j < (int)resp.length() && resp[j] >= '0' && resp[j] <= '9') {
      idx = idx * 10 + (resp[j] - '0');
      j++;
      num = true;
    }
    if (num && idx > 0) out.push_back(idx);
    pos = i + 6;
  }
}

// 从 AT+CMGR 响应中提取 PDU 十六进制串。
// 兼容：+CMGR: 头行、长 PDU 被模组分行、行内空格/引号；拒绝 OK/ERROR 之后的内容，
// 以及头行之前混入的纯 hex 行（上一条命令的串包残留），避免把别处的 PDU
// 当成本条短信解码。要求必须见到 +CMGR/+CMGL 头，否则返回空由调用方重试。
String SmsService::extractPduHex(const String &resp) {
  bool seenHeader = false;
  String pdu;
  int pos = 0;
  while (pos <= (int)resp.length()) {
    int nl = resp.indexOf('\n', pos);
    String line = (nl >= 0) ? resp.substring(pos, nl) : resp.substring(pos);
    pos = (nl >= 0) ? nl + 1 : (int)resp.length() + 1;
    line.trim();
    if (!line.length()) continue;
    if (line == "OK" || line.startsWith("ERROR") ||
        line.startsWith("+CMS ERROR") || line.startsWith("+CME ERROR")) break;
    if (line.startsWith("+CMGR:") || line.startsWith("+CMGL:")) {
      seenHeader = true;
      continue;
    }
    // 响应窗口里混入的 URC（+CMTI/+CDS/+CEREG/RING 等）跳过，不打断 PDU 拼接
    if (line[0] == '+' || line.startsWith("RING") ||
        line.startsWith("NO CARRIER") || line == "BUSY" || line == "NO ANSWER") continue;
    // 去掉行内空白与引号（部分模组回显 PDU 时会带空格或引号）
    String clean;
    for (size_t i = 0; i < line.length(); i++) {
      char c = line[i];
      if (c == ' ' || c == '\r' || c == '\n' || c == '\t' || c == '"') continue;
      clean += c;
    }
    if (!clean.length()) continue;
    if (!isHexOnly(clean)) {
      // TEXT 模式下的正文 / 其它非 hex 内容：已有 PDU 就收口，没有则跳过
      if (pdu.length()) break;
      continue;
    }
    if (!seenHeader) continue;  // 头之前的 hex 只可能是串包残留
    pdu += clean;               // 长 PDU 分行时拼接
  }
  return (seenHeader && pdu.length() && !(pdu.length() & 1)) ? pdu : String();
}

// 去重表查询：TTL 内同键视为已处理过
bool SmsService::dedupHit(const String &key) {
  uint32_t now = millis();
  for (uint8_t i = 0; i < cfg::kSmsDedupSlots; i++) {
    if (!_dedup[i].key.length()) continue;
    if (now - _dedup[i].ms >= cfg::kSmsDedupTtlMs) continue;  // 已过期
    if (_dedup[i].key == key) return true;
  }
  return false;
}

// 去重表登记（环形覆盖最旧）
void SmsService::dedupAdd(const String &key) {
  _dedup[_dedupNext].key = key;
  _dedup[_dedupNext].ms = millis();
  _dedupNext = (uint8_t)((_dedupNext + 1) % cfg::kSmsDedupSlots);
}

// 请求查询通信模组存储中的短信（异步，不阻塞调用方）
bool SmsService::requestQuery(uint8_t port) {
  if (port < 1 || port > cfg::kSmsMaxModems) return false;
  if (!_modems[port].enabled) return false;
  _modems[port].queryPending = true;
  return true;
}

// 对所有启用的通信模组各发起一次查询
void SmsService::requestQueryAll() {
  for (uint8_t p = 1; p <= cfg::kSmsMaxModems; p++) requestQuery(p);
}

// 查询状态 JSON（Web 展示）
String SmsService::queryStatusJson() const {
  JsonDocument doc;
  doc["storeMode"] = _cnmiStore;
  doc["deleteAfterRead"] = _deleteAfterRead;
  doc["lastMs"] = _queryLastMs;
  doc["safetySec"] = (uint32_t)(_safetyPollMs / 1000);  // 兜底轮询周期（秒）
  JsonArray arr = doc["ports"].to<JsonArray>();
  for (uint8_t p = 1; p <= cfg::kSmsMaxModems; p++) {
    JsonObject o = arr.add<JsonObject>();
    o["port"] = p;
    o["enabled"] = _modems[p].enabled;
    o["pending"] = _modems[p].queryPending;
    o["running"] = _modems[p].queryRunning;
    o["lastFound"] = _modems[p].queryLastFound;
    o["lastHandled"] = _modems[p].queryLastHandled;
    o["lastDeleted"] = _modems[p].queryLastDeleted;
    o["clearBusy"] = _modems[p].clearPending || _modems[p].clearRunning;
    o["clearLastOk"] = _modems[p].clearLastOk;
    o["clearLastDeleted"] = _modems[p].clearLastDeleted;
    o["clearLastMs"] = _modems[p].clearLastMs;
    o["clearLastErr"] = _modems[p].clearLastErr;
    // 存储模式诊断：不接 USB 也能从 /api/sms/query 判断链路停在哪一步
    o["indexes"] = _modems[p].queryIndexCount;      // 保留待重试的 +CMTI 索引数
    o["cmgfChecked"] = _modems[p].cmgfChecked;
    o["cmgfOk"] = _modems[p].cmgfOk;                // false = 模组可能停在 TEXT 模式
    o["failStreak"] = _modems[p].queryFailStreak;
    o["lastCmgr"] = _modems[p].lastCmgrResp;        // 最近一次 AT+CMGR 原始响应片段
    o["lastError"] = _modems[p].lastQueryErr;
  }
  String out;
  serializeJson(doc, out);
  return out;
}

// 判断定时查询是否到期；到期仅置待查标记（真正的 AT 由查询任务执行）
// 每日定点模式依赖本地时间（需时间已同步）；固定间隔模式基于开机时长。
void SmsService::pollSchedule() {
  // ---- 兜底轮询（常开）：与 +CMTI 自动上报互为补充 ----
  // 自动上报（URC 索引）丢失时，本周期主动下发 AT 指令把模组存储里的短信拉回来，
  // 保证不漏收；只在存储模式（短信落模组侧）下有意义。
  if (_cnmiStore && millis() - _safetyLastMs >= _safetyPollMs) {
    _safetyLastMs = millis();
    requestQueryAll();
    _log.addf(LogLevel::kDebug, "sms", "兜底轮询触发主动查询（每 %u 秒）",
              (unsigned)(_safetyPollMs / 1000));
  }

  if (!_pollEnabled) return;

  if (_pollMode == cfg::kSchedModeInterval) {
    uint32_t ivMs = (uint32_t)_pollIntervalH * 3600UL * 1000UL;
    if (millis() - _pollBaseMs < ivMs) return;
    _pollBaseMs = millis();
    requestQueryAll();
    _log.addf(LogLevel::kInfo, "sms", "定时主动查询触发（间隔 %u 小时）",
              (unsigned)_pollIntervalH);
    return;
  }

  // 每日定点：需时间已同步（未同步时 nowTs() 返回开机秒数 < 1.6e9）
  uint32_t now = nowTs();
  if (now < 1600000000) return;
  time_t tt = (time_t)now;
  struct tm t;
  localtime_r(&tt, &t);
  uint32_t curSec = (uint32_t)(t.tm_hour * 3600 + t.tm_min * 60 + t.tm_sec);
  uint32_t targetSec = (uint32_t)_pollDailyMin * 60UL;
  if (curSec < targetSec || curSec >= targetSec + cfg::kSchedDailyWindowSec) return;
  // 本地天序号：同一天只触发一次
  struct tm tl = t;
  tl.tm_hour = 0; tl.tm_min = 0; tl.tm_sec = 0;
  uint32_t day = (uint32_t)mktime(&tl) / 86400UL;
  if (_pollLastDay == day) return;
  _pollLastDay = day;
  saveConfig();  // 持久化，防重启后同日重复触发
  requestQueryAll();
  _log.addf(LogLevel::kInfo, "sms", "定时主动查询触发（每日 %02u:%02u）",
            (unsigned)(_pollDailyMin / 60), (unsigned)(_pollDailyMin % 60));
}

// 解析并应用定时查询配置字段（不落盘；载入与设置共用）
void SmsService::setPollFields(JsonObjectConst o) {
  if (!o) return;
  if (o["enabled"].is<bool>()) _pollEnabled = o["enabled"].as<bool>();
  if (o["mode"].is<int>()) {
    int m = o["mode"].as<int>();
    _pollMode = (m == cfg::kSchedModeDaily) ? cfg::kSchedModeDaily
                                            : cfg::kSchedModeInterval;
  }
  if (o["intervalH"].is<int>()) {
    int h = o["intervalH"].as<int>();
    _pollIntervalH = (uint16_t)constrain(h, (int)cfg::kSchedIntervalHMin,
                                         (int)cfg::kSchedIntervalHMax);
  }
  if (o["hour"].is<int>() || o["minute"].is<int>()) {
    int hh = o["hour"] | 0;
    int mm = o["minute"] | 0;
    _pollDailyMin = (uint16_t)constrain(hh * 60 + mm, 0, 1439);
  }
  if (o["safetySec"].is<int>()) {
    int s = o["safetySec"].as<int>();
    _safetyPollMs = (uint32_t)constrain(s, 30, 3600) * 1000UL;
  }
}

// 应用定时查询配置并落盘
bool SmsService::applyPollConfig(JsonObjectConst o) {
  if (!o) return false;
  setPollFields(o);
  _pollBaseMs = millis();  // 配置变更后重置基线，避免用旧基线立刻触发
  if (!saveConfig()) {
    _log.addf(LogLevel::kError, "sms", "定时查询配置落盘失败");
    return false;
  }
  _log.addf(LogLevel::kInfo, "sms", "定时主动查询配置已更新：%s，%s",
            _pollEnabled ? "启用" : "停用",
            _pollMode == cfg::kSchedModeDaily ? "每日定点" : "固定间隔");
  return true;
}

// 切换短信接收模式（CNMI）：落盘保存 + 标记热下发。
// 真正的 CNMI 下发由查询任务串行执行（阻塞 AT 不进 Web 任务）。
bool SmsService::setStoreMode(bool on) {
  if (_cnmiStore == on) return true;
  _cnmiStore = on;
  _cnmiDirty = true;
  // 模式切换会改变 URC 形态：清掉上一模式遗留的 +CMT 等待标志，否则切到存储
  // 模式后第一条 +CMTI 可能被当成「+CMT 后的 PDU 行」吞掉。
  for (uint8_t p = 1; p <= cfg::kSmsMaxModems; p++) {
    _modems[p].waitingPdu = false;
    _modems[p].cmgfChecked = false;
    _modems[p].queryFailStreak = 0;
  }
  if (!saveConfig()) {
    _log.addf(LogLevel::kError, "sms", "短信接收模式配置落盘失败");
    return false;
  }
  _log.addf(LogLevel::kInfo, "sms", "短信接收模式已切换：%s",
            on ? "存储+索引上报(CNMI=2,1)" : "直投(CNMI=2,2)");
  if (on) requestQueryAll();  // 立刻扫一次模组存量，切模式前收到的短信不漏
  return true;
}

// 最新一条短信的摘要 JSON
String SmsService::latestJson() {
  SmsMessage m;
  JsonDocument doc;
  if (_store.detail(_lastSmsId, m)) {
    doc["id"] = m.id;
    doc["incoming"] = m.incoming;
    doc["phone"] = m.phone;
    doc["ts"] = m.ts;
    doc["modem"] = m.modem;
  }
  String out;
  serializeJson(doc, out);
  return out;
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
  // 管理员号码列表（adminPhones 数组）
  _adminPhoneCount = 0;
  if (doc["adminPhones"].is<JsonArray>()) {
    for (JsonVariantConst v : doc["adminPhones"].as<JsonArray>()) {
      if (_adminPhoneCount >= cfg::kAdminPhoneMax) break;
      String p = v.as<String>();
      p.trim();
      if (p.length()) _adminPhones[_adminPhoneCount++] = p;
    }
  }
  // 总开关：缺省（旧配置/未写入）一律视为禁用，符合「默认关」约定
  _adminEnabled = doc["adminEnabled"] | false;
  // 收信模式与查询后删除策略：缺省沿用当前默认（存储模式 + 读完删除）
  _cnmiStore = doc["cnmiStore"] | true;
  _deleteAfterRead = doc["deleteAfterRead"] | true;
  // 定时主动查询配置（poll 节）
  if (doc["poll"].is<JsonObject>()) {
    JsonObject po = doc["poll"].as<JsonObject>();
    setPollFields(po);
    if (po["lastDay"].is<uint32_t>()) _pollLastDay = po["lastDay"].as<uint32_t>();
  }
  for (JsonObject o : doc["modems"].as<JsonArray>()) {
    int port = o["port"] | 0;
    if (port >= 1 && port <= cfg::kSmsMaxModems) {
      _modems[port].enabled = o["enabled"] | true;
      _modems[port].alias = o["alias"] | "";
    }
  }
  _log.addf(LogLevel::kInfo, "sms", "短信配置已载入：上限 %u 条，通信模组=[%d,%d]，管理员=%u 条（总开关%s）",
            (unsigned)_max, (int)_modems[1].enabled, (int)_modems[2].enabled,
            (unsigned)_adminPhoneCount, _adminEnabled ? "开" : "关");
  return true;
}

// 从给定 JSON 对象导入通信模组配置并落盘
bool SmsService::importFrom(const JsonObject &o) {
  if (o["max"].is<int>()) {
    _max = (uint16_t)constrain(o["max"].as<int>(), 10, 1000);
  }
  // 管理员号码列表（adminPhones 数组）
  if (o["adminPhones"].is<JsonArray>()) {
    _adminPhoneCount = 0;
    for (JsonVariantConst v : o["adminPhones"].as<JsonArray>()) {
      if (_adminPhoneCount >= cfg::kAdminPhoneMax) break;
      String p = v.as<String>();
      p.trim();
      if (p.length()) _adminPhones[_adminPhoneCount++] = p;
    }
  }
  if (o["adminEnabled"].is<bool>()) _adminEnabled = o["adminEnabled"].as<bool>();
  if (o["cnmiStore"].is<bool>()) _cnmiStore = o["cnmiStore"].as<bool>();
  if (o["deleteAfterRead"].is<bool>()) _deleteAfterRead = o["deleteAfterRead"].as<bool>();
  if (o["poll"].is<JsonObject>()) setPollFields(o["poll"].as<JsonObject>());
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
  doc["adminEnabled"] = _adminEnabled;
  doc["cnmiStore"] = _cnmiStore;
  doc["deleteAfterRead"] = _deleteAfterRead;
  {
    JsonObject po = doc["poll"].to<JsonObject>();
    po["enabled"] = _pollEnabled;
    po["mode"] = _pollMode;
    po["intervalH"] = _pollIntervalH;
    po["hour"] = _pollDailyMin / 60;
    po["minute"] = _pollDailyMin % 60;
    po["safetySec"] = (uint32_t)(_safetyPollMs / 1000);  // 兜底轮询周期
    po["lastDay"] = _pollLastDay;   // 运行时状态一并持久化（每日模式防重复触发）
  }
  {
    JsonArray ap = doc["adminPhones"].to<JsonArray>();
    for (uint8_t i = 0; i < _adminPhoneCount; i++) ap.add(_adminPhones[i]);
  }
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
