#include "sms/BlackList.h"

#include <Preferences.h>
#include <string.h>

#include <Arduino.h>

#include "log/LogStore.h"   // devLog() 全局日志出口

namespace {

// 本模块日志出口
LogStore &_log = devLog();

// 取数字位掩码：mode 只在 0..2 合法；越界回落精确
uint8_t clampMode(int m) {
  return (m >= 0 && m <= 2) ? (uint8_t)m : (uint8_t)BlackList::kExact;
}

}  // namespace

void BlackList::begin() {
  _mutex = xSemaphoreCreateMutex();
  loadConfig();
}

/* ---------------- 归一化与匹配 ---------------- */

String BlackList::normalize(const String &s) {
  String o;
  o.reserve(s.length());
  for (size_t i = 0; i < s.length(); i++) {
    char c = s[i];
    if (c >= '0' && c <= '9') o += c;
    else if (c == '+') o += c;          // 仅保留前导 +，作为国际号标识
    // 其余分隔符（空格 - ( ) . / 等）全部丢弃
  }
  return o;
}

bool BlackList::wildcardMatch(const char *pat, const char *txt) {
  // 经典双指针通配符匹配（* 多字符 / ? 单字符），输入均为归一化串（数字与 +）。
  // 非递归，不会栈溢出；时间由调用方限制（pattern 最大 32 字符，且 * 过多时直接判不命中）。
  const char *p = pat, *t = txt;
  const char *star = nullptr, *mark = nullptr;
  while (*t) {
    if (*p == '?' || *p == *t) {
      p++; t++;
    } else if (*p == '*') {
      star = p++; mark = t;
    } else if (star) {
      p = star + 1; t = ++mark;
    } else {
      return false;
    }
  }
  while (*p == '*') p++;   // 尾部 * 可匹配空
  return *p == '\0';
}

bool BlackList::matchEntry(const String &normTarget, const Entry &e) const {
  // 生成待匹配的目标候选集：原始 + 去掉前导 + + 去掉 +86（兼容「存本地号拦 +86 来号」）
  // 这样用户既可用 +86138... 也可只用 138... 录入，均可命中。
  if (e.pattern.length() == 0) return false;
  String np = normalize(e.pattern);
  if (np.length() == 0) return false;

  if (normTarget.length() == 0) return false;

  // 候选目标：完整、去 + 、去 +86
  const char *t = normTarget.c_str();
  const char *cands[3];
  cands[0] = t;
  static String s1, s2;   // 复用静态缓冲，避免临时分配（调用频率低）
  s1 = (normTarget.startsWith("+")) ? normTarget.substring(1) : "";
  s2 = (normTarget.startsWith("+86")) ? normTarget.substring(3) : "";
  cands[1] = s1.length() ? s1.c_str() : nullptr;
  cands[2] = s2.length() ? s2.c_str() : nullptr;

  for (int i = 0; i < 3; i++) {
    const char *cand = cands[i];
    if (!cand) continue;
    switch (e.mode) {
      case kExact:
        if (np == cand) return true;
        break;
      case kPrefix:
        if (strlen(cand) >= np.length() && strncmp(cand, np.c_str(), np.length()) == 0)
          return true;
        break;
      case kWildcard: {
        // 限制 * 数量，避免极端规则在短信接收路径上回溯过久
        int stars = 0;
        for (const char *q = np.c_str(); *q; ++q) if (*q == '*') ++stars;
        if (stars <= 8 && wildcardMatch(np.c_str(), cand)) return true;
        break;
      }
      default:
        break;
    }
  }
  return false;
}

bool BlackList::testMatch(const String &target, uint8_t scope) {
  String norm = normalize(target);
  lock();
  bool hit = false;
  _lastNote = "";
  for (uint8_t i = 0; i < _count; i++) {
    const Entry &e = _entries[i];
    if (!e.enabled) continue;
    if (!(e.scope & scope)) continue;          // 作用域不符（如来电规则不拦短信）
    if (matchEntry(norm, e)) {
      hit = true;
      _lastNote = e.note;
      break;
    }
  }
  unlock();
  return hit;
}

