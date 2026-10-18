#pragma once

#include <ESPAsyncWebServer.h>

#include "app/Config.h"
#include "hal/Display.h"
#include "hal/RgbLed.h"
#include "net/WifiManager.h"
#include "serial/SerialBridge.h"

// HTTP 控制台：状态页面 + RGB 控制 + 串口 AT 命令 + WiFi 配网管理
// 页面资源存放在 LittleFS 专有分区（项目 data/ 目录打包上传），
// AP 配网模式下所有请求先过 Basic Auth（admin/admin123）中间件
class WebPanel {
 public:
  // 注入各模块引用（不持有所有权）
  // bridge：AT 串口桥；led：RGB 灯；wifi：WiFi 管理器；display：屏幕
  WebPanel(SerialBridge &bridge, RgbLed &led, WifiManager &wifi, Display &display);

  // 挂载 LittleFS、注册路由并启动 HTTP 服务
  void begin();

 private:
  // 注册全部 URL 路由与处理器（精确路由需先于 serveStatic 注册）
  void setupRoutes();

  // 以 200 + application/json 发送响应体
  // req：请求对象；json：JSON 文本
  void sendJson(AsyncWebServerRequest *req, const String &json);

  AsyncWebServer _server{cfg::kHttpPort};  // 异步 HTTP 服务器（端口见配置）
  SerialBridge &_bridge;   // AT 串口桥引用
  RgbLed &_led;            // RGB 灯引用
  WifiManager &_wifi;      // WiFi 管理器引用
  Display &_display;       // 屏幕引用
};
