#include "net/MqttService.h"

#if ENABLE_MQTT

#include <ArduinoJson.h>
#include <WiFi.h>
#include <Preferences.h>
#include <mqtt_client.h>
#include <esp_event.h>
#include <mbedtls/ssl.h>        // mbedtls_ssl_set_verify：用于关闭证书校验
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/idf_additions.h>   // xQueueCreateWithCaps：把命令队列优先放 PSRAM
#include <time.h>

#include "app/Psram.h"                // psramJsonAllocator()：JsonDocument 内存池优先 PSRAM
#include "log/LogStore.h"
#include "net/WifiManager.h"
#include "ota/Ota.h"            // Ota::version()：与前端 /api/ota/version 同源
#include "utils/TlsTurn.h"      // MQTT-TLS 复用 4 通道计数信号量（从末尾槽分配）
#include "serial/SerialBridge.h"
#include "sms/SmsService.h"
#include "time/TimeSync.h"

// ── 通信模组级状态 ─────────────────────────────────────────────
// esp-mqtt 的 config 只保存指针，不会拷贝字符串，因此 URI/ID/Will/账号等
// 必须存活到客户端销毁，这里用通信模组级 String 长期持有。

// 待执行命令队列元素（文件作用域，供 MQTT 网络任务的事件回调直接构造并入队，
// 无需访问类私有成员；由主循环 tick 取出并在 loop 任务执行）
struct MqttCmdMsg {
  char data[512];
};

String MqttService::s_uri, MqttService::s_cid, MqttService::s_will,
        MqttService::s_willMsg, MqttService::s_user, MqttService::s_pass;
esp_mqtt_client_handle_t MqttService::s_client = nullptr;
bool   MqttService::s_connected       = false;
String MqttService::s_lastError      = "";
bool   MqttService::s_wasConnected    = false;
bool   MqttService::s_bootNotified    = false;
bool   MqttService::s_pendingBoot     = false;
QueueHandle_t MqttService::s_cmdQueue = nullptr;

// TLS 通道守卫（仅 _useTls 时占用 4 通道之一；release 幂等，明文连接不占）
TlsTurn MqttService::_tlsGuard;
unsigned long MqttService::_mqttTlsNextTryMs = 0;

// 配置（NVS: appcfg/mqtt）
bool     MqttService::_enabled          = false;
String   MqttService::_server;
uint16_t MqttService::_port             = 1883;
bool     MqttService::_useTls           = false;
String   MqttService::_user;
String   MqttService::_pass;
String   MqttService::_clientId;
String   MqttService::_topicPrefix      = "smsf";
uint16_t MqttService::_keepAlive        = 60;
bool     MqttService::_subscribeEnabled = true;
uint32_t MqttService::_notifyTypes      = cfg::kMqttNotifyDefault;

// 依赖
WifiManager *MqttService::_wifi = nullptr;
SmsService  *MqttService::_sms  = nullptr;
TimeSync    *MqttService::_time = nullptr;
LogStore    *MqttService::_log  = nullptr;

// 日志限流
constexpr int MqttService::LOG_THROTTLE_PER_SEC;
unsigned long MqttService::s_logLastSec  = 0;
int           MqttService::s_logSecCount = 0;

// ── 设备标识 ───────────────────────────────────────────────
String MqttService::deviceId() {
  uint64_t mac = ESP.getEfuseMac() >> 24;  // 高 48 位为 MAC
  char buf[13];
  snprintf(buf, sizeof(buf), "%012llx", (unsigned long long)mac);
  return String(buf);
}

String MqttService::deviceName() {
  return "SMS-Forwarder-" + deviceId();
}

String MqttService::fullTopic(const String &sub) {
  String p = _topicPrefix.length() > 0 ? _topicPrefix : "smsf";
  return p + "/" + deviceId() + "/" + sub;
}

// ── 时间格式化（与 EmailService 同款）──────────────────────
static String mqttFormatTs(uint32_t ts) {
  if (ts < 1600000000u) return "时间未同步";
  time_t t = (time_t)ts;
  struct tm tmInfo;
  localtime_r(&t, &tmInfo);
  char buf[32];
  strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tmInfo);
  return String(buf);
}

static String mqttFormatUptime(uint32_t ms) {
  uint32_t seconds = ms / 1000;
  uint32_t minutes = seconds / 60;
  uint32_t hours = minutes / 60;
  uint32_t days = hours / 24;
  if (ms < 60000UL) return "不足1分钟";
  if (ms < 3600000UL) return String(minutes) + "分钟";
  if (ms < 86400000UL) return String(hours) + "小时" + String(minutes % 60) + "分";
  return String(days) + "天" + String(hours % 24) + "小时";
}

