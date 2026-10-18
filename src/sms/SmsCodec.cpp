#include "sms/SmsCodec.h"

#include <pdulib.h>

#include <Arduino.h>

#include "log/LogStore.h"

namespace {

// 本模块日志出口：全局日志环（串口由 LogStore 统一镜像输出）
LogStore &_log = devLog();

}  // namespace

// 工作区大小：单段 PDU 解码/编码足够（长短信按段处理，不在单实例内拼接）
static constexpr int kPduWorkBuf = 1024;

// 解码一条 PDU
bool SmsCodec::decode(const String &pduHex, String &sender, String &timestamp,
                      String &text, int *concat) {
  static PDU pdu(kPduWorkBuf);  // 仅接收任务使用
  // 去掉可能的空白字符（\r\n 与空格）
  String hex;
  for (size_t i = 0; i < pduHex.length(); i++) {
    char c = pduHex[i];
    if (c != ' ' && c != '\r' && c != '\n' && c != '\t') hex += c;
  }
  if (!hex.length()) return false;
  if (!pdu.decodePDU(hex.c_str())) return false;
  sender = pdu.getSender();
  timestamp = pdu.getTimeStamp();
  text = pdu.getText();
  const int *ci = pdu.getConcatInfo();
  concat[0] = ci[0];
  concat[1] = ci[1];
  concat[2] = ci[2];
  return true;
}

// 长短信重组：喂入一片
bool SmsCodec::reassemble(SmsConcatSlot *slots, const String &sender,
                          const String &timestamp, const String &text, int ref,
                          int partNo, int totalParts, String &outText,
                          String &outTs) {
  if (totalParts < 1 || totalParts > cfg::kSmsConcatMaxParts) return false;
  if (partNo < 1 || partNo > totalParts) return false;

  // 1) 匹配已有槽位（ref + total + sender 三者全同）
  SmsConcatSlot *s = nullptr;
  for (uint8_t i = 0; i < cfg::kSmsConcatSlots; i++) {
    if (slots[i].inUse && slots[i].ref == (uint16_t)ref &&
        slots[i].total == (uint8_t)totalParts && slots[i].sender == sender) {
      s = &slots[i];
      break;
    }
  }
  // 2) 空闲槽位
  if (!s) {
    for (uint8_t i = 0; i < cfg::kSmsConcatSlots; i++) {
      if (!slots[i].inUse) {
        s = &slots[i];
        break;
      }
    }
  }
  // 3) 全满：覆盖最旧的一组
  if (!s) {
    s = &slots[0];
    for (uint8_t i = 1; i < cfg::kSmsConcatSlots; i++) {
      if (slots[i].firstMs < s->firstMs) s = &slots[i];
    }
    _log.addf(LogLevel::kWarn, "sms", "长短信重组槽位已满，丢弃最旧的一组");
  }

  if (!s->inUse) {
    s->inUse = true;
    s->ref = (uint16_t)ref;
    s->total = (uint8_t)totalParts;
    s->received = 0;
    s->gotMask = 0;
    s->sender = sender;
    s->ts = timestamp;
    s->firstMs = millis();
    for (uint8_t i = 0; i < cfg::kSmsConcatMaxParts; i++) s->parts[i] = String();
  }

  uint8_t idx = (uint8_t)(partNo - 1);
  if (!(s->gotMask & (1 << idx))) {
    s->parts[idx] = text;
    s->gotMask |= (1 << idx);
    s->received++;
    s->firstMs = millis();  // 超时自「最后一段收到」起算
  }
  if (s->received < s->total) return false;  // 未收齐

  // 收齐：按段拼接，缺失段插入占位（理论上 gotMask 满，不会发生）
  outText = String();
  for (uint8_t i = 0; i < s->total; i++) {
    outText += s->parts[i].length() ? s->parts[i]
                                    : String("[缺失分段") + (i + 1) + "]";
  }
  outTs = s->ts;
  s->inUse = false;
  return true;
}

// 清理超时槽位
void SmsCodec::pruneTimeouts(SmsConcatSlot *slots) {
  uint32_t now = millis();
  for (uint8_t i = 0; i < cfg::kSmsConcatSlots; i++) {
    if (slots[i].inUse && now - slots[i].firstMs >= cfg::kSmsConcatTimeoutMs) {
      _log.addf(LogLevel::kWarn, "sms", "长短信槽位 %u 重组超时（ref=%u，%u/%u 段）",
                (unsigned)i, (unsigned)slots[i].ref,
                (unsigned)slots[i].received, (unsigned)slots[i].total);
      slots[i].inUse = false;
    }
  }
}

// 编码待发短信（长短信自动拆分）
int SmsCodec::encodeSend(const String &phone, const String &text, uint8_t ref,
                         SmsPduPart *parts, int maxParts) {
  static PDU pdu(kPduWorkBuf);  // 仅发送任务（HTTP）使用
  bool ucs2 = hasMultibyte(text);
  int totalUnits = ucs2 ? countUcs2Units(text) : (int)text.length();
  int singleMax = ucs2 ? 70 : 160;   // 单条容量（UCS-2 单元 / GSM-7 字符）
  int partMax = ucs2 ? 66 : 152;     // 长短信每段容量（UDH 占 7 字节）

  if (totalUnits <= singleMax) {
    // 单条：csms=0
    pdu.setSCAnumber();
    int len = pdu.encodePDU(phone.c_str(), text.c_str());
    if (len < 0) return len;
    parts[0].hex = pdu.getSMS();
    parts[0].tpduLen = len;
    return 1;
  }

  int numParts = (totalUnits + partMax - 1) / partMax;
  if (numParts > maxParts) return PDU::MULTIPART_NUMBERS;

  size_t pos = 0;
  for (int p = 0; p < numParts; p++) {
    String chunk = cutChunk(text, pos, ucs2, partMax);
    if (!chunk.length()) return PDU::GSM7_TOO_LONG;
    pos += chunk.length();
    pdu.setSCAnumber();
    int len = pdu.encodePDU(phone.c_str(), chunk.c_str(), ref,
                            (unsigned char)numParts, (unsigned char)(p + 1));
    if (len < 0) return len;
    parts[p].hex = pdu.getSMS();
    parts[p].tpduLen = len;
  }
  return numParts;
}

