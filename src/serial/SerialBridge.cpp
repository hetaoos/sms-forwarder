#include "serial/SerialBridge.h"

#include "log/LogStore.h"

// 静态成员：单例指针，供中断/静态回调转发使用
SerialBridge *SerialBridge::sInstance = nullptr;

namespace {

// 本通信模组日志出口：全局日志环（串口由 LogStore 统一镜像输出）
LogStore &_log = devLog();

constexpr uint32_t kRxBufferSize = 4096;   // 串口接收缓冲（加大防溢出）
constexpr uint32_t kTxBufferSize = 1024;   // 串口发送缓冲
constexpr uint8_t kRxFifoThrhd = 32;       // 触发中断的 FIFO 阈值，越小越不容易溢出
constexpr uint32_t kUnsolicitedGapMs = 120;  // 自发上报分段的静默间隔
constexpr TickType_t kPumpPeriod = pdMS_TO_TICKS(5);  // 抽数任务周期 5ms
// 串口接收错误的最小上报间隔：onReceiveError 回调（串口事件任务上下文）只计数，
// 抽数任务最多每 2s 合并打印一次，错误风暴时既不失可观测性也不刷屏
constexpr uint32_t kErrReportMinGapMs = 2000;
}  // namespace

// 把字符按可打印形式追加到 out：控制字符转成 \xNN 文本
// out：目标字符串；c：待追加字符
void SerialBridge::appendPrintable(String &out, char c) {
  if (c == '\r' || c == '\n' || c == '\t' || (uint8_t)c >= 0x20) {
    out += c;
  } else {
    char tmp[8];
    snprintf(tmp, sizeof(tmp), "\\x%02X", (uint8_t)c);
    out += tmp;
  }
}

// 判断 s 的 pos 处是否正好是前缀 pfx（不依赖 String 的 offset 重载）
bool matchAt(const String &s, int pos, const char *pfx) {
  int n = (int)strlen(pfx);
  if (pos < 0 || pos + n > (int)s.length()) return false;
  for (int i = 0; i < n; i++) {
    if (s[pos + i] != pfx[i]) return false;
  }
  return true;
}

// 确保串口处于打开状态；已打开且波特率不同时热更新
// p：端口状态；baud：目标波特率
void SerialBridge::ensureOpen(Port &p, uint32_t baud) {
  if (p.opened) {
    if (p.baud != baud) {
      p.dev->updateBaudRate(baud);
      p.baud = baud;
    }
    return;
  }

  p.dev->setRxBufferSize(kRxBufferSize);
  p.dev->setTxBufferSize(kTxBufferSize);
  p.dev->begin(baud, SERIAL_8N1, p.rx, p.tx, false, 20000UL, kRxFifoThrhd);
  p.dev->onReceiveError([&p](hardwareSerial_error_t err) {
    // ⚠️ 该回调运行在串口事件任务（uart_event_task）上下文，默认栈仅 2048B
    // （已通过 CONFIG_ARDUINO_SERIAL_EVENT_TASK_STACK_SIZE 扩到 4096B 兜底）。
    // 绝不可在这里调用 LogStore::addf / Serial.printf：vsnprintf + 串口镜像
    // + MQTT sink 链路深且吃栈，高频错误（线序接反/波特率不匹配）时叠加
    // UART 中断嵌套会打爆栈，曾引发 exccause=0x42(DoubleException) panic。
    // 正确做法：只做 O(1) 计数与错误码记录，日志由抽数任务 reportErrors()
    // 按节流合并打印。
    p.errCount++;
    p.errCode = (uint8_t)err;
  });
  p.baud = baud;
  p.opened = true;
  _log.addf(LogLevel::kInfo, "uart", "串口 %u 已打开，波特率 %u",
            (unsigned)(p.index + 1), (unsigned)baud);
}