// ── 设备信息（boot/online 事件的 info 块）──────────────────
static void buildDeviceStatus(JsonObject info) {
  info["device"]    = MqttService::deviceName();
  info["deviceId"]  = MqttService::deviceId();
  info["time"]      = (MqttService::_time && MqttService::_time->isSynced())
                          ? MqttService::_time->dateStr() : String("未同步");
  info["timestamp"] = (MqttService::_time && MqttService::_time->isSynced())
                          ? (uint32_t)time(nullptr) : 0;
  info["uptime"]    = millis() / 1000;
  info["uptimeText"]= mqttFormatUptime(millis());
  if (MqttService::_wifi) {
    info["ip"]   = MqttService::_wifi->ip();
    info["ssid"] = MqttService::_wifi->ssid();
    info["rssi"] = MqttService::_wifi->rssi();
  }
  if (MqttService::_sms) {
    JsonArray modems = info["modems"].to<JsonArray>();
    JsonDocument mdoc(psramJsonAllocator());
    if (deserializeJson(mdoc, MqttService::_sms->modemsJson()) == DeserializationError::Ok &&
        mdoc.is<JsonArray>()) {
      for (JsonObject m : mdoc.as<JsonArray>()) {
        JsonObject mo = modems.add<JsonObject>();
        mo["port"]    = m["port"];
        mo["enabled"] = m["enabled"];
        mo["ready"]   = m["ready"];
        mo["state"]   = m["state"];
        mo["sim"]     = m["sim"];
        mo["num"]     = m["num"];
        mo["csq"]     = m["csq"];
        mo["operCode"]= m["operCode"];
        mo["alias"]   = m["alias"];
      }
    }
  }
  String chipModel = ESP.getChipModel();
  info["chipModel"] = (chipModel.length() > 0)
      ? (chipModel + " rev " + String((int)ESP.getChipRevision())) : String("未知");
  float tempC = temperatureRead();
  if (isnan(tempC)) info["temperature"] = nullptr;
  else             info["temperature"] = (float)(round(tempC * 10) / 10.0);
  info["firmware"] = Ota::version();
}

// ── 日志 sink（任意任务上下文，须轻量）──────────────────────
void MqttService::onLog(uint8_t level, const char *tag, const char *msg, uint32_t ts) {
  if (!_enabled || !isConfigured()) return;
  if ((_notifyTypes & cfg::kMqttNotifyLog) == 0) return;
  // 轻量限流：每自然秒最多 LOG_THROTTLE_PER_SEC 条，超出丢弃，避免日志风暴
  unsigned long sec = millis() / 1000;
  if (sec != s_logLastSec) { s_logLastSec = sec; s_logSecCount = 0; }
  if (s_logSecCount >= LOG_THROTTLE_PER_SEC) return;
  s_logSecCount++;

  JsonDocument doc(psramJsonAllocator());
  doc["type"]    = "log";
  doc["time"]    = ts;
  doc["level"]   = (int)level;
  doc["tag"]     = tag;
  doc["message"] = msg;
  String payload;
  serializeJson(doc, payload);
  // 直接发布：断开时 esp-mqtt 将其存入 outbox，重连后自动补发（qos=1）
  publish("log", payload, 1, false);
}

// ── 发布 ───────────────────────────────────────────────────
bool MqttService::publish(const String &subtopic, const String &payload, int qos,
                          bool retain) {
  if (s_client == nullptr) return false;
  String topic = fullTopic(subtopic);
  int id = esp_mqtt_client_enqueue(s_client, topic.c_str(), payload.c_str(),
                                  payload.length(), qos, retain ? 1 : 0, 0);
  return id >= 0;
}

void MqttService::publishStatus(const String &status) {
  JsonDocument doc(psramJsonAllocator());
  doc["type"]      = "status";
  doc["status"]    = status;
  doc["device"]    = deviceName();
  doc["deviceId"]  = deviceId();
  doc["timestamp"] = (_time && _time->isSynced()) ? (uint32_t)time(nullptr) : 0;
  doc["uptime"]    = millis() / 1000;
  doc["uptimeText"]= mqttFormatUptime(millis());
  doc["ip"]        = _wifi ? _wifi->ip() : String("");
  doc["wifiRssi"]  = _wifi ? _wifi->rssi() : 0;
  doc["simReady"]  = _sms ? _sms->modemsSettled() : true;
  String payload;
  serializeJson(doc, payload);
  publish("status", payload, 1, true);  // retained：订阅者连上即拿到最近状态
}