// 是否含多字节字符（决定编码方式：UCS-2 可编码任意字符）
bool SmsCodec::hasMultibyte(const String &text) {
  for (size_t i = 0; i < text.length(); i++) {
    if ((uint8_t)text[i] >= 0x80) return true;
  }
  return false;
}

// 截取一个分段的 UTF-8 子串（按字符边界，不切断多字节字符）
String SmsCodec::cutChunk(const String &text, size_t pos, bool ucs2, int maxUnits) {
  size_t i = pos;
  int units = 0;
  while (i < text.length()) {
    uint8_t c = (uint8_t)text[i];
    int clen = 1;
    if ((c & 0x80) == 0) clen = 1;
    else if ((c & 0xE0) == 0xC0) clen = 2;
    else if ((c & 0xF0) == 0xE0) clen = 3;
    else if ((c & 0xF8) == 0xF0) clen = 4;
    else clen = 1;  // 非法字节按 1 处理，避免死循环
    int u = ucs2 ? ((clen == 4) ? 2 : 1) : clen;  // UCS-2：代理对占 2 单元
    if (units + u > maxUnits) break;
    units += u;
    i += clen;
  }
  return text.substring(pos, i);
}

// 统计整条文本的 UCS-2 单元数
int SmsCodec::countUcs2Units(const String &text) {
  int units = 0;
  size_t i = 0;
  while (i < text.length()) {
    uint8_t c = (uint8_t)text[i];
    int clen = 1;
    if ((c & 0x80) == 0) clen = 1;
    else if ((c & 0xE0) == 0xC0) clen = 2;
    else if ((c & 0xF0) == 0xE0) clen = 3;
    else if ((c & 0xF8) == 0xF0) clen = 4;
    else clen = 1;
    units += (clen == 4) ? 2 : 1;
    i += clen;
  }
  return units;
}

// 天数 -> 纪元秒（Howard Hinnant days_from_civil，免时区库）
int64_t SmsCodec::daysFromCivil(int y, int m, int d) {
  y -= m <= 2;
  const int64_t era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = (unsigned)(y - era * 400);
  const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + (unsigned)(d - 1);
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + (int64_t)doe - 719468;
}

// SCTS（pdulib 的 14 字符原文 "YYMMDDHHMMSS" + 两位时区）转纪元秒。
// 时区字节规则（GSM 03.40 §9.2.3.11，与参考项目 esp32-sms-forwarding 的
// formatSmsTimestamp 一致：两位时区、单位为 15 分钟）：
//   低半字节存十位，且 bit 3 为符号位（置位 = 西/负）；高半字节存个位。
//   pdulib 逐字节输出 (低 nibble)(高 nibble)，负时区的符号位会把十位字符
//   变成非数字（如 0x4A -> ":4"），故按「字符 - '0' = nibble」还原而非isdigit。
// 最终 epoch = 当地数字时间(按 UTC 解释) - 时区偏移，前端按浏览器本地时区
// 渲染即为真实当地时间（设备自身无 RTC/NTP，不参与此换算）。
uint32_t SmsCodec::sctsToEpoch(const String &scts) {
  if (scts.length() < 12) return 0;
  for (int i = 0; i < 12; i++) {
    if (scts[i] < '0' || scts[i] > '9') return 0;
  }
  auto d2 = [&](int i) { return (scts[i] - '0') * 10 + (scts[i + 1] - '0'); };
  int yy = d2(0), mo = d2(2), dd = d2(4), hh = d2(6), mi = d2(8), ss = d2(10);
  if (yy < 20 || yy > 99) return 0;          // 2020-2099 之外视为不可信
  int year = 2000 + yy;
  if (mo < 1 || mo > 12 || dd < 1 || dd > 31 || hh > 23 || mi > 59 || ss > 59) {
    return 0;
  }
  int64_t days = daysFromCivil(year, mo, dd);
  int64_t secs = days * 86400 + (int64_t)hh * 3600 + mi * 60 + ss;

  // 时区偏移：第 13~14 字符，15 分钟为单位，东正西负（缺失/异常按 0 处理）
  int tzSecs = 0;
  if (scts.length() >= 14) {
    int tens = scts[12] - '0';   // pdulib 对负时区此处为非数字，按 nibble 值还原
    int units = scts[13] - '0';
    if (tens >= 0 && tens <= 15 && units >= 0 && units <= 9) {
      int quarters = (tens & 0x07) * 10 + units;  // 十位 nibble 的 bit 3 是符号位
      if (quarters <= 96) {                        // 防御：最大 ±24h
        tzSecs = quarters * 900;
        if (tens & 0x08) tzSecs = -tzSecs;         // 符号位置位 = 西/负
      }
    }
  }
  secs -= tzSecs;
  if (secs < 0 || secs > 4000000000LL) return 0;
  return (uint32_t)secs;
}
