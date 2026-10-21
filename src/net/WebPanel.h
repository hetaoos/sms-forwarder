#pragma once

#include <ESPAsyncWebServer.h>

#include "app/Config.h"
#include "app/UIMenu.h"   // 屏幕设置的持有者（亮度 / 自动熄屏 / 自动返回状态页）
#include "hal/Display.h"
#include "hal/RgbLed.h"
#include "log/LogStore.h"
#include "net/AdminStore.h"
#include "net/EmailService.h"
#include "net/MqttService.h"
#include "net/StatusBus.h"
#include "push/PushService.h"
#include "net/WifiManager.h"
#include "serial/SerialBridge.h"
#include "sms/BlackList.h"
#include "sms/SmsService.h"
#include "ota/Ota.h"   // OTA 升级（固件 + LittleFS 烧录）

// 短信转发器 Web 控制台：状态页面 + RGB 控制 + 串口 AT 命令 + WiFi 配网 + 账号管理
// + 运行日志查询（PSRAM 环形缓冲分页拉取）
//
// 代码组织约定：setupRoutes() 是纯路由表——只写路径、方法注释与一行转发
// （_server.on(...) 内部不写业务逻辑），具体实现放在同名的 handleXxx() 成员
// 函数里。新增接口时照此办理：注册处加一行，逻辑写成一个 handleXxx()。
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
//      供脚本 / 第三方客户端使用；需在系统设置「API 访问令牌」中启用，
//      启用时自动生成，可在该页重置；关闭时 Bearer 调用一律拒绝。
// 账号密码存于 NVS（AdminStore），修改后登录会话作废（API 令牌独立保留）。
class WebPanel {
 public:
  // 注入各通信模组引用（不持有所有权）
  // bridge：AT 串口桥；led：RGB 灯；wifi：WiFi 管理器；display：屏幕；
  // ui：菜单状态机（屏幕设置的持有者，/api/disp 读写它）；
  // auth：管理账号存储（NVS）；events：SSE 状态推送总线；sms：短信服务；
  // time：时间同步（状态 JSON 输出当前时间/来源）；
  // email：SMTP 邮件通知（配置读写 + 测试发送）；
  // blacklist：号码黑名单（拦截匹配 + 配置读写）；
  // log：设备日志环形存储（PSRAM，分页查询 / 清空）
  WebPanel(SerialBridge &bridge, RgbLed &led, WifiManager &wifi, Display &display,
           UIMenu &ui, AdminStore &auth, StatusBus &events, SmsService &sms,
           TimeSync &time, EmailService &email, LogStore &log, MqttService &mqtt,
           PushService &push, BlackList &blacklist);

  // 挂载 LittleFS、注册路由并启动 HTTP 服务
  void begin();

  // 当前 Web 登录用户名（供 LCD 展示，不回传密码）
  String webUser() const;
  // 重置 Web 登录密码为随机密码：写 NVS + 作废全部会话，返回新密码（失败返回空串）
  String resetWebPassword();

 private:
  // 注册全部 URL 路由与处理器。
  //
  // 【注册顺序规则 —— 必读，新增接口时必须遵守】
  // 同一前缀下，路径长的必须先注册（子路径在前，父路径在后）：
  //   /api/sms/ports 先于 /api/sms；/api/logs/stats 先于 /api/logs；
  //   /api/email/test 先于 /api/email。
  // 原因：ESPAsyncWebServer 对普通 URI 采用 BackwardCompatible 匹配
  // （_value == path || path.startsWith(_value + "/")），父路径会连子路径一起
  // 命中；而服务器按注册顺序取第一个匹配的 handler 就结束查找。父路径若先注册，
  // 子路径请求会被父 handler 截获并返回父接口的响应（字段名对不上，页面出现
  // 「-」或按钮走错分支等隐蔽故障）。详见 setupRoutes() 前的说明。
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
  bool _apiTokenEnabled = false;           // API 访问令牌功能开关（NVS，默认关闭）
  String _apiToken;                        // 持久化 API 访问令牌（NVS，重启不失效）

  // 载入 API 令牌配置：NVS 以 JSON {"ver":1,"enabled":bool,"token":""} 存储于
  // kNvsKeyToken（沿用 apitoken 键）；缺失/解析失败视为关闭，启用但令牌缺失则生成并写回
  void loadApiToken();
  // 将当前 _apiTokenEnabled / _apiToken 以 JSON 写回 NVS
  void saveApiTokenCfg();
  // 重新生成 API 令牌（写 NVS），返回新令牌
  String resetApiToken();

  // ================= 路由处理函数 =================
  // 约定：setupRoutes() 里只注册路径 + 一行转发，具体逻辑一律在这些函数中实现，
  // 避免注册处堆积长 lambda。全部运行于 HTTP（async_tcp）任务：不得执行长耗时
  // 或大栈操作（如 SMTP/TLS），重活应入队交给主循环。

  // 中间件：/api/ 统一鉴权（登录相关端点放行）
  void handleAuthMiddleware(AsyncWebServerRequest *req, ArMiddlewareNext next);

