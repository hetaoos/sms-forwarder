#include "push/PushService.h"
#include "log/LogStore.h"

#include <WiFi.h>
#include <Preferences.h>
#include <time.h>

// 设备日志（指向全局唯一实例，与 EmailService 一致）
static LogStore &_log = devLog();

// 递归互斥锁守卫（配置/队列/重试表的读写可能嵌套加锁）
struct Guard {
  SemaphoreHandle_t m;
  explicit Guard(SemaphoreHandle_t m_) : m(m_) {
    // 允许 NULL 句柄：模块 begin() 前（如开机首帧状态采集）也会读到配置，
    // 直接 xSemaphoreTakeRecursive(NULL) 会触发断言并无限重启
    if (m) xSemaphoreTakeRecursive(m, portMAX_DELAY);
  }
  ~Guard() { if (m) xSemaphoreGiveRecursive(m); }
};

// 将控制字符（0x00-0x1F，保留 \t \n \r \b \f）替换为 \uXXXX 字面量，防止
// ArduinoJson 将原始控制字节写入 JSON（与参考实现一致）。
static String sanitizeText(const String& s) {
  String out; out.reserve(s.length());
  for (unsigned int i = 0; i < s.length(); i++) {
    unsigned char c = (unsigned char)s.charAt(i);
    if (c < 0x20 && c != '\t' && c != '\n' && c != '\r' && c != '\b' && c != '\f') {
      char buf[7];
      snprintf(buf, sizeof(buf), "\\u%04X", c);
      out += buf;
    } else {
      out += (char)c;
    }
  }
  return out;
}

// 全部通道均为 HTTP 类，依赖 WiFi 上网
static bool isHttpPush(PushType t) {
  return t >= PUSH_TYPE_POST_JSON && t <= PUSH_TYPE_MAILGUN && t != PUSH_TYPE_NONE;
}

// ---------------- 标识 / 时间 ----------------
String PushService::deviceId() {
  uint64_t mac = ESP.getEfuseMac() >> 24;  // 高 48 位为 MAC
  char buf[13];
  snprintf(buf, sizeof(buf), "%012llX", mac);
  return String(buf);
}

String PushService::deviceName() {
  return "SMS-Forwarder-" + deviceId();
}

String PushService::localDateStr() {
  time_t now = time(nullptr);
  if (now < 100000) return "";  // 未同步
  struct tm t;
  localtime_r(&now, &t);
  char buf[32];
  strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &t);
  return String(buf);
}

// ---------------- 生命周期 ----------------
void PushService::begin() {
  _mutex = xSemaphoreCreateRecursiveMutex();
  loadConfig();
  _log.addf(LogLevel::kInfo, "push", "推送服务就绪（总开关%s，通道数 %d，策略 %s）",
            _enabled ? "开" : "关", _count,
            _strategy == PUSH_STRATEGY_FAILOVER ? "故障转移" : "广播");
}

void PushService::notifySms(const String& sender, const String& message,
                           uint32_t ts, int8_t modem) {
  {
    Guard g(_mutex);
    if (!_enabled) return;
    if (_count == 0) return;
  }
  PushJob job;
  job.sender = sender;
  job.message = message;
  job.timestamp = String(ts);
  job.modem = modem;
  job.type = MSG_TYPE_SMS;
  {
    Guard g(_mutex);
    enqueueJobLocked(job);
  }
}

bool PushService::requestTest() {
  {
    Guard g(_mutex);
    if (!_enabled) return false;
    if (_count == 0) return false;
  }
  PushJob job;
  job.sender = "测试号码";
  job.message = "这是一条来自 SMS-Forwarder 的测试推送消息。";
  job.timestamp = String((uint32_t)time(nullptr));
  job.modem = -1;
  job.type = MSG_TYPE_SMS;
  {
    Guard g(_mutex);
    enqueueJobLocked(job);
  }
  return true;
}

