#include "serial/AtHistory.h"

namespace {

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

// port：来源串口号；cmd：命令内容（自发上报传空串）；resp：响应内容
// 缓冲区写满时淘汰最旧记录
void AtHistory::add(uint8_t port, const String &cmd, const String &resp) {
  uint32_t id = _end++;
  _buf[id % kMax] = Entry{id, port, cmd, resp};
  if (_end - _start > kMax) _start = _end - kMax;
}

// id：目标记录 ID，越界或已被淘汰时返回 nullptr
const AtHistory::Entry *AtHistory::get(uint32_t id) const {
  if (id < _start || id >= _end) return nullptr;
  return &_buf[id % kMax];
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
  for (uint32_t id = from; id < _end && (limit == 0 || n < limit); id++) {
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
