#pragma once

#include <stdint.h>

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

// 应用层：持有各模块，负责装配与主循环调度
class App {
 public:
  // 初始化所有子模块，成功返回 true
  bool begin();

  // 主循环调度：推进 WiFi 重连、SSE 状态推送、短信重组超时、按周期刷新屏幕
  void tick();

 private:
  // 从各模块聚合一份屏幕渲染用的状态快照，填充到 out
  void collectStatus(StatusInfo &out);

  RgbLed _led;              // 板载 RGB 灯（WS2812）
  Display _display;         // ST7789 屏幕
  Keypad _keypad;           // 4 键输入（OK/UP/DOWN/BACK）
  UIMenu _ui{_display};     // LCD 菜单导航状态机（依赖 _display）
  WifiManager _wifi;        // WiFi 连接管理 + NVS 配网
  SerialBridge _bridge;     // 双串口 AT 透传桥
  SmsService _sms{_bridge}; // 短信服务（4G 模组收发 + 文件存储）
  TimeSync _time;           // 时间同步（NTP 为主，SIM NITZ 兜底；依赖 _bridge/_sms）
  StatusBus _events{_wifi, _bridge, _led, _display, _sms, _time};  // SSE 状态推送总线
  AdminStore _auth;         // 管理账号存储（NVS，JSON）
  EmailService _email;      // SMTP 邮件通知（模板 + 通知类型 + 发送队列）
  MqttService _mqtt;        // MQTT 服务（事件上报 + 命令订阅）
  PushService _push;        // 统一 webhook 推送通道（广播 / 故障转移策略）
  BlackList _blacklist;     // 号码黑名单（拦截命中规则的短信）
  // 设备日志环形存储（PSRAM，Web 分页查询 + 串口镜像）；
  // 指向全局唯一实例（devLog()），其余模块取的也是同一份
  LogStore &_log = devLog();
  WebPanel _panel{_bridge, _led, _wifi, _display, _auth, _events, _sms, _time, _email, _log, _mqtt, _push, _blacklist};  // HTTP 控制台（依赖上述模块）

  uint32_t _lastUiMs = 0;   // 上一次屏幕刷新的时间戳（ms），用于节流
};