// ---------------- 主循环 ----------------
void PushService::tick() {
  // 先处理重试表（到期项），再处理主队列（每 tick 至多一条，避免长时间阻塞 loop）
  uint32_t now = millis();
  if (_retryCount > 0 && WiFi.status() == WL_CONNECTED && (int32_t)(now - _nextRetryMs) >= 0) {
    RetryItem item;
    bool have = false;
    {
      Guard g(_mutex);
      // 取一个到期的重试项
      for (uint8_t i = 0; i < _retryCount; i++) {
        if ((int32_t)(now - _retry[i].nextAt) >= 0) {
          item = _retry[i];
          for (uint8_t j = i + 1; j < _retryCount; j++) _retry[j - 1] = _retry[j];
          _retryCount--;
          have = true;
          break;
        }
      }
    }
    if (have) {
      if (item.channelIdx >= 0) {
        bool ok = executeChannel(item.channelIdx, item.job);
        if (!ok && item.attempts < kMaxAttempts) {
          item.attempts++;
          item.nextAt = millis() + (kRetryBackoffBase << (item.attempts - 1));
          Guard g(_mutex);
          enqueueRetryLocked(item);
        }
      } else {
        executeChain(item.job);  // 整链重试
      }
      _nextRetryMs = millis() + HTTP_COOLDOWN_MS;
      return;  // 本 tick 只处理一个重试/主任务
    }
  }

  // 主队列
  PushJob job;
  {
    Guard g(_mutex);
    if (_queueCount == 0) return;
    job = _queue[0];
    for (uint8_t i = 1; i < _queueCount; i++) _queue[i - 1] = _queue[i];
    _queueCount--;
  }
  executeChain(job);
}

// ---------------- 策略引擎 ----------------
void PushService::executeChain(const PushJob& job) {
  // 先持锁快照配置，随后释放，避免 HTTP 发送期间长期持锁阻塞 Web 保存 / 回调
  PushChannel snap[MAX_PUSH_CHANNELS];
  int  snapCount;
  PushStrategy snapStrategy;
  {
    Guard g(_mutex);
    if (!_enabled || _count == 0) return;
    snapCount = _count;
    snapStrategy = _strategy;
    for (int i = 0; i < _count; i++) snap[i] = _channels[i];
  }

  _log.addf(LogLevel::kInfo, "push", "=== 开始多通道推送（策略=%s，通道数=%d）===",
            snapStrategy == PUSH_STRATEGY_FAILOVER ? "故障转移" : "广播", snapCount);

  bool wifiOk = (WiFi.status() == WL_CONNECTED);
  bool anyAction = false;

  // 故障转移：收集失败通道，仅在整链全部失败时才入队重试。
  int  failoverRetry[MAX_PUSH_CHANNELS];
  int  failoverRetryCount = 0;
  bool failoverChainDone = false;

  String messageType = MsgTypeInfo(job.type, job.modem).toString();
  MessageContext ctx = buildMsgContext(job, messageType);

  for (int i = 0; i < snapCount; i++) {
    const PushChannel& ch = snap[i];
    if (!pushChannelValid(ch)) continue;
    if (isHttpPush(ch.type) && !wifiOk) {
      _log.addf(LogLevel::kInfo, "push", "WiFi未连接，跳过HTTP通道: %s", ch.name.c_str());
      continue;
    }

    anyAction = true;
    String name = ch.name.length() > 0 ? ch.name : ("通道" + String(i + 1));
    _log.addf(LogLevel::kInfo, "push", "发送到推送通道: %s", name.c_str());

    ctx.channelName = ch.name;
    ctx.channelType = pushTypeLabel(ch.type);
    String renderedBody = renderChannelBody(ch, ctx);

    bool ok = _sendOneChannel(ch, ctx, job.sender, job.message, renderedBody, job.timestamp);

    if (snapStrategy == PUSH_STRATEGY_FAILOVER) {
      if (ok) {
        _log.addf(LogLevel::kInfo, "push", "故障转移：通道 %s 成功，停止", name.c_str());
        failoverChainDone = true;
        break;
      }
      _log.addf(LogLevel::kInfo, "push", "故障转移：通道 %s 失败，继续下一个", name.c_str());
      if (ch.retryOnFail && failoverRetryCount < MAX_PUSH_CHANNELS) {
        failoverRetry[failoverRetryCount++] = i;
      }
    } else {
      // 广播模式：继续所有通道
      if (!ok && ch.retryOnFail) {
        RetryItem it; it.channelIdx = i; it.job = job;
        enqueueRetryLocked(it);
        _log.addf(LogLevel::kInfo, "push", "通道 %s 失败，已加入重试队列", name.c_str());
      }
    }
  }

  // 故障转移：只有整链全部失败时才入队重试
  if (snapStrategy == PUSH_STRATEGY_FAILOVER && !failoverChainDone) {
    for (int j = 0; j < failoverRetryCount; j++) {
      RetryItem it; it.channelIdx = failoverRetry[j]; it.job = job;
      enqueueRetryLocked(it);
      _log.addf(LogLevel::kInfo, "push", "故障转移链全部失败，通道 %s 加入重试队列",
                snap[failoverRetry[j]].name.c_str());
    }
    // 整链全部失败且无任何可重试通道：整链重试一次（退避）
    if (failoverRetryCount == 0) {
      RetryItem it; it.channelIdx = -1; it.job = job;
      enqueueRetryLocked(it);
    }
  }

  if (!anyAction) {
    _log.addf(LogLevel::kInfo, "push", "未配置任何有效推送通道，跳过推送");
  }
  _log.addf(LogLevel::kInfo, "push", "=== 多通道推送完成 ===");
}

