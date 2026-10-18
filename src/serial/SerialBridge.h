#pragma once

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
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

  // 阻塞式：写命令后在 timeoutMs 内收集响应
  // port：串口号 1-2；baud：波特率；cmd：命令文本；eol：行尾符；timeoutMs：收集超时
  Result send(uint8_t port, uint32_t baud, const String &cmd, Eol eol,
              uint32_t timeoutMs);

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
    String pend;            // 自发上报的未成段数据（等间隔超时后归档）
    uint32_t pendMs = 0;    // pend 最后一次变化的时间戳
    uint8_t index = 0;      // 逻辑序号 0/1（日志用，显示为 index+1）
  };

  // FreeRTOS 任务入口，arg 指向 SerialBridge 实例
  static void taskEntry(void *arg);

  // 抽数一轮：搬运接收数据，间隔超时的自发上报归档为历史
  void pumpOnce();

  // 确保串口已打开；波特率变化时热更新
  void ensureOpen(Port &p, uint32_t baud);

  // 关闭再重开串口：自愈/手动复位用
  void reopen(Port &p, uint32_t baud);

  // 在 waitMs 内持续读响应，静默 gapMs 后提前结束
  String readResponse(Port &p, uint32_t waitMs, uint32_t gapMs);

  // 取/放成员互斥锁（临界区保护）
  void lock() { xSemaphoreTake(_mutex, portMAX_DELAY); }
  void unlock() { xSemaphoreGive(_mutex); }

  // 把字符追加为可打印形式（控制字符转为 \xNN）
  static void appendPrintable(String &out, char c);

  // 串口错误中断的静态跳板（当前 onReceiveError 用 lambda 直接捕获 Port，此跳板作为无捕获场景的备用）
  static void onErrorThunk(hardwareSerial_error_t err);

  // 两个串口的状态（索引 0 -> 串口1，索引 1 -> 串口2）
  Port _ports[2] = {
      {&Serial1, cfg::kUart1Rx, cfg::kUart1Tx, 0, false, 0, 0, 0, 0, 0, 0,
       String(), 0, 0},
      {&Serial2, cfg::kUart2Rx, cfg::kUart2Tx, 0, false, 0, 0, 0, 0, 0, 0,
       String(), 0, 1},
  };
  static SerialBridge *sInstance;       // 单例指针，供静态中断回调使用
  AtHistory _hist;                      // AT 历史环形缓冲
  SemaphoreHandle_t _mutex = nullptr;   // 保护共享状态的互斥量
  TaskHandle_t _task = nullptr;         // 抽数任务句柄
};
