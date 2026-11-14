#pragma once
#include <Arduino.h>
#include <ArduinoJson.h>

// 推送通道类型（移植自 esp32-sms-forwarding）。
// 仅纳入 webhook 类通道；MAILGUN 走 Web API（Basic 认证），同样受本框架调度。
// PUSH_TYPE_SMS（短信回传备份）不经 HTTP：网络不可用时改走模组发短信，
// 是「互联网全断」场景下的最后通知手段，故不受 isHttpPush 的 WiFi 判定约束。
enum PushType {
  PUSH_TYPE_NONE         = 0,   // 占位/未配置
  PUSH_TYPE_POST_JSON    = 1,   // 通用 HTTP POST：按固定 JSON 结构 POST 到 url
  PUSH_TYPE_BARK         = 2,   // iOS Bark：GET/POST url + title/body
  PUSH_TYPE_GET          = 3,   // 通用 HTTP GET：url 拼接查询参数（如 Server酱类）
  PUSH_TYPE_DINGTALK     = 4,   // 钉钉自定义机器人：markdown 消息 POST
  PUSH_TYPE_PUSHPLUS     = 5,   // Pushplus 推送加
  PUSH_TYPE_SERVERCHAN   = 6,   // Server 酱（GET）
  PUSH_TYPE_CUSTOM       = 7,   // 自定义：用 customBody 模板生成请求体，支持占位符
  PUSH_TYPE_FEISHU       = 8,   // 飞书自定义机器人（签名 + 富文本）
  PUSH_TYPE_GOTIFY       = 9,   // Gotify：自托管 Android 推送
  PUSH_TYPE_TELEGRAM     = 10,  // Telegram Bot：通过 Bot API 发 sendMessage
  PUSH_TYPE_WECHAT_WORK  = 11,  // 企业微信机器人（签名）
  PUSH_TYPE_SMS          = 12,  // 短信回传备份：把消息经 SIM 卡以短信发到 url（目标手机号）
  PUSH_TYPE_MAILGUN      = 13   // Mailgun 邮件 API（HTTP POST，Basic 认证）
};

// 推送策略：决定「一条消息」如何分发到多个已启用通道。
enum PushStrategy {
  PUSH_STRATEGY_BROADCAST = 0,  // 广播：同时向所有已启用通道发送（任一失败不影响其它）
  PUSH_STRATEGY_FAILOVER  = 1   // 故障转移：按通道顺序逐个尝试，第一个成功即停止
};

constexpr int MAX_PUSH_CHANNELS = 10;  // 推送通道数量上限（数组维度）

// 单个推送通道的配置（NVS 以 JSON 数组保存）。
struct PushChannel {
  bool     enabled     = false;  // 是否启用（关闭后不参与发送与校验）
  PushType type        = PUSH_TYPE_POST_JSON;
  String   name;                 // 用户自定义名称（仅展示用）
  String   url;                  // 目标地址：HTTP 类为完整 URL；Mailgun 为基址
  String   key1;                 // 额外参数1（按 type 含义不同，如 Token / API Key / 域名）
  String   key2;                 // 额外参数2（Mailgun：域名）
  String   key3;                 // 额外参数3（Mailgun：收件人）
  String   key4;                 // 额外参数4（Mailgun：发件人）
  String   key5;                 // 额外参数5（Mailgun：邮件标题模板）
  String   customBody;           // 自定义请求体模板（PUSH_TYPE_CUSTOM 使用，支持占位符）
  bool     retryOnFail = false;  // 发送失败时是否自动重试
  int8_t   modemSlot = -1;       // PUSH_TYPE_SMS 备份通道从哪个模组发出（串口号 1..2）；
                                 // <0 = 自动挑第一个就绪模组（单卡用户无需关心此概念）
};

// 消息类型（与参考项目一致，但本框架当前主要触发 SMS）。
enum MsgType {
  MSG_TYPE_SMS     = 0,  // 短信（默认消息类型）
  MSG_TYPE_CALL    = 1,  // 来电通知
  MSG_TYPE_SIM     = 2,  // SIM 卡状态变更
  MSG_TYPE_BOOT    = 3,  // 设备启动完成
  MSG_TYPE_ONLINE  = 4   // 定时在线通知
};

// 消息类型描述：由调用方构造，按 type 路由到对应渲染分支。
struct MsgTypeInfo {
  MsgType type = MSG_TYPE_SMS;
  String  typeLabel;   // 仅在 SMS 时使用（如「普通短信」/「STK 推送」）
  int8_t  modemSlot = -1;  // 事件来源模组序号（0 基）；-1 表示与模组无关

  MsgTypeInfo() = default;
  MsgTypeInfo(MsgType t) : type(t) {}
  MsgTypeInfo(MsgType t, const String& cls) : type(t), typeLabel(cls) {}
  MsgTypeInfo(MsgType t, int8_t slot) : type(t), modemSlot(slot) {}
  MsgTypeInfo(MsgType t, const String& cls, int8_t slot)
      : type(t), typeLabel(cls), modemSlot(slot) {}

  // 渲染为人类可读字符串（用于推送标题等场景）
  String toString() const {
    switch (type) {
      case MSG_TYPE_CALL:    return "来电";
      case MSG_TYPE_SIM:     return "SIM事件";
      case MSG_TYPE_BOOT:    return "启动事件";
      case MSG_TYPE_ONLINE:  return "在线通知";
      case MSG_TYPE_SMS:     return typeLabel.length() > 0 ? ("短信：" + typeLabel) : "短信";
      default: {
        char buf[32];
        snprintf(buf, sizeof(buf), "未知消息(type=%d)", (int)type);
        return String(buf);
      }
    }
  }
};

// 通道类型中文标签（前端下拉 / 日志用）
const char* pushTypeLabel(PushType t);

// 该类型的「密钥字段」名称（如 key1 / key2），前端对该字段做掩码、
// 后端在 configJson 中不回显其真实值（仅返回 hasKeyX 标记）。
const char* pushTypeSecretField(PushType t);

// 通道是否「有效」（启用且必填字段齐全）。无效通道不参与发送。
bool pushChannelValid(const PushChannel& ch);
