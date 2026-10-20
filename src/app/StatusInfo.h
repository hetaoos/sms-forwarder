#pragma once

#include <stdint.h>

#include "sms/ModemSnapshot.h"

// App 采集 -> LVGL 状态页渲染的数据快照（不含动态内存）
//
// 设计约定（沿用原有约定，扩字段时保持）：
//   - const char* 一律指向外部长期存在的字符串（WifiManager / App 的静态缓冲），
//     快照本身不持有堆内存；
//   - 需要 App 现场拼出来的文本（MAC、运行时长、通信模组简报、服务状态）放在定长
//     char 数组里，由 App::collectStatus 用 snprintf 填充，同样零堆分配，
//     也避免了跨任务的字符串生命周期问题。
//
// 后半段（SOC / 通信模组详情 / 邮件 / MQTT / 推送 / RGB）是 LCD 各级菜单的数据源：
// 菜单页不像状态页那样每 500ms 无条件刷新，只在可见时按需抓一次快照，
// 因此这些字段的填充成本必须低（不碰文件系统、不做网络请求）。
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
  char modem[2][28] = {"-", "-"};  // 通信模组简报（下标 0/1 对应串口 U1/U2）
  char mqtt[16] = "-";        // MQTT：已连接 / 未连接 / 未配置 / 已关闭
  char mail[16] = "-";        // 邮件：已配置 / 未配置 / 已关闭
  char push[16] = "-";        // 推送："3 个通道" / 未配置 / 已关闭

  // ---- 屏幕显示页仍沿用（状态页不再展示这两项）----
  uint8_t lastPort = 0;       // 最近一次 AT 交互所在串口号，0 表示暂无 AT 记录
  const char *lastCmd = "";   // 最近一条 AT 命令（空 = 串口自发上报）
  const char *lastResp = "";  // 最近一条 AT 响应内容

  // ================= SOC / 系统信息（菜单「SOC」页）=================
  char chip[24] = "-";        // 芯片型号 + 步进："ESP32-S3 rev 2"
  char cpuFreq[12] = "-";     // 主频："240MHz"
  char cores[10] = "-";       // 内核数："2"
  char flash[12] = "-";       // Flash 容量："16MB"
  char cpu[12] = "-";         // CPU 使用率："12.3%"（不可用时 "-"）
  char temp[12] = "-";        // 芯片温度："45.2℃"
  char heapSize[16] = "-";    // 内部堆总容量
  char psramTotal[16] = "-";  // PSRAM 总容量
  char dateTime[22] = "-";    // 本地时间："2026-10-20 12:00:00"（未同步为"未同步"）
  char timeSrc[10] = "-";     // 时间来源：ntp / sim / nvs / none
  bool timeSynced = false;    // 时钟是否可用

  // ================= WiFi 补充（菜单「WIFI」页）=================
  char wifiMode[12] = "-";    // 工作模式："STA" / "AP" / "连接中"
  uint8_t wifiSaved = 0;      // NVS 已保存的配置组数

  // ================= 通信模组详情（菜单「通信模组」页）=================
  ModemSnapshot modems[2];    // 下标 0/1 对应串口 U1/U2

  // ================= 邮件通知（菜单「邮件通知」页）=================
  bool mailOn = false;        // 总开关
  bool mailCfg = false;       // SMTP 四项配置是否齐全
  char mailServer[28] = "-";  // SMTP 服务器
  uint16_t mailPort = 0;      // SMTP 端口
  char mailUser[28] = "-";    // 发件邮箱
  char mailTo[28] = "-";      // 收件邮箱
  uint8_t mailQueue = 0;      // 待发邮件数
  char mailSched[20] = "-";   // 定时在线通知排程："每 24 小时" / "每日 09:00"
  uint32_t mailTypes = 0;     // 通知类型位掩码（cfg::kEmailNotify*）

  // ================= MQTT（菜单「MQTT」页）=================
  bool mqttOn = false;        // 总开关
  bool mqttCfg = false;       // 服务器是否已配置
  bool mqttConn = false;      // 是否已连上 Broker
  char mqttServer[28] = "-";  // "host:1883"
  char mqttClient[20] = "-";  // 客户端 ID
  char mqttTopic[24] = "-";   // 主题前缀
  char mqttErr[28] = "-";     // 最近一次连接错误（空 = 无错误）
  bool mqttSub = false;       // 是否订阅命令主题

  // ================= 推送通道（菜单「推送通道」页）=================
  bool pushOn = false;        // 总开关
  uint8_t pushCh = 0;         // 有效通道数
  char pushStrategy[10] = "-";  // "广播" / "故障转移"
  // 通道简报（最多展示前 4 条，其余由页脚「共 N 条」概括）
  struct ChanBrief {
    char label[20] = "-";
    bool on = false;
    bool valid = false;
  };
  ChanBrief pushList[4];
  uint8_t pushListCount = 0;

  // ================= RGB 灯（菜单「RGB 灯」页）=================
  char rgbName[10] = "-";     // 当前颜色名
  uint8_t rgbBright = 0;      // 亮度 0-100
  uint8_t rgbIdx = 0;         // 预设颜色索引
};
