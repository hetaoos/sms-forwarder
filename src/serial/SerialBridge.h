#pragma once

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <functional>
#include <stdint.h>

#include "app/Config.h"
#include "serial/AtHistory.h"

// 双串口 AT 透传：专用任务持续抽数据 + 命令/响应同步模型
class SerialBridge {
 public:
  // 命令行尾符类型
  enum Eol {
    kCrlf = 0,  // \r\n
    kCr = 1,    // \r
    kLf = 2,    // \n
    kNone = 3,  // 不追加行尾
  };

  // send() 的返回结果
  struct Result {
    bool ok;         // 命令是否被受理执行
    uint32_t id;     // 对应历史记录 ID（失败为 (uint32_t)-1）
    String resp;     // 收集到的响应文本
  };

  // 单串口运行统计
  struct Stats {
    bool opened = false;      // 串口是否已打开
    uint32_t baud = 0;        // 当前波特率
    uint32_t rx = 0;          // 累计接收字节数
    uint32_t tx = 0;          // 累计发送字节数
    uint32_t errors = 0;      // 接收错误中断计数
    uint32_t timeouts = 0;    // 连续无响应次数（达到阈值自动复位）
    uint32_t recovers = 0;    // 串口自愈复位次数
    uint32_t lastRxMs = 0;    // 距上次收到数据的毫秒数（从未收到为 0xFFFFFFFF）
  };

  // 连续无响应达到该次数时自动复位该串口
  static constexpr uint32_t kTimeoutBeforeReset = 3;

  // 创建互斥量并启动串口抽数任务（重复调用无副作用）
  void begin();

  // 注入时间戳供给（转发给 AT 历史环形缓冲）：已同步返回纪元秒，否则开机秒数。
  // 须在 begin() 之后调用，使串口日志输出带上时间（与短信/运行日志一致）。
  void setTimeProvider(std::function<uint32_t()> fn) { _hist.setTimeProvider(fn); }

  // 阻塞式：写命令后在 timeoutMs 内收集响应，必定记入 AT 历史（含后台命令）。
  // port：串口号 1-2；baud：波特率；cmd：命令文本；eol：行尾符；timeoutMs：收集超时；
  // type：来源类型，默认 kCmd（Web 控制台用户命令）；后台/内部流量传 kBg。
  Result send(uint8_t port, uint32_t baud, const String &cmd, Eol eol,
              uint32_t timeoutMs, AtHistory::Type type = AtHistory::Type::kCmd);

  // 非阻塞式（try-lock）：若端口正忙（如正在发送短信）立即返回 false（out 无效），
  // 不阻塞抽数任务。成功取到端口锁返回 true（out 有效，resp 为空表示超时无响应）。
  // 由抽数任务在每端口处理完后调用，用于通信模组初始化/心跳等周期性 AT 交互。
  // type：来源类型（后台/内部流量统一传 kBg），必定记入 AT 历史。
  bool trySend(uint8_t port, uint32_t baud, const String &cmd, Eol eol,
               uint32_t timeoutMs, AtHistory::Type type, Result &out);

  // 短信发送专用：AT+CMGS=<len> 两段式握手（全程持端口锁，流程参照
  // esp32-sms-forwarding 的成熟实现）：
  //   1) 先抽干 RX 缓冲（残留 URC 碎片会污染 >/OK 判定；抽出的字节交回 pend，
  //      不丢弃，避免顺带吞掉刚到的新短信 URC）
  //   2) 发 AT+CMGS=<len>，逐字节等 '>' 提示符（上限 kSmsCmgsPromptMs）
  //   3) 发 PDU 十六进制（末尾已内嵌 0x1A），逐字节收集响应（上限 kSmsSendTimeoutMs）：
  //      见 OK 即成功；未见 +CMGS 先见 ERROR 即失败；+CMGS 已确认但 2s 内无 OK
  //      也算成功（通信模组已入队，OK 可能迟至或不到）
  // 返回 Result{true, 0, resp}（ok=false 仅表示 port 越界）；成功与否看 err 是否为空
  Result sendSmsPdu(uint8_t port, uint32_t baud, int tpduLen, const String &pduHex,
                    String &err);