void MqttService::publishEvent(MqttMsgType type, const String &sender,
                               const String &message, uint32_t ts,
                               const String &typeLabel, const String &modemLabel,
                               int8_t modemSlot) {
  uint32_t bit = 0;
  const char *sub = nullptr;
  switch (type) {
    case MqttMsgType::kSms:    bit = cfg::kMqttNotifySms;    sub = "sms";    break;
    case MqttMsgType::kSim:    bit = cfg::kMqttNotifySim;    sub = "sim";    break;
    case MqttMsgType::kBoot:   bit = cfg::kMqttNotifyStartup; sub = "boot";  break;
    case MqttMsgType::kOnline: bit = cfg::kMqttNotifyOnline; sub = "online"; break;
    case MqttMsgType::kCall:   bit = cfg::kMqttNotifyCall;   sub = "call";   break;
    default: return;
  }
  if (!_enabled || !isConfigured()) return;
  if ((_notifyTypes & bit) == 0) return;

  JsonDocument doc(psramJsonAllocator());
  doc["type"]      = sub;
  doc["device"]    = deviceName();
  doc["deviceId"]  = deviceId();
  doc["timestamp"] = ts;
  doc["time"]      = mqttFormatTs(ts);

  if (type == MqttMsgType::kSms || type == MqttMsgType::kCall) {
    // 短信：from=发件人；来电：from=主叫号码（可能为「未知号码」/「号码保密」）
    doc["from"]      = sender;
    doc["message"]   = message;
    if (typeLabel.length() > 0) doc["typeLabel"] = typeLabel;
    if (modemLabel.length() > 0) doc["modem"] = modemLabel;
  } else if (type == MqttMsgType::kSim) {
    doc["port"]  = modemSlot > 0 ? modemSlot : 1;
    doc["ready"] = (message == "ready");
    if (modemLabel.length() > 0) doc["modem"] = modemLabel;
    // SIM 事件附带与事件来源通信模组相关的 4G/SIM 字段
    JsonObject info = doc["info"].to<JsonObject>();
    buildDeviceStatus(info);
  } else {  // boot / online：附带与启动同款设备信息
    JsonObject info = doc["info"].to<JsonObject>();
    buildDeviceStatus(info);
  }

  String payload;
  serializeJson(doc, payload);
  publish(sub, payload, 1, false);
}

// ── 命令响应 ───────────────────────────────────────────────
static void respond(const JsonDocument &req, bool ok, const String &msg) {
  JsonDocument r(psramJsonAllocator());
  r["type"]    = "response";
  r["ok"]      = ok;
  r["message"] = msg;
  if (!req["reqId"].isNull()) r["reqId"] = req["reqId"];
  String payload;
  serializeJson(r, payload);
  MqttService::publish("command/response", payload, 1, false);
}

static void respondHelp(const JsonDocument &req) {
  JsonDocument r(psramJsonAllocator());
  r["type"]    = "response";
  r["ok"]      = true;
  r["message"] = "可用命令列表";
  if (!req["reqId"].isNull()) r["reqId"] = req["reqId"];

  JsonArray cmds = r["commands"].to<JsonArray>();
  auto addCmd = [&](const char *action, const char *desc) {
    JsonObject c = cmds.add<JsonObject>();
    c["action"]      = action;
    c["description"] = desc;
  };
  addCmd("send_sms", "通过指定通信模组发送短信（参数 phone/content，可选 modem）");
  addCmd("send_at",  "向指定通信模组发送 AT 指令并返回响应（参数 cmd，可选 modem/timeout）");
  addCmd("restart",  "重启设备");
  addCmd("reinit",   "请求指定通信模组热重初始化（参数 modem）");
  addCmd("info",     "返回设备信息（与启动通知同款）");
  addCmd("help",     "返回本帮助");
  String payload;
  serializeJson(r, payload);
  MqttService::publish("command/response", payload, 1, false);
}

static void respondInfo(const JsonDocument &req) {
  JsonDocument r(psramJsonAllocator());
  r["type"]    = "response";
  r["ok"]      = true;
  r["message"] = "设备信息（启动信息）";
  if (!req["reqId"].isNull()) r["reqId"] = req["reqId"];
  JsonObject status = r["status"].to<JsonObject>();
  buildDeviceStatus(status);
  String payload;
  serializeJson(r, payload);
  MqttService::publish("command/response", payload, 1, false);
}

