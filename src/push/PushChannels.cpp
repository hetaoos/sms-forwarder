#include "push/PushChannels.h"
#include "utils/HttpSession.h"
#include "utils/TlsTurn.h"
#include "log/LogStore.h"

#include <ArduinoJson.h>
#include <memory>
#include <mbedtls/md.h>
#include <mbedtls/base64.h>
#include <sys/time.h>

// 设备日志（与 EmailService 一致，指向全局唯一实例）。
static LogStore &_log = devLog();

// ---------- helpers ----------

static String urlEncode(const String& str) {
  String encoded;
  for (unsigned int i = 0; i < str.length(); i++) {
    unsigned char c = (unsigned char)str.charAt(i);
    if (c == ' ') {
      encoded += '+';
    } else if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
      encoded += (char)c;
    } else {
      char buf[4];
      snprintf(buf, sizeof(buf), "%%%02X", c);
      encoded += buf;
    }
  }
  return encoded;
}

// 用 mbedtls 做标准 base64 编码（避免依赖外部 base64 库）
static String base64Encode(const uint8_t* data, size_t len) {
  size_t olen = 0;
  if (mbedtls_base64_encode(nullptr, 0, &olen, data, len) != MBEDTLS_ERR_BASE64_BUFFER_TOO_SMALL) {
    // 首次调用（dst=nullptr）仅计算长度，返回 MBEDTLS_ERR_BASE64_BUFFER_TOO_SMALL 属正常
  }
  if (olen == 0) return "";
  uint8_t* buf = (uint8_t*)malloc(olen);
  if (!buf) return "";
  size_t written = 0;
  if (mbedtls_base64_encode(buf, olen, &written, data, len) != 0) {
    free(buf);
    return "";
  }
  String out;
  out.reserve(written);
  for (size_t i = 0; i < written; i++) out += (char)buf[i];
  free(buf);
  return out;
}

static String computeHmacSha256Base64(const String& key, const String& data) {
  uint8_t hmacResult[32];
  mbedtls_md_context_t ctx;
  mbedtls_md_init(&ctx);
  mbedtls_md_setup(&ctx, mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), 1);
  mbedtls_md_hmac_starts(&ctx, (const unsigned char*)key.c_str(), key.length());
  mbedtls_md_hmac_update(&ctx, (const unsigned char*)data.c_str(), data.length());
  mbedtls_md_hmac_finish(&ctx, hmacResult);
  mbedtls_md_free(&ctx);
  return base64Encode(hmacResult, 32);
}

static String dingtalkSign(const String& secret, int64_t timestamp) {
  return urlEncode(computeHmacSha256Base64(secret, String(timestamp) + "\n" + secret));
}

static int64_t getUtcMillis() {
  struct timeval tv;
  if (gettimeofday(&tv, NULL) == 0) {
    return (int64_t)tv.tv_sec * 1000LL + tv.tv_usec / 1000;
  }
  return (int64_t)time(nullptr) * 1000LL;
}

// 上次 HTTP 请求完成的时刻（millis()）；0 表示尚未发送过任何请求。
static unsigned long s_lastHttpEndMs = 0;

// ── TLS 轮次：把 HTTP 会话与它所持有的 TLS 轮次绑在一起 ──────────────
// 与 OTA 检查 / SMTP 邮件等其它 HTTPS 流量互斥，避免同时建出第二个 TLS 上下文
// 导致 MBEDTLS_ERR_SSL_ALLOC_FAILED（-32512）。
struct TlsSession {
  std::unique_ptr<HttpSession> session;
  TlsTurn turn;
  explicit operator bool() const { return (bool)session && turn.held(); }
};

static constexpr uint32_t TLS_TURN_WAIT_MS = 10000;

// 先冷却（在取 TLS 轮次之前，绝不能占着锁睡），再取轮次，最后建连。
static TlsSession request(const String& url) {
  unsigned long now = millis();
  if (s_lastHttpEndMs > 0 && now - s_lastHttpEndMs < HTTP_COOLDOWN_MS) {
    delay(HTTP_COOLDOWN_MS - (now - s_lastHttpEndMs));
  }
  TlsSession ts;
  ts.turn = TlsTurn(TLS_TURN_WAIT_MS);
  if (!ts.turn.held()) {
    _log.addf(LogLevel::kWarn, "pushch", "TLS 通道被占用，跳过本次推送");
    return ts;
  }
  ts.session = std::unique_ptr<HttpSession>(HttpSession::request(url));
  if (!ts.session) {
    _log.addf(LogLevel::kError, "pushch", "推送：无法建立 HTTP 连接");
  }
  return ts;
}