// 释放并重开串口：用于自愈
// p：端口状态；baud：重开时使用的波特率
void SerialBridge::reopen(Port &p, uint32_t baud) {
  _log.addf(LogLevel::kWarn, "uart", "串口 %u 自愈：以 %u 波特率重新打开",
            (unsigned)(p.index + 1), (unsigned)baud);
  p.dev->end();
  delay(20);
  p.dev->setRxBufferSize(kRxBufferSize);
  p.dev->setTxBufferSize(kTxBufferSize);
  p.dev->begin(baud, SERIAL_8N1, p.rx, p.tx, false, 20000UL, kRxFifoThrhd);
  p.baud = baud;
  p.opened = true;
  p.pend = String();
  p.recovers++;
  // 回调里捕获的是 Port 指针，端口对象没变，回调依然有效
}

// 初始化互斥量并创建抽数任务（固定到核心 1，优先级 2）
// 栈 8KB：本任务的调用链很深——URC 回调 -> SmsService::onUnsolicited ->
// handleCmt（pdulib 解码 + LittleFS 写入 + 日志输出）-> 上层回调
// （如邮件通知入队）。原 4KB 余量不足，一旦栈溢出抽数任务死亡，
// 表现为「短信既收不到也发不出」（状态机与响应收集都随之停摆）。
void SerialBridge::begin() {
  if (_histMutex) return;
  sInstance = this;
  for (uint8_t i = 0; i < 2; i++) _ports[i].mutex = xSemaphoreCreateMutex();
  _histMutex = xSemaphoreCreateMutex();
  // AT 历史环形缓冲：PSRAM 优先（容量放宽），失败退化内部堆（收紧条数），
  // 与设备日志 _log 同策略；分配就绪后再起抽数任务，避免历史写入竞态
  _hist.begin(cfg::kAtHistoryRingSize);
  xTaskCreatePinnedToCore(&SerialBridge::taskEntry, "uart-pump", 8192, this, 2,
                          &_task, 1);
  _log.addf(LogLevel::kInfo, "uart",
            "AT 历史缓冲就绪：%u 条 x %u B = %u B，位于 %s", (unsigned)_hist.max(),
            (unsigned)sizeof(AtHistory::Entry),
            (unsigned)(sizeof(AtHistory::Entry) * _hist.max()),
            _hist.psramOk() ? "PSRAM" : "内部堆");
  // 兜底说明：板子无 PSRAM 时属预期，有 PSRAM 却退化则说明 PSRAM 初始化失败
  // 或剩余空间不足，串口一眼可辨
  if (!_hist.psramOk()) {
    _log.addf(LogLevel::kWarn, "uart",
              "AT 历史 PSRAM 分配失败，已退化到内部堆（PSRAM %u B，剩余 %u B）",
              (unsigned)ESP.getPsramSize(), (unsigned)ESP.getFreePsram());
  }
}

// FreeRTOS 任务入口：arg 为 SerialBridge 实例指针
void SerialBridge::taskEntry(void *arg) {
  SerialBridge *self = static_cast<SerialBridge *>(arg);
  _log.addf(LogLevel::kInfo, "uart", "串口抽数任务已启动");
  for (;;) {
    self->pumpOnce();
    vTaskDelay(kPumpPeriod);
  }
}

// 由独立任务调用：把收到的自发上报拼成一条历史记录
// 逻辑：尽量抽干 RX，数据静默超过 kUnsolicitedGapMs 即视为一段结束。
// 关键：每端口独立 try-lock，故障/忙端口取不到锁就跳过本轮（绝不等待），
// 因此某个通信模组未连接或正在长交互时，抽数任务仍能照常服务另一通信模组，不阻塞。
void SerialBridge::pumpOnce() {
  for (uint8_t i = 0; i < 2; i++) {
    if (!tryLockPort(i)) continue;  // 端口忙（如另一通信模组正在 AT 交互）-> 跳过本轮
    Port &p = _ports[i];
    if (!p.opened) {
      unlockPort(i);
    } else {
      int c;
      while ((c = p.dev->read()) >= 0) {
        appendPrintable(p.pend, (char)c);
        p.rxBytes++;
        p.lastRxMs = millis();
        p.pendMs = millis();
      }
      if (p.pend.length() && millis() - p.pendMs >= kUnsolicitedGapMs) {
        // 取走数据后先释放端口锁，再做可能较慢的回调（文件 I/O / 短信重组），
        // 避免持端口锁阻塞同端口的后续操作
        String seg = p.pend;
        p.pend = String();
        p.pendMs = 0;
        unlockPort(i);
        bool consumed = _urc && _urc(i + 1, seg);
        if (!consumed) {
          lockHist();
          _hist.add(i + 1, String(), seg);  // cmd 为空 = 自发上报
          unlockHist();
        }
      } else {
        unlockPort(i);
      }
    }
    // 端口锁已释放：交由轮询回调（通信模组初始化/心跳状态机）用 trySend 驱动，
    // 非阻塞——端口忙则 trySend 直接跳过，绝不拖住抽数任务，也不影响另一通信模组。
    if (_poll) _poll(i + 1);

    // 接收错误合并上报：onReceiveError 回调（串口事件任务小栈上下文）只计数，
    // 打印在这里做——抽数任务栈充裕（8KB），且按节流合并，错误风暴时不刷屏
    reportErrors(i);
  }
}