  // 未请求上报（URC）消费回调：抽数任务每归档一段通信模组自发数据前调用，
  // 返回 true 表示已被消费（不再记入 AT 历史）。
  // 注意：该回调在抽数任务上下文调用，不持任何桥锁（端口锁已先释放），
  // 因此可安全地做文件 I/O / 短信重组；仍不得回调本类会再次取端口锁的方法。
  // 短信 +CMT 等实时 URC 经此接入。
  using UnsolicitedFn = std::function<bool(uint8_t port, const String &text)>;
  void setUnsolicitedHandler(UnsolicitedFn fn) { _urc = fn; }

  // 每端口处理完后的周期性轮询回调（不持锁）：通信模组状态机用它驱动初始化/心跳。
  // 回调内应只用 trySend（非阻塞），以免拖住抽数任务。
  using PollFn = std::function<void(uint8_t port)>;
  void setPollHandler(PollFn fn) { _poll = fn; }

  // 手动复位指定串口（关闭后按当前波特率重开）
  void reset(uint8_t port);

  // 清空全部 AT 历史
  void clearHistory();

  // 读取指定串口的统计信息
  Stats stats(uint8_t port);

  // 两个串口统计的 JSON 数组
  String statsJson();

  // 导出历史记录 JSON 数组；afterId 之前的忽略，limit 限制条数（0 不限）
  String historyJson(int32_t afterId, size_t limit);

  // 按 ID 导出单条记录 JSON
  String entryJson(uint32_t id);

  // 最新历史记录 ID
  uint32_t lastHistoryId();

  // AT 历史存储统计 JSON（PSRAM/容量等，与 /api/logs/stats 对称）
  String historyStats();

  // 取最近一条记录；有则通过引用返回内容并返回 true
  // port/cmd/resp：出参，分别写回串口号/命令/响应
  bool lastEntry(uint8_t &port, String &cmd, String &resp);

 private:
  // 单个串口的运行状态与统计
  struct Port {
    HardwareSerial *dev;    // 硬件串口对象（Serial1/Serial2）
    int8_t rx, tx;          // RX / TX 引脚号
    uint32_t baud = 0;      // 当前波特率
    bool opened = false;    // 是否已打开
    uint32_t rxBytes = 0, txBytes = 0;      // 收发字节计数
    uint32_t errCount = 0, timeouts = 0, recovers = 0;  // 错误/超时/复位计数
    uint32_t lastRxMs = 0;  // 上次收到数据的时间戳
    // 接收错误（onReceiveError）上报节流：回调只做计数，日志由抽数任务合并打印
    uint8_t errCode = 0;        // 最近一次错误码（hardwareSerial_error_t）
    uint32_t errRepCount = 0;   // 已上报的错误数（errCount - errRepCount = 待上报）
    uint32_t errRepMs = 0;      // 上次上报时刻（millis）
    String pend;            // 自发上报的未成段数据（等间隔超时后归档）
    uint32_t pendMs = 0;    // pend 最后一次变化的时间戳
    uint8_t index = 0;      // 逻辑序号 0/1（日志用，显示为 index+1）
    SemaphoreHandle_t mutex = nullptr;     // 每端口独立锁（避免故障通信模组阻塞另一通信模组）
  };

  // FreeRTOS 任务入口，arg 指向 SerialBridge 实例
  static void taskEntry(void *arg);

  // 抽数一轮：搬运接收数据，间隔超时的自发上报归档为历史
  void pumpOnce();

  // 确保串口已打开；波特率变化时热更新
  void ensureOpen(Port &p, uint32_t baud);

