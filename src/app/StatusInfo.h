#pragma once

#include <stdint.h>

// App 采集 -> Display 渲染的数据快照（不含动态内存）
// 字段全部使用 const char* 指向外部长期存在的字符串，避免快照结构体持有堆内存
struct StatusInfo {
  bool wifiUp = false;        // WiFi 是否已连接（STA）
  bool apMode = false;        // 是否处于 AP 配网模式
  const char *ip = "-";       // 本机 IP 地址字符串（AP 模式为 192.168.4.1）
  int8_t rssi = 0;            // WiFi 信号强度，单位 dBm（未连接时为 0）
  uint32_t uptimeSec = 0;     // 系统运行秒数
  uint32_t freeHeap = 0;      // 剩余堆内存，单位字节
  const char *ledName = "-";  // RGB 灯当前颜色名（如 "RED"）
  uint8_t lastPort = 0;       // 最近一次 AT 交互所在串口号，0 表示暂无 AT 记录
  const char *lastCmd = "";   // 最近一条 AT 命令（空 = 串口自发上报）
  const char *lastResp = "";  // 最近一条 AT 响应内容
};