// 把端口累计的接收错误按节流打成一条日志（抽数任务上下文调用，栈充裕）
// i：端口索引 0/1。未发生新错误或距上次上报不足 kErrReportMinGapMs 时静默跳过；
// 每次上报把自上次以来的新增错误数一次说清，避免高频错误时逐条刷屏
void SerialBridge::reportErrors(uint8_t i) {
  Port &p = _ports[i];
  uint32_t pending = p.errCount - p.errRepCount;
  if (pending == 0) return;
  uint32_t now = millis();
  if (now - p.errRepMs < kErrReportMinGapMs) return;
  p.errRepCount = p.errCount;
  p.errRepMs = now;
  _log.addf(LogLevel::kWarn, "uart",
            "串口 %u 接收错误 x%u，最近错误码 %d（累计 %u 次，已收 %u B，已发 %u B）",
            (unsigned)(p.index + 1), (unsigned)pending, (int)p.errCode,
            (unsigned)p.errCount, (unsigned)p.rxBytes, (unsigned)p.txBytes);
}

// 把 AT 响应收集窗口内混入的短信 URC（+CMT/+CMTI/+CDS）交回 pend：
// readResponse 按 80ms 静默判停，心跳/遥测 AT 执行期间到达的 URC 会被
// 一并读进 resp 而丢弃（quiet 时）或仅入 AT 历史，导致新短信丢失。
// 本方法持端口锁把 URC 段追加到 p.pend，由抽数任务按常规自发上报流程
// （120ms 静默后回调 _urc）归档；resp 本身不改动，调用方解析不受影响。
// p：端口状态（调用方须持有端口锁）；resp：刚收集到的 AT 响应
void SerialBridge::stashUrc(Port &p, const String &resp) {
  // 找响应中第一个短信类 URC 起始位置
  int idx = -1;
  for (int i = 0; i < (int)resp.length(); i++) {
    if (resp[i] != '+') continue;
    if (matchAt(resp, i, "+CMT:") || matchAt(resp, i, "+CMTI:") ||
        matchAt(resp, i, "+CDS:")) {
      idx = i;
      break;
    }
  }
  if (idx < 0) return;  // 无 URC 混入：零行为变化

  // 回退到本行行首，避免把上一行残留（如 OK\r\n）切进 URC 段
  int start = idx;
  while (start > 0 && resp[start - 1] != '\n') start--;
  String tail = resp.substring(start);
  if (!tail.length()) return;

  // 追加到 pend（端口锁保护，与 pumpOnce 的读写一致）；pendMs 置当前时刻，
  // 抽数任务在 120ms 静默后按自发上报归档 -> _urc -> SmsService 解码入库
  p.pend += tail;
  p.pendMs = millis();
  _log.addf(LogLevel::kDebug, "uart", "串口 %u 从 AT 窗口转存自发上报（%u 字符）",
            (unsigned)(p.index + 1), (unsigned)tail.length());
}