bool BlackList::match(const String &target, uint8_t scope) {
  if (!_enabled || _count == 0) return false;
  return testMatch(target, scope);
}

/* ---------------- 配置读写 ---------------- */

String BlackList::configJson() const {
  JsonDocument doc;
  doc["ok"] = true;
  doc["enabled"] = _enabled;
  doc["action"] = (uint8_t)_action;
  doc["count"] = _count;
  JsonArray arr = doc["list"].to<JsonArray>();
  lock();
  for (uint8_t i = 0; i < _count; i++) {
    JsonObject o = arr.add<JsonObject>();
    o["pattern"] = _entries[i].pattern;
    o["mode"] = _entries[i].mode;
    o["scope"] = _entries[i].scope;
    o["note"] = _entries[i].note;
    o["enabled"] = _entries[i].enabled;
  }
  unlock();
  String out;
  serializeJson(doc, out);
  return out;
}

bool BlackList::saveFromJson(const JsonObject &o) {
  // 总开关与动作（缺省保持原值）
  bool newEnabled = _enabled;
  Action newAction = _action;
  if (o["enabled"].is<bool>()) newEnabled = o["enabled"].as<bool>();
  if (o["action"].is<int>()) {
    int a = o["action"].as<int>();
    if (a == (int)kDrop) newAction = kDrop;
    else if (a == (int)kRecord) newAction = kRecord;
  }

  // 规则列表：校验 + 去重（pattern+mode+scope 三元组唯一）
  Entry buf[kMax];
  uint8_t n = 0;
  if (o["list"].is<JsonArray>()) {
    for (JsonObject e : o["list"].as<JsonArray>()) {
      if (n >= kMax) break;
      String pat = e["pattern"] | "";
      pat.trim();
      String np = normalize(pat);
      if (np.length() == 0) continue;              // 空号码跳过
      uint8_t mode = clampMode(e["mode"] | 0);
      int scope = e["scope"] | (int)kSms;
      if (scope <= 0) scope = (int)kSms;           // 作用域不可为 0
      scope &= (int)(kSms | kCall);                // 仅保留已知位
      if (scope == 0) scope = (int)kSms;
      // 去重：同 (np, mode, scope) 只保留第一条
      bool dup = false;
      for (uint8_t k = 0; k < n; k++) {
        if (normalize(buf[k].pattern) == np && buf[k].mode == mode &&
            buf[k].scope == (uint8_t)scope) { dup = true; break; }
      }
      if (dup) continue;
      buf[n].pattern = pat;
      buf[n].mode = mode;
      buf[n].scope = (uint8_t)scope;
      buf[n].note = e["note"] | "";
      buf[n].enabled = e["enabled"] | true;
      n++;
    }
  }

  // 落盘
  lock();
  _enabled = newEnabled;
  _action = newAction;
  _count = n;
  for (uint8_t i = 0; i < n; i++) _entries[i] = buf[i];
  unlock();
  return saveConfig();
}

bool BlackList::saveConfig() {
  JsonDocument doc;
  doc["ver"] = 1;
  doc["enabled"] = _enabled;
  doc["action"] = (uint8_t)_action;
  JsonArray arr = doc["list"].to<JsonArray>();
  for (uint8_t i = 0; i < _count; i++) {
    JsonObject o = arr.add<JsonObject>();
    o["pattern"] = _entries[i].pattern;
    o["mode"] = _entries[i].mode;
    o["scope"] = _entries[i].scope;
    o["note"] = _entries[i].note;
    o["enabled"] = _entries[i].enabled;
  }
  String out;
  serializeJson(doc, out);

  Preferences prefs;
  if (!prefs.begin(cfg::kNvsNamespace, false)) return false;
  size_t written = prefs.putString(cfg::kNvsKeyBlacklist, out);
  prefs.end();
  return written == out.length();
}