bool PushService::executeChannel(int channelIdx, const PushJob& job) {
  // 先持锁快照该通道，释放后再发送
  PushChannel ch;
  {
    Guard g(_mutex);
    if (channelIdx < 0 || channelIdx >= _count) return false;
    ch = _channels[channelIdx];
  }
  if (!pushChannelValid(ch)) return false;
  if (isHttpPush(ch.type) && WiFi.status() != WL_CONNECTED) return false;

  String messageType = MsgTypeInfo(job.type, job.modem).toString();
  MessageContext ctx = buildMsgContext(job, messageType);
  ctx.channelName = ch.name;
  ctx.channelType = pushTypeLabel(ch.type);
  String renderedBody = renderChannelBody(ch, ctx);
  return _sendOneChannel(ch, ctx, job.sender, job.message, renderedBody, job.timestamp);
}

// ---------------- 单通道分发 ----------------
bool PushService::_sendOneChannel(const PushChannel& ch, const MessageContext& ctx,
                                  const String& sender, const String& message,
                                  const String& renderedBody, const String& timestamp) {
  // 在通道副本中渲染 key1/key2/key5（不修改原通道配置）
  PushChannel rendered = ch;
  rendered.key1 = MsgContext::render(ch.key1, ctx);
  rendered.key2 = MsgContext::render(ch.key2, ctx);
  rendered.key5 = MsgContext::render(ch.key5, ctx);

  // 集中判断：有自定义 body 时用 CUSTOM，否则用原始短信内容；两者均净化。
  PushBody body;
  if (renderedBody.length() > 0) {
    body = { PUSH_BODY_CUSTOM,  sanitizeText(renderedBody), sanitizeText(message) };
  } else {
    body = { PUSH_BODY_DEFAULT, sanitizeText(message),      sanitizeText(message) };
  }

  bool ok = false;
  switch (rendered.type) {
    case PUSH_TYPE_POST_JSON:   ok = PushChannels::sendPostJson(rendered, sender, body, timestamp);    break;
    case PUSH_TYPE_BARK:        ok = PushChannels::sendBark(rendered, sender, body, timestamp);         break;
    case PUSH_TYPE_GET:         ok = PushChannels::sendGet(rendered, sender, body, timestamp);          break;
    case PUSH_TYPE_DINGTALK:    ok = PushChannels::sendDingtalk(rendered, sender, body, timestamp);     break;
    case PUSH_TYPE_PUSHPLUS:    ok = PushChannels::sendPushPlus(rendered, sender, body, timestamp);     break;
    case PUSH_TYPE_SERVERCHAN:  ok = PushChannels::sendServerChan(rendered, sender, body, timestamp);   break;
    case PUSH_TYPE_CUSTOM:      ok = PushChannels::sendCustom(rendered, sender, body, timestamp);       break;
    case PUSH_TYPE_FEISHU:      ok = PushChannels::sendFeishu(rendered, sender, body, timestamp);       break;
    case PUSH_TYPE_GOTIFY:      ok = PushChannels::sendGotify(rendered, sender, body, timestamp);       break;
    case PUSH_TYPE_TELEGRAM:    ok = PushChannels::sendTelegram(rendered, sender, body, timestamp);     break;
    case PUSH_TYPE_WECHAT_WORK: ok = PushChannels::sendWechatWork(rendered, sender, body, timestamp);   break;
    case PUSH_TYPE_MAILGUN:     ok = PushChannels::sendMailgun(rendered, sender, body, timestamp, ctx.simSlot); break;
    default:
      _log.addf(LogLevel::kWarn, "push", "未知推送类型: %d", (int)rendered.type);
      break;
  }
  return ok;
}