// 记录响应结果并更新冷却时间戳；失败时额外打印首 120 字节响应 body。
// bodyMarker 非空时，2xx 也需要响应体包含该标记才算成功。
static bool isResponseSuccessful(HttpSession* session, int code, const char* bodyMarker = nullptr) {
  bool ok = code >= 200 && code < 300;
  if (ok && bodyMarker != nullptr) {
    String resp = session->http()->getString();
    if (resp.indexOf(bodyMarker) < 0) {
      if (resp.length() > 120) resp = resp.substring(0, 120) + "...";
      _log.addf(LogLevel::kWarn, "pushch", "响应码: %d 但响应体缺少成功标记，视为失败: %s", code, resp.c_str());
      ok = false;
    }
  }
  if (ok) {
    _log.addf(LogLevel::kInfo, "pushch", "响应码: %d 成功", code);
  } else if (bodyMarker == nullptr || code < 200 || code >= 300) {
    String resp = session->http()->getString();
    if (resp.length() > 120) resp = resp.substring(0, 120) + "...";
    _log.addf(LogLevel::kWarn, "pushch", "响应码: %d 失败，响应体: %s", code, resp.c_str());
  }
  s_lastHttpEndMs = millis();
  return ok;
}

// Mailgun 的短信邮件模板（自包含，String 版，避免与 EmailService 的 PsBuffer 实现耦合）。
static void buildSmsMail(const String& sender, const String& sms, const String& timestamp,
                         const String& modemLabel, String& subject, String& html, String& text) {
  subject = "短信来自: " + sender;
  text = "发送者: " + sender + "\n内容: " + sms + "\n时间: " + timestamp;
  if (modemLabel.length() > 0) {
    text += "\n模组: " + modemLabel;
  }
  // 简易 HTML 卡片（与 EmailService 视觉风格一致）
  String safeSender = sender, safeSms = sms, safeTs = timestamp;
  // 转义（参考 EmailService::escape）
  auto esc = [](const String& s) {
    String o; o.reserve(s.length());
    for (unsigned int i = 0; i < s.length(); i++) {
      char c = s.charAt(i);
      if (c == '&') o += "&amp;"; else if (c == '<') o += "&lt;";
      else if (c == '>') o += "&gt;"; else if (c == '"') o += "&quot;";
      else o += c;
    }
    return o;
  };
  safeSender = esc(safeSender); safeSms = esc(safeSms); safeTs = esc(safeTs);
  html = "<div style='font-family:-apple-system,Segoe UI,Roboto,sans-serif;max-width:560px;margin:0 auto'>"
         "<h3 style='color:#2c7be5'>短信通知</h3>"
         "<table style='border-collapse:collapse;width:100%'>"
         "<tr><td style='padding:6px 0;color:#666'>发送者</td><td style='padding:6px 0'>" + safeSender + "</td></tr>"
         "<tr><td style='padding:6px 0;color:#666'>时间</td><td style='padding:6px 0'>" + safeTs + "</td></tr>";
  if (modemLabel.length() > 0) {
    html += "<tr><td style='padding:6px 0;color:#666'>模组</td><td style='padding:6px 0'>" + esc(modemLabel) + "</td></tr>";
  }
  html += "</table><div style='margin-top:10px;padding:12px;background:#f5f7fa;border-radius:8px;white-space:pre-wrap'>"
          + safeSms + "</div></div>";
}

// ---------- channel implementations ----------

bool PushChannels::sendPostJson(const PushChannel& ch, const String& sender, const PushBody& message, const String& timestamp) {
  TlsSession ts = request(ch.url);
  if (!ts) return false;
  ts.session->http()->addHeader("Content-Type", "application/json");

  String body;
  if (message.type == PUSH_BODY_CUSTOM) {
    body = message.content;
  } else {
    JsonDocument doc;
    doc["sender"]    = sender;
    doc["message"]   = message.content;
    doc["timestamp"] = timestamp;
    serializeJson(doc, body);
  }

  _log.addf(LogLevel::kInfo, "pushch", "POST JSON to %s", ch.url.c_str());
  int code = ts.session->http()->POST(body);
  return isResponseSuccessful(ts.session.get(), code);
}