// 同步收集响应：直到超时或静默 gapMs
// p：端口状态；waitMs：总等待上限；gapMs：收到数据后的静默判停间隔
String SerialBridge::readResponse(Port &p, uint32_t waitMs, uint32_t gapMs) {
  String out;
  uint32_t start = millis(), last = start;
  while (millis() - start < waitMs) {
    int c = p.dev->read();
    if (c >= 0) {
      appendPrintable(out, (char)c);
      p.rxBytes++;
      p.lastRxMs = millis();
      last = millis();
    } else if (out.length() && millis() - last >= gapMs) {
      break;  // 已收到内容且再次静默，视为响应结束
    } else {
      delay(2);
    }
  }
  return out;
}

// 阻塞发送一条 AT 命令并等待响应，同时记入历史（quiet 时不记）
// port：串口号 1-2；baud：波特率；cmd：命令文本；eol：行尾符；timeoutMs：收集超时；
// quiet：true 时不记入 AT 历史
SerialBridge::Result SerialBridge::send(uint8_t port, uint32_t baud,
                                        const String &cmd, Eol eol,
                                        uint32_t timeoutMs, bool quiet) {
  if (port < 1 || port > 2) return Result{false, (uint32_t)-1, String()};

  lockPort(port - 1);
  Port &p = _ports[port - 1];
  ensureOpen(p, baud);

  size_t written = p.dev->write((const uint8_t *)cmd.c_str(), cmd.length());
  switch (eol) {
    case kCrlf: written += p.dev->write("\r\n", 2); break;
    case kCr: written += p.dev->write('\r'); break;
    case kLf: written += p.dev->write('\n'); break;
    default: break;  // kNone：不追加行尾
  }
  p.txBytes += written;
  p.dev->flush();

  String resp = readResponse(p, timeoutMs, 80);

  // 响应窗口内混入的短信 URC 交回 pend，避免被当作 AT 响应丢弃（新短信丢失）
  stashUrc(p, resp);

  // 自愈：连续多次无响应 -> 复位串口
  if (resp.length()) {
    p.timeouts = 0;
  } else if (++p.timeouts >= kTimeoutBeforeReset) {
    _log.addf(LogLevel::kWarn, "uart", "串口 %u 连续 %u 次无响应",
              (unsigned)(p.index + 1), (unsigned)p.timeouts);
    reopen(p, p.baud);
    p.timeouts = 0;
  }

  uint32_t id = 0;
  if (!quiet) {
    lockHist();
    _hist.add(port, cmd, resp);
    id = _hist.lastId();
    unlockHist();
  }
  unlockPort(port - 1);

  return Result{true, id, resp};
}

// 非阻塞发送：端口正忙（锁取不到）立即返回 false（out 无效）；否则取到端口锁、
// 完成 AT 交互后返回 true（out 有效，resp 为空表示超时无响应）。由抽数任务的
// 轮询回调调用，确保通信模组初始化/心跳的周期性 AT 永不阻塞抽数任务或主循环。
bool SerialBridge::trySend(uint8_t port, uint32_t baud, const String &cmd, Eol eol,
                           uint32_t timeoutMs, bool quiet, Result &out) {
  out = Result{false, (uint32_t)-1, String()};
  if (port < 1 || port > 2) return false;
  if (!tryLockPort(port - 1)) return false;  // 端口忙（如正在发送短信）-> 非阻塞跳过

  Port &p = _ports[port - 1];
  ensureOpen(p, baud);

  size_t written = p.dev->write((const uint8_t *)cmd.c_str(), cmd.length());
  switch (eol) {
    case kCrlf: written += p.dev->write("\r\n", 2); break;
    case kCr: written += p.dev->write('\r'); break;
    case kLf: written += p.dev->write('\n'); break;
    default: break;  // kNone：不追加行尾
  }
  p.txBytes += written;
  p.dev->flush();

  String resp = readResponse(p, timeoutMs, 80);

  // 响应窗口内混入的短信 URC 交回 pend，避免被当作 AT 响应丢弃（新短信丢失）
  stashUrc(p, resp);

  // 自愈：连续多次无响应 -> 复位串口（与 send 一致）
  if (resp.length()) {
    p.timeouts = 0;
  } else if (++p.timeouts >= kTimeoutBeforeReset) {
    _log.addf(LogLevel::kWarn, "uart", "串口 %u 连续 %u 次无响应",
              (unsigned)(p.index + 1), (unsigned)p.timeouts);
    reopen(p, p.baud);
    p.timeouts = 0;
  }

  uint32_t id = 0;
  if (!quiet) {
    lockHist();
    _hist.add(port, cmd, resp);
    id = _hist.lastId();
    unlockHist();
  }
  unlockPort(port - 1);
  out = Result{true, id, resp};
  return true;
}

