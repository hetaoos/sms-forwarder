#pragma once

#include <ESPAsyncWebServer.h>

#include "app/Config.h"
#include "hal/Display.h"
#include "hal/RgbLed.h"
#include "net/AdminStore.h"
#include "net/StatusBus.h"
#include "net/WifiManager.h"
#include "serial/SerialBridge.h"
#include "sms/SmsService.h"

// 短信转发器 Web 控制台：状态页面 + RGB 控制 + 串口 AT 命令 + WiFi 配网 + 账号管理
// 页面资源存放在 LittleFS 专有分区（项目 data/ 目录打包上传）。
// URL 划分：页面与静态资源在根路径（公开，不含敏感数据），
// 全部 API 统一在 /api/ 前缀下（除登录/登出/会话探测外均需认证）。
//
// 身份认证支持两种方式（任一通过即可）：
//   1. 会话 Cookie：POST /api/login 成功后服务端下发 sid
//      （HttpOnly + SameSite=Strict，浏览器自动携带、无法被 JS 读取，
//      且不会随跨站请求发送，天然防 CSRF）；
//   2. 访问令牌：Authorization: Bearer <token> 头，登录响应体返回，
//      供脚本 / 第三方客户端使用。
// 身份支持两种方式（任一通过即可）：
//   1. 会话 Cookie（sid）：登录后下发，存内存，重启失效；
//   2. 访问令牌（Bearer）：长期有效的 API 令牌，持久化于 NVS，重启不失效，
//      供脚本 / 第三方客户端使用；设备首次启动自动生成，可在 UI 重置。
// 账号密码存于 NVS（AdminStore），修改后登录会话作废（API 令牌独立保留）。
class WebPanel {
 public:
  // 注入各模块引用（不持有所有权）
  // bridge：AT 串口桥；led：RGB 灯；wifi：WiFi 管理器；display：屏幕；
  // auth：管理账号存储（NVS）；events：SSE 状态推送总线；sms：短信服务；
  // time：时间同步（状态 JSON 输出当前时间/来源）
  WebPanel(SerialBridge &bridge, RgbLed &led, WifiManager &wifi, Display &display,
           AdminStore &auth, StatusBus &events, SmsService &sms, TimeSync &time);

  // 挂载 LittleFS、注册路由并启动 HTTP 服务
  void begin();

 private:
  // 注册全部 URL 路由与处理器（精确路由需先于 serveStatic 注册）
  void setupRoutes();

  // 以 200 + application/json 发送响应体
  // req：请求对象；json：JSON 文本
  void sendJson(AsyncWebServerRequest *req, const String &json);

  // 判断请求是否已认证：会话 Cookie（sid）或 Bearer 访问令牌任一有效即可
  // req：请求对象
  bool authorized(AsyncWebServerRequest *req);

  // 作废全部会话与访问令牌（账号密码变更后调用，强制重新登录）
  void invalidateAllSessions();

  // 生成 32 位十六进制随机凭证（esp_random 硬件熵源，用于 sid / Bearer 令牌）
  static String genSecret();

  // 内存凭证表操作：新增（满时返回 false）、移除、查找
  static bool storeAdd(String *store, uint8_t &count, const String &v);
  static void storeRemove(String *store, uint8_t &count, const String &v);
  static bool storeHas(const String *store, uint8_t count, const String &v);

  AsyncWebServer _server{cfg::kHttpPort};  // 异步 HTTP 服务器（端口见配置）
  String _sessions[cfg::kMaxSessions];     // 有效会话 Cookie 值（重启失效）
  String _tokens[cfg::kMaxSessions];       // 有效 Bearer 访问令牌（重启失效）
  uint8_t _sessionCount = 0;               // 会话表当前数量
  uint8_t _tokenCount = 0;                 // 令牌表当前数量
  String _apiToken;                        // 持久化 API 访问令牌（NVS，重启不失效）

  // 载入/生成持久化 API 令牌：NVS 已有则复用，否则生成并写回
  void ensureApiToken();
  // 重新生成 API 令牌（写 NVS），返回新令牌
  String resetApiToken();
  SerialBridge &_bridge;   // AT 串口桥引用
  RgbLed &_led;            // RGB 灯引用
  WifiManager &_wifi;      // WiFi 管理器引用
  Display &_display;       // 屏幕引用
  AdminStore &_auth;       // 管理账号存储引用（NVS）
  StatusBus &_events;      // SSE 状态推送总线引用
  SmsService &_sms;        // 短信服务引用
  TimeSync &_time;         // 时间同步引用
};
