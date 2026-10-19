#pragma once
#include <Arduino.h>

// 消息上下文：用户在自定义模板中可引用的全部字段。
// 占位符规则：模板中使用 `{字段名}` 引用；未识别的占位符按原样保留，方便排查。
// 说明：carrier/signal/iccid/to 等模组专属字段本框架暂不填充（留空），
// 默认推送体不使用这些字段；自定义模板若引用则保留字面量。
struct MessageContext {
  String from;          // 来源（短信发件人）
  String message;       // 消息正文
  String timestamp;     // 时间戳（短信用 SCTS，其它用本地时间）
  String date;          // 本地化时间字符串 "YYYY-MM-DD HH:mm:ss"
  String deviceId;      // 设备唯一 ID（完整芯片 eFuse MAC，12 位十六进制）
  String carrier;       // 运营商（暂未填充）
  String to;            // 接收方号码（本机号码，暂未填充）
  String simSlot;       // SIM 槽位，形如 "SIM1" / "SIM2"
  String modemIndex;    // 来源模组序号，从 1 开始，形如 "1" / "2"
  String iccid;         // ICCID（暂未填充）
  String signal;        // 信号强度（暂未填充）
  String remark;        // 设备备注（暂未填充）
  String uptime;        // 设备已运行时长（人类可读）
  String channelName;   // 当前推送渠道名称
  String channelType;   // 当前推送渠道类型
  String deviceName;    // 设备名 "SMS-Forwarder-<deviceId>"
  String messageType;   // 消息类型（{message_type}；兼容旧别名 {trigger_type}）
};

// MsgContext：模板渲染工具类。
class MsgContext {
 public:
  // 将 millis() 时长格式化为中文可读字符串，例如 "3天 2小时 5分钟"。
  static String formatUptime(unsigned long ms);

  // 渲染模板：替换已识别的 `{字段名}` 占位符；未识别占位符原样保留。
  // 兼容旧别名（例如 {trigger_type} -> {message_type}）。
  // html=true 时占位符取值先做 HTML 转义再填入（模板自身的标签不动），
  // 用于正文是 HTML 上下文的通道。
  static String render(const String& tmpl, const MessageContext& ctx, bool html = false);

  // render(tmpl, ctx, true) 的简写（HTML 上下文专用）。
  static String renderHtml(const String& tmpl, const MessageContext& ctx);
};