  // 页面：首页 / 独立登录页
  void handleIndex(AsyncWebServerRequest *req);
  void handleLoginPage(AsyncWebServerRequest *req);

  // 认证：登录 / 登出 / 会话探测
  void handleLogin(AsyncWebServerRequest *req, JsonVariant &json);
  void handleLogout(AsyncWebServerRequest *req);
  void handleSession(AsyncWebServerRequest *req);

  // API 令牌与账号：查询、开关、重置、查询/修改账号
  void handleTokenGet(AsyncWebServerRequest *req);
  void handleTokenPost(AsyncWebServerRequest *req, JsonVariant &json);
  void handleTokenReset(AsyncWebServerRequest *req);
  void handleAuthGet(AsyncWebServerRequest *req);
  void handleAuthPost(AsyncWebServerRequest *req, JsonVariant &json);

  // RGB：切换下一色 / 查询 / 设置
  void handleLedNext(AsyncWebServerRequest *req);
  void handleRgbGet(AsyncWebServerRequest *req);
  void handleRgbPost(AsyncWebServerRequest *req, JsonVariant &json);

  // 串口 AT：发命令 / 历史增量 / 清空 / 重初始化 / 串口统计 / 默认波特率
  void handleAtSend(AsyncWebServerRequest *req);
  void handleAtHist(AsyncWebServerRequest *req);
  void handleAtClear(AsyncWebServerRequest *req);
  void handleAtStats(AsyncWebServerRequest *req);
  void handleAtReset(AsyncWebServerRequest *req);
  void handleUartStats(AsyncWebServerRequest *req);
  void handleAtBaud(AsyncWebServerRequest *req);

  // 设备日志：统计 / 分页与增量查询 / 清空
  void handleLogStats(AsyncWebServerRequest *req);
  void handleLogList(AsyncWebServerRequest *req);
  void handleLogClear(AsyncWebServerRequest *req);

  // 屏幕：查询 / 设置开关、亮度与自动行为（自动熄屏 / 自动返回状态页）
  void handleDispGet(AsyncWebServerRequest *req);
  void handleDispPost(AsyncWebServerRequest *req, JsonVariant &json);

  // 远程按键注入：POST {"key":"up"|"down"|"ok"|"back", "event"?:"click"|"long"|"repeat"}
  // 事件经 KeyInject 队列交给主循环 UI 状态机，与物理按键同一路径
  void handleKeyPost(AsyncWebServerRequest *req, JsonVariant &json);

  // 屏幕实时预览：抓取当前帧缓冲编码为 BMP 返回（方案 B 的 /api/screen）
  void handleScreenGet(AsyncWebServerRequest *req);

  // 系统汇总状态
  void handleStatus(AsyncWebServerRequest *req);

  // 系统维护：立即重启 / 出厂复位（清全部配置 + 用户数据）
  void handleSystemRestart(AsyncWebServerRequest *req);
  void handleSystemReset(AsyncWebServerRequest *req);
  // 定时重启配置（仅保存，不重启）
  void handleSchedPost(AsyncWebServerRequest *req, JsonVariant &json);

  // WiFi：状态 / 扫描 / 列表 / 保存 / 删除 / 排序 / 试连
  void handleWifiStatus(AsyncWebServerRequest *req);
  void handleWifiScan(AsyncWebServerRequest *req);
  void handleWifiList(AsyncWebServerRequest *req);
  void handleWifiSave(AsyncWebServerRequest *req, JsonVariant &json);
  void handleWifiDelete(AsyncWebServerRequest *req, JsonVariant &json);
  void handleWifiMove(AsyncWebServerRequest *req, JsonVariant &json);
  void handleWifiConnect(AsyncWebServerRequest *req, JsonVariant &json);
  // AP 配网热点密码：查询 / 修改（NVS 持久化，运行时可改）
  void handleWifiApGet(AsyncWebServerRequest *req);
  void handleWifiApPost(AsyncWebServerRequest *req, JsonVariant &json);

  // 短信：统计 / 最新 / 通信模组列表 / 列表 / 删除 / 配置通信模组 / 重初始化 / 发送
  void handleSmsStats(AsyncWebServerRequest *req);
  void handleSmsLatest(AsyncWebServerRequest *req);
  void handleSmsPortsGet(AsyncWebServerRequest *req);
  void handleSmsList(AsyncWebServerRequest *req);
  void handleSmsDelete(AsyncWebServerRequest *req);
  void handleSmsPortsPost(AsyncWebServerRequest *req, JsonVariant &json);
  void handleSmsReinit(AsyncWebServerRequest *req, JsonVariant &json);
  void handleSmsSend(AsyncWebServerRequest *req, JsonVariant &json);
  // 管理员短信远程控制配置：GET 读号码 / POST {"adminPhone":".."} 设置（空串=关闭）
  void handleSmsAdminGet(AsyncWebServerRequest *req);
  void handleSmsAdminPost(AsyncWebServerRequest *req, JsonVariant &json);

