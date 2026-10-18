#include "serial/SerialBridge.h"

// 静态成员：单例指针，供中断/静态回调转发使用
SerialBridge *SerialBridge::sInstance = nullptr;

namespace {
constexpr uint32_t kRxBufferSize = 4096;   // 串口接收缓冲（加大防溢出）
constexpr uint32_t kTxBufferSize = 1024;   // 串口发送缓冲
constexpr uint8_t kRxFifoThrhd = 32;       // 触发中断的 FIFO 阈值，越小越不容易溢出
constexpr uint32_t kUnsolicitedGapMs = 120;  // 自发上报分段的静默间隔
constexpr TickType_t kPumpPeriod = pdMS_TO_TICKS(5);  // 抽数任务周期 5ms
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
    p.errCount++;
    Serial.printf("[uart%u] err=%d rx=%u tx=%u\r\n", p.index + 1, (int)err,
                  (unsigned)p.rxBytes, (unsigned)p.txBytes);
  });
  p.baud = baud;
  p.opened = true;
  Serial.printf("[uart%u] open baud=%u\r\n", p.index + 1, (unsigned)baud);
}

// 释放并重开串口：用于自愈
// p：端口状态；baud：重开时使用的波特率
void SerialBridge::reopen(Port &p, uint32_t baud) {
  Serial.printf("[uart%u] recover: reopening at %u\r\n", p.index + 1,
                (unsigned)baud);
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
void SerialBridge::begin() {
  if (_histMutex) return;
  sInstance = this;
  for (uint8_t i = 0; i < 2; i++) _ports[i].mutex = xSemaphoreCreateMutex();
  _histMutex = xSemaphoreCreateMutex();
  xTaskCreatePinnedToCore(&SerialBridge::taskEntry, "uart-pump", 4096, this, 2,
                          &_task, 1);
}

// FreeRTOS 任务入口：arg 为 SerialBridge 实例指针
void SerialBridge::taskEntry(void *arg) {
  SerialBridge *self = static_cast<SerialBridge *>(arg);
  Serial.println("[uart] pump task started");
  for (;;) {
    self->pumpOnce();
    vTaskDelay(kPumpPeriod);
  }
}

// 由独立任务调用：把收到的自发上报拼成一条历史记录
// 逻辑：尽量抽干 RX，数据静默超过 kUnsolicitedGapMs 即视为一段结束。
// 关键：每端口独立 try-lock，故障/忙端口取不到锁就跳过本轮（绝不等待），
// 因此某个模组未连接或正在长交互时，抽数任务仍能照常服务另一模组，不阻塞。
void SerialBridge::pumpOnce() {
  for (uint8_t i = 0; i < 2; i++) {
    if (!tryLockPort(i)) continue;  // 端口忙（如另一模组正在 AT 交互）-> 跳过本轮
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
    // 端口锁已释放：交由轮询回调（模组初始化/心跳状态机）用 trySend 驱动，
    // 非阻塞——端口忙则 trySend 直接跳过，绝不拖住抽数任务，也不影响另一模组。
    if (_poll) _poll(i + 1);
  }
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

  // 自愈：连续多次无响应 -> 复位串口
  if (resp.length()) {
    p.timeouts = 0;
  } else if (++p.timeouts >= kTimeoutBeforeReset) {
    Serial.printf("[uart%u] %lu consecutive timeouts\r\n", p.index + 1,
                  (unsigned long)p.timeouts);
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
// 轮询回调调用，确保模组初始化/心跳的周期性 AT 永不阻塞抽数任务或主循环。
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

  // 自愈：连续多次无响应 -> 复位串口（与 send 一致）
  if (resp.length()) {
    p.timeouts = 0;
  } else if (++p.timeouts >= kTimeoutBeforeReset) {
    Serial.printf("[uart%u] %lu consecutive timeouts\r\n", p.index + 1,
                  (unsigned long)p.timeouts);
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