bool PushChannels::sendBark(const PushChannel& ch, const String& sender, const PushBody& message, const String& timestamp) {
  TlsSession ts = request(ch.url);
  if (!ts) return false;
  ts.session->http()->addHeader("Content-Type", "application/json");

  JsonDocument doc;
  String title = ch.key1.length() > 0 ? ch.key1 : sender;
  doc["title"] = title;
  doc["body"]  = message.content;
  String body;
  serializeJson(doc, body);

  _log.addf(LogLevel::kInfo, "pushch", "Bark to %s", ch.url.c_str());
  int code = ts.session->http()->POST(body);
  return isResponseSuccessful(ts.session.get(), code);
}

bool PushChannels::sendGet(const PushChannel& ch, const String& sender, const PushBody& message, const String& timestamp) {
  String url = ch.url;
  url += (url.indexOf('?') == -1) ? "?" : "&";
  url += "sender=" + urlEncode(sender);
  url += "&message=" + urlEncode(message.content);
  url += "&timestamp=" + urlEncode(timestamp);

  _log.addf(LogLevel::kInfo, "pushch", "GET %s", url.c_str());
  TlsSession ts = request(url);
  if (!ts) return false;
  int code = ts.session->http()->GET();
  return isResponseSuccessful(ts.session.get(), code);
}

bool PushChannels::sendDingtalk(const PushChannel& ch, const String& sender, const PushBody& message, const String& timestamp) {
  String webhookUrl = ch.url;

  if (ch.key1.length() > 0) {
    int64_t ts = getUtcMillis();
    String sign = dingtalkSign(ch.key1, ts);
    webhookUrl += (webhookUrl.indexOf('?') == -1) ? "?" : "&";
    char tsBuf[21];
    snprintf(tsBuf, sizeof(tsBuf), "%lld", ts);
    webhookUrl += "timestamp=" + String(tsBuf) + "&sign=" + sign;
  }

  TlsSession ts = request(webhookUrl);
  if (!ts) return false;
  ts.session->http()->addHeader("Content-Type", "application/json");

  String content = message.type == PUSH_BODY_CUSTOM ? message.content
          : ("📱短信通知\n发送者: " + sender + "\n内容: " + message.content + "\n时间: " + timestamp);

  JsonDocument doc;
  doc["msgtype"] = "text";
  doc["text"]["content"] = content;
  String body;
  serializeJson(doc, body);

  _log.addf(LogLevel::kInfo, "pushch", "DingTalk: %s", body.c_str());
  int code = ts.session->http()->POST(body);
  return isResponseSuccessful(ts.session.get(), code);
}

bool PushChannels::sendPushPlus(const PushChannel& ch, const String& sender, const PushBody& message, const String& timestamp) {
  String url = ch.url.length() > 0 ? ch.url : "http://www.pushplus.plus/send";
  TlsSession ts = request(url);
  if (!ts) return false;
  ts.session->http()->addHeader("Content-Type", "application/json");

  String channelValue = "wechat";
  if (ch.key2.length() > 0) {
    if (ch.key2 == "wechat" || ch.key2 == "extension" || ch.key2 == "app") {
      channelValue = ch.key2;
    } else {
      _log.addf(LogLevel::kWarn, "pushch", "Invalid PushPlus channel '%s'. Using default 'wechat'.", ch.key2.c_str());
    }
  }

  String content = message.type == PUSH_BODY_CUSTOM ? message.content
          : ("<b>发送者:</b> " + sender + "<br><b>时间:</b> " + timestamp + "<br><b>内容:</b><br>" + message.content);

  JsonDocument doc;
  doc["token"]   = ch.key1;
  doc["title"]   = "短信来自: " + sender;
  doc["content"] = content;
  doc["channel"] = channelValue;
  String body;
  serializeJson(doc, body);

  _log.addf(LogLevel::kInfo, "pushch", "PushPlus: %s", body.c_str());
  int code = ts.session->http()->POST(body);
  return isResponseSuccessful(ts.session.get(), code);
}