// ── 命令分发（在主循环线程执行）────────────────────────────
void MqttService::handleCommand(const String &json) {
  JsonDocument doc(psramJsonAllocator());
  if (deserializeJson(doc, json) != DeserializationError::Ok) {
    JsonDocument empty(psramJsonAllocator());
    respond(empty, false, "JSON 解析失败");
    return;
  }
  const char *action = doc["action"] | "";

  if (strcmp(action, "send_sms") == 0) {
    String phone   = doc["phone"]   | "";
    String content = doc["content"] | "";
    int8_t port    = doc["modem"]   | 1;
    if (phone.length() == 0 || content.length() == 0) {
      respond(doc, false, "phone / content 不能为空");
      return;
    }
    if (!_sms) { respond(doc, false, "短信服务不可用"); return; }
    String err;
    bool stored = true;
    uint32_t id = _sms->send((uint8_t)port, phone, content, err, &stored);
    if (!id) { respond(doc, false, err.length() ? err : "短信发送失败"); return; }
    respond(doc, true, "短信发送成功（记录 #" + String((unsigned)id) + "）");

  } else if (strcmp(action, "send_at") == 0) {
    String cmd = doc["cmd"] | "";
    int8_t port = doc["modem"] | 1;
    unsigned long timeout = doc["timeout"] | 5000UL;
    if (cmd.length() == 0) { respond(doc, false, "cmd 不能为空"); return; }
    if (!_sms) { respond(doc, false, "短信服务不可用"); return; }
    String err;
    auto result = _sms->sendAt((uint8_t)port, cfg::kUartBaud, cmd,
                               SerialBridge::kCrlf, timeout, err);
    if (!result.ok) { respond(doc, false, err.length() ? err : "AT 发送失败"); return; }
    respond(doc, true, result.resp);

  } else if (strcmp(action, "restart") == 0) {
    respond(doc, true, "设备即将重启");
    delay(500);
    ESP.restart();

  } else if (strcmp(action, "reinit") == 0) {
    int8_t port = doc["modem"] | 0;
    if (port < 1 || port > cfg::kSmsMaxModems) { respond(doc, false, "modem 越界"); return; }
    if (!_sms) { respond(doc, false, "短信服务不可用"); return; }
    if (!_sms->reinitPort((uint8_t)port)) { respond(doc, false, "通信模组未启用或不存在"); return; }
    respond(doc, true, "已请求通信模组" + String(port) + "热重初始化");

  } else if (strcmp(action, "help") == 0) {
    respondHelp(doc);

  } else if (strcmp(action, "info") == 0) {
    respondInfo(doc);

  } else {
    respond(doc, false, String("未知操作: ") + action);
  }
}

// 把 ESP-TLS 组件错误码翻译成中文（0x8000 基址），便于排查 MQTT 连接失败。
// 0x8004 = ESP_ERR_ESP_TLS_FAILED_CONNECT_TO_HOST（连接主机失败）。
static const char *tlsErrText(uint32_t code) {
  switch (code) {
    case 0x8001: return "域名解析失败（检查服务器地址）";
    case 0x8002: return "无法创建 socket（资源不足）";
    case 0x8004: return "连接主机失败（地址/端口错误、网络不通、或 TLS 开关与端口不匹配）";
    case 0x8006: return "连接超时（主机无响应）";
    case 0x800B: return "TLS 握手失败（明文端口勿开 TLS，加密端口应为 8883 等）";
    case 0x800C: case 0x800F: return "证书校验失败";
    case 0x8010: return "证书主机名不匹配";
    default: return nullptr;
  }
}

// ── esp-mqtt 事件回调（运行在 esp-mqtt 网络任务）──────────────
static void mqtt_event_handler(void *handler_args, esp_event_base_t base,
                               int32_t event_id, void *event_data) {
  auto event = (esp_mqtt_event_handle_t)event_data;
  switch ((esp_mqtt_event_id_t)event_id) {
    case MQTT_EVENT_CONNECTED:
      // 仅置标志。订阅 / 上线状态 / 启动通知补发统一放到 tick() 主循环处理，
      // 避免在 MQTT 网络任务的事件回调里直接调用 subscribe/publish（esp-idf 不推荐）。
      MqttService::s_connected = true;
      MqttService::s_lastError = "";   // 连上即清空上次错误，供 Web 状态显示
      break;
    case MQTT_EVENT_DISCONNECTED:
      MqttService::s_connected = false;
      break;
    case MQTT_EVENT_ERROR: {
      auto *eh = event->error_handle;
      if (eh != nullptr) {
        // conn_return=0 / 未知：失败发生在 TCP/TLS 连接阶段，尚未收到 CONNACK
        const char *reason = "未收到 CONNACK（连接/握手阶段已失败）";
        switch (eh->connect_return_code) {
          case 1: reason = "协议不被接受"; break;
          case 2: reason = "客户端 ID 被拒绝"; break;
          case 3: reason = "服务器不可用"; break;
          case 4: reason = "用户名/密码错误"; break;
          case 5: reason = "未授权"; break;
          default: break;  // 0：连接层失败，沿用上面的默认说明
        }
        // 翻译 ESP-TLS 错误码（0x8004 = 连接主机失败），其余原样打印
        const char *t1 = tlsErrText((uint32_t)eh->esp_tls_last_esp_err);
        String extra;
        if (t1)
          extra = String("; tls_err=0x") + String((unsigned)eh->esp_tls_last_esp_err, 16)
                + "(" + t1 + ")";
        else if (eh->esp_tls_last_esp_err)
          extra = String("; tls_err=0x") + String((unsigned)eh->esp_tls_last_esp_err, 16);
        if (eh->esp_tls_stack_err)
          extra += String("; tls_stack_err=") + String(eh->esp_tls_stack_err);
        if (eh->esp_transport_sock_errno)
          extra += String("; sock_errno=") + String(eh->esp_transport_sock_errno);
        // 组合成可读错误描述，存入 s_lastError 供 Web 状态接口 / 仪表盘展示
        String msg = "conn_return=" + String((int)eh->connect_return_code) +
                     "(" + reason + ")" + extra;
        MqttService::s_lastError = msg;
        if (MqttService::_log) {
          MqttService::_log->addf(LogLevel::kError, "mqtt",
            "MQTT 连接错误: %s", msg.c_str());
        }
      }
      break;
    }
    case MQTT_EVENT_DATA: {
      if (event->topic == nullptr || event->data == nullptr) break;
      String topic(event->topic, event->topic_len);
      if (topic.endsWith("/command") && MqttService::s_cmdQueue != nullptr) {
        MqttCmdMsg msg;
        size_t n = (size_t)event->data_len < sizeof(msg.data) - 1
                       ? (size_t)event->data_len : sizeof(msg.data) - 1;
        memcpy(msg.data, event->data, n);
        msg.data[n] = 0;
        xQueueSend(MqttService::s_cmdQueue, &msg, 0);  // 满则丢弃，命令不重试
      }
      break;
    }
    default:
      break;
  }
}

