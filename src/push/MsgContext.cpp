#include "MsgContext.h"

#include <time.h>

// 将 millis() 时长格式化为中文可读字符串。
String MsgContext::formatUptime(unsigned long ms) {
  unsigned long s = ms / 1000;
  unsigned long days = s / 86400;
  unsigned long hours = (s % 86400) / 3600;
  unsigned long mins = (s % 3600) / 60;
  unsigned long secs = s % 60;
  String out;
  if (days > 0) out += String(days) + "天";
  if (hours > 0 || days > 0) out += String(hours) + "小时";
  if (mins > 0 || hours > 0 || days > 0) out += String(mins) + "分钟";
  out += String(secs) + "秒";
  return out;
}

// 取得字段值（按名称）；返回 false 表示未识别该字段。
static bool fieldValue(const String& name, const MessageContext& ctx, String& out) {
  if (name == "from")         out = ctx.from;
  else if (name == "message") out = ctx.message;
  else if (name == "timestamp") out = ctx.timestamp;
  else if (name == "date")    out = ctx.date;
  else if (name == "deviceId") out = ctx.deviceId;
  else if (name == "carrier") out = ctx.carrier;
  else if (name == "to")      out = ctx.to;
  else if (name == "simSlot") out = ctx.simSlot;
  else if (name == "modemIndex") out = ctx.modemIndex;
  else if (name == "iccid")   out = ctx.iccid;
  else if (name == "signal")  out = ctx.signal;
  else if (name == "remark")  out = ctx.remark;
  else if (name == "uptime")  out = ctx.uptime;
  else if (name == "channelName") out = ctx.channelName;
  else if (name == "channelType") out = ctx.channelType;
  else if (name == "deviceName") out = ctx.deviceName;
  else if (name == "message_type" || name == "trigger_type") out = ctx.messageType;
  else return false;
  return true;
}

// HTML 转义（& < > "），用于把动态文本安全填入 HTML 模板。
static String escapeHtml(const String& s) {
  String out;
  out.reserve(s.length());
  for (unsigned int i = 0; i < s.length(); i++) {
    char c = s.charAt(i);
    switch (c) {
      case '&': out += "&amp;"; break;
      case '<': out += "&lt;"; break;
      case '>': out += "&gt;"; break;
      case '"': out += "&quot;"; break;
      default:  out += c;
    }
  }
  return out;
}

String MsgContext::render(const String& tmpl, const MessageContext& ctx, bool html) {
  String out;
  out.reserve(tmpl.length() + 32);
  int i = 0;
  while (i < (int)tmpl.length()) {
    if (tmpl.charAt(i) == '{') {
      int end = tmpl.indexOf('}', i);
      if (end > i) {
        String name = tmpl.substring(i + 1, end);
        String val;
        if (fieldValue(name, ctx, val)) {
          out += html ? escapeHtml(val) : val;
          i = end + 1;
          continue;
        }
      }
    }
    out += tmpl.charAt(i);
    i++;
  }
  return out;
}

String MsgContext::renderHtml(const String& tmpl, const MessageContext& ctx) {
  return render(tmpl, ctx, true);
}
