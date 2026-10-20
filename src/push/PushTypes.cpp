#include "push/PushTypes.h"

const char* pushTypeLabel(PushType t) {
  switch (t) {
    case PUSH_TYPE_POST_JSON:   return "通用POST(JSON)";
    case PUSH_TYPE_BARK:        return "Bark";
    case PUSH_TYPE_GET:         return "通用GET";
    case PUSH_TYPE_DINGTALK:    return "钉钉机器人";
    case PUSH_TYPE_PUSHPLUS:    return "PushPlus";
    case PUSH_TYPE_SERVERCHAN:  return "Server酱";
    case PUSH_TYPE_CUSTOM:      return "自定义POST";
    case PUSH_TYPE_FEISHU:      return "飞书机器人";
    case PUSH_TYPE_GOTIFY:      return "Gotify";
    case PUSH_TYPE_TELEGRAM:    return "Telegram Bot";
    case PUSH_TYPE_WECHAT_WORK: return "企业微信机器人";
    case PUSH_TYPE_SMS:         return "SMS 短信备份";
    case PUSH_TYPE_MAILGUN:     return "Mailgun 邮件";
    default:                    return "未知";
  }
}

// 密钥字段：用于前端掩码输入、后端 configJson 不回显真实值
const char* pushTypeSecretField(PushType t) {
  switch (t) {
    case PUSH_TYPE_TELEGRAM:   return "key2";  // Bot Token
    case PUSH_TYPE_MAILGUN:    return "key1";  // API Key
    default:                   return "key1";  // 其余类型 key1 多为 Token/密钥/签名
  }
}

// 通道是否「有效」（启用且必填字段齐全）。无效通道不参与发送。
bool pushChannelValid(const PushChannel& ch) {
  if (!ch.enabled) return false;
  switch (ch.type) {
    case PUSH_TYPE_NONE:        return false;
    case PUSH_TYPE_POST_JSON:
    case PUSH_TYPE_GET:
    case PUSH_TYPE_BARK:
    case PUSH_TYPE_DINGTALK:
    case PUSH_TYPE_FEISHU:
    case PUSH_TYPE_WECHAT_WORK:
    case PUSH_TYPE_CUSTOM:      return ch.url.length() > 0;
    case PUSH_TYPE_PUSHPLUS:    return ch.key1.length() > 0;
    case PUSH_TYPE_SERVERCHAN:  return ch.url.length() > 0 || ch.key1.length() > 0;
    case PUSH_TYPE_GOTIFY:      return ch.url.length() > 0 && ch.key1.length() > 0;
    case PUSH_TYPE_TELEGRAM:    return ch.key1.length() > 0 && ch.key2.length() > 0;
    case PUSH_TYPE_SMS:         return ch.url.length() > 0;  // url = 目标手机号
    case PUSH_TYPE_MAILGUN:     return ch.key1.length() > 0 && ch.key2.length() > 0 && ch.key3.length() > 0;
    default:                    return false;
  }
}
