#include "log/LogStore.h"

#include <esp_heap_caps.h>

#include <ctype.h>
#include <stdarg.h>
#include <string.h>

#include <Arduino.h>

namespace {

// 安全拷贝定长字段：超长截断并保证 '\0' 结尾
// dst：目标缓冲区；cap：容量（含 '\0'）；src：源串（可为空）
void copyTrunc(char *dst, size_t cap, const char *src) {
  if (!src) {
    dst[0] = '\0';
    return;
  }
  size_t n = strlen(src);
  if (n > cap - 1) n = cap - 1;
  memcpy(dst, src, n);
  dst[n] = '\0';
}

// 级别 -> 固定宽度短名（串口镜像行用，便于肉眼扫描）
const char *levelName(uint8_t level) {
  switch ((LogLevel)level) {
    case LogLevel::kDebug: return "DEBUG";
    case LogLevel::kWarn:  return "WARN ";
    case LogLevel::kError: return "ERROR";
    default:               return "INFO ";
  }
}

// 大小写不敏感的子串查找；needle 为空视为命中
// hay：被查找串（'\0' 结尾）；needle：关键字（已转为小写）
bool containsCI(const char *hay, const char *needle) {
  if (!hay || !needle) return false;
  size_t nl = strlen(needle);
  if (nl == 0) return true;
  for (const char *p = hay; *p; p++) {
    size_t i = 0;
    // p[i] 到 hay 末尾即停（*p 链保证不越界），全部匹配上则命中
    while (i < nl && p[i] &&
           tolower((unsigned char)p[i]) == tolower((unsigned char)needle[i])) {
      i++;
    }
    if (i == nl) return true;
  }
  return false;
}

}  // namespace

// 全局唯一实例：函数内静态对象，首次访问时构造，
// 规避跨编译单元的静态初始化顺序问题（各模块的 _log 别名可能在 App 之前初始化）
LogStore &devLog() {
  static LogStore inst;
  return inst;
}

// 日志 sink 全局钩子：默认空（不推送）。MqttService 在 begin() 里注册，
// 把运行日志实时 enqueue 到 MQTT Broker。
static LogSinkFn g_logSink = nullptr;
void setLogSink(LogSinkFn fn) { g_logSink = fn; }

// max：条数上限（0 或越界取 cfg::kLogRingSize）。
// PSRAM 优先分配（与短信/HTTP 等 DRAM 使用者隔离，容量按上限给足）；
// 失败退化为内部堆，并按 kLogRingSizeNoPsram 收紧条数，避免挤占 DRAM
void LogStore::begin(uint16_t max) {
  _mutex = xSemaphoreCreateMutex();
  if (max == 0 || max > cfg::kLogRingSize) max = cfg::kLogRingSize;
  _max = max;
  _count = 0;
  _nextId = 1;

  _buf = (Entry *)heap_caps_calloc(_max, sizeof(Entry), MALLOC_CAP_SPIRAM);
  _psram = _buf != nullptr;
  if (!_buf) {
    // 无 PSRAM 或 PSRAM 分配失败：退化到内部堆，功能不变，只保留最近若干条
    if (_max > cfg::kLogRingSizeNoPsram) _max = cfg::kLogRingSizeNoPsram;
    _buf = (Entry *)calloc(_max, sizeof(Entry));
  }
  if (!_buf) {
    _max = 0;
    addf(LogLevel::kError, "log", "环形缓冲分配失败，日志仅输出到串口");
    return;
  }
  addf(LogLevel::kInfo, "log", "日志环就绪：%u 条 x %u B = %u B，位于 %s",
       (unsigned)_max, (unsigned)sizeof(Entry),
       (unsigned)(sizeof(Entry) * _max), _psram ? "PSRAM" : "内部堆");
  // 兜底说明：板子无 PSRAM 时属预期，有 PSRAM 却退化则说明 PSRAM 初始化失败
  // 或剩余空间不足（8MB 板型上通常是前者），串口一眼可辨
  if (!_psram) {
    addf(LogLevel::kWarn, "log",
         "PSRAM 分配失败，已退化到内部堆（PSRAM %u B，剩余 %u B）",
         (unsigned)ESP.getPsramSize(), (unsigned)ESP.getFreePsram());
  }
}

// level：级别；tag：模块标签；msg：正文（均可超长截断）
void LogStore::add(LogLevel level, const char *tag, const char *msg) {
  uint32_t id = 0;
  lock();
  if (_buf) {  // 缓冲不可用（begin 之前或分配失败）：只做下面的串口输出
    id = _nextId++;
    Entry &e = _buf[id % _max];
    e.id = id;
    e.ts = _timeFn ? _timeFn() : (uint32_t)(millis() / 1000);
    e.level = (uint8_t)level;
    copyTrunc(e.tag, sizeof(e.tag), tag);
    copyTrunc(e.msg, sizeof(e.msg), msg);
    if (_count < _max) _count++;
  }
  unlock();
  // 串口镜像：放在锁外，避免串口拥塞时把日志互斥锁连同调用方一起拖住。
  // 并发下最坏情况只是两行交错，不影响环形缓冲的一致性。
  Serial.printf("[%u][%s][%s] %s\r\n", (unsigned)id, levelName((uint8_t)level),
                tag ? tag : "", msg ? msg : "");
  // 日志 sink：注册的消费方（如 MQTT）在每条日志后拿到结构化字段；
  // 在锁外调用，回调自身负责线程安全与轻量化（不可再回调 add 造成递归）。
  if (g_logSink) g_logSink((uint8_t)level, tag ? tag : "", msg ? msg : "",
                           _timeFn ? _timeFn() : (uint32_t)(millis() / 1000));
}