// 短信发送专用：AT+CMGS=<len> 两段式握手（流程参照 esp32-sms-forwarding）。
// 与通用 send() 的区别：不用 80ms 静默判停（通信模组回包慢/分片时会误判超时），
// 改为逐字节持续收集，并以「+CMGS 已确认」作为成功条件之一——通信模组经常先回
// +CMGS:<ref> 再回 OK，个别通信模组甚至只回 +CMGS，按 OK 判定会把已发出的短信
// 误报为失败。
SerialBridge::Result SerialBridge::sendSmsPdu(uint8_t port, uint32_t baud,
                                              int tpduLen, const String &pduHex,
                                              String &err) {
  if (port < 1 || port > 2) {
    err = "invalid port";
    return Result{false, (uint32_t)-1, String()};
  }
  lockPort(port - 1);
  Port &p = _ports[port - 1];
  ensureOpen(p, baud);

  // 1) 抽干 RX：上一条命令/URC 的残留字节若留在缓冲，会混进本次握手响应，
  //    使 >/OK 判定失真。抽出的字节交回 pend（不丢弃）：其中若含刚到的新短信
  //    URC，抽数任务会按自发上报正常归档，不会顺手吞掉来信。
  String stale;
  for (int i = 0; i < 1024; i++) {
    int c = p.dev->read();
    if (c < 0) break;
    p.rxBytes++;
    appendPrintable(stale, (char)c);
  }
  if (stale.length()) {
    p.pend += stale;
    p.pendMs = millis();
  }
  p.timeouts = 0;

  // 2) 报长度，逐字节等 '>' 提示符（不用静默判停：'>' 前无其它内容可依赖）
  String cmd = "AT+CMGS=" + String(tpduLen);
  p.dev->write((const uint8_t *)cmd.c_str(), cmd.length());
  p.dev->write("\r\n", 2);
  p.txBytes += cmd.length() + 2;
  p.dev->flush();

  String pre;
  bool gotPrompt = false;
  uint32_t start = millis();
  while (millis() - start < cfg::kSmsCmgsPromptMs) {
    int c = p.dev->read();
    if (c >= 0) {
      p.rxBytes++;
      p.lastRxMs = millis();
      appendPrintable(pre, (char)c);
      if (c == '>') { gotPrompt = true; break; }
      if (pre.indexOf("ERROR") >= 0) break;  // 通信模组直接拒绝，不必等满超时
    } else {
      delay(2);  // 让出 CPU：本方法运行于 HTTP(async_tcp) 任务上下文，不能自旋
    }
  }
  if (!gotPrompt) {
    err = pre.length() ? ("通信模组未返回 > 提示（响应: " + pre + "）")
                       : "通信模组未返回 > 提示（无响应）";
    unlockPort(port - 1);
    return Result{true, 0, pre};
  }

  // 3) 发 PDU 十六进制（末尾已内嵌 0x1A，不再追加行尾），逐字节收集响应
  p.dev->write((const uint8_t *)pduHex.c_str(), pduHex.length());
  p.txBytes += pduHex.length();
  p.dev->flush();

  String resp;
  bool cmgs = false, error = false;
  uint32_t cmgsAt = 0;
  start = millis();
  while (millis() - start < cfg::kSmsSendTimeoutMs) {
    int c = p.dev->read();
    if (c >= 0) {
      p.rxBytes++;
      p.lastRxMs = millis();
      appendPrintable(resp, (char)c);
      if (!cmgs && resp.indexOf("+CMGS") >= 0) {
        cmgs = true;
        cmgsAt = millis();
      }
      if (resp.indexOf("OK") >= 0) break;               // 事务完成
      if (!cmgs && resp.indexOf("ERROR") >= 0) {        // 未见 +CMGS 先见 ERROR 才是真失败
        error = true;
        break;
      }
    } else {
      // +CMGS 已确认但 2s 内没等到 OK：通信模组已入队，按成功处理
      if (cmgs && millis() - cmgsAt >= 2000) break;
      delay(2);  // 让出 CPU（同上）
    }
  }
  unlockPort(port - 1);

  if (resp.indexOf("OK") >= 0 || (cmgs && !error)) {
    return Result{true, 0, resp};
  }
  err = error ? "通信模组返回 ERROR（检查号码/信号/SIM）"
              : (resp.length() ? ("发送超时（响应: " + resp + "）")
                               : "发送超时（通信模组无响应）");
  return Result{true, 0, resp};
}