MessageContext PushService::buildMsgContext(const PushJob& job, const String& messageType) {
  MessageContext ctx;
  ctx.from        = job.sender;
  ctx.message     = job.message;
  ctx.timestamp   = job.timestamp;
  ctx.date        = localDateStr();
  ctx.deviceId    = deviceId();
  ctx.deviceName  = deviceName();
  ctx.uptime      = MsgContext::formatUptime(millis());
  ctx.remark      = "";           // 本框架暂不配置设备备注
  ctx.messageType = messageType;
  ctx.simSlot     = (job.modem >= 0) ? ("SIM" + String(job.modem + 1)) : "";
  ctx.modemIndex  = (job.modem >= 0) ? String(job.modem + 1) : "";
  return ctx;
}

String PushService::renderChannelBody(const PushChannel& ch, const MessageContext& ctx) {
  if (ch.customBody.length() == 0) return "";
  return ch.type == PUSH_TYPE_MAILGUN ? MsgContext::renderHtml(ch.customBody, ctx)
                                      : MsgContext::render(ch.customBody, ctx);
}

// ---------------- 队列入队（持锁）----------------
void PushService::enqueueJobLocked(const PushJob& job) {
  if (_queueCount >= kQueueSize) {
    // 满时丢弃最旧
    for (uint8_t i = 1; i < _queueCount; i++) _queue[i - 1] = _queue[i];
    _queueCount--;
    _log.addf(LogLevel::kWarn, "push", "推送队列已满，丢弃最旧任务");
  }
  _queue[_queueCount++] = job;
}

void PushService::enqueueRetryLocked(const RetryItem& item) {
  if (_retryCount >= kRetrySize) {
    _log.addf(LogLevel::kWarn, "push", "重试表已满，丢弃重试项");
    return;
  }
  _retry[_retryCount++] = item;
}

// ---------------- 配置读写 ----------------
bool PushService::enabled() const {
  Guard g(_mutex);
  return _enabled;
}

int PushService::validChannelCount() const {
  Guard g(_mutex);
  int n = 0;
  for (int i = 0; i < _count; i++) {
    if (pushChannelValid(_channels[i])) n++;
  }
  return n;
}

bool PushService::setEnabled(bool on) {
  {
    Guard g(_mutex);
    if (_enabled == on) return true;
    _enabled = on;
    if (!persistConfig()) return false;
  }
  _log.addf(LogLevel::kInfo, "push", "推送总开关已%s", on ? "开启" : "停用");
  return true;
}