  // 经通信模组 ICMP 连通性测试（PING）：POST 启动（{"port":1,"host"?:"8.8.8.8}，
  // 走 SmsService::pingHost，独立 FreeRTOS 任务异步执行，不阻塞 Web 任务）；
  // GET 轮询结果 {"busy":bool,"ok":bool,"report":".."}（host 缺省 8.8.8.8）。
  void handleSmsPingPost(AsyncWebServerRequest *req, JsonVariant &json);
  void handleSmsPingGet(AsyncWebServerRequest *req);

  // 邮件：配置读取 / 总开关 / 测试发送 / 配置保存
  void handleEmailGet(AsyncWebServerRequest *req);
  void handleEmailEnabled(AsyncWebServerRequest *req, JsonVariant &json);
  void handleEmailTest(AsyncWebServerRequest *req);
  void handleEmailPost(AsyncWebServerRequest *req, JsonVariant &json);

  // MQTT：配置读取 / 总开关 / 测试发布 / 配置保存
  void handleMqttGet(AsyncWebServerRequest *req);
  void handleMqttEnabled(AsyncWebServerRequest *req, JsonVariant &json);
  void handleMqttTest(AsyncWebServerRequest *req);
  void handleMqttPost(AsyncWebServerRequest *req, JsonVariant &json);

  // 推送通道：配置读取 / 类型清单 / 总开关 / 测试发送 / 配置保存
  void handlePushGet(AsyncWebServerRequest *req);
  void handlePushTypes(AsyncWebServerRequest *req);
  void handlePushEnabled(AsyncWebServerRequest *req, JsonVariant &json);
  void handlePushTest(AsyncWebServerRequest *req);
  void handlePushPost(AsyncWebServerRequest *req, JsonVariant &json);

  // 黑名单：配置读取 / 整体保存 / 总开关 / 测试匹配
  void handleBlackGet(AsyncWebServerRequest *req);
  void handleBlackPost(AsyncWebServerRequest *req, JsonVariant &json);
  void handleBlackEnabled(AsyncWebServerRequest *req, JsonVariant &json);
  void handleBlackTest(AsyncWebServerRequest *req);

  // OTA：状态查询 / 触发版本检查 / 启动在线升级 / 本地固件上传 / 本地 Web UI 上传
  // 固件与 LittleFS 镜像均通过 onUpload 分片回调写入，避免一次性把整个文件读进内存
  void handleOtaStatus(AsyncWebServerRequest *req);
  void handleOtaVersion(AsyncWebServerRequest *req);
  void handleOtaStart(AsyncWebServerRequest *req);
  void handleOtaStartBody(AsyncWebServerRequest *req, uint8_t *data, size_t len,
                          size_t index, size_t total);
  void handleOtaUploadReq(AsyncWebServerRequest *req);
  void handleOtaUploadChunk(AsyncWebServerRequest *req, String filename, size_t index,
                            uint8_t *data, size_t len, bool final);
  void handleOtaUploadFsReq(AsyncWebServerRequest *req);
  void handleOtaUploadFsChunk(AsyncWebServerRequest *req, String filename, size_t index,
                              uint8_t *data, size_t len, bool final);

  // 配置导入导出：导出全部配置为 JSON 下载；导入 JSON 覆盖配置并重启
  void handleConfigExport(AsyncWebServerRequest *req);
  void handleConfigImportReq(AsyncWebServerRequest *req);
  void handleConfigImportChunk(AsyncWebServerRequest *req, uint8_t *data,
                              size_t len, size_t index, size_t total);

  // 崩溃转储（coredump）：信息查询 / 导出快照 / 清除分区。
  // panic 时 ESP-IDF 把 ELF 快照写入 coredump 分区，这里只做查询与搬运，
  // 元信息（崩溃时间 / 版本）由 app/Coredump 维护
  void handleCoredumpInfo(AsyncWebServerRequest *req);
  void handleCoredumpExport(AsyncWebServerRequest *req);
  void handleCoredumpClear(AsyncWebServerRequest *req);

  // 兜底 404
  void handleNotFound(AsyncWebServerRequest *req);
  SerialBridge &_bridge;   // AT 串口桥引用
  RgbLed &_led;            // RGB 灯引用
  WifiManager &_wifi;      // WiFi 管理器引用
  Display &_display;       // 屏幕引用
  UIMenu &_ui;             // 菜单状态机引用（屏幕设置的持有者）
  AdminStore &_auth;       // 管理账号存储引用（NVS）
  StatusBus &_events;      // SSE 状态推送总线引用
  SmsService &_sms;        // 短信服务引用
  TimeSync &_time;         // 时间同步引用
  EmailService &_email;    // SMTP 邮件通知引用
  MqttService &_mqtt;      // MQTT 服务引用
  PushService &_push;      // 统一 webhook 推送通道引用
  BlackList &_blacklist;   // 号码黑名单引用
  LogStore &_log;          // 设备日志存储引用（PSRAM 环形缓冲）

  // PING 任务共享结果：ping 任务写、GET 轮询读，portMUX 临界区保护
  mutable portMUX_TYPE _pingMux = portMUX_INITIALIZER_UNLOCKED;
  bool    _pingBusy = false;   // 有 PING 任务进行中
  bool    _pingOk   = false;   // 最近一次结果是否成功
  String  _pingResult;         // 最近一次结果文本（人类可读）
};