// 手动复位串口：port 为 1-2，越界忽略
void SerialBridge::reset(uint8_t port) {
  if (port < 1 || port > 2) return;
  lockPort(port - 1);
  Port &p = _ports[port - 1];
  if (p.opened) reopen(p, p.baud);
  unlockPort(port - 1);
}

// 读取指定串口的统计快照；port 越界时返回全零结构
SerialBridge::Stats SerialBridge::stats(uint8_t port) {
  Stats s;
  if (port < 1 || port > 2) return s;
  lockPort(port - 1);
  const Port &p = _ports[port - 1];
  s.opened = p.opened;
  s.baud = p.baud;
  s.rx = p.rxBytes;
  s.tx = p.txBytes;
  s.errors = p.errCount;
  s.timeouts = p.timeouts;
  s.recovers = p.recovers;
  s.lastRxMs = p.lastRxMs ? millis() - p.lastRxMs : 0xFFFFFFFF;
  unlockPort(port - 1);
  return s;
}

// 汇总两个串口的统计为 JSON 数组字符串
String SerialBridge::statsJson() {
  String out = "[";
  for (uint8_t i = 0; i < 2; i++) {
    Stats s = stats(i + 1);
    if (i) out += ",";
    char buf[192];
    snprintf(buf, sizeof(buf),
             "{\"port\":%u,\"open\":%u,\"baud\":%u,\"rx\":%u,\"tx\":%u,\"err\":%u,"
             "\"timeouts\":%u,\"recovers\":%u,\"idle\":%lu}",
             (unsigned)(i + 1), s.opened ? 1 : 0, (unsigned)s.baud,
             (unsigned)s.rx, (unsigned)s.tx, (unsigned)s.errors,
             (unsigned)s.timeouts, (unsigned)s.recovers,
             (unsigned long)s.lastRxMs);
    out += buf;
  }
  out += "]";
  return out;
}

// 清空全部 AT 历史记录
void SerialBridge::clearHistory() {
  lockHist();
  _hist.clear();
  unlockHist();
}

// 导出历史 JSON；afterId 之前的记录忽略，limit 限制条数（0 不限）
String SerialBridge::historyJson(int32_t afterId, size_t limit) {
  lockHist();
  String json = _hist.toJson(afterId, limit);
  unlockHist();
  return json;
}

// 按 ID 导出单条记录 JSON，不存在返回 "{}"
String SerialBridge::entryJson(uint32_t id) {
  lockHist();
  String json = _hist.entryJson(id);
  unlockHist();
  return json;
}

// 返回最新一条历史记录的 ID
uint32_t SerialBridge::lastHistoryId() {
  lockHist();
  uint32_t id = _hist.lastId();
  unlockHist();
  return id;
}

// AT 历史存储统计（PSRAM 落点 / 容量 / 占用），供 /api/at/stats 使用
String SerialBridge::historyStats() {
  lockHist();
  String json = _hist.statsJson();
  unlockHist();
  return json;
}

// 取最近一条记录内容；port/cmd/resp 为出参，找到返回 true
bool SerialBridge::lastEntry(uint8_t &port, String &cmd, String &resp) {
  lockHist();
  const AtHistory::Entry *e = _hist.get(_hist.lastId());
  bool found = e != nullptr;
  if (found) {
    port = e->port;
    cmd = e->cmd;
    resp = e->resp;
  }
  unlockHist();
  return found;
}
