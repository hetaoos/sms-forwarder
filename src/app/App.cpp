#include "app/App.h"

#include <Arduino.h>

#include "app/Config.h"

// 初始化调试串口与所有子模块；返回 true 表示装配完成
bool App::begin() {
  Serial.begin(115200);

  _led.begin(1);                       // RGB 灯默认点亮索引 1（RED）
  _display.begin();                    // 屏幕 + 静态框架
  _bridge.begin();                     // 启动串口抽数任务

  _display.render(collectStatus());    // 先出一帧，避免开机黑屏
  _wifi.begin();                       // 读 NVS 配置并启动连接状态机（非阻塞）
  _panel.begin();                      // 启动 HTTP 服务

  Serial.println("[app] ready");
  return true;
}

// 聚合屏幕渲染所需的全部状态字段
// 注意：ip / lastCmd / lastResp 为 const char*，必须指向生命周期超出本函数的
// 存储，这里使用 static String 持有副本
StatusInfo App::collectStatus() {
  StatusInfo s;
  s.wifiUp = _wifi.connected();
  s.apMode = _wifi.apMode();
  s.uptimeSec = millis() / 1000;
  s.freeHeap = ESP.getFreeHeap();
  s.ledName = _led.color().name;
  s.rssi = s.wifiUp ? _wifi.rssi() : 0;

  static String sIp;         // 需要持有字符串，供 const char* 引用
  static String sCmd, sResp;
  sIp = _wifi.ip();
  s.ip = sIp.c_str();

  uint8_t port = 0;
  if (_bridge.lastEntry(port, sCmd, sResp)) {
    s.lastPort = port;
    s.lastCmd = sCmd.c_str();
    s.lastResp = sResp.c_str();
  }
  return s;
}

// 主循环调度逻辑：
// 1. 推进 WiFi 断线重连
// 2. 每 kUiRefreshMs 毫秒重绘一次屏幕（内容变化才会实际刷新）
// 3. 短暂延时让出 CPU
void App::tick() {
  _wifi.tick();

  if (millis() - _lastUiMs >= cfg::kUiRefreshMs) {
    _lastUiMs = millis();
    _display.render(collectStatus());
  }
  delay(5);
}
