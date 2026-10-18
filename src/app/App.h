#pragma once

#include <stdint.h>

#include "app/StatusInfo.h"
#include "hal/Display.h"
#include "hal/RgbLed.h"
#include "net/AdminStore.h"
#include "net/StatusBus.h"
#include "net/WifiManager.h"
#include "net/WebPanel.h"
#include "serial/SerialBridge.h"

// 应用层：持有各模块，负责装配与主循环调度
class App {
 public:
  // 初始化所有子模块，成功返回 true
  bool begin();

  // 主循环调度：推进 WiFi 重连、SSE 状态推送、按周期刷新屏幕
  void tick();

 private:
  // 从各模块聚合一份屏幕渲染用的状态快照
  StatusInfo collectStatus();

  RgbLed _led;              // 板载 RGB 灯（WS2812）
  Display _display;         // ST7789 屏幕
  WifiManager _wifi;        // WiFi 连接管理 + NVS 配网
  SerialBridge _bridge;     // 双串口 AT 透传桥
  StatusBus _events{_wifi, _bridge, _led, _display};  // SSE 状态推送总线
  AdminStore _auth;         // 管理账号存储（NVS，JSON）
  WebPanel _panel{_bridge, _led, _wifi, _display, _auth, _events};  // HTTP 控制台（依赖上述模块）

  uint32_t _lastUiMs = 0;   // 上一次屏幕刷新的时间戳（ms），用于节流
};
