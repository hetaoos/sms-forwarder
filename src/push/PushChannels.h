#pragma once
#include <Arduino.h>
#include <ArduinoJson.h>
#include "push/PushTypes.h"

class SmsService;  // 前向声明：SMS 备份通道经指针回调发送，避免头文件循环依赖

// 两次 HTTP 请求之间的最小冷却时间，防止密集请求导致 ESP32 TLS/TCP 栈资源
// （mbedtls 堆、lwIP socket）尚未充分释放就开始下一次 TLS 握手。
constexpr unsigned long HTTP_COOLDOWN_MS = 500;

// 消息体类型：DEFAULT = 原始短信内容，CUSTOM = 用户自定义渲染后的 body。
// content 均已经过 sanitizeText 净化。
enum PushBodyType { PUSH_BODY_DEFAULT, PUSH_BODY_CUSTOM };
struct PushBody {
  PushBodyType type;
  String       content;  // 已净化：CUSTOM = 自定义模板渲染结果，DEFAULT = 原始短信
  String       raw;      // 已净化的原始短信文本（不受自定义模板影响，供邮件类通道取用）
};

// 各推送通道的具体发送实现。
// 约定：message.type == PUSH_BODY_CUSTOM 时 message.content 是用户自定义请求体，
//   否则 message.content 是原始短信文本；两者均已净化。
// 所有方法都是同步阻塞的；返回 true 表示 HTTP 状态码符合该通道的成功语义。
//
// 重要：每个发送函数内部都使用**栈上/堆上**的 WiFiClientSecure / HTTPClient，
// 严禁在并发推送或与 OTA 同时下载时共享 TLS 上下文（由 TlsTurn 串行化）。
class PushChannels {
 public:
  static bool sendPostJson  (const PushChannel& ch, const String& sender, const PushBody& message, const String& timestamp);
  static bool sendBark      (const PushChannel& ch, const String& sender, const PushBody& message, const String& timestamp);
  static bool sendGet       (const PushChannel& ch, const String& sender, const PushBody& message, const String& timestamp);
  static bool sendDingtalk  (const PushChannel& ch, const String& sender, const PushBody& message, const String& timestamp);
  static bool sendPushPlus  (const PushChannel& ch, const String& sender, const PushBody& message, const String& timestamp);
  static bool sendServerChan(const PushChannel& ch, const String& sender, const PushBody& message, const String& timestamp);
  static bool sendCustom    (const PushChannel& ch, const String& sender, const PushBody& message, const String& timestamp);
  static bool sendFeishu    (const PushChannel& ch, const String& sender, const PushBody& message, const String& timestamp);
  static bool sendGotify    (const PushChannel& ch, const String& sender, const PushBody& message, const String& timestamp);
  static bool sendTelegram  (const PushChannel& ch, const String& sender, const PushBody& message, const String& timestamp);
  static bool sendWechatWork(const PushChannel& ch, const String& sender, const PushBody& message, const String& timestamp);
  static bool sendMailgun   (const PushChannel& ch, const String& sender, const PushBody& message, const String& timestamp, const String& modemLabel = "");

  // SMS 短信备份（唯一不经 HTTP 的通道）：把消息经模组以短信发到 ch.url
  // （目标手机号），用于网络不可用时的兜底通知。type 用于区分来电/短信默认格式；
  // sms 为短信服务（发送入口），为空时视为不可用。
  static bool sendSmsBackup (const PushChannel& ch, const String& sender, const PushBody& message,
                             const String& timestamp, MsgType type, SmsService* sms);
};
