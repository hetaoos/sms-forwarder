#pragma once

#include <stdint.h>

#include "app/Outbox.h"
#include "app/StatusInfo.h"
#include "app/UIMenu.h"
#include "hal/Button.h"
#include "hal/Display.h"
#include "hal/RgbLed.h"
#include "log/LogStore.h"
#include "net/AdminStore.h"
#include "net/EmailService.h"
#include "net/MqttService.h"
#include "net/StatusBus.h"
#include "push/PushService.h"
#include "net/WifiManager.h"
#include "net/WebPanel.h"
#include "serial/SerialBridge.h"
#include "sms/BlackList.h"
#include "sms/SmsService.h"
#include "time/TimeSync.h"

// 应用层：持有各通信模组，负责装配与主循环调度
class App {
 public:
  // 初始化所有子通信模组，成功返回 true
  bool begin();

  // 主循环调度：推进 WiFi 重连、SSE 状态推送、短信重组超时、按周期刷新屏幕
  void tick();

 private:
  // 从各通信模组聚合一份屏幕渲染用的状态快照，填充到 out
  void collectStatus(StatusInfo &out);

  // 秒 -> "1天02:03:04" / "02:03:04"
  static void fmtUptime(uint32_t sec, char *out, size_t n);
  // 字节数 -> "7.80M" / "123.4K" / "512B"；返回静态缓冲（一轮内可同时用 4 个）
  static const char *fmtBytes(uint32_t bytes);

  // ---- LCD 菜单「短信」页数据源 ----
  // 填充最多 max 条最近短信预览（时间倒序），total 出参为存储总条数，返回实际条数。
  // 扫文件有成本，这里做 3 秒节流：菜单页按 500ms 刷新，但只每 3 秒真读一次。
  int smsPreview(SmsPreview *out, int max, int &total);
  // 单条记录 -> 预览条目（号码 / 时间 / 正文一律截断到定长）
  void fillPreview(const SmsMessage &m, SmsPreview &out);

  // 新短信检测（主循环每轮调用）：lastSmsId 变化时把最新一条弹到屏幕上
  void checkNewSms();

  RgbLed _led;              // 板载 RGB 灯（WS2812）
  Display _display;         // ST7789 屏幕
  Keypad _keypad;           // 4 键输入（OK/UP/DOWN/BACK）
  UIMenu _ui{_display};     // LCD 菜单导航状态机（依赖 _display）
  WifiManager _wifi;        // WiFi 连接管理 + NVS 配网
  SerialBridge _bridge;     // 双串口 AT 透传桥
  SmsService _sms{_bridge}; // 短信服务（通信模组收发 + 文件存储）
  TimeSync _time;           // 时间同步（NTP 为主，SIM NITZ 兜底；依赖 _bridge/_sms）
  // SSE 状态推送总线（需 _ui 以输出屏幕设置，故排在 _ui 之后）
  StatusBus _events{_wifi, _bridge, _led, _display, _ui, _sms, _time};
  AdminStore _auth;         // 管理账号存储（NVS，JSON）
  EmailService _email;      // SMTP 邮件通知（模板 + 通知类型 + 发送队列）
  MqttService _mqtt;        // MQTT 服务（事件上报 + 命令订阅）
  PushService _push;        // 统一 webhook 推送通道（广播 / 故障转移策略）
  BlackList _blacklist;     // 号码黑名单（拦截命中规则的短信）
  // 持久化发件箱：短信先落盘再逐通道投递，重启续投（须在 _panel 之前声明）
  Outbox _outbox;
  // 设备日志环形存储（PSRAM，Web 分页查询 + 串口镜像）；
  // 指向全局唯一实例（devLog()），其余通信模组取的也是同一份
  LogStore &_log = devLog();
  // HTTP 控制台（依赖上述通信模组）；_ui 用于读写屏幕设置（亮度 / 自动熄屏 / 自动返回）
  WebPanel _panel{_bridge, _led, _wifi, _display, _ui, _auth, _events, _sms, _time,
                  _email, _log, _mqtt, _push, _blacklist, _outbox};

  // 邮件 / MQTT / 推送 / 短信是否已完成 begin()。
  // 开机首帧状态页会先于这些通信模组初始化采集一次，此时它们的互斥量句柄还是 NULL，
  // 碰到就断言重启；采集里凡是要碰这些通信模组的分支都必须先看这个标记。
  bool _svcReady = false;

  uint32_t _lastUiMs = 0;   // 上一次屏幕刷新的时间戳（ms），用于节流

  // ---- 新短信弹窗 ----
  uint32_t _lastSmsId = 0;      // 已处理到的最新短信 ID（0 = 尚无记录）
  bool _smsBaseReady = false;   // 开机基线是否已建立（建立前不弹历史短信）
  SmsPreview _smsCache[cfg::kUiSmsPreviewMax];  // 预览缓存（避免每 500ms 扫文件）
  int _smsCacheCount = 0;                       // 缓存条数
  int _smsCacheTotal = 0;                       // 缓存时的存储总条数
  uint32_t _smsCacheMs = 0;                     // 缓存时刻（节流用）

  // ---- 开机页窗口 ----
  bool _bootDone = false;     // 开机页是否已展示完毕（之后按常规渲染状态页）
  uint32_t _bootMs = 0;       // 开机页起始时刻（0 = 首轮 tick 时才起算）
  uint32_t _lastBootBarMs = 0;  // 上次推进进度条的相对时刻
};