String PushService::configJson() const {
  Guard g(_mutex);
  JsonDocument doc;
  doc["ok"] = true;
  doc["enabled"] = _enabled;
  doc["strategy"] = (int)_strategy;
  // configured：存在任一有效（启用且字段齐全）通道
  bool configured = false;
  for (int i = 0; i < _count; i++) {
    if (pushChannelValid(_channels[i])) { configured = true; break; }
  }
  doc["configured"] = configured;

  JsonArray arr = doc["channels"].to<JsonArray>();
  for (int i = 0; i < _count; i++) {
    const PushChannel& ch = _channels[i];
    JsonObject o = arr.add<JsonObject>();
    const char* secret = pushTypeSecretField(ch.type);
    o["type"]        = (int)ch.type;
    o["name"]        = ch.name;
    o["url"]         = ch.url;
    o["hasKey1"]     = ch.key1.length() > 0;
    o["hasKey2"]     = ch.key2.length() > 0;
    o["hasKey3"]     = ch.key3.length() > 0;
    o["hasKey4"]     = ch.key4.length() > 0;
    o["hasKey5"]     = ch.key5.length() > 0;
    // 密钥字段不回显真实值（仅返回 hasKeyX 标记）；其余字段正常返回
    if (String(secret) != "key1") o["key1"] = ch.key1;
    if (String(secret) != "key2") o["key2"] = ch.key2;
    if (String(secret) != "key3") o["key3"] = ch.key3;
    if (String(secret) != "key4") o["key4"] = ch.key4;
    if (String(secret) != "key5") o["key5"] = ch.key5;
    o["customBody"]  = ch.customBody;
    o["retryOnFail"] = ch.retryOnFail;
    o["enabled"]     = ch.enabled;            // 单通道启用位（覆盖在 valid 之外）
    o["valid"]       = pushChannelValid(ch);
  }
  String out;
  serializeJson(doc, out);
  return out;
}

String PushService::typesJson() {
  JsonDocument doc;
  JsonArray arr = doc.to<JsonArray>();
  // 每类通道：required=必填字段，optional=可选字段，body=是否支持 customBody，
  // secretField=需要掩码回显的字段
  auto add = [&](PushType t, const char* req, const char* opt, bool body, const char* secret) {
    JsonObject o = arr.add<JsonObject>();
    o["type"] = (int)t;
    o["label"] = pushTypeLabel(t);
    JsonArray r = o["required"].to<JsonArray>();
    if (req) { String s(req); int start = 0; while (start < (int)s.length()) {
      int comma = s.indexOf(',', start); if (comma < 0) comma = s.length();
      String f = s.substring(start, comma); f.trim();
      if (f.length()) r.add(f); start = comma + 1; } }
    JsonArray op = o["optional"].to<JsonArray>();
    if (opt) { String s(opt); int start = 0; while (start < (int)s.length()) {
      int comma = s.indexOf(',', start); if (comma < 0) comma = s.length();
      String f = s.substring(start, comma); f.trim();
      if (f.length()) op.add(f); start = comma + 1; } }
    o["body"] = body;
    o["secretField"] = pushTypeSecretField(t);
  };
  add(PUSH_TYPE_POST_JSON, "url", "name", false, "");
  add(PUSH_TYPE_BARK, "url", "name,key1", false, "key1");
  add(PUSH_TYPE_GET, "url", "name", false, "");
  add(PUSH_TYPE_DINGTALK, "url", "name,key1", false, "key1");
  add(PUSH_TYPE_PUSHPLUS, "key1", "url,key2,name", false, "key1");
  add(PUSH_TYPE_SERVERCHAN, "", "url,key1,name", false, "key1");
  add(PUSH_TYPE_CUSTOM, "url,customBody", "name", true, "");
  add(PUSH_TYPE_FEISHU, "url", "name,key1", false, "key1");
  add(PUSH_TYPE_GOTIFY, "url,key1", "name", false, "key1");
  add(PUSH_TYPE_TELEGRAM, "key1,key2", "url,name", false, "key2");
  add(PUSH_TYPE_WECHAT_WORK, "url", "name,key1", false, "key1");
  add(PUSH_TYPE_MAILGUN, "key1,key2,key3", "url,key4,key5,name", false, "key1");

  String out;
  serializeJson(doc, out);
  return out;
}