// ── 客户端构建 / 销毁 ──────────────────────────────────────
// 自签名 / 局域网 Broker 的「接受任意证书」实现（与参考实现同款思路）：
// 本框架 esp-mqtt 的 config 未暴露 disable_certificate_validation，故借道
// crt_bundle_attach 钩子（其入参即 mbedtls_ssl_config*），在其中把校验模式设为
// MBEDTLS_SSL_VERIFY_NONE，从而跳过对端证书验证（仅加密、不验证身份，存在中间人风险）。
static esp_err_t mqttInsecureCertAttach(void *conf) {
  mbedtls_ssl_conf_authmode((mbedtls_ssl_config *)conf, MBEDTLS_SSL_VERIFY_NONE);
  return ESP_OK;
}

void MqttService::buildClient() {
  if (!_enabled || s_client != nullptr) return;
  if (_server.length() == 0) return;

  // 启用 TLS 时：从末尾槽申请一个 TLS 通道（长连接，避免占用临时通道的前部槽）。
  // 拿不到（4 通道全忙）则延后建连，下个 tick 重试（非阻塞，避免卡住主循环）。
  if (_useTls) {
    unsigned long now = millis();
    if (now < _mqttTlsNextTryMs) return;
    _tlsGuard = TlsTurn("MQTT-TLS", 0, /*preferBack=*/true);
    if (!_tlsGuard.held()) {
      _mqttTlsNextTryMs = now + 5000;
      if (_log) _log->addf(LogLevel::kWarn, "mqtt",
                           "TLS 通道已满，MQTT 延后建连（5s 后重试）");
      return;
    }
  }

  // 规范 server：去除可能误带的协议前缀(mqtt://、mqtts://)、多余的 "//" 与行首 ":"
  String host = _server;
  host.trim();
  int schemeEnd = host.indexOf("://");
  if (schemeEnd >= 0) host = host.substring(schemeEnd + 3);
  while (host.startsWith("//")) host = host.substring(2);
  while (host.startsWith(":"))  host = host.substring(1);
  host.trim();

  s_uri  = _useTls ? "mqtts://" : "mqtt://";
  s_uri += host;
  s_uri += ":";
  s_uri += String(_port > 0 ? _port : 1883);
  if (_clientId.length() > 0) {
    s_cid = _clientId;
  } else {
    // 默认前缀 "SMSFwd-" 仅 7 字符，deviceId 为完整芯片 MAC（12 个 16 进制字符），
    // 拼接后共 19 字符，整体控制在 MQTT 3.1.1 规定的 ClientID 23 字符上限内。
    s_cid = "SMSFwd-" + deviceId();
  }
  s_will = fullTopic("status");
  {
    JsonDocument wdoc(psramJsonAllocator());
    wdoc["type"]      = "status";
    wdoc["status"]    = "offline";
    wdoc["device"]    = deviceName();
    wdoc["deviceId"]  = deviceId();
    String wp;
    serializeJson(wdoc, wp);
    s_willMsg = wp;
  }
  s_user = _user;
  s_pass = _pass;

  // Arduino 3.x（esp-mqtt 随 ESP-IDF 5.x 升级）把扁平字段重组为嵌套结构体
  esp_mqtt_client_config_t mqtt_cfg = {};
  mqtt_cfg.broker.address.uri = s_uri.c_str();
  if (s_user.length() > 0) mqtt_cfg.credentials.username = s_user.c_str();
  if (s_pass.length() > 0) mqtt_cfg.credentials.authentication.password = s_pass.c_str();
  mqtt_cfg.credentials.client_id   = s_cid.c_str();
  mqtt_cfg.session.keepalive       = _keepAlive > 0 ? _keepAlive : 60;
  mqtt_cfg.session.last_will.topic = s_will.c_str();
  mqtt_cfg.session.last_will.msg   = s_willMsg.c_str();
  mqtt_cfg.session.last_will.qos   = 0;
  mqtt_cfg.session.last_will.retain = 0;  // 非保留：避免重启后新订阅者先收到陈旧 offline
  mqtt_cfg.network.reconnect_timeout_ms = 5000;
  mqtt_cfg.buffer.size = 4096;            // 启动/状态消息含多字段 JSON，放大缓冲避免单帧超限

  if (_useTls) {
    // 跳过对端证书校验（仅加密、不验证身份，存在中间人风险，仅适合可信局域网环境）
    mqtt_cfg.broker.verification.skip_cert_common_name_check = true;
    mqtt_cfg.broker.verification.crt_bundle_attach = mqttInsecureCertAttach;
  }

  s_client = esp_mqtt_client_init(&mqtt_cfg);
  if (s_client != nullptr) {
    esp_mqtt_client_register_event(s_client, MQTT_EVENT_ANY, mqtt_event_handler, nullptr);
    esp_mqtt_client_start(s_client);
  }
}

