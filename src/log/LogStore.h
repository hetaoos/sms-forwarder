#pragma once

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include <functional>
#include <stddef.h>
#include <stdint.h>
#include <vector>

#include "app/Config.h"

// 日志级别（数值即严重度；查询时按「不低于该级别」过滤）
enum class LogLevel : uint8_t {
  kDebug = 0,  // 调试信息（默认不产出，供后续需要时使用）
  kInfo = 1,   // 常规事件（启动、连接、收发、状态变更...）
  kWarn = 2,   // 警告（可恢复的异常，如单次发送失败）
  kError = 3,  // 错误（功能失败，如存储不可用）
};

// 设备日志环形存储：条目缓冲区一次性分配在 PSRAM 的定长记录环，
// 供 Web 控制台分页查看（GET /api/logs）；同时每条记录同步镜像一行到串口，
// 因此全工程只需要这一个日志出口（各模块不再直接 Serial.printX）。设计要点：
//   0. 串口镜像：add()/addf() 在入环之外固定向 Serial 输出一行
//      （[id][级别][标签] 正文）；环形缓冲未就绪（begin 之前）或分配失败
//      降级时仍然输出，保证启动期与降级期的日志不会凭空消失；
//   1. PSRAM 驻留：与 WiFi/LwIP/SSL 等内部堆使用者隔离，不争抢 DRAM；
//      板子无 PSRAM（或分配失败）时自动退化为内部堆分配，功能不变
//      （psramOk() 可查询实际落点）；
//   2. 定长条目：tag/msg 为内嵌字符数组（超长截断），写入只做 memcpy，
//      运行期零堆分配、无碎片，抽数任务 / HTTP 任务等任意上下文可安全调用；
//   3. FIFO 淘汰：写满后覆盖最旧条目；ID 自增不回绕，clear() 也只做逻辑
//      清空（保留 ID 计数），前端可据此区分清空前后的新旧记录；
//   4. 并发安全：互斥锁保护写入与查询；查询只在锁内收集命中的 ID 与
//      当前页条目副本，持锁时间与页大小成正比。
class LogStore {
 public:
  // 一条日志记录（定长结构，按值拷贝；约 228B：id/ts/level + 标签 + 正文）
  struct Entry {
    uint32_t id = 0;   // 自增 ID（从 1 起，不回绕）
    uint32_t ts = 0;   // 时间戳：纪元秒（时钟已同步）或开机秒数
    uint8_t level = 0;  // LogLevel
    char tag[cfg::kLogTagMax] = {0};  // 模块标签（"app"/"wifi"/"sms"/"sim"...）
    char msg[cfg::kLogMsgMax] = {0};  // 日志正文（超长截断）
  };

  // 时间戳供给类型（与短信记录一致）：已同步返回纪元秒，否则开机秒数。
  // 未注入时退化为开机秒数（millis()/1000）
  using TimeProviderFn = std::function<uint32_t()>;
  void setTimeProvider(TimeProviderFn fn) { _timeFn = fn; }

  // 分配环形缓冲：max 为条数上限（0 或越界取 cfg::kLogRingSize）。
  // PSRAM 优先（按上限给足）；失败退化为内部堆并按 cfg::kLogRingSizeNoPsram
  // 收紧条数；两者都分配失败则不入环（仍逐条输出到串口）
  void begin(uint16_t max);

  // 追加一条日志（level：级别；tag：模块标签；msg：正文；均超长自动截断）
  // 除入环外还向串口镜像一行；环形缓冲不可用时仅输出串口
  void add(LogLevel level, const char *tag, const char *msg);

  // String 重载（便利接口，转发到上面的 const char* 版本）
  void add(LogLevel level, const String &tag, const String &msg) {
    add(level, tag.c_str(), msg.c_str());
  }

  // printf 风格追加一条日志（自动截断到 kLogMsgMax-1）
  // level：级别；tag：模块标签；fmt：格式串
  void addf(LogLevel level, const char *tag, const char *fmt, ...)
      __attribute__((format(printf, 4, 5)));

