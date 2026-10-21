#include "serial/AtHistory.h"

#include <ArduinoJson.h>
#include <esp_heap_caps.h>
#include <string.h>

namespace {

// 安全拷贝定长字段：超长截断并保证 '\0' 结尾
// dst：目标缓冲区；cap：容量（含 '\0'）；src：源串（可为空/空指针）
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

}  // namespace

// max：条数上限（0 或越界取 cfg::kAtHistoryRingSize）。
// PSRAM 优先分配（与内部堆使用者隔离）；失败退化为内部堆，并按
// kAtHistoryRingSizeNoPsram 收紧条数；两者都仍失败则退回编译期静态缓冲
// （BSS，必定可用，容量取最小档）——保证 AT 历史至少能记录最近若干条、
// 存储信息块不显示空，行为与原先的静态数组一致。
void AtHistory::begin(uint16_t max) {
  if (max == 0 || max > cfg::kAtHistoryRingSize) max = cfg::kAtHistoryRingSize;
  _max = max;

  _buf = (Entry *)heap_caps_calloc(_max, sizeof(Entry), MALLOC_CAP_SPIRAM);
  _psram = _buf != nullptr;
  if (!_buf) {
    // 无 PSRAM 或 PSRAM 分配失败：退化到内部堆，功能不变，只保留最近若干条
    if (_max > cfg::kAtHistoryRingSizeNoPsram) _max = cfg::kAtHistoryRingSizeNoPsram;
    _buf = (Entry *)calloc(_max, sizeof(Entry));
  }
  if (!_buf) {
    // 仍失败（极少见：DRAM 极度紧张）：退回编译期静态缓冲（BSS，必定可用），
    // 容量取最小档，避免缓冲为空导致存储信息块显示空、AT 历史被静默丢弃
    static Entry sFallback[cfg::kAtHistoryRingSizeNoPsram];
    _max = cfg::kAtHistoryRingSizeNoPsram;
    _buf = sFallback;
  }
}

// port：来源串口号；cmd：命令（自发上报传空串）；resp：响应内容
// 缓冲区未就绪（begin 之前或分配失败）：静默丢弃，不影响串口交互
void AtHistory::add(uint8_t port, const String &cmd, const String &resp) {
  if (!_buf) return;
  uint32_t id = _end++;
  Entry &e = _buf[id % _max];
  e.id = id;
  e.port = port;
  copyTrunc(e.cmd, sizeof(e.cmd), cmd.c_str());
  copyTrunc(e.resp, sizeof(e.resp), resp.c_str());
  if (_end - _start > _max) _start = _end - _max;
}

// id：目标记录 ID，越界或已被淘汰时返回 nullptr
const AtHistory::Entry *AtHistory::get(uint32_t id) const {
  if (!_buf || id < _start || id >= _end) return nullptr;
  return &_buf[id % _max];
}

// id：目标记录 ID；返回该记录的 JSON 对象，不存在时返回 "{}"
String AtHistory::entryJson(uint32_t id) const {
  const Entry *e = get(id);
  if (!e) return String("{}");   // 未找到记录时返回空对象 "{}"
  JsonDocument doc;
  doc["id"] = e->id;
  doc["port"] = e->port;
  doc["cmd"] = e->cmd;   // 直接赋值，ArduinoJson 自动转义特殊字符
  doc["resp"] = e->resp;
  String out;
  serializeJson(doc, out);
  return out;
}

// 逻辑清空：把区间起点推到末尾，旧 ID 仍不可见
void AtHistory::clear() { _start = _end; }

// afterId：只导出该 ID 之后的记录，-1 表示最早一条起；
// limit：最多条数，0 表示导出全部
String AtHistory::toJson(int32_t afterId, size_t limit) const {
  uint32_t from = afterId < 0 ? _start : (uint32_t)(afterId + 1);
  if (from < _start) from = _start;

  JsonDocument doc;
  JsonArray arr = doc.to<JsonArray>();
  for (uint32_t id = from; _buf && id < _end && (limit == 0 || arr.size() < limit);
       id++) {
    const Entry *e = get(id);
    if (!e) continue;
    JsonObject o = arr.add<JsonObject>();
    o["id"] = e->id;
    o["port"] = e->port;
    o["cmd"] = e->cmd;   // 自动转义
    o["resp"] = e->resp;
  }
  String out;
  serializeJson(doc, out);
  return out;
}

String AtHistory::statsJson() const {
  JsonDocument doc;
  if (!_buf) {
    doc["ok"] = false;
    doc["error"] = "at history not ready";
  } else {
    doc["ok"] = true;
    doc["total"] = (unsigned)(_end - _start);
    doc["max"] = _max;
    doc["psram"] = _psram;
    doc["bytes"] = (unsigned)(sizeof(Entry) * _max);
    doc["psramTotal"] = (unsigned)ESP.getPsramSize();
    doc["psramFree"] = (unsigned)ESP.getFreePsram();
  }
  String out;
  serializeJson(doc, out);
  return out;
}
