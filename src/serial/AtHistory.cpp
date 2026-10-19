#include "serial/AtHistory.h"

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

// 转义 JSON 字符串中的特殊字符
// s：原始字符串；返回可直接放进双引号内的安全内容
String escape(const String &s) {
  String out;
  out.reserve(s.length() + 8);
  for (size_t i = 0; i < s.length(); i++) {
    char c = s[i];
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\r': out += "\\r"; break;
      case '\n': out += "\\n"; break;
      case '\t': out += "\\t"; break;
      default:
        if ((uint8_t)c < 0x20) {  // 其余控制字符按 \u00XX 转义
          char tmp[8];
          snprintf(tmp, sizeof(tmp), "\\u%04x", c);
          out += tmp;
        } else {
          out += c;
        }
    }
  }
  return out;
}

}  // namespace

// max：条数上限（0 或越界取 cfg::kAtHistoryRingSize）。
// PSRAM 优先分配（与内部堆使用者隔离）；失败退化为内部堆，并按
// kAtHistoryRingSizeNoPsram 收紧条数；两者都失败则缓冲不可用（仍不报错，
// 串口交互本身不受影响，只是历史不可回溯）。calloc 已把字段清零为 '\0'。
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
  if (!_buf) _max = 0;  // 两者都失败：缓冲不可用，max 归零便于页面/日志识别
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
  if (!e) return String("{}");
  return "{\"id\":" + String(e->id) + ",\"port\":" + String(e->port) +
         ",\"cmd\":\"" + escape(e->cmd) + "\",\"resp\":\"" + escape(e->resp) +
         "\"}";
}

// 逻辑清空：把区间起点推到末尾，旧 ID 仍不可见
void AtHistory::clear() { _start = _end; }

// afterId：只导出该 ID 之后的记录，-1 表示最早一条起；
// limit：最多条数，0 表示导出全部
String AtHistory::toJson(int32_t afterId, size_t limit) const {
  uint32_t from = afterId < 0 ? _start : (uint32_t)(afterId + 1);
  if (from < _start) from = _start;

  String out = "[";
  size_t n = 0;
  for (uint32_t id = from; _buf && id < _end && (limit == 0 || n < limit); id++) {
    const Entry *e = get(id);
    if (!e) continue;
    if (n++) out += ",";  // 第二条起补逗号
    out += "{\"id\":" + String(e->id) + ",\"port\":" + String(e->port) +
           ",\"cmd\":\"" + escape(e->cmd) + "\",\"resp\":\"" + escape(e->resp) +
           "\"}";
  }
  out += "]";
  return out;
}

String AtHistory::statsJson() const {
  if (!_buf) return "{\"ok\":false,\"error\":\"at history not ready\"}";
  String js = String("{\"ok\":true,\"total\":") + (unsigned)(_end - _start) +
              ",\"max\":" + _max +
              ",\"psram\":" + (_psram ? "true" : "false") +
              ",\"bytes\":" + (unsigned)(sizeof(Entry) * _max) +
              ",\"psramTotal\":" + (unsigned)ESP.getPsramSize() +
              ",\"psramFree\":" + (unsigned)ESP.getFreePsram() + "}";
  return js;
}
