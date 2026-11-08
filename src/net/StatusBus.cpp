#include "net/StatusBus.h"

#include <ArduinoJson.h>

#include "app/Psram.h"  // PsBuffer：SSE 快照经 PSRAM 直发（方案 #2）
#include "log/LogStore.h"
#include "net/StateJson.h"

#include <Arduino.h>

namespace {

// 本通信模组日志出口：全局日志环（串口由 LogStore 统一镜像输出）
LogStore &_log = devLog();

}  // namespace

// wifi/bridge/led/display/ui/sms/time：各通信模组引用，保存供 tick 采集使用
StatusBus::StatusBus(WifiManager &wifi, SerialBridge &bridge, RgbLed &led,
                     Display &display, UIMenu &ui, SmsService &sms,
                     TimeSync &time, LogStore &log, Outbox &outbox)
    : _wifi(wifi), _bridge(bridge), _led(led), _display(display), _ui(ui),
      _sms(sms), _time(time), _log(log), _outbox(outbox) {}

// 注册 SSE handler；onConnect/onDisconnect 用于维护客户端列表与上限
void StatusBus::begin(AsyncWebServer &server) {
  _events.onConnect([this](AsyncEventSourceClient *client) {
    onClientConnect(client);
  });
  _events.onDisconnect([this](AsyncEventSourceClient *client) {
    onClientDisconnect(client);
  });
  server.addHandler(&_events);
  _log.addf(LogLevel::kInfo, "sse", "SSE 事件流已挂载：%s", cfg::kSsePath);
}

// 采集、比对、推送；无客户端时重置基线（下次有人接入全量发）
void StatusBus::tick() {
  if (_clients.empty()) {
    _baseline = false;
    _lastLogId = 0;        // 无客户端时放弃日志基线，下次连接重新播种
    _lastOutboxRev = 0;    // 发件箱修订号同理
    return;
  }
  uint32_t now = millis();
  if (now - _lastPushMs < cfg::kSsePushMs) return;
  _lastPushMs = now;

  // ---- 六类状态：与上次快照字符串比对，变化才推 ----
  String js;

  js = stateStatusJson(_wifi, _led, _display, _time, _sms.store());
  if (!_baseline || js != _lastStatus) {
    publish("status", js);
    _lastStatus = js;
  }

  js = _bridge.statsJson();
  if (!_baseline || js != _lastStats) {
    publish("stats", js);
    _lastStats = js;
  }

  js = _wifi.statusJson();
  if (!_baseline || js != _lastWifi) {
    publish("wifi", js);
    _lastWifi = js;
  }

  js = _wifi.listJson();
  if (!_baseline || js != _lastSaved) {
    publish("saved", js);
    _lastSaved = js;
  }

  js = stateRgbJson(_led);
  if (!_baseline || js != _lastRgb) {
    publish("rgb", js);
    _lastRgb = js;
  }

  js = stateDispJson(_display, _ui);
  if (!_baseline || js != _lastDisp) {
    publish("disp", js);
    _lastDisp = js;
  }

  // ---- OTA 升级进度：变化才推（镜像 /api/ota/status）----
  // 升级是分钟级长任务，下载阶段 progress 频繁变化；ESPAsyncWebServer 的 SSE
  // 为异步写、客户端上限 8，频繁推送可控
  String ota = stateOtaJson();
  if (!_baseline || ota != _lastOta) {
    publish("ota", ota);
    _lastOta = ota;
  }

  // ---- AT 历史：只推 lastHistoryId 之后的新记录 ----
  uint32_t lastId = _bridge.lastHistoryId();
  if (lastId != _lastHistId) {
    String json = _bridge.historyJson((int32_t)_lastHistId, 50);
    if (json.length() > 2) {  // "[]" 表示无新记录
      publish("at", json);
    }
    _lastHistId = lastId;
  }

  // ---- 短信：有新记录才推最新一条摘要 ----
  if (_sms.lastSmsId() != _lastSmsId) {
    _lastSmsId = _sms.lastSmsId();
    publish("sms", _sms.latestJson());
  }

  // ---- 来电：有新派发才推最新来电摘要（与 sms 事件对称）----
  if (_sms.lastCallSeq() != _lastCallSeq) {
    _lastCallSeq = _sms.lastCallSeq();
    publish("call", _sms.lastCallJson());
  }

  // ---- 通信模组配置：变化才推 ----
  String ports = _sms.modemsJson();
  if (!_baseline || ports != _lastSmsPorts) {
    JsonDocument doc;
    doc["ok"] = true;
    doc["modems"] = serialized(ports);  // 复用已序列化的通信模组数组（raw JSON）
    String out;
    serializeJson(doc, out);
    publish("smsports", out);
    _lastSmsPorts = ports;
  }

  // ---- 短信存储统计：变化才推 ----
  js = _sms.store().statsJson();
  if (!_baseline || js != _lastSmsStats) {
    publish("smsstats", js);
    _lastSmsStats = js;
  }

  // ---- 运行日志：增量推送新记录（仿 at 历史）----
  // 与前端 logs 页增量拉取同口径：只发 lastId 之后、本函数不加过滤（前端按自身
  // level/q 过滤），total 为全局条数（前端无过滤时才同步计数）。
  if (_log.size() && _log.lastId() != _lastLogId) {
    std::vector<LogStore::Entry> inc;
    int total = 0;
    _log.tail(_lastLogId, 200, -1, "", inc, total);
    if (!inc.empty()) {
      JsonDocument doc;
      doc["total"] = total;
      JsonArray a = doc["logs"].to<JsonArray>();
      for (const auto &e : inc) {
        JsonObject o = a.add<JsonObject>();
        o["id"] = e.id;
        o["ts"] = e.ts;
        o["level"] = e.level;
        o["tag"] = e.tag;
        o["msg"] = e.msg;
      }
      String out;
      serializeJson(doc, out);
      publish("log", out);
    }
    // 推进基线：本批已满上限则只到末条（避免漏推），否则直接到最新
    _lastLogId = (inc.size() >= 200) ? inc.back().id : _log.lastId();
  }

  // ---- 发件箱：变更时推脏信号（前端收到即刷新当前视图）----
  {
    uint32_t rev = _outbox.revision();
    if (rev != _lastOutboxRev) {
      _lastOutboxRev = rev;
      publish("outbox", "{}");
    }
  }

  _baseline = true;
}