bool PushChannels::sendServerChan(const PushChannel& ch, const String& sender, const PushBody& message, const String& timestamp) {
  String url = ch.url.length() > 0 ? ch.url : ("https://sctapi.ftqq.com/" + ch.key1 + ".send");
  TlsSession ts = request(url);
  if (!ts) return false;
  ts.session->http()->addHeader("Content-Type", "application/x-www-form-urlencoded");

  String desp = message.type == PUSH_BODY_CUSTOM ? message.content
          : ("**发送者:** " + sender + "\n\n**时间:** " + timestamp + "\n\n**内容:**\n\n" + message.content);
  String postData = "title=" + urlEncode("短信来自: " + sender);
  postData += "&desp=" + urlEncode(desp);

  _log.addf(LogLevel::kInfo, "pushch", "Server酱: %s", postData.c_str());
  int code = ts.session->http()->POST(postData);
  return isResponseSuccessful(ts.session.get(), code);
}

bool PushChannels::sendCustom(const PushChannel& ch, const String& sender, const PushBody& message, const String& timestamp) {
  TlsSession ts = request(ch.url);
  if (!ts) return false;
  ts.session->http()->addHeader("Content-Type", "application/json");

  String body = message.content;
  _log.addf(LogLevel::kInfo, "pushch", "POST请求: %s，body长度: %d", ch.url.c_str(), body.length());
  int code = ts.session->http()->POST(body);
  return isResponseSuccessful(ts.session.get(), code);
}

bool PushChannels::sendFeishu(const PushChannel& ch, const String& sender, const PushBody& message, const String& timestamp) {
  TlsSession ts = request(ch.url);
  if (!ts) return false;
  ts.session->http()->addHeader("Content-Type", "application/json");

  JsonDocument doc;

  if (ch.key1.length() > 0) {
    int64_t ts = time(nullptr);
    doc["timestamp"] = String(ts);
    doc["sign"]      = computeHmacSha256Base64(ch.key1, String(ts) + "\n" + ch.key1);
  }

  String text = message.type == PUSH_BODY_CUSTOM ? message.content
          : ("📱短信通知\n发送者: " + sender + "\n内容: " + message.content + "\n时间: " + timestamp);
  doc["msg_type"]        = "text";
  doc["content"]["text"] = text;

  String body;
  serializeJson(doc, body);

  _log.addf(LogLevel::kInfo, "pushch", "飞书: %s", body.c_str());
  int code = ts.session->http()->POST(body);
  return isResponseSuccessful(ts.session.get(), code);
}

bool PushChannels::sendGotify(const PushChannel& ch, const String& sender, const PushBody& message, const String& timestamp) {
  String url = ch.url;
  if (!url.endsWith("/")) url += "/";
  url += "message?token=" + ch.key1;
  TlsSession ts = request(url);
  if (!ts) return false;
  ts.session->http()->addHeader("Content-Type", "application/json");

  String msg = message.type == PUSH_BODY_CUSTOM ? message.content : (message.content + "\n\n时间: " + timestamp);
  JsonDocument doc;
  doc["title"]    = "短信来自: " + sender;
  doc["message"]  = msg;
  doc["priority"] = 5;
  String body;
  serializeJson(doc, body);

  _log.addf(LogLevel::kInfo, "pushch", "Gotify: %s", body.c_str());
  int code = ts.session->http()->POST(body);
  return isResponseSuccessful(ts.session.get(), code);
}

bool PushChannels::sendTelegram(const PushChannel& ch, const String& sender, const PushBody& message, const String& timestamp) {
  String baseUrl = ch.url.length() > 0 ? ch.url : "https://api.telegram.org";
  if (baseUrl.endsWith("/")) baseUrl.remove(baseUrl.length() - 1);
  String url = baseUrl + "/bot" + ch.key2 + "/sendMessage";
  TlsSession ts = request(url);
  if (!ts) return false;
  ts.session->http()->addHeader("Content-Type", "application/json");

  String text = message.type == PUSH_BODY_CUSTOM ? message.content
          : ("📱短信通知\n发送者: " + sender + "\n内容: " + message.content + "\n时间: " + timestamp);
  JsonDocument doc;
  doc["chat_id"] = ch.key1;
  doc["text"]    = text;
  String body;
  serializeJson(doc, body);

  _log.addf(LogLevel::kInfo, "pushch", "Telegram: %s", body.c_str());
  int code = ts.session->http()->POST(body);
  return isResponseSuccessful(ts.session.get(), code);
}