void MqttService::destroyClient() {
  if (s_client == nullptr) {
    _tlsGuard.release();  // 释放 TLS 通道（_useTls 时才占着，release 幂等）
    return;
  }
  esp_mqtt_client_stop(s_client);
  esp_mqtt_client_destroy(s_client);
  s_client = nullptr;
  s_connected = false;
  _tlsGuard.release();    // 释放 TLS 通道（_useTls 时才占着，release 幂等）
}

// ── 对外接口 ───────────────────────────────────────────────
void MqttService::begin(WifiManager &wifi, SmsService &sms, TimeSync &time,
                        LogStore &log) {
  _wifi = &wifi;
  _sms  = &sms;
  _time = &time;
  _log  = &log;
  if (s_cmdQueue == nullptr) {
    // 命令队列（8×512B ≈ 4KB）优先放 PSRAM，与 DRAM/LwIP/SSL 隔离；
    // 无 PSRAM 或分配失败时退化到内部堆，功能不变。
    s_cmdQueue = xQueueCreateWithCaps(8, sizeof(MqttCmdMsg), MALLOC_CAP_SPIRAM);
    if (s_cmdQueue == nullptr) s_cmdQueue = xQueueCreate(8, sizeof(MqttCmdMsg));
  }
  // 每次开机重置，使首次连接可补发启动通知
  s_bootNotified = false;
  s_pendingBoot  = false;
  // 始终注册 sink：onLog 内部会按当前配置判定是否真正推送，
  // 这样运行时通过网页启用 MQTT 也能立即生效，无需重启。
  setLogSink(onLog);
  loadConfig();
}

void MqttService::reconfigure() {
  destroyClient();
  if (_enabled && _server.length() > 0) buildClient();
}

void MqttService::tick() {
  if (!_enabled) {
    if (s_client != nullptr) destroyClient();
    return;
  }
  if (!isConfigured()) {
    if (s_client != nullptr) destroyClient();
    return;
  }
  // 等 WiFi 就绪再建连；WiFi 掉线交给 esp-mqtt 自动重连
  if (WiFi.status() != WL_CONNECTED) return;
  if (s_client == nullptr) buildClient();

  // 连接刚建立（本 tick 检测到跳变）时，在主循环完成订阅 / 上线状态 / 启动通知补发
  if (s_connected && !s_wasConnected) {
    s_wasConnected = true;
    if (_subscribeEnabled) {
      String cmdTopic = fullTopic("command");
      esp_mqtt_client_subscribe(s_client, cmdTopic.c_str(), 0);
    }
    publishStatus("online");
    notifyOnline();   // 上线事件（按 kMqttNotifyOnline 位决定是否发布，每次（重）连接发布一次）
    if (!s_bootNotified) s_pendingBoot = true;
  }
  if (!s_connected) s_wasConnected = false;

  // 执行入队命令
  if (s_cmdQueue != nullptr) {
    MqttCmdMsg msg;  // 队列元素定义于文件作用域
    while (xQueueReceive(s_cmdQueue, &msg, 0) == pdTRUE) {
      handleCommand(String(msg.data));
    }
  }

  // 连接建立后补发启动通知（避免开机时 MQTT 未连上导致启动事件被丢弃）
  if (s_pendingBoot && s_connected) {
    s_pendingBoot = false;
    publishEvent(MqttMsgType::kBoot, "设备", "🚀 设备已启动（MQTT 已连接）",
                 0, "", "", 0);
    s_bootNotified = true;
  }
}