  // 分页查询（与短信列表同约定）：page 从 1 起、pageSize 越界自动钳制；
  // level < 0 不过滤级别，否则只返回「不低于该级别」的记录；
  // q 为空不筛关键字，否则对 tag/msg 做大小写不敏感的子串匹配；
  // out 按 ID 倒序（最新在前）填充本页记录副本，total 为匹配总数
  void list(int page, int pageSize, int8_t level, const String &q,
            std::vector<Entry> &out, int &total);

  // 增量拉取（轮询追加用）：after 之后（不含）的匹配记录，按 ID 升序
  // （旧 -> 新，便于前端顺序追加），至多 limit 条；过滤条件与 list() 一致
  // （level/q）。after 之前已被淘汰的记录不再返回。
  // out：新记录（升序）；total：匹配总数（供前端同步计数，可大于 out.size()）
  void tail(uint32_t after, size_t limit, int8_t level, const String &q,
            std::vector<Entry> &out, int &total);

  // 当前条数 / 容量上限 / 缓冲是否位于 PSRAM
  uint32_t size() const { return _count; }
  uint16_t max() const { return _max; }
  bool psramOk() const { return _psram; }

  // 最新记录的 ID（无记录返回 0；SSE / 增量拉取的基线用）
  uint32_t lastId() const { return _count == 0 ? 0 : _nextId - 1; }

  // 是否为空
  bool empty() const { return _count == 0; }

  // 逻辑清空（保留 ID 计数不回退）
  void clear();

  // 统计 JSON：
  // {"ok":true,"total":n,"max":n,"psram":bool,"bytes":n,
  //  "psramTotal":n,"psramFree":n}
  // psram=false 表示缓冲未落在 PSRAM（无 PSRAM 或分配失败，已退化内部堆）；
  // 此时 psramTotal/psramFree 可区分两种原因（0 = 设备无 PSRAM）
  String statsJson() const;

 private:
  // 取/放互斥锁（保护环形缓冲与计数；add 可能来自抽数/HTTP 等多任务上下文）
  // _mutex 为空（begin() 之前）时空转：此时只做串口输出，不落缓冲
  void lock() const { if (_mutex) xSemaphoreTake(_mutex, portMAX_DELAY); }
  void unlock() const { if (_mutex) xSemaphoreGive(_mutex); }

  Entry *_buf = nullptr;   // 环形缓冲（PSRAM 优先，begin 时整块分配）
  uint16_t _max = 0;       // 容量上限（条）
  uint32_t _count = 0;     // 当前条数
  uint32_t _nextId = 1;    // 下一条记录的 ID（最旧者 = _nextId - _count）
  bool _psram = false;     // 缓冲是否成功分配到 PSRAM
  TimeProviderFn _timeFn;  // 时间戳供给（未注入时用开机秒数）
  mutable SemaphoreHandle_t _mutex = nullptr;  // 保护写入与查询
};

// 日志 sink：注册后每条日志在 add() 末尾回调（用于 MQTT 实时推送等）。
// 回调在持锁外的串口镜像之后执行，运行于调用 add() 的任意任务上下文；
// 参数 ts 为日志时间戳（纪元秒或开机秒数）。回调须自行保证线程安全与轻量
// （如直接 esp_mqtt enqueue，内部队列已做同步）。
using LogSinkFn = void (*)(uint8_t level, const char *tag, const char *msg, uint32_t ts);
void setLogSink(LogSinkFn fn);

// 全局日志实例访问器：全工程唯一的日志出口，各模块与 App 取到的是同一份。
// 用于不便显式注入依赖的上下文（串口抽数任务回调、命名空间级函数、静态辅助
// 等）；由 App::begin() 里的 _log.begin() 完成初始化，此前调用只输出到串口。
// 各模块惯例：在 .cpp 的匿名命名空间里取一个引用别名
//   namespace { LogStore &_log = devLog(); }
// 之后统一用 _log.addf(LogLevel::kXxx, "tag", "...") 记日志。
LogStore &devLog();