  // 关闭再重开串口：自愈/手动复位用
  void reopen(Port &p, uint32_t baud);

  // 在 waitMs 内持续读响应：见到标准终止行（OK / ERROR / +CMS ERROR 等）即结束，
  // 否则静默 gapMs 后提前结束（兼容不回终止行的模组）
  String readResponse(Port &p, uint32_t waitMs, uint32_t gapMs);

  // 下发命令前抽干 RX 残留（调用方须持有端口锁）：残留字节交回 pend 由抽数任务
  // 按自发上报归档，避免上一条命令的尾部（PDU / OK）混进本次响应造成解析串包
  void drainRxLocked(Port &p);

  // ---- 锁助手：每端口锁 + 历史锁，互不嵌套（先端口锁后历史锁）----
  // 取/放端口锁（阻塞）
  void lockPort(uint8_t i) { xSemaphoreTake(_ports[i].mutex, portMAX_DELAY); }
  void unlockPort(uint8_t i) { xSemaphoreGive(_ports[i].mutex); }
  // 尝试取端口锁（不阻塞）：成功返回 true
  bool tryLockPort(uint8_t i) { return xSemaphoreTake(_ports[i].mutex, 0) == pdTRUE; }
  // 取/放 AT 历史锁。句柄 NULL（begin() 之前被调用）时直接放过而不是把
  // xQueueSemaphoreTake 喂 NULL 句柄——那会触发 FreeRTOS 断言直接重启
  void lockHist() { if (_histMutex) xSemaphoreTake(_histMutex, portMAX_DELAY); }
  void unlockHist() { if (_histMutex) xSemaphoreGive(_histMutex); }

  // 把字符追加为可打印形式（控制字符转为 \xNN）
  static void appendPrintable(String &out, char c);

  // 把 AT 响应收集窗口内混入的短信 URC（+CMT/+CMTI/+CDS）交回 pend：
  // 响应收集窗口内到达的 URC 会被一并读进 resp；本方法把该段从响应里捞出、
  // 追加到 p.pend，由抽数任务按常规
  // 自发上报流程（120ms 静默后回调 _urc 并记入 AT 历史为 kUrc）归档，避免被当作
  // 本次 AT 响应的一部分而干扰调用方解析、或随响应窗口丢失。resp 本身不改动。
  void stashUrc(Port &p, const String &resp);

  // 串口错误中断的静态跳板（当前 onReceiveError 用 lambda 直接捕获 Port，此跳板作为无捕获场景的备用）
  static void onErrorThunk(hardwareSerial_error_t err);

  // 把端口累计的接收错误按节流打成一条日志（抽数任务上下文调用，栈充裕）。
  // onReceiveError 回调运行在串口事件任务（仅 2~4KB 栈）里只负责计数，
  // 打印合并到这里，避免高频错误时回调内 printf 链路把事件任务栈打爆
  void reportErrors(uint8_t i);

  // 两个模组的串口状态（索引 0 -> 模组1，索引 1 -> 模组2）
  Port _ports[2] = {
      {&Serial1, cfg::kUart1Rx, cfg::kUart1Tx, 0, false, 0, 0, 0, 0, 0, 0, 0, 0, 0,
       String(), 0, 0, nullptr},
      {&Serial2, cfg::kUart2Rx, cfg::kUart2Tx, 0, false, 0, 0, 0, 0, 0, 0, 0, 0, 0,
       String(), 0, 1, nullptr},
  };
  static SerialBridge *sInstance;       // 单例指针，供静态中断回调使用
  UnsolicitedFn _urc;                   // URC consumer callback (pump task context)
  PollFn _poll;                         // 每端口周期轮询回调（初始化/心跳，非阻塞）
  AtHistory _hist;                      // AT 历史环形缓冲
  SemaphoreHandle_t _histMutex = nullptr;  // 保护 AT 历史（独立于端口锁）
  TaskHandle_t _task = nullptr;         // 抽数任务句柄
};