bool PushChannels::sendWechatWork(const PushChannel& ch, const String& sender, const PushBody& message, const String& timestamp) {
  String webhookUrl = ch.url;

  if (ch.key1.length() > 0) {
    int64_t ts = getUtcMillis();
    char tsBuf[21];
    snprintf(tsBuf, sizeof(tsBuf), "%lld", ts);
    webhookUrl += (webhookUrl.indexOf('?') == -1) ? "?" : "&";
    webhookUrl += "timestamp=" + String(tsBuf) + "&sign=" + urlEncode(computeHmacSha256Base64(ch.key1, String(tsBuf) + "\n" + ch.key1));
  }

  TlsSession ts = request(webhookUrl);
  if (!ts) return false;
  ts.session->http()->addHeader("Content-Type", "application/json");

  String content = message.type == PUSH_BODY_CUSTOM ? message.content
          : ("📱短信通知\n发件人: " + sender + "\n内容: " + message.content + "\n时间: " + timestamp);
  JsonDocument doc;
  doc["msgtype"] = "text";
  doc["text"]["content"] = content;
  String body;
  serializeJson(doc, body);

  _log.addf(LogLevel::kInfo, "pushch", "企业微信: %s", body.c_str());
  int code = ts.session->http()->POST(body);
  return isResponseSuccessful(ts.session.get(), code);
}

// Mailgun：走 TLS 且要上传表单，单次请求上限比普通通道放宽。
static constexpr uint16_t MAILGUN_TIMEOUT_MS = 15000;

bool PushChannels::sendMailgun(const PushChannel& ch, const String& sender, const PushBody& message, const String& timestamp, const String& modemLabel) {
  if (ch.key1.length() == 0 || ch.key2.length() == 0 || ch.key3.length() == 0) {
    _log.addf(LogLevel::kWarn, "pushch", "Mailgun 配置不完整（需 API Key / 域名 / 收件人），跳过");
    return false;
  }

  String base = ch.url;
  base.trim();
  if (base.length() == 0) base = "https://api.mailgun.net";
  if (!base.startsWith("http://") && !base.startsWith("https://")) base = "https://" + base;
  while (base.endsWith("/")) base.remove(base.length() - 1);

  String domain = ch.key2;
  domain.trim();

  String from = ch.key4;
  from.trim();
  if (from.length() == 0) from = "SMS Notification <sms@" + domain + ">";

  String sms = message.raw.length() > 0 ? message.raw : message.content;
  String defSubject, defHtml, defText;
  buildSmsMail(sender, sms, timestamp, modemLabel, defSubject, defHtml, defText);

  String subject = ch.key5.length() > 0 ? ch.key5 : defSubject;
  String html = message.type == PUSH_BODY_CUSTOM ? message.content : defHtml;
  String text = defText;

  String body = "from=" + urlEncode(from);

  String tos = ch.key3;
  tos.replace(";", ",");
  int pos = 0;
  while (pos <= tos.length()) {
    int next = tos.indexOf(',', pos);
    if (next < 0) next = tos.length();
    String one = tos.substring(pos, next);
    one.trim();
    if (one.length() > 0) body += "&to=" + urlEncode(one);
    pos = next + 1;
  }

  body += "&subject=" + urlEncode(subject);
  body += "&html=" + urlEncode(html);
  body += "&text=" + urlEncode(text);

  String url = base + "/v3/" + domain + "/messages";
  TlsSession ts = request(url);
  if (!ts) return false;
  ts.session->http()->setTimeout(MAILGUN_TIMEOUT_MS);
  ts.session->http()->addHeader("Content-Type", "application/x-www-form-urlencoded; charset=utf-8");
  String basicAuth = "api:" + ch.key1;
  String authHeader = "Basic " + base64Encode((const uint8_t*)basicAuth.c_str(), basicAuth.length());
  ts.session->http()->addHeader("Authorization", authHeader);

  _log.addf(LogLevel::kInfo, "pushch", "Mailgun: %s 主题=%s", domain.c_str(), subject.c_str());
  int code = ts.session->http()->POST(body);
  return isResponseSuccessful(ts.session.get(), code, "Queued");
}