bool PushService::saveFromJson(const JsonObject& o) {
  {
    Guard g(_mutex);
    _enabled = o["enabled"] | _enabled;
    int s = o["strategy"] | (int)_strategy;
    _strategy = (s == (int)PUSH_STRATEGY_FAILOVER) ? PUSH_STRATEGY_FAILOVER
                                                  : PUSH_STRATEGY_BROADCAST;

    int cnt = 0;
    if (o["channels"].is<JsonArray>()) {
      JsonArray arr = o["channels"].as<JsonArray>();
      for (JsonObject ch : arr) {
        if (cnt >= MAX_PUSH_CHANNELS) break;
        PushChannel& dst = _channels[cnt];
        dst.enabled     = ch["enabled"] | false;
        dst.type        = (PushType)(ch["type"] | (int)PUSH_TYPE_POST_JSON);
        dst.name        = ch["name"] | "";
        dst.url         = ch["url"] | "";
        // key1 为掩码字段：仅当提供非空值才覆盖（空串保留已存值）
        if (ch["key1"].is<const char*>()) {
          String k1 = ch["key1"].as<String>();
          if (k1.length() > 0) dst.key1 = k1;
        }
        dst.key2        = ch["key2"] | "";
        dst.key3        = ch["key3"] | "";
        dst.key4        = ch["key4"] | "";
        dst.key5        = ch["key5"] | "";
        dst.customBody  = ch["customBody"] | "";
        dst.retryOnFail = ch["retryOnFail"] | false;
        if (dst.name.length() == 0) dst.name = "通道" + String(cnt + 1);
        cnt++;
      }
    }
    _count = cnt;
    if (!persistConfig()) return false;
  }
  _log.addf(LogLevel::kInfo, "push", "推送配置已保存（总开关 %s，通道数 %d，策略 %s）",
            _enabled ? "开" : "关", _count,
            _strategy == PUSH_STRATEGY_FAILOVER ? "故障转移" : "广播");
  return true;
}

void PushService::exportTo(JsonObject& o) const {
  Guard g(_mutex);
  o["enabled"] = _enabled;
  o["strategy"] = (int)_strategy;
  JsonArray arr = o["channels"].to<JsonArray>();
  for (int i = 0; i < _count; i++) {
    const PushChannel& ch = _channels[i];
    JsonObject c = arr.add<JsonObject>();
    c["enabled"]    = ch.enabled;
    c["type"]       = (int)ch.type;
    c["name"]       = ch.name;
    c["url"]        = ch.url;
    c["key1"]       = ch.key1;
    c["key2"]       = ch.key2;
    c["key3"]       = ch.key3;
    c["key4"]       = ch.key4;
    c["key5"]       = ch.key5;
    c["customBody"] = ch.customBody;
    c["retryOnFail"] = ch.retryOnFail;
  }
}

