#pragma once

#include <Arduino.h>

#include "app/Config.h"
#include "sms/SmsRecord.h"

// PDU 编解码 + 长短信重组（基于 lib/pdulib）。
// 注意：pdulib 的 PDU 实例非线程安全——接收侧（串口抽数任务）与发送侧
// （HTTP 任务）必须使用不同实例，因此本类分别持有 Rx / Tx 两个。
class SmsCodec {
 public:
  // 解码一条 PDU（十六进制字符串，可含空白）
  // 成功返回 true 并填充 sender / timestamp / text；
  // concat[0]=参考号 concat[1]=本段序号 concat[2]=总段数（非长短信全为 0）
  bool decode(const String &pduHex, String &sender, String &timestamp,
              String &text, int *concat);

  // 长短信重组：喂入一片；收齐返回 true（outText/outTs 为完整短信），
  // 否则返回 false（仍在等待其余分段）。slots 为调用方持有的槽位数组。
  bool reassemble(SmsConcatSlot *slots, const String &sender,
                  const String &timestamp, const String &text, int ref,
                  int partNo, int totalParts, String &outText, String &outTs);

  // 清理超时未收齐的槽位（App::tick 周期调用）
  void pruneTimeouts(SmsConcatSlot *slots);

  // 把一条待发短信编码为若干 PDU 段（超过单条容量自动拆分长短信）。
  // ref 为长短信参考号（单条时忽略）；parts 输出各段，返回段数，
  // 返回值 < 0 为编码错误（错误码见 pdulib eEncodeError）。
  int encodeSend(const String &phone, const String &text, uint8_t ref,
                 SmsPduPart *parts, int maxParts);

  // 判断字符串是否含多字节字符（决定 UCS-2 / GSM-7 编码与分段容量）
  static bool hasMultibyte(const String &text);

  // SCTS 半八位组时间戳（pdulib getTimeStamp() 的 14 字符原文
  // "YYMMDDHHMMSS" + 两位时区）转纪元秒；时区按 GSM 03.40 规则换算
  // （15 分钟/单位、低半字节十位 + bit 3 符号位），东正西负。
  // 年份不在 2020-2099 视为不可信，返回 0（调用方回退到开机秒数）
  static uint32_t sctsToEpoch(const String &scts);

 private:
  // 从 text 的 pos 处截取最多 maxUnits 个 UCS-2 单元（代理对算 2 个）的
  // UTF-8 子串；ucs2=false 时按字节截取（GSM-7 纯 ASCII）
  static String cutChunk(const String &text, size_t pos, bool ucs2, int maxUnits);

  // 整条文本的 UCS-2 单元数（代理对算 2 个）
  static int countUcs2Units(const String &text);

  // 天数 -> 纪元秒（ civil from days，免时区库）
  static int64_t daysFromCivil(int y, int m, int d);
};