// ---------------- 配置读写 ----------------
bool MqttService::enabled() const { return _enabled; }

bool MqttService::setEnabled(bool on) {
  if (_enabled == on) return true;
  _enabled = on;
  if (!on) {
    destroyClient();           // 停用：停止连接（tick 也不再重建）
  }
  return persistConfig();
}

bool MqttService::isConfigured() {
  return _server.length() > 0;
}

bool MqttService::isEnabled() {
  return _enabled;
}

bool MqttService::connected() {
  return s_connected;
}

const String &MqttService::lastError() {
  return s_lastError;
}

bool MqttService::notifyEnabled(uint32_t mask) const {
  return (_notifyTypes & mask) != 0;
}

bool MqttService::saveConfig(const String &server, uint16_t port, bool useTls,
                            const String &user, const String &password,
                            const String &clientId, const String &topicPrefix,
                            uint16_t keepAlive, bool subscribeEnabled,
                            uint32_t notifyTypes, bool enabled) {
  _server           = server;
  _server.trim();
  _port             = (port > 0 && port <= 65535) ? port : 1883;
  _useTls           = useTls;
  _user             = user;
  _user.trim();
  if (password.length() > 0) _pass = password;  // 空串保持已保存的密码
  _clientId         = clientId;
  _clientId.trim();
  _topicPrefix      = topicPrefix;
  _topicPrefix.trim();
  if (_topicPrefix.length() == 0) _topicPrefix = "smsf";
  _keepAlive        = (keepAlive > 0 && keepAlive <= 3600) ? keepAlive : 60;
  _subscribeEnabled = subscribeEnabled;
  _notifyTypes      = notifyTypes & cfg::kMqttNotifyAll;
  _enabled          = enabled;

  if (!persistConfig()) return false;
  // 保存后立即重建客户端（无需重启即可生效）
  reconfigure();
  return true;
}

String MqttService::configJson() const {
  JsonDocument doc(psramJsonAllocator());
  doc["ok"]         = true;
  doc["enabled"]    = _enabled;
  doc["configured"] = isConfigured();
  doc["connected"]  = s_connected;   // 当前是否真正连上（实时，跨任务安全读取）
  doc["error"]      = s_lastError;   // 最近一次连接错误描述（空串=无错误）
  doc["server"]     = _server;
  doc["port"]       = _port;
  doc["useTls"]     = _useTls;
  doc["user"]       = _user;
  doc["hasPass"]    = _pass.length() > 0;
  doc["clientId"]   = _clientId;
  doc["topicPrefix"]= _topicPrefix;
  doc["keepAlive"]  = _keepAlive;
  doc["subscribeEnabled"] = _subscribeEnabled;
  doc["notifyTypes"]= _notifyTypes;
  String out;
  serializeJson(doc, out);
  return out;
}

void MqttService::exportTo(JsonObject &o) const {
  o["enabled"]          = _enabled;
  o["server"]           = _server;
  o["port"]             = _port;
  o["useTls"]           = _useTls;
  o["user"]             = _user;
  o["pass"]             = _pass;            // 备份用，含密码
  o["clientId"]         = _clientId;
  o["topicPrefix"]      = _topicPrefix;
  o["keepAlive"]        = _keepAlive;
  o["subscribeEnabled"] = _subscribeEnabled;
  o["notifyTypes"]      = _notifyTypes;
}

bool MqttService::importFrom(const JsonObject &o) {
  _enabled  = o["enabled"] | _enabled;
  _server   = o["server"] | _server;
  int port  = o["port"] | (int)_port;
  _port     = (port > 0 && port <= 65535) ? (uint16_t)port : _port;
  _useTls   = o["useTls"] | _useTls;
  _user     = o["user"] | _user;
  if (o["pass"].is<const char *>()) _pass = o["pass"].as<String>();
  _clientId = o["clientId"] | _clientId;
  _topicPrefix = o["topicPrefix"] | _topicPrefix;
  if (_topicPrefix.length() == 0) _topicPrefix = "smsf";
  int ka = o["keepAlive"] | (int)_keepAlive;
  _keepAlive = (ka > 0 && ka <= 3600) ? (uint16_t)ka : 60;
  _subscribeEnabled = o["subscribeEnabled"] | _subscribeEnabled;
  uint32_t types = o["notifyTypes"] | _notifyTypes;
  _notifyTypes = types & cfg::kMqttNotifyAll;
  if (!persistConfig()) return false;
  reconfigure();
  return true;
}

