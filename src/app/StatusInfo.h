#pragma once

#include <stdint.h>

// App 采集 -> LVGL 状态页渲染的数据快照（不含动态内存）
//
// 设计约定（沿用原有约定，扩字段时保持）：
//   - const char* 一律指向外部长期存在的字符串（WifiManager / App 的静态缓冲），
//     快照本身不持有堆内存；
//   - 需要 App 现场拼出来的文本（MAC、运行时长、模组简报、服务状态）放在定长
//     char 数组里，由 App::collectStatus 用 snprintf 填充，同样零堆分配，
//     也避免了跨任务的字符串生命周期问题。
struct StatusInfo {
  bool wifiUp = false;        // WiFi 是否已连接（STA）
  bool apMode = false;        // 是否处于 AP 配网模式
  const char *ip = "-";       // 本机 IP 地址字符串（AP 模式为 192.168.4.1）
  const char *ssid = "-";     // 当前网络名（STA 已连 = SSID；AP = 热点名）
  const char *apPass = "-";   // AP 热点密码文本（开放热点为“开放无密码”）
  const char *time = "--:--:--";  // 当前时间 "HH:MM:SS"（时钟未同步为 --:--:--）
  int8_t rssi = 0;            // WiFi 信号强度，单位 dBm（未连接时为 0）
  uint32_t uptimeSec = 0;     // 系统运行秒数
  uint32_t freeHeap = 0;      // 剩余内部堆内存（DRAM），单位字节
  uint32_t psramSize = 0;     // PSRAM 总容量（板载 8MB；无 PSRAM 为 0），单位字节
  uint32_t freePsram = 0;     // 剩余 PSRAM，单位字节
  const char *ledName = "-";  // RGB 灯当前颜色名（如 "RED"）

  // ---- 现场拼装的展示文本（定长，App 填充）----
  char mac[18] = "-";         // 本机 MAC："AA:BB:CC:DD:EE:FF"
  char uptime[24] = "-";      // 运行时长："1天02:03:04" / "02:03:04"
  char mem[24] = "-";         // 内存："123.4K / 7.8M"（内部堆剩余 / PSRAM 剩余）
  char load[24] = "-";        // 负载："12.3% 45.2℃"（CPU 使用率 芯片温度）
  char modem[2][28] = {"-", "-"};  // 模组简报（下标 0/1 对应串口 U1/U2）
  char mqtt[16] = "-";        // MQTT：已连接 / 未连接 / 未配置 / 已关闭
  char mail[16] = "-";        // 邮件：已配置 / 未配置 / 已关闭
  char push[16] = "-";        // 推送："3 个通道" / 未配置 / 已关闭

  // ---- 屏幕显示页仍沿用（状态页不再展示这两项）----
  uint8_t lastPort = 0;       // 最近一次 AT 交互所在串口号，0 表示暂无 AT 记录
  const char *lastCmd = "";   // 最近一条 AT 命令（空 = 串口自发上报）
  const char *lastResp = "";  // 最近一条 AT 响应内容
};
