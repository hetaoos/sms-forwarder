#include "net/StatusBus.h"

#include "net/StateJson.h"

#include <Arduino.h>

// wifi/bridge/led/display/sms/time：各模块引用，保存供 tick 采集使用
StatusBus::StatusBus(WifiManager &wifi, SerialBridge &bridge, RgbLed &led,
                     Display &display, SmsService &sms, TimeSync &time)
    : _wifi(wifi), _bridge(bridge), _led(led), _display(display), _sms(sms),
      _time(time) {}

// 注册 SSE handler；onConnect/onDisconnect 用于维护客户端列表与上限
void StatusBus::begin(AsyncWebServer &server) {
  _events.onConnect([this](AsyncEventSourceClient *client) {
    onClientConnect(client);
  });
  _events.onDisconnect([this](AsyncEventSourceClient *client) {
    onClientDisconnect(client);
  });
  server.addHandler(&_events);
  Serial.printf("[sse] event stream on %s\r\n", cfg::kSsePath);
}

// 采集、比对、推送；无客户端时重置基线（下次有人接入全量发）
void StatusBus::tick() {
  if (_clients.empty()) {
    _baseline = false;
    return;
  }
  uint32_t now = millis();
  if (now - _lastPushMs < cfg::kSsePushMs) return;
  _lastPushMs = now;

  // ---- 六类状态：与上次快照字符串比对，变化才推 ----
  String js;

  js = stateStatusJson(_wifi, _led, _display, _time);
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

  js = stateDispJson(_display);
  if (!_baseline || js != _lastDisp) {
    publish("disp", js);
    _lastDisp = js;
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

  // ---- 模组配置：变化才推 ----
  String ports = _sms.modemsJson();
  if (!_baseline || ports != _lastSmsPorts) {
    publish("smsports", "{\"ok\":true,\"modems\":" + ports + "}");
    _lastSmsPorts = ports;
  }

  // ---- 短信存储统计：变化才推 ----
  js = _sms.store().statsJson();
  if (!_baseline || js != _lastSmsStats) {
    publish("smsstats", js);
    _lastSmsStats = js;
  }

  _baseline = true;
}

// 新客户端接入：超限关最旧，然后给它发一份全量快照
void StatusBus::onClientConnect(AsyncEventSourceClient *client) {
  if (!client) return;

  // 超限：关闭最早接入的连接（其 onDisconnect 会把它从列表移除）
  while (_clients.size() >= cfg::kSseMaxClients) {
    AsyncEventSourceClient *oldest = _clients.front();
    Serial.printf("[sse] client limit %u reached, closing oldest\r\n",
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
    Serial.printf("[sse] client resume from last-event-id %u\r\n",
                  (unsigned)resumeFrom);
  }
  _lastHistId = resumeFrom < _lastHistId ? resumeFrom : _lastHistId;

  publishSnapshot(client);
  Serial.printf("[sse] client connected (%u/%u)\r\n", (unsigned)_clients.size(),
                (unsigned)cfg::kSseMaxClients);
}

// 客户端断开：从列表移除
void StatusBus::onClientDisconnect(AsyncEventSourceClient *client) {
  auto it = std::find(_clients.begin(), _clients.end(), client);
  if (it != _clients.end()) _clients.erase(it);
  Serial.printf("[sse] client disconnected (%u left)\r\n", (unsigned)_clients.size());
}

// 广播一个事件
void StatusBus::publish(const char *event, const String &json) {
  _events.send(json.c_str(), event, ++_eventId);
}

// 向单个客户端发送一个事件
void StatusBus::publishTo(AsyncEventSourceClient *client, const char *event,
                          const String &json) {
  if (client) client->send(json.c_str(), event, ++_eventId);
}

// 全量快照：六类状态 + 短信统计 + 最近的 AT 历史
void StatusBus::publishSnapshot(AsyncEventSourceClient *client) {
  publishTo(client, "status", stateStatusJson(_wifi, _led, _display, _time));
  publishTo(client, "stats", _bridge.statsJson());
  publishTo(client, "wifi", _wifi.statusJson());
  publishTo(client, "saved", _wifi.listJson());
  publishTo(client, "rgb", stateRgbJson(_led));
  publishTo(client, "disp", stateDispJson(_display));
  publishTo(client, "smsstats", _sms.store().statsJson());
  // 历史从较大的一端给：要么断点续传点，要么当前最新
  uint32_t from = _lastHistId;
  String json = _bridge.historyJson((int32_t)from, 50);
  if (json.length() > 2) publishTo(client, "at", json);
}