bool PushService::importFrom(const JsonObject& o) {
  {
    Guard g(_mutex);
    _enabled = o["enabled"] | _enabled;
    int s = o["strategy"] | (int)_strategy;
    _strategy = (s == (int)PUSH_STRATEGY_FAILOVER) ? PUSH_STRATEGY_FAILOVER
                                                  : PUSH_STRATEGY_BROADCAST;
    int cnt = 0;
    if (o["channels"].is<JsonArray>()) {
      JsonArray arr = o["channels"].as<JsonArray>();
      for (JsonObject ch : arr) {
        if (cnt >= MAX_PUSH_CHANNELS) break;
        PushChannel& dst = _channels[cnt];
        dst.enabled     = ch["enabled"] | false;
        dst.type        = (PushType)(ch["type"] | (int)PUSH_TYPE_POST_JSON);
        dst.name        = ch["name"] | "";
        dst.url         = ch["url"] | "";
        dst.key1        = ch["key1"] | "";
        dst.key2        = ch["key2"] | "";
        dst.key3        = ch["key3"] | "";
        dst.key4        = ch["key4"] | "";
        dst.key5        = ch["key5"] | "";
        dst.customBody  = ch["customBody"] | "";
        dst.retryOnFail = ch["retryOnFail"] | false;
        cnt++;
      }
    }
    _count = cnt;
    if (!persistConfig()) return false;
  }
  return true;
}

bool PushService::loadConfig() {
  Preferences prefs;
  String json;
  if (prefs.begin(cfg::kNvsNamespace, true)) {
    json = prefs.getString(cfg::kNvsKeyPush, "");
    prefs.end();
  }
  if (!json.length()) {
    _log.addf(LogLevel::kInfo, "push", "NVS 无推送配置，使用默认值（总开关关，无通道）");
    return false;
  }
  JsonDocument doc;
  if (deserializeJson(doc, json) != DeserializationError::Ok) {
    _log.addf(LogLevel::kWarn, "push", "推送配置解析失败，使用默认值");
    return false;
  }
  Guard g(_mutex);
  _enabled = doc["enabled"] | false;
  int s = doc["strategy"] | (int)PUSH_STRATEGY_BROADCAST;
  _strategy = (s == (int)PUSH_STRATEGY_FAILOVER) ? PUSH_STRATEGY_FAILOVER
                                                : PUSH_STRATEGY_BROADCAST;
  int cnt = 0;
  if (doc["channels"].is<JsonArray>()) {
    for (JsonObject ch : doc["channels"].as<JsonArray>()) {
      if (cnt >= MAX_PUSH_CHANNELS) break;
      PushChannel& dst = _channels[cnt];
      dst.enabled     = ch["enabled"] | false;
      dst.type        = (PushType)(ch["type"] | (int)PUSH_TYPE_POST_JSON);
      dst.name        = ch["name"] | "";
      dst.url         = ch["url"] | "";
      dst.key1        = ch["key1"] | "";
      dst.key2        = ch["key2"] | "";
      dst.key3        = ch["key3"] | "";
      dst.key4        = ch["key4"] | "";
      dst.key5        = ch["key5"] | "";
      dst.customBody  = ch["customBody"] | "";
      dst.retryOnFail = ch["retryOnFail"] | false;
      cnt++;
    }
  }
  _count = cnt;
  return true;
}

bool PushService::persistConfig() {
  JsonDocument doc;
  doc["ver"] = 1;
  doc["enabled"] = _enabled;
  doc["strategy"] = (int)_strategy;
  JsonArray arr = doc["channels"].to<JsonArray>();
  for (int i = 0; i < _count; i++) {
    const PushChannel& ch = _channels[i];
    JsonObject c = arr.add<JsonObject>();
    c["enabled"]    = ch.enabled;
    c["type"]       = (int)ch.type;
    c["name"]       = ch.name;
    c["url"]         = ch.url;
    c["key1"]        = ch.key1;
    c["key2"]        = ch.key2;
    c["key3"]        = ch.key3;
    c["key4"]        = ch.key4;
    c["key5"]        = ch.key5;
    c["customBody"]  = ch.customBody;
    c["retryOnFail"] = ch.retryOnFail;
  }
  String out;
  serializeJson(doc, out);

  Preferences prefs;
  if (!prefs.begin(cfg::kNvsNamespace, false)) return false;
  size_t written = prefs.putString(cfg::kNvsKeyPush, out);
  prefs.end();
  return written == out.length();
}
