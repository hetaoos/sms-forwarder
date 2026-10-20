#pragma once

#include <Arduino.h>

#include "app/Config.h"

// 一条短信记录（解码 / 重组后的统一结构，也是存储单元）
struct SmsMessage {
  uint32_t id = 0;         // 存储 ID（0 = 未入库）
  bool incoming = true;    // true = 收到，false = 发出
  String phone;            // 对方号码（发出时为目标号码）
  String content;          // 正文（UTF-8）
  uint32_t ts = 0;         // 时间戳（秒）；>= 1.6e9 视为真实纪元时间，否则为开机秒数
  int8_t modem = -1;       // 通信模组串口号 1..kSmsMaxModems，-1 = 未知
};

// 长短信重组槽位：同一槽位收齐 ref+total+sender 相同的全部分段
struct SmsConcatSlot {
  bool inUse = false;
  uint16_t ref = 0;        // 长短信参考号（CSMS）
  uint8_t total = 0;       // 总段数
  uint8_t received = 0;    // 已收段数
  uint32_t gotMask = 0;    // 已收段位图（bit i = 第 i+1 段；32 位支持到 20 段）
  String sender;           // 发送方号码
  String ts;               // 第一片的 SCTS 原文（收齐后作为整条时间戳）
  uint32_t firstMs = 0;    // 首片到达时刻（超时判定）
  String parts[cfg::kSmsConcatMaxParts];  // 各段正文
};

// 发送用的一段 PDU
struct SmsPduPart {
  String hex;        // 完整 hex 串（含 SCA），末尾内嵌 0x1A（Ctrl+Z）
  int tpduLen = 0;   // AT+CMGS=<n> 的长度参数（TPDU 字节数）
};
