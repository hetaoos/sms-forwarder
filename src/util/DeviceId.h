#pragma once

#include <Arduino.h>

// 设备唯一 ID：完整芯片 eFuse MAC（48 位 → 12 位十六进制，小写）。
//
// 关键点：
// 1) ESP.getEfuseMac() 返回的 64 位整数中，48 位 MAC 位于【低 48 位】
//    （高 16 位恒为 0），绝不能右移，否则会丢弃 MAC 中逐设备唯一的序列号段
//    （低 24 位），只留下厂商 OUI，导致同一厂商不同设备撞 id。
// 2) getEfuseMac() 构造时把 mac[0] 放在最低字节，若直接 %012llx 打印会
//    得到与 WiFi.macAddress()（mac[0] 在最前）相反的字节序。这里逐字节按
//    mac[0..5] 顺序格式化，保证 deviceId 与「MAC 地址」的字节顺序完全一致
//    （仅大小写 / 冒号差异）。
//
// MQTT / 推送 / 邮件 / 状态接口统一调用本函数，保证同一设备在各通道下
// 算出的 id（与大小写）完全一致。
namespace Device {
inline String id() {
  uint64_t v = ESP.getEfuseMac();
  uint8_t b[6];
  for (int i = 0; i < 6; i++) b[i] = (uint8_t)(v >> (8 * i));  // 还原 mac[0..5] 原顺序
  char buf[13];
  snprintf(buf, sizeof(buf), "%02x%02x%02x%02x%02x%02x",
           b[0], b[1], b[2], b[3], b[4], b[5]);
  return String(buf);
}
inline String name() { return "SMS-Forwarder-" + id(); }
}