bool BlackList::loadConfig() {
  Preferences prefs;
  String json;
  if (prefs.begin(cfg::kNvsNamespace, true)) {
    json = prefs.getString(cfg::kNvsKeyBlacklist, "");
    prefs.end();
  }
  if (!json.length()) {
    // 无记录：默认总开关关、动作=仅记录、空表（不写盘，等首次保存）
    _enabled = false;
    _action = kRecord;
    _count = 0;
    return false;
  }
  JsonDocument doc;
  if (deserializeJson(doc, json) != DeserializationError::Ok) return false;
  lock();
  _enabled = doc["enabled"] | false;
  _action = (doc["action"] | 0) == (int)kDrop ? kDrop : kRecord;
  _count = 0;
  if (doc["list"].is<JsonArray>()) {
    for (JsonObject e : doc["list"].as<JsonArray>()) {
      if (_count >= kMax) break;
      String pat = e["pattern"] | "";
      if (normalize(pat).length() == 0) continue;
      uint8_t mode = clampMode(e["mode"] | 0);
      int scope = e["scope"] | (int)kSms;
      scope &= (int)(kSms | kCall);
      if (scope == 0) scope = (int)kSms;
      _entries[_count].pattern = pat;
      _entries[_count].mode = mode;
      _entries[_count].scope = (uint8_t)scope;
      _entries[_count].note = e["note"] | "";
      _entries[_count].enabled = e["enabled"] | true;
      _count++;
    }
  }
  unlock();
  _log.addf(LogLevel::kInfo, "black", "黑名单已载入：%u 条，总开关=%s",
            (unsigned)_count, _enabled ? "开" : "关");
  return true;
}

void BlackList::exportTo(JsonObject &o) const {
  lock();
  o["enabled"] = _enabled;
  o["action"] = (uint8_t)_action;
  JsonArray arr = o["list"].to<JsonArray>();
  for (uint8_t i = 0; i < _count; i++) {
    JsonObject e = arr.add<JsonObject>();
    e["pattern"] = _entries[i].pattern;
    e["mode"] = _entries[i].mode;
    e["scope"] = _entries[i].scope;
    e["note"] = _entries[i].note;
    e["enabled"] = _entries[i].enabled;
  }
  unlock();
}

bool BlackList::importFrom(const JsonObject &o) {
  // 缺省字段保持当前内存态（沿用 saveFromJson 的合并语义）
  JsonDocument tmp;
  JsonObject wrap = tmp.to<JsonObject>();
  wrap["enabled"] = o["enabled"] | _enabled;
  // action：导入以 0/1 表示；缺失则保持
  if (o["action"].is<int>()) wrap["action"] = o["action"].as<int>();
  else wrap["action"] = (uint8_t)_action;
  JsonArray dst = wrap["list"].to<JsonArray>();
  if (o["list"].is<JsonArray>()) {
    for (JsonObject e : o["list"].as<JsonArray>()) {
      JsonObject d = dst.add<JsonObject>();
      d["pattern"] = e["pattern"] | "";
      d["mode"] = e["mode"] | 0;
      d["scope"] = e["scope"] | (int)kSms;
      d["note"] = e["note"] | "";
      d["enabled"] = e["enabled"] | true;
    }
  } else {
    // 未提供 list：沿用当前规则
    for (uint8_t i = 0; i < _count; i++) {
      JsonObject d = dst.add<JsonObject>();
      d["pattern"] = _entries[i].pattern;
      d["mode"] = _entries[i].mode;
      d["scope"] = _entries[i].scope;
      d["note"] = _entries[i].note;
      d["enabled"] = _entries[i].enabled;
    }
  }
  if (!saveFromJson(wrap)) {
    _log.addf(LogLevel::kError, "black", "黑名单配置导入落盘失败");
    return false;
  }
  _log.addf(LogLevel::kInfo, "black", "已从备份导入黑名单配置");
  return true;
}