// level：级别；tag：模块标签；fmt：格式串（自动截断到 kLogMsgMax-1）
void LogStore::addf(LogLevel level, const char *tag, const char *fmt, ...) {
  if (!fmt) return;
  char msg[cfg::kLogMsgMax];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(msg, sizeof(msg), fmt, ap);
  va_end(ap);
  add(level, tag, msg);
}

// page：页码（1 起）；pageSize：每页条数（越界钳制）；
// level：最低级别过滤（<0 不过滤）；q：关键字（空不筛，匹配 tag/msg）
void LogStore::list(int page, int pageSize, int8_t level, const String &q,
                    std::vector<Entry> &out, int &total) {
  out.clear();
  total = 0;
  if (!_buf) return;
  if (page < 1) page = 1;
  if (pageSize < 1) pageSize = 20;
  if (pageSize > cfg::kLogPageSizeMax) pageSize = cfg::kLogPageSizeMax;

  String ql = q;
  ql.toLowerCase();
  const char *needle = ql.length() ? ql.c_str() : nullptr;

  lock();
  if (_count == 0) {
    unlock();
    return;
  }
  // 最新在前（ID 倒序）扫描：先只收集命中的 ID，再按页复制条目副本，
  // 避免为整环匹配结果临时拷贝（定长条目约 228B，拷贝成本远高于 4B ID）
  std::vector<uint32_t> ids;
  ids.reserve(_count);
  uint32_t oldest = _nextId - _count;
  for (uint32_t id = _nextId; id-- > oldest;) {
    const Entry &e = _buf[id % _max];
    if (level >= 0 && e.level < (uint8_t)level) continue;
    if (needle && !containsCI(e.tag, needle) && !containsCI(e.msg, needle)) {
      continue;
    }
    ids.push_back(id);
  }
  total = (int)ids.size();

  int start = (page - 1) * pageSize;
  if (start < total) {
    int end = start + pageSize;
    if (end > total) end = total;
    out.reserve(end - start);
    for (int i = start; i < end; i++) {
      out.push_back(_buf[ids[i] % _max]);
    }
  }
  unlock();
}

// after：起始 ID（不含）；limit：最多条数（越界钳制）；
// level：最低级别过滤（<0 不过滤）；q：关键字（空不筛，匹配 tag/msg）
// 扫描区间为 [max(after+1, 现存最旧), 最新]，一次遍历同时计数与取页，
// 保证 out（升序）与 total 来自同一快照（新记录可能在两次调用间插入，无影响）
void LogStore::tail(uint32_t after, size_t limit, int8_t level, const String &q,
                    std::vector<Entry> &out, int &total) {
  out.clear();
  total = 0;
  if (!_buf) return;
  if (limit == 0) limit = 1;
  if (limit > cfg::kLogPageSizeMax) limit = cfg::kLogPageSizeMax;

  String ql = q;
  ql.toLowerCase();
  const char *needle = ql.length() ? ql.c_str() : nullptr;

  lock();
  if (_count == 0) {
    unlock();
    return;
  }
  uint32_t oldest = _nextId - _count;
  uint32_t from = (after == 0xFFFFFFFFu) ? 0xFFFFFFFFu : after + 1;  // 溢出保护
  if (from < oldest) from = oldest;   // 该段已被淘汰：从现存最旧开始

  for (uint32_t id = from; id < _nextId; id++) {
    const Entry &e = _buf[id % _max];
    if (level >= 0 && e.level < (uint8_t)level) continue;
    if (needle && !containsCI(e.tag, needle) && !containsCI(e.msg, needle)) {
      continue;
    }
    total++;
    if (out.size() < limit) out.push_back(e);
  }
  unlock();
}

// 逻辑清空：_count 归零即全部不可见；保留 _nextId 不回退
void LogStore::clear() {
  lock();
  _count = 0;
  unlock();
}

String LogStore::statsJson() const {
  if (!_buf) return "{\"ok\":false,\"error\":\"log ring not ready\"}";
  lock();
  // psramTotal/psramFree：设备 PSRAM 容量与剩余（页面据此区分「无 PSRAM」与
  // 「有 PSRAM 但分配失败」，而不是只显示一句落点结论）
  String js = String("{\"ok\":true,\"total\":") + _count + ",\"max\":" + _max +
              ",\"psram\":" + (_psram ? "true" : "false") +
              ",\"bytes\":" + (unsigned)(sizeof(Entry) * _max) +
              ",\"psramTotal\":" + (unsigned)ESP.getPsramSize() +
              ",\"psramFree\":" + (unsigned)ESP.getFreePsram() + "}";
  unlock();
  return js;
}