// 新客户端接入：超限关最旧，然后给它发一份全量快照
void StatusBus::onClientConnect(AsyncEventSourceClient *client) {
  if (!client) return;

  // 超限：关闭最早接入的连接（其 onDisconnect 会把它从列表移除）
  while (_clients.size() >= cfg::kSseMaxClients) {
    AsyncEventSourceClient *oldest = _clients.front();
    _log.addf(LogLevel::kWarn, "sse", "SSE 客户端已达上限 %u，关闭最旧连接",
              (unsigned)cfg::kMaxSessions);
    oldest->close();
    // close() 触发断开回调是异步的，这里先从列表摘除，避免向量抖动
    auto it = std::find(_clients.begin(), _clients.end(), oldest);
    if (it != _clients.end()) _clients.erase(it);
  }
  _clients.push_back(client);

  // 新连接可能携带 Last-Event-ID（断线重连）：续传 AT 历史
  uint32_t resumeFrom = 0;
  if (client->lastId() > 0) {
    resumeFrom = client->lastId();
    _log.addf(LogLevel::kInfo, "sse", "SSE 客户端从 last-event-id %u 续传",
              (unsigned)resumeFrom);
  }
  _lastHistId = resumeFrom < _lastHistId ? resumeFrom : _lastHistId;
  // 日志增量基线播种到当前最新：只推「连接之后」的新记录，避免首连涌出全部历史
  _lastLogId = _log.lastId();

  publishSnapshot(client);
  _log.addf(LogLevel::kInfo, "sse", "SSE 客户端已连接（%u/%u）",
            (unsigned)_clients.size(), (unsigned)cfg::kSseMaxClients);
}

// 客户端断开：从列表移除
void StatusBus::onClientDisconnect(AsyncEventSourceClient *client) {
  auto it = std::find(_clients.begin(), _clients.end(), client);
  if (it != _clients.end()) _clients.erase(it);
  _log.addf(LogLevel::kInfo, "sse", "SSE 客户端已断开（剩余 %u）",
            (unsigned)_clients.size());
}

// 广播一个事件
void StatusBus::publish(const char *event, const String &json) {
  _events.send(json.c_str(), event, ++_eventId);
}

// 广播一个事件（PSRAM 缓冲直发，省去 DRAM String 副本，见方案 #2）
void StatusBus::publish(const char *event, const PsBuffer &buf) {
  _events.send(buf.c_str(), event, ++_eventId);
}

// 向单个客户端发送一个事件
void StatusBus::publishTo(AsyncEventSourceClient *client, const char *event,
                          const String &json) {
  if (client) client->send(json.c_str(), event, ++_eventId);
}

// 向单个客户端发送一个事件（PSRAM 缓冲直发）
void StatusBus::publishTo(AsyncEventSourceClient *client, const char *event,
                          const PsBuffer &buf) {
  if (client) client->send(buf.c_str(), event, ++_eventId);
}

// 全量快照：六类状态 + 短信统计 + 最近的 AT 历史
// status / rgb / disp / ota 直接序列化进 PSRAM 缓冲并直发；其余（stats/wifi/saved/
// smsstats/at）经 String 兼容接口发送，其 JsonDocument 池已改为 PSRAM，最终字符串
// 在 CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL 调低后亦落 PSRAM（方案 #3）。
void StatusBus::publishSnapshot(AsyncEventSourceClient *client) {
  PsBuffer b;
  stateStatusJson(b, _wifi, _led, _display, _time, _sms.store());
  publishTo(client, "status", b);
  publishTo(client, "stats", _bridge.statsJson());
  publishTo(client, "wifi", _wifi.statusJson());
  publishTo(client, "saved", _wifi.listJson());
  stateRgbJson(b, _led);
  publishTo(client, "rgb", b);
  stateDispJson(b, _display, _ui);
  publishTo(client, "disp", b);
  publishTo(client, "smsstats", _sms.store().statsJson());
  stateOtaJson(b);
  publishTo(client, "ota", b);
  // 来电：仅当已有过派发才推（避免首连推一份空 "{}"）
  if (_sms.lastCallSeq() > 0) publishTo(client, "call", _sms.lastCallJson());
  // 历史从较大的一端给：要么断点续传点，要么当前最新
  uint32_t from = _lastHistId;
  String json = _bridge.historyJson((int32_t)from, 50);
  if (json.length() > 2) publishTo(client, "at", json);
}