bool MqttService::loadConfig() {
  Preferences prefs;
  String json;
  if (prefs.begin(cfg::kNvsNamespace, true)) {
    json = prefs.getString(cfg::kNvsKeyMqtt, "");
    prefs.end();
  }
  if (!json.length()) {
    if (_log) _log->addf(LogLevel::kInfo, "mqtt", "NVS 无 MQTT 配置，使用默认值（功能关闭）");
    return false;
  }
  JsonDocument doc(psramJsonAllocator());
  if (deserializeJson(doc, json) != DeserializationError::Ok) {
    if (_log) _log->addf(LogLevel::kWarn, "mqtt", "MQTT 配置解析失败，使用默认值");
    return false;
  }
  _enabled          = doc["enabled"] | false;   // NVS 无该字段默认关闭
  _server           = doc["server"] | "";
  int port          = doc["port"] | 1883;
  _port             = (port > 0 && port <= 65535) ? (uint16_t)port : 1883;
  _useTls           = doc["useTls"] | false;
  _user             = doc["user"] | "";
  _pass             = doc["pass"] | "";
  _clientId         = doc["clientId"] | "";
  _topicPrefix      = doc["topicPrefix"] | "smsf";
  int ka            = doc["keepAlive"] | 60;
  _keepAlive        = (ka > 0 && ka <= 3600) ? (uint16_t)ka : 60;
  _subscribeEnabled = doc["subscribeEnabled"] | true;
  uint32_t types    = doc["notifyTypes"] | (int)cfg::kMqttNotifyDefault;
  _notifyTypes      = types & cfg::kMqttNotifyAll;
  return true;
}

bool MqttService::persistConfig() {
  JsonDocument doc(psramJsonAllocator());
  doc["ver"]              = 1;
  doc["enabled"]          = _enabled;
  doc["server"]           = _server;
  doc["port"]             = _port;
  doc["useTls"]           = _useTls;
  doc["user"]             = _user;
  doc["pass"]             = _pass;
  doc["clientId"]         = _clientId;
  doc["topicPrefix"]      = _topicPrefix;
  doc["keepAlive"]        = _keepAlive;
  doc["subscribeEnabled"] = _subscribeEnabled;
  doc["notifyTypes"]      = _notifyTypes;
  String out;
  serializeJson(doc, out);
  Preferences prefs;
  if (!prefs.begin(cfg::kNvsNamespace, false)) return false;
  size_t written = prefs.putString(cfg::kNvsKeyMqtt, out);
  prefs.end();
  return written == out.length();
}

// ---------------- 事件上报 ----------------
void MqttService::notifySms(const String &sender, const String &message, uint32_t ts,
                            int8_t modem) {
  if (!_enabled || !isConfigured()) return;
  if (!(_notifyTypes & cfg::kMqttNotifySms)) return;
  String label = (modem >= 1 && modem <= cfg::kSmsMaxModems)
                     ? ("通信模组" + String(modem)) : String("");
  publishEvent(MqttMsgType::kSms, sender, message, ts, "", label, modem);
}

void MqttService::notifySim(uint8_t port, bool ready) {
  if (!_enabled || !isConfigured()) return;
  if (!(_notifyTypes & cfg::kMqttNotifySim)) return;
  publishEvent(MqttMsgType::kSim, "设备", ready ? "ready" : "removed",
               0, "", ("通信模组" + String(port)), (int8_t)port);
}

void MqttService::notifyCall(const String &number, uint32_t ts, int8_t modem) {
  if (!_enabled || !isConfigured()) return;
  if (!(_notifyTypes & cfg::kMqttNotifyCall)) return;
  String label = (modem >= 1 && modem <= cfg::kSmsMaxModems)
                     ? ("通信模组" + String(modem)) : String("");
  publishEvent(MqttMsgType::kCall, number, "来电", ts, "", label, modem);
}

void MqttService::notifyStartup() {
  // 启动通知由 tick() 在首次连上时统一发布一次（避免开机 MQTT 未连上导致丢弃）
  s_pendingBoot = true;
}

void MqttService::notifyOnline() {
  if (!_enabled || !isConfigured()) return;
  if (!(_notifyTypes & cfg::kMqttNotifyOnline)) return;
  uint32_t ts = (_time && _time->isSynced()) ? (uint32_t)time(nullptr)
                                             : (uint32_t)(millis() / 1000);
  publishEvent(MqttMsgType::kOnline, "设备", "设备已上线", ts, "", "", 0);
}

bool MqttService::notifyTest() {
  if (!_enabled || !isConfigured()) return false;
  uint32_t ts = (_time && _time->isSynced()) ? (uint32_t)time(nullptr)
                                             : (uint32_t)(millis() / 1000);
  publishEvent(MqttMsgType::kOnline, "设备", "✅ MQTT 测试消息", ts, "", "", 0);
  return true;
}

#endif // ENABLE_MQTT
