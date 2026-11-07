#include "net/WebPanel.h"

#include <ArduinoJson.h>
#include <AsyncJson.h>  // AsyncJsonResponse：OTA 状态等结构化响应
#include <LittleFS.h>
#include <Preferences.h>
#include <memory>       // std::unique_ptr：/api/screen 抓图缓冲自动释放

#include <Arduino.h>

#include "app/KeyInject.h"   // 远程按键注入队列（Web 驱动设备 UI）
#include "app/Coredump.h"    // 崩溃转储（coredump）分区查询
#include "app/SystemService.h"  // 定时重启（单例）
#include "app/TaskWatchdog.h"   // 看门狗故障注入自检（POST /api/wdt/selftest）
#include "app/UserFs.h"      // userfs::format()：出厂复位清空用户数据
#include "hal/Button.h"      // KeyId / KeyEvent：/api/key 解析
#include "net/StateJson.h"
#include "ota/Ota.h"  // OTA 升级（固件 + LittleFS 烧录）

#include <esp_system.h>            // esp_restart()
#include <esp_random.h>             // esp_random()：随机 Web 密码生成
#include <freertos/FreeRTOS.h>    // xTaskCreate / vTaskDelay
#include <freertos/task.h>
#include <time.h>                 // localtime_r / time()

namespace {

// 前向声明：公共 JSON 应答辅助函数（定义见文件后部）
void sendJsonDoc(AsyncWebServerRequest *req, const JsonDocument &doc);
void sendErr(AsyncWebServerRequest *req, int code, const char *msg);

// 取字符串参数；req：请求对象；name：参数名；def：缺省值
// 参数存在时返回其值（指针生命周期跟随请求），否则返回 def
const char *param(AsyncWebServerRequest *req, const char *name,
                  const char *def) {
  return req->hasParam(name) ? req->getParam(name)->value().c_str() : def;
}

// 取整型参数；req：请求对象；name：参数名；def：缺省值
int paramInt(AsyncWebServerRequest *req, const char *name, int def) {
  return req->hasParam(name) ? req->getParam(name)->value().toInt() : def;
}

// 取字符串参数；req：请求对象；name：参数名；不存在时返回空串
String stringParam(AsyncWebServerRequest *req, const char *name) {
  if (!req->hasParam(name)) return String();
  return req->getParam(name)->value();
}

// 生成随机可读密码（去掉易混字符 0/O/1/l/I），默认 16 位
String genRandomPassword(size_t len = 16) {
  static const char k[] = "ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnpqrstuvwxyz23456789";
  String s;
  s.reserve(len);
  for (size_t i = 0; i < len; i++) s += k[esp_random() % (sizeof(k) - 1)];
  return s;
}

// 从 LittleFS 发送页面资源：优先同名 .gz（省 flash，浏览器自动解压），
// 否则原文件；都不存在返回 404。
// 一律附 Cache-Control: no-store：设备会频繁重烧页面，禁缓存可避免
// 浏览器拿到旧的 index.html / app.js / login.html 导致行为不一致
// （如旧 app.js 缺少新增组件、旧登录页仍发表单编码数据）
// req：请求对象；path：LittleFS 内的路径（如 "/index.html"）；mime：MIME 类型
void sendFile(AsyncWebServerRequest *req, const char *path, const char *mime) {
  String gz = String(path) + ".gz";
  if (LittleFS.exists(gz.c_str())) {
    AsyncWebServerResponse *resp = req->beginResponse(LittleFS, gz, mime);
    resp->addHeader("Content-Encoding", "gzip");
    resp->addHeader("Cache-Control", "no-store");
    req->send(resp);
    return;
  }
  if (LittleFS.exists(path)) {
    AsyncWebServerResponse *resp = req->beginResponse(LittleFS, path, mime);
    resp->addHeader("Cache-Control", "no-store");
    req->send(resp);
    return;
  }
  sendErr(req, 404, "文件不存在");
}

// 从 Cookie 头中解析指定名称的值；不存在返回空串
// req：请求对象；name：Cookie 名（如 "sid"）
String cookieValue(AsyncWebServerRequest *req, const char *name) {
  if (!req->hasHeader("Cookie")) return String();
  String cookies = req->getHeader("Cookie")->value();
  String key = String(name) + "=";
  int start = 0;
  while (start < (int)cookies.length()) {
    int sep = cookies.indexOf(';', start);
    String pair = (sep < 0 ? cookies.substring(start) : cookies.substring(start, sep));
    pair.trim();
    if (pair.startsWith(key)) {
      String v = pair.substring(key.length());
      v.trim();
      return v;
    }
    if (sep < 0) break;
    start = sep + 1;
  }
  return String();
}

// 发送带 Set-Cookie 头的 JSON 响应（登录/登出使用）
// req：请求对象；json：响应体；cookie：Set-Cookie 值
void sendJsonWithCookie(AsyncWebServerRequest *req, const String &json,
                        const String &cookie) {
  AsyncWebServerResponse *resp = req->beginResponse(200, "application/json", json);
  resp->addHeader("Set-Cookie", cookie);
  req->send(resp);
}

// 统一错误响应：{"ok":false,"error":".."} + HTTP 状态码
// req：请求对象；code：HTTP 状态码；msg：错误描述（自动转义）
void sendErr(AsyncWebServerRequest *req, int code, const char *msg) {
  JsonDocument doc;
  doc["ok"] = false;
  doc["error"] = msg ? msg : "";
  String out;
  serializeJson(doc, out);
  req->send(code, "application/json", out);
}

// 统一成功响应：{"ok":true}
void sendOk(AsyncWebServerRequest *req) {
  JsonDocument doc;
  doc["ok"] = true;
  sendJsonDoc(req, doc);
}

// 把已构造好的 JsonDocument 序列化并以 200 + application/json 发出
// （替代每个 handler 重复的 String out; serializeJson(doc,out); req->send(...)）
void sendJsonDoc(AsyncWebServerRequest *req, const JsonDocument &doc) {
  String out;
  serializeJson(doc, out);
  req->send(200, "application/json", out);
}

// 成功响应并附带一条消息：{"ok":true,"message":"..."}
void sendOkMsg(AsyncWebServerRequest *req, const char *msg) {
  JsonDocument doc;
  doc["ok"] = true;
  doc["message"] = msg ? msg : "";
  sendJsonDoc(req, doc);
}

// 成功响应并附带一个布尔字段：{"ok":true,"<key>":v}
void sendOkFlag(AsyncWebServerRequest *req, const char *key, bool v) {
  JsonDocument doc;
  doc["ok"] = true;
  doc[key] = v;
  sendJsonDoc(req, doc);
}

// 成功响应并附带一个整数字段：{"ok":true,"<key>":v}
void sendOkInt(AsyncWebServerRequest *req, const char *key, int v) {
  JsonDocument doc;
  doc["ok"] = true;
  doc[key] = v;
  sendJsonDoc(req, doc);
}

// 带 Set-Cookie 的成功响应：序列化 JsonDocument 并附加 Cookie 头
void sendJsonDocWithCookie(AsyncWebServerRequest *req, const JsonDocument &doc,
                           const String &cookie) {
  String out;
  serializeJson(doc, out);
  AsyncWebServerResponse *resp = req->beginResponse(200, "application/json", out);
  resp->addHeader("Set-Cookie", cookie);
  req->send(resp);
}

// 从 JSON 变体取整数：兼容 int 与数字字符串
// （部分客户端/表单序列化会把数值发成字符串，宽松处理避免静默失败）
// v：JSON 变体；out：输出；解析成功返回 true
bool jsonInt(const JsonVariant &v, int &out) {
  if (v.is<int>()) {
    out = v.as<int>();
    return true;
  }
  if (v.is<float>()) {  // 50.0 也接受，截断取整
    out = (int)v.as<float>();
    return true;
  }
  if (v.is<const char *>()) {
    String s = v.as<String>();
    s.trim();
    if (!s.length()) return false;
    for (size_t i = 0; i < s.length(); i++) {
      char c = s[i];
      bool ok = (c >= '0' && c <= '9') || (i == 0 && (c == '-' || c == '+'));
      if (!ok) return false;
    }
    out = s.toInt();
    return true;
  }
  return false;
}

// 从 JSON 变体取布尔：兼容 bool 与 "true"/"1"/"false"/"0" 字符串
bool jsonBool(const JsonVariant &v, bool &out) {
  if (v.is<bool>()) {
    out = v.as<bool>();
    return true;
  }
  if (v.is<const char *>()) {
    String s = v.as<String>();
    if (s == "true" || s == "1") { out = true; return true; }
    if (s == "false" || s == "0") { out = false; return true; }
  }
  return false;
}

}  // namespace

// bridge/led/wifi/display/ui/auth/events/sms/time/email/blacklist/log/mqtt/push：各通信模组引用
WebPanel::WebPanel(SerialBridge &bridge, RgbLed &led, WifiManager &wifi,
                   Display &display, UIMenu &ui, AdminStore &auth,
                   StatusBus &events, SmsService &sms, TimeSync &time,
                   EmailService &email, LogStore &log, MqttService &mqtt,
                   PushService &push, BlackList &blacklist, Outbox &outbox)
    : _bridge(bridge), _led(led), _wifi(wifi), _display(display), _ui(ui),
      _auth(auth), _events(events), _sms(sms), _time(time), _email(email),
      _mqtt(mqtt), _push(push), _blacklist(blacklist), _outbox(outbox),
      _log(log) {}

// 挂载 LittleFS 专有分区，注册路由与认证中间件，启动 HTTP 服务
void WebPanel::begin() {
  loadApiToken();     // 载入 API 令牌配置（NVS，默认关闭）
  // 挂载失败时格式化一次重试；仍失败则仅 API 可用，页面返回 404。
  // 显式指定 label（与 ota/Ota.cpp 的 findFsPartition 同源宏），避免依赖
  // LittleFS 库默认 label；base_path "/littlefs" 为库默认值，保持现状。
  // 挂载资源分区：先以 formatOnFail=false 尝试，避免「分区本有效却因瞬时故障
  // 挂载失败」时被自动格式化清空（OTA 写入不完整等情况会触发，清空后 Web UI
  // 永久丢失）。仅当确实无有效文件系统（首次 / 损坏）时才 begin(true) 格式化一次。
  if (!LittleFS.begin(false, "/littlefs", 10, cfg::kPartLabelSpiffs)) {
    _log.addf(LogLevel::kWarn, "http", "LittleFS 初次挂载失败，尝试格式化重建");
    if (!LittleFS.begin(true, "/littlefs", 10, cfg::kPartLabelSpiffs)) {
      _log.addf(LogLevel::kError, "http", "LittleFS 挂载失败，网页不可用");
    } else {
      _log.addf(LogLevel::kInfo, "http", "LittleFS 已挂载（已重建）");
    }
  } else {
    _log.addf(LogLevel::kInfo, "http", "LittleFS 已挂载");
  }

  setupRoutes();
  _events.begin(_server);   // 注册 SSE 事件流（/api/sse，复用上面的认证中间件）
  _server.begin();
  _log.addf(LogLevel::kInfo, "http", "HTTP 服务已启动，端口 %u", cfg::kHttpPort);
}

// 生成 32 位十六进制随机凭证（esp_random 硬件熵源，用于 sid / Bearer 令牌）
String WebPanel::genSecret() {
  static const char kHex[] = "0123456789abcdef";
  uint8_t buf[16];
  for (size_t i = 0; i < sizeof(buf); i += 4) {
    uint32_t r = esp_random();
    memcpy(&buf[i], &r, sizeof(buf) - i < 4 ? sizeof(buf) - i : 4);
  }
  String s;
  s.reserve(sizeof(buf) * 2);
  for (size_t i = 0; i < sizeof(buf); i++) {
    s += kHex[buf[i] >> 4];
    s += kHex[buf[i] & 0x0F];
  }
  return s;
}

// 判断请求是否已认证：会话 Cookie（sid）或 Bearer 访问令牌任一有效即可
// req：请求对象
bool WebPanel::authorized(AsyncWebServerRequest *req) {
  String sid = cookieValue(req, cfg::kCookieName);
  if (sid.length() && storeHas(_sessions, _sessionCount, sid)) return true;
  if (req->hasHeader("Authorization")) {
    String a = req->getHeader("Authorization")->value();
    if (a.startsWith("Bearer ")) {
      String t = a.substring(7);
      if (t.length() && _apiTokenEnabled &&
          (storeHas(_tokens, _tokenCount, t) || t == _apiToken)) return true;
    }
  }
  return false;
}

// 作废全部会话与访问令牌（账号密码变更后调用，所有客户端需重新登录）
void WebPanel::invalidateAllSessions() {
  _sessionCount = 0;
  _tokenCount = 0;
  _log.addf(LogLevel::kInfo, "http", "已注销全部会话与访问令牌");
}

// 当前 Web 登录用户名（供 LCD 展示，不回传密码）
String WebPanel::webUser() const { return _auth.user(); }

// 重置 Web 登录密码为随机密码：写 NVS + 作废全部会话，返回新密码（失败返回空串）
String WebPanel::resetWebPassword() {
  String np = genRandomPassword();
  String err;
  if (!_auth.update(_auth.user(), np, &err)) {
    _log.addf(LogLevel::kWarn, "auth", "LCD 重置 Web 密码失败：%s", err.c_str());
    return String();
  }
  invalidateAllSessions();  // 旧密码与所有会话立即失效
  _log.addf(LogLevel::kInfo, "auth", "LCD 重置 Web 登录密码为随机密码");
  return np;
}

// 载入 API 令牌配置：NVS 以 JSON {"ver":1,"enabled":bool,"token":""} 存储于
// kNvsKeyToken（沿用 apitoken 键）；缺失/解析失败视为关闭，启用但令牌缺失则生成并写回。
// 旧版明文令牌（非 JSON）不兼容，按未启用处理（默认关闭，符合需求）。
void WebPanel::loadApiToken() {
  _apiTokenEnabled = false;
  _apiToken = "";
  String raw;
  Preferences prefs;
  if (prefs.begin(cfg::kNvsNamespace, true)) {
    raw = prefs.getString(cfg::kNvsKeyToken, "");
    prefs.end();
  }
  JsonDocument doc;
  if (!deserializeJson(doc, raw) && doc.is<JsonObject>()) {
    _apiTokenEnabled = doc["enabled"] | false;
    _apiToken = doc["token"] | "";
  }
  if (_apiTokenEnabled && _apiToken.length() != 32) {
    _apiToken = genSecret();
    saveApiTokenCfg();
    _log.addf(LogLevel::kInfo, "http", "API 令牌启用且缺失，已生成并写入 NVS");
  }
  _log.addf(LogLevel::kInfo, "http", "API 令牌功能%s",
            _apiTokenEnabled ? "已启用" : "未启用（默认）");
}

// 将当前 _apiTokenEnabled / _apiToken 以 JSON 写回 NVS（apitoken 键）
void WebPanel::saveApiTokenCfg() {
  JsonDocument doc;
  doc["ver"] = 1;
  doc["enabled"] = _apiTokenEnabled;
  doc["token"] = _apiToken;
  String s;
  serializeJson(doc, s);
  Preferences prefs;
  if (prefs.begin(cfg::kNvsNamespace, false)) {
    prefs.putString(cfg::kNvsKeyToken, s);
    prefs.end();
  }
}

// 重新生成 API 令牌（写 NVS），返回新令牌
String WebPanel::resetApiToken() {
  _apiToken = genSecret();
  saveApiTokenCfg();
  _log.addf(LogLevel::kInfo, "http", "API 令牌已重置");
  return _apiToken;
}

// 凭证表新增：store 数组；count：数量引用；v：凭证值；满返回 false
bool WebPanel::storeAdd(String *store, uint8_t &count, const String &v) {
  if (!v.length() || count >= cfg::kMaxSessions) return false;
  store[count++] = v;
  return true;
}

// 凭证表移除：找到后与末位交换删除（数组无序）
void WebPanel::storeRemove(String *store, uint8_t &count, const String &v) {
  for (uint8_t i = 0; i < count; i++) {
    if (store[i] == v) {
      store[i] = store[--count];
      return;
    }
  }
}

// 凭证表查找：store 数组；count：数量；v：凭证值
bool WebPanel::storeHas(const String *store, uint8_t count, const String &v) {
  if (!v.length()) return false;
  for (uint8_t i = 0; i < count; i++) {
    if (store[i] == v) return true;
  }
  return false;
}

// req：请求对象；json：要发送的 JSON 文本
void WebPanel::sendJson(AsyncWebServerRequest *req, const String &json) {
  req->send(200, "application/json", json);
}

// 注册全部路由（纯路由表：只注册 + 一行转发，逻辑见下方 handleXxx()）。
//
// ============================ 注册顺序规则（必读）============================
// 同一前缀下，【路径长的必须排在前面】：子路径先注册，父路径后注册。
//   /api/sms/ports   先于  /api/sms
//   /api/logs/stats  先于  /api/logs
//   /api/email/test  先于  /api/email
// 原因：ESPAsyncWebServer 对普通 URI 采用 BackwardCompatible 匹配（见库
// src/WebServer.cpp 的 AsyncURIMatcher::matches）：
//       (_value == path) || path.startsWith(_value + "/")
// 即注册 "/api/sms" 的 handler 会同时命中 "/api/sms" 与 "/api/sms/ports"；
// 而服务器按注册顺序取【第一个】匹配的 handler 就结束查找，后面的不再比对。
// 父路径若先注册，子路径请求会被父 handler 截获，返回的是父接口的响应
// （字段名对不上 —— 表现为页面出现「-」、按钮走错分支等隐蔽故障）。
// 例外：父路径未注册该方法时不受影响（如 /api/sms 没有 POST，则
// POST /api/sms/ports 放在后面也安全）；但为一致与可维护，仍统一长路径在前。
// 新增接口时请照此排序，不要只按功能分组插到父路径后面。
// ============================================================================
//
// 其余约定：
// 1. 认证中间件：/api/ 前缀统一要求登录（Cookie 或 Bearer 令牌），
//    登录/登出/会话探测三个端点放行；页面与静态资源公开（不含敏感数据）；
// 2. 精确路由（API / 首页）先注册，serveStatic 兜底其余静态资源；
// 3. 每组内先列子路径（长），再列父路径（短），两者之间用注释标明接口契约。
void WebPanel::setupRoutes() {
  // /api/ 统一鉴权（登录相关端点放行），实现见 handleAuthMiddleware()
  _server.addMiddleware([this](AsyncWebServerRequest *req, ArMiddlewareNext next) {
    handleAuthMiddleware(req, next);
  });

  // ---- 页面与静态资源（LittleFS，公开）----
  // 首页：短信转发器 Web 控制台单页应用（左侧菜单 + 右侧内容，hash 路由）
  _server.on("/", HTTP_GET, [this](AsyncWebServerRequest *req) { handleIndex(req); });
  // 独立登录页（已登录时页面脚本自行跳回 /）
  _server.on("/login", HTTP_GET, [this](AsyncWebServerRequest *req) { handleLoginPage(req); });
  // 其余静态资源（css/js/图标等）直接映射到 LittleFS 根目录；
  // serveStatic 会自动探测同名 .gz 文件；no-store 防浏览器缓存旧版本
  _server.serveStatic("/", LittleFS, "/", "no-store");

  // ---- 认证 ----
  // 登录：JSON 请求体 {"user":"..","pass":".."} -> 下发 sid Cookie +
  // 返回 {"ok":true,"user":"..","token":".."}。
  // 用 AsyncCallbackJsonWebHandler 重载：body 由库自动累积（无副作用），
  // 解析与业务逻辑经中间件链在认证之后执行；Content-Type 必须是
  // application/json，否则该处理器不匹配（JSON-only API）。
  // Cookie 为 HttpOnly + SameSite=Strict（浏览器自动携带且不随跨站请求
  // 发送，防 CSRF）；返回的 token 供脚本/第三方客户端以 Bearer 头使用
  _server.on("/api/login", HTTP_POST,
             [this](AsyncWebServerRequest *req, JsonVariant &json) { handleLogin(req, json); });

  // 登出：作废当前会话 Cookie 与 Bearer 令牌，并清除浏览器 Cookie
  _server.on("/api/logout", HTTP_POST, [this](AsyncWebServerRequest *req) { handleLogout(req); });

  // 会话探测：前端据此判断是否已登录（未认证返回 401）
  _server.on("/api/session", HTTP_GET, [this](AsyncWebServerRequest *req) { handleSession(req); });

  // ---- API 访问令牌（持久化，重启不失效；默认关闭，需在系统设置启用）----
  // 重置令牌（子路径，先注册）：生成新令牌并写 NVS，旧令牌立即失效（需启用）。
  // 普通 POST（无需 JSON body），避免缺少 Content-Type 时 404。
  _server.on("/api/token/reset", HTTP_POST,
             [this](AsyncWebServerRequest *req) { handleTokenReset(req); });
  // 查询当前令牌与开关（需登录）；UI 默认掩码展示，点按后才显示完整
  _server.on("/api/token", HTTP_GET, [this](AsyncWebServerRequest *req) { handleTokenGet(req); });
  // 开关 API 令牌功能：JSON {"enabled":bool}；启用且无令牌时自动生成
  _server.on("/api/token", HTTP_POST,
             [this](AsyncWebServerRequest *req, JsonVariant &json) { handleTokenPost(req, json); });

  // ---- 管理账号 ----
  // 查询当前用户名（密码不回传）
  _server.on("/api/auth", HTTP_GET, [this](AsyncWebServerRequest *req) { handleAuthGet(req); });

  // 修改用户名 / 密码：JSON {"user":"新用户","pass":"新密码","passOld":"当前密码"}
  // 需验证当前密码（防登录态被冒用）；成功后作废全部会话与令牌，
  // 所有客户端（含当前浏览器）需用新账号重新登录
  _server.on("/api/auth", HTTP_POST,
             [this](AsyncWebServerRequest *req, JsonVariant &json) { handleAuthPost(req, json); });

  // ---- RGB ----
  // 切换到下一个预设颜色，返回最新 RGB 状态
  _server.on("/api/led", HTTP_GET, [this](AsyncWebServerRequest *req) { handleLedNext(req); });

  // 查询 RGB 状态：{"ok":true,"color":名,"rgb":[r,g,b],"index":n,"bright":0-100}
  _server.on("/api/rgb", HTTP_GET, [this](AsyncWebServerRequest *req) { handleRgbGet(req); });

  // 设置 RGB：JSON，三个维度可任意组合（都可选）：
  //   {"c":索引}        走预设（0-4，越界 400）
  //   {"r":n,"g":n,"b":n}  任意颜色（0-255，越界钳制）
  //   {"bright":0-100}  亮度百分比（越界钳制，实时折算输出）
  _server.on("/api/rgb", HTTP_POST,
             [this](AsyncWebServerRequest *req, JsonVariant &json) { handleRgbPost(req, json); });

  // ---- 模组调试（AT 命令） ----
  // 发送 AT 命令：?port=1|2&baud=115200&eol=0..3&cmd=...&t=超时ms
  // 经短信通信模组发送（与短信收发共用端口锁、自动还原 CMGF/CNMI），避免与短信冲突
  _server.on("/api/at/send", HTTP_GET, [this](AsyncWebServerRequest *req) { handleAtSend(req); });

  // 增量拉取历史：?after=上一页最后ID&n=条数
  _server.on("/api/at/hist", HTTP_GET, [this](AsyncWebServerRequest *req) { handleAtHist(req); });

  // 清空历史记录
  _server.on("/api/at/clear", HTTP_GET, [this](AsyncWebServerRequest *req) { handleAtClear(req); });

  // AT 历史存储统计（PSRAM 环形缓冲，与 /api/logs/stats 对称）
  _server.on("/api/at/stats", HTTP_GET, [this](AsyncWebServerRequest *req) { handleAtStats(req); });

  // 模组串口重新初始化（手动兜底）：?port=1|2
  _server.on("/api/at/reset", HTTP_GET, [this](AsyncWebServerRequest *req) { handleAtReset(req); });

  // 模组串口运行统计 JSON
  _server.on("/api/uart/stats", HTTP_GET, [this](AsyncWebServerRequest *req) { handleUartStats(req); });

  // 查询默认波特率（供前端下拉框初始化）
  _server.on("/api/at/baud", HTTP_GET, [this](AsyncWebServerRequest *req) { handleAtBaud(req); });

  // ================= 设备日志（PSRAM 环形缓冲）=================
  // 路由顺序注意：ESPAsyncWebServer 对普通 URI 采用「精确 或 前缀 + /」匹配
  // （AsyncURIMatcher 的 BackwardCompatible），handler 按注册顺序取第一个匹配，
  // 故子路径（/api/logs/stats）必须先于父路径（/api/logs）注册，否则会被父路由
  // 吞掉——表现为返回父路由的 JSON（字段对不上，页面出现「-」）。

  // 存储统计：{"ok":true,"total":n,"max":n,"psram":bool,"bytes":n,
  //  "psramTotal":n,"psramFree":n}
  // （psram 表示环形缓冲实际是否落在 PSRAM，bytes 为占用字节数）
  _server.on("/api/logs/stats", HTTP_GET, [this](AsyncWebServerRequest *req) { handleLogStats(req); });

  // 分页查询：?page=1&pageSize=20&level=2&q=关键字
  // page 从 1 起；pageSize 越界钳制（上限 kLogPageSizeMax）；
  // level 为「最低级别」过滤（0 调试 / 1 信息 / 2 警告 / 3 错误，缺省不过滤）；
  // q 对 tag/msg 做大小写不敏感的子串匹配；结果按 ID 倒序（最新在前）：
  // {"ok":true,"total":n,"page":p,"pageSize":ps,
  //  "logs":[{id,ts,level,tag,msg}...]}
  _server.on("/api/logs", HTTP_GET, [this](AsyncWebServerRequest *req) { handleLogList(req); });

  // 清空全部日志（逻辑清空，ID 计数保留，便于前端区分新旧）；
  // 清空动作本身补记一条日志
  _server.on("/api/logs", HTTP_DELETE, [this](AsyncWebServerRequest *req) { handleLogClear(req); });

  // ---- 屏幕开关 / 亮度 ----
  // 查询当前状态：{"ok":true,"on":bool,"bright":0-100}
  _server.on("/api/disp", HTTP_GET, [this](AsyncWebServerRequest *req) { handleDispGet(req); });

  // 屏幕实时预览：抓取帧缓冲编码 BMP 返回（方案 B）；前端 <img> 轮询实现 ~1-2fps
  _server.on("/api/screen", HTTP_GET, [this](AsyncWebServerRequest *req) { handleScreenGet(req); });

  // 设置屏幕：JSON {"on":bool?,"bright":0-100?}，两个字段都可选
  // （只传其一即可；bright 越界自动钳制）；返回最新状态
  _server.on("/api/disp", HTTP_POST,
             [this](AsyncWebServerRequest *req, JsonVariant &json) { handleDispPost(req, json); });

  // 远程按键注入：POST {"key":"up"|"down"|"ok"|"back"，可选 "event":"click"|"long"|"repeat"}
  // 仅 click 默认可不传；事件经队列交给主循环 UI 状态机（与物理按键同路径）
  _server.on("/api/key", HTTP_POST,
             [this](AsyncWebServerRequest *req, JsonVariant &json) { handleKeyPost(req, json); });

  // ---- 系统 ----
  // 汇总状态 JSON（页面为静态文件，所有状态由前端从此接口拉取渲染；
  // 浏览器端同时经 /api/sse 实时推送，序列化共用 StateJson.h）
  _server.on("/api/status", HTTP_GET, [this](AsyncWebServerRequest *req) { handleStatus(req); });

  // ================= 系统维护 =================
  // 立即重启（延迟 2s，给响应留出发送时间）
  _server.on("/api/system/restart", HTTP_POST,
             [this](AsyncWebServerRequest *req) { handleSystemRestart(req); });
  // 出厂复位：清除 NVS 全部配置 + 格式化用户数据分区，随后重启
  _server.on("/api/system/reset", HTTP_POST,
             [this](AsyncWebServerRequest *req) { handleSystemReset(req); });

  // 定时重启配置（独立接口：仅保存，即时生效，不重启设备）
  _server.on("/api/sched", HTTP_POST,
             [this](AsyncWebServerRequest *req, JsonVariant &json) {
               handleSchedPost(req, json);
             });

  // 任务看门狗故障注入自检：POST {"task":"loopTask","timeoutMs":10000?}。
  // 仅停止指定任务的喂狗，约 timeoutMs 后 TWDT panic 自动重启；会中断服务，
  // 需认证（全局 /api/ 中间件）+ 前端二次确认，仅供设备验收使用。
  _server.on("/api/wdt/selftest", HTTP_POST,
             [this](AsyncWebServerRequest *req, JsonVariant &json) {
               handleWdtSelfTest(req, json);
             });

  // ================= WiFi 配网 =================
  // 状态查询：模式 / 连接 / AP 信息 / 已保存数量
  _server.on("/api/wifi/status", HTTP_GET, [this](AsyncWebServerRequest *req) { handleWifiStatus(req); });

  // 扫描附近 WiFi：扫描中返回 {"busy":1}，完成后返回 {"busy":0,"nets":[...]}
  // scanComplete() 语义：RUNNING=扫描中 / FAILED=无扫描且无结果 / >=0=结果就绪
  _server.on("/api/wifi/scan", HTTP_GET, [this](AsyncWebServerRequest *req) { handleWifiScan(req); });

  // 已保存的 WiFi 配置列表（启动时按顺序尝试）
  _server.on("/api/wifi/list", HTTP_GET, [this](AsyncWebServerRequest *req) { handleWifiList(req); });

  // 保存配置：JSON {"ssid":"..","pass":"..","connect":true}
  // SSID 已存在则覆盖密码；成功后按需立即尝试连接
  _server.on("/api/wifi/save", HTTP_POST,
             [this](AsyncWebServerRequest *req, JsonVariant &json) { handleWifiSave(req, json); });

  // 删除配置：JSON {"index":列表下标}
  _server.on("/api/wifi/delete", HTTP_POST,
             [this](AsyncWebServerRequest *req, JsonVariant &json) { handleWifiDelete(req, json); });

  // 调整顺序：JSON {"from":原下标,"to":新下标}
  _server.on("/api/wifi/move", HTTP_POST,
             [this](AsyncWebServerRequest *req, JsonVariant &json) { handleWifiMove(req, json); });

  // 试连（不保存）：JSON {"ssid":"..","pass":".."}
  _server.on("/api/wifi/connect", HTTP_POST,
             [this](AsyncWebServerRequest *req, JsonVariant &json) { handleWifiConnect(req, json); });

  // AP 配网热点密码：GET 查询当前（secure + pass），POST 修改（empty=开放）
  _server.on("/api/wifi/appass", HTTP_GET, [this](AsyncWebServerRequest *req) { handleWifiApGet(req); });
  _server.on("/api/wifi/appass", HTTP_POST,
             [this](AsyncWebServerRequest *req, JsonVariant &json) { handleWifiApPost(req, json); });

  // ================= 短信 =================
  // 顺序：所有 /api/sms/<子路径> 先注册，/api/sms（父路径）放最后

  // 存储统计：{"ok":true,"total":n,"max":n,"used":bytes,"fsTotal":bytes}
  _server.on("/api/sms/stats", HTTP_GET, [this](AsyncWebServerRequest *req) { handleSmsStats(req); });

  // 最新一条短信摘要（仪表盘用；与 SSE sms 事件同格式），无记录返回 {}
  _server.on("/api/sms/latest", HTTP_GET, [this](AsyncWebServerRequest *req) { handleSmsLatest(req); });

  // 通信模组列表：{"ok":true,"modems":[{"port":1,"enabled":true},...]}
  _server.on("/api/sms/ports", HTTP_GET, [this](AsyncWebServerRequest *req) { handleSmsPortsGet(req); });

  // 配置通信模组：JSON {"port":1,"enabled":true,"alias":"客厅卡槽"}（enabled/alias 可缺省其一）
  _server.on("/api/sms/ports", HTTP_POST,
             [this](AsyncWebServerRequest *req, JsonVariant &json) { handleSmsPortsPost(req, json); });

  // 重新初始化通信模组：JSON {"port":1}（仅启用态有效；置位 needReinit 由抽数任务重跑）
  _server.on("/api/sms/reinit", HTTP_POST,
             [this](AsyncWebServerRequest *req, JsonVariant &json) { handleSmsReinit(req, json); });

  // 发送短信：POST 启动（{"port":1,"phone":"..","content":".."}）后立即返回
  // {"ok":true,"started":true}；实际阻塞式 CMGS 握手在独立任务执行，结果经
  // GET /api/sms/send 轮询（{busy,ok,id,stored,warn,err}）。如此 Web（async_tcp）
  // 任务不被长时间阻塞，避免饿死任务看门狗导致整机复位。
  _server.on("/api/sms/send", HTTP_POST,
             [this](AsyncWebServerRequest *req, JsonVariant &json) { handleSmsSend(req, json); });
  _server.on("/api/sms/send", HTTP_GET, [this](AsyncWebServerRequest *req) { handleSmsSendGet(req); });

  // 管理员短信远程控制：GET -> {"ok":true,"enabled":bool,"phones":[".."]}；
  // POST {"enabled":bool,"phones":["138...","139..."]}（字段均可选；空 phones = 关闭；
  // enabled 缺省表示不改开关）。开关与号码任一校验失败返回 400。
  _server.on("/api/sms/admin", HTTP_GET,
             [this](AsyncWebServerRequest *req) { handleSmsAdminGet(req); });
  _server.on("/api/sms/admin", HTTP_POST,
             [this](AsyncWebServerRequest *req, JsonVariant &json) { handleSmsAdminPost(req, json); });

  // 经通信模组 PING：POST 启动（{"port":1,"host"?:"8.8.8.8}），GET 轮询结果（{busy,ok,report}）
  _server.on("/api/sms/ping", HTTP_POST,
             [this](AsyncWebServerRequest *req, JsonVariant &json) { handleSmsPingPost(req, json); });
  _server.on("/api/sms/ping", HTTP_GET, [this](AsyncWebServerRequest *req) { handleSmsPingGet(req); });

  // 主动查询通信模组存储中的短信（存储模式兜底）：POST {"port":n?} 触发
  // （缺省 = 全部启用模组），异步执行；GET 返回查询状态
  _server.on("/api/sms/query", HTTP_POST,
             [this](AsyncWebServerRequest *req, JsonVariant &json) { handleSmsQueryPost(req, json); });
  _server.on("/api/sms/query", HTTP_GET, [this](AsyncWebServerRequest *req) { handleSmsQueryGet(req); });

  // 持久化发件箱（不丢消息转发队列）：子路径必须先于 /api/outbox 注册
  // GET  ?page=&pageSize=&id= -> 统计 + 分页记录 + 每通道投递回执
  // POST retry/clear/delete -> JSON {"id":n}（retry/clear 的 id 可省略）
  _server.on("/api/outbox/retry", HTTP_POST,
             [this](AsyncWebServerRequest *req, JsonVariant &json) { handleOutboxRetry(req, json); });
  _server.on("/api/outbox/clear", HTTP_POST,
             [this](AsyncWebServerRequest *req) { handleOutboxClear(req); });
  _server.on("/api/outbox/delete", HTTP_POST,
             [this](AsyncWebServerRequest *req, JsonVariant &json) { handleOutboxDelete(req, json); });
  _server.on("/api/outbox", HTTP_GET,
             [this](AsyncWebServerRequest *req) { handleOutboxGet(req); });

  // 清空指定通信模组 SIM 上的全部短信：POST {"port":n} 异步触发（查询任务执行），
  // GET 返回清空状态（复用查询状态 JSON：各端口 clearBusy / 上次结果）。
  _server.on("/api/sms/clear", HTTP_POST,
             [this](AsyncWebServerRequest *req, JsonVariant &json) { handleSmsClearPost(req, json); });
  _server.on("/api/sms/clear", HTTP_GET, [this](AsyncWebServerRequest *req) {
    sendJson(req, _sms.queryStatusJson());
  });

  // 定时主动查询配置：GET 读当前配置；POST {"enabled","mode","intervalH","hour","minute"}
  _server.on("/api/sms/poll", HTTP_GET, [this](AsyncWebServerRequest *req) { handleSmsPollGet(req); });
  _server.on("/api/sms/poll", HTTP_POST,
             [this](AsyncWebServerRequest *req, JsonVariant &json) { handleSmsPollPost(req, json); });

  // 短信接收模式（CNMI）：GET 读当前模式；POST {"store":bool} 切换
  _server.on("/api/sms/store", HTTP_GET, [this](AsyncWebServerRequest *req) { handleSmsStoreGet(req); });
  _server.on("/api/sms/store", HTTP_POST,
             [this](AsyncWebServerRequest *req, JsonVariant &json) { handleSmsStorePost(req, json); });

  // ---- 以下是父路径 /api/sms，必须排在上述子路径之后 ----
  // 列表：?page=&pageSize=&q=&port= -> {"ok":true,"total":n,"page":p,
  // "pageSize":ps,"messages":[{id,dir,phone,content,ts,modem}],...}
  _server.on("/api/sms", HTTP_GET, [this](AsyncWebServerRequest *req) { handleSmsList(req); });

  // 删除：?id=n 删单条（不存在 404）；?all=1 清空
  _server.on("/api/sms", HTTP_DELETE, [this](AsyncWebServerRequest *req) { handleSmsDelete(req); });

  // ================= 黑名单 =================
  // 顺序：/api/blacklist/<子路径> 先注册，/api/blacklist（父路径）放最后
  // 总开关：JSON {"enabled":true|false} —— 只翻转启用位，不动规则与动作
  _server.on("/api/blacklist/enabled", HTTP_POST,
             [this](AsyncWebServerRequest *req, JsonVariant &json) { handleBlackEnabled(req, json); });

  // 测试匹配：?num=... 返回 {"ok":true,"matched":bool,"scope":n,"note":".."}
  _server.on("/api/blacklist/test", HTTP_GET,
             [this](AsyncWebServerRequest *req) { handleBlackTest(req); });

  // ---- 以下是父路径 /api/blacklist，必须排在上述子路径之后 ----
  // 配置读取：{"ok":true,"enabled":bool,"action":n,"count":n,
  //   "list":[{"pattern":"..","mode":n,"scope":n,"note":"..","enabled":bool}]}
  _server.on("/api/blacklist", HTTP_GET, [this](AsyncWebServerRequest *req) { handleBlackGet(req); });

  // 配置整体保存：JSON {"enabled":bool,"action":0|1,"list":[{pattern,mode,scope,note,enabled}...]}
  // 逐条校验（mode 0..2、scope 非 0、pattern 非空）+ 去重，落盘后返回最新配置
  _server.on("/api/blacklist", HTTP_POST,
             [this](AsyncWebServerRequest *req, JsonVariant &json) { handleBlackPost(req, json); });

  // ================= 邮件通知（SMTP）=================
  // 顺序：/api/email/<子路径> 先注册，/api/email（父路径）放最后

  // 总开关：JSON {"enabled":true|false} —— 只翻转启用位，不动其余配置
  // （停用时丢弃待发队列，已保存的 SMTP 参数与通知类型保留）
  _server.on("/api/email/enabled", HTTP_POST,
             [this](AsyncWebServerRequest *req, JsonVariant &json) { handleEmailEnabled(req, json); });

  // 测试发送：只入队并立即返回（{"ok":true,"queued":true}），真正的构建与
  // SMTP 发送由 App::tick 在 loop 任务完成（与通知邮件同一条队列）。
  // 这里绝不能同步发邮件：本回调运行于 async_tcp 任务，其栈不足以支撑
  // TLS 握手，同步发送会栈溢出导致设备重启，且最长 15s 的阻塞会拖死 Web。
  _server.on("/api/email/test", HTTP_POST, [this](AsyncWebServerRequest *req) { handleEmailTest(req); });

  // ---- 以下是父路径 /api/email，必须排在上述子路径之后 ----
  // 配置读取：{"ok":true,"configured":bool,"server":"..","port":n,"user":"..",
  //   "sendTo":"..","hasPass":bool,"notifyTypes":n,"onlineH":n}
  // SMTP 授权码不回显（hasPass 表示是否已保存），避免泄露
  _server.on("/api/email", HTTP_GET, [this](AsyncWebServerRequest *req) { handleEmailGet(req); });

  // 配置保存：JSON {"server":"..","port":465,"user":"..","pass":"..","sendTo":"..",
  //   "enabled":true,"notifyTypes":n,"schedMode":0|1,"schedHour":n,"schedMinute":n,
  //   "intervalH":n}
  // pass 为空串表示保持已保存的授权码不变（表单不回显）；enabled 为 SMTP
  // 邮件通知总开关（缺省 true，兼容不带该字段的旧客户端）；notifyTypes 为位掩码
  // （bit0=设备启动 bit1=SIM状态 bit2=收到短信 bit3=定时在线）；
  // 定时在线通知排程两种模式：schedMode=1 固定间隔（intervalH 小时）/
  // schedMode=0 每日定点（schedHour:schedMinute，依赖时间同步）。
  _server.on("/api/email", HTTP_POST,
             [this](AsyncWebServerRequest *req, JsonVariant &json) { handleEmailPost(req, json); });

  // ================= MQTT =================
  // 顺序：/api/mqtt/<子路径> 先注册，/api/mqtt（父路径）放最后

  // 总开关：JSON {"enabled":true|false} —— 只翻转启用位，不动其余配置
  // （停用时销毁客户端停止连接，已保存的连接/主题配置保留）
  _server.on("/api/mqtt/enabled", HTTP_POST,
             [this](AsyncWebServerRequest *req, JsonVariant &json) { handleMqttEnabled(req, json); });

  // 测试发布：只发布一条测试事件并立即返回（{"ok":true}），真正的发布由
  // MqttService::tick 在 loop 任务完成（与事件同一条链路）。
  // 这里绝不能同步发布：本回调运行于 async_tcp 任务，其栈不足以支撑 MQTT 握手。
  _server.on("/api/mqtt/test", HTTP_POST, [this](AsyncWebServerRequest *req) { handleMqttTest(req); });

  // ---- 以下是父路径 /api/mqtt，必须排在上述子路径之后 ----
  // 配置读取：{"ok":true,"enabled":bool,"configured":bool,"scheme":"mqtt|mqtts|ws|wss",
  //   "server":"host[:port]","user":"..","hasPass":bool,"clientId":"..","topicPrefix":"..",
  //   "keepAlive":n,"subscribeEnabled":bool,"notifyTypes":n}
  // 密码不回显（hasPass 表示是否已保存），避免泄露
  _server.on("/api/mqtt", HTTP_GET, [this](AsyncWebServerRequest *req) { handleMqttGet(req); });

  // 配置保存：JSON {"scheme":"..","server":"host[:port]","user":"..","pass":"..",
  //   "clientId":"..","topicPrefix":"..","keepAlive":n,"subscribeEnabled":bool,
  //   "notifyTypes":n,"enabled":bool}
  // pass 为空串表示保持已保存的密码不变（表单不回显）；enabled 为 MQTT
  // 总开关（缺省 false）；notifyTypes 为位掩码（bit0=启动 bit1=SIM bit2=短信
  // bit3=上线 bit16=日志）；保存后立即重建客户端生效。
  _server.on("/api/mqtt", HTTP_POST,
             [this](AsyncWebServerRequest *req, JsonVariant &json) { handleMqttPost(req, json); });

  // ================= 推送通道 =================
  // 顺序：/api/push/<子路径> 先注册，/api/push（父路径）放最后

  // 支持的通道类型清单及字段 schema（前端动态渲染表单用）
  _server.on("/api/push/types", HTTP_GET,
             [this](AsyncWebServerRequest *req) { handlePushTypes(req); });

  // 总开关：JSON {"enabled":true|false} —— 只翻转启用位，不动其余配置
  _server.on("/api/push/enabled", HTTP_POST,
             [this](AsyncWebServerRequest *req, JsonVariant &json) { handlePushEnabled(req, json); });

  // 测试发送：只入队并立即返回（{"ok":true,"queued":true}），真正的发送由
  // App::tick 在 loop 任务完成（与通知推送同一条链路）。
  _server.on("/api/push/test", HTTP_POST, [this](AsyncWebServerRequest *req) { handlePushTest(req); });

  // ---- 以下是父路径 /api/push，必须排在上述子路径之后 ----
  // 配置读取：{"ok":true,"enabled":bool,"strategy":int,"configured":bool,
  //   "channels":[{"type":n,"name":"..","url":"..","hasKey1":bool,"key2":"..",
  //   "key3":"..","key4":"..","key5":"..","customBody":"..","retryOnFail":bool,
  //   "enabled":bool,"valid":bool},...]}
  _server.on("/api/push", HTTP_GET, [this](AsyncWebServerRequest *req) { handlePushGet(req); });

  // 配置保存：JSON {"enabled":bool,"strategy":int,"channels":[{...}]}
  // key1 为空串表示保持已保存值（掩码字段）；保存后立即生效
  _server.on("/api/push", HTTP_POST,
             [this](AsyncWebServerRequest *req, JsonVariant &json) { handlePushPost(req, json); });

  // ================= OTA 升级 =================
  // 顺序：子路径（/api/ota/status 等）在父路径（/api/ota/upload 等）前注册无冲突，
  // 这里均为不同路径，按功能分组排列即可。

  // 状态快照：轮询用。返回 { state, progress, message, currentVersion,
  // latestVersion, available }，前端据此渲染进度与「发现新版本」提示
  _server.on("/api/ota/status", HTTP_GET,
             [this](AsyncWebServerRequest *req) { handleOtaStatus(req); });

  // 触发版本检查（异步后台任务），立即返回当前版本；最新版本由后续轮询
  // /api/ota/status 获取（state 由 CHECKING 转为 IDLE 时 latestVersion 已填好）
  _server.on("/api/ota/version", HTTP_GET,
             [this](AsyncWebServerRequest *req) { handleOtaVersion(req); });

  // 启动在线升级：JSON {"tag":"<完整版本标识>"} -> {ok:true}；实际下载在后台任务进行，
  // 进度经 /api/ota/status 轮询。tag 即 Release tag（与 /api/ota/version 返回的
  // latestVersion 一致），白名单校验见 Ota::startOnlineUpgrade
  _server.on("/api/ota/start", HTTP_POST,
             [this](AsyncWebServerRequest *req) { handleOtaStart(req); },
             nullptr,
             [this](AsyncWebServerRequest *req, uint8_t *data, size_t len,
                    size_t index, size_t total) { handleOtaStartBody(req, data, len, index, total); });

  // 本地固件上传：multipart/form-data 的 file 字段原始字节即固件。onUpload 分片写入
  // OTA 分区；onRequest 在收尾时返回结果。升级成功后会自动重启
  _server.on("/api/ota/upload", HTTP_POST,
             [this](AsyncWebServerRequest *req) { handleOtaUploadReq(req); },
             [this](AsyncWebServerRequest *req, String filename, size_t index,
                    uint8_t *data, size_t len, bool final) {
               handleOtaUploadChunk(req, filename, index, data, len, final);
             });

  // 本地 Web UI（LittleFS 镜像）上传：同样走 onUpload 分片写入 data 分区
  _server.on("/api/ota/upload-fs", HTTP_POST,
             [this](AsyncWebServerRequest *req) { handleOtaUploadFsReq(req); },
             [this](AsyncWebServerRequest *req, String filename, size_t index,
                    uint8_t *data, size_t len, bool final) {
               handleOtaUploadFsChunk(req, filename, index, data, len, final);
             });

  // ================= 配置导入导出 =================
  // 导出：GET 触发浏览器下载完整配置 JSON（含密码等敏感信息）
  _server.on("/api/config/export", HTTP_GET,
             [this](AsyncWebServerRequest *req) { handleConfigExport(req); });

  // 导入：POST 原始 JSON 请求体（application/json），经 onBody 分片累积后整体解析。
  // 第 4 参 onUpload 留空（非 multipart）；第 5 参 onBody 接收原始 body 分片。
  _server.on("/api/config/import", HTTP_POST,
             [this](AsyncWebServerRequest *req) { handleConfigImportReq(req); },
             nullptr,
             [this](AsyncWebServerRequest *req, uint8_t *data, size_t len,
                    size_t index, size_t total) {
               handleConfigImportChunk(req, data, len, index, total);
             });

  // ================= 崩溃转储（coredump） =================
  // 顺序：/api/tools/coredump/<子路径> 之间路径互不为前缀，无父子冲突，
  // 按功能分组排列即可。

  // 信息查询：{"hasCoredump":bool,"size":n,"crashTime":epoch,"crashVersion":"..",
  //   "elfUrl":".."}。无分区或无数据时 hasCoredump=false，其余字段省略
  _server.on("/api/tools/coredump/info", HTTP_GET,
             [this](AsyncWebServerRequest *req) { handleCoredumpInfo(req); });

  // 导出：GET 触发浏览器下载 coredump.bin（panic 时 ESP-IDF 写入分区的 ELF
  // 快照，可能含内存内容，分享前请自行评估）。文件名带崩溃时间便于归档
  _server.on("/api/tools/coredump/export", HTTP_GET,
             [this](AsyncWebServerRequest *req) { handleCoredumpExport(req); });

  // 清除：POST 整分区擦除（分区 0xFE0000 起、128KB，按 4KB 扇区对齐）。
  // 只清分区快照；NVS 里的崩溃时间 / 版本记录保留（供追溯），
  // 下次 panic 会被覆盖
  _server.on("/api/tools/coredump/clear", HTTP_POST,
             [this](AsyncWebServerRequest *req) { handleCoredumpClear(req); });

  // 未匹配路径统一返回 404（JSON，与全站一致）
  _server.onNotFound([this](AsyncWebServerRequest *req) { handleNotFound(req); });
}

// ============================================================================
// 路由处理函数：setupRoutes() 只负责注册与转发，具体实现一律落在这里，
// 便于单条接口的逻辑独立阅读 / 复用 / 单测，也避免注册处堆积长 lambda。
// 以下函数均运行于 HTTP（async_tcp）任务上下文：不要在其中执行长耗时或
// 大栈操作（如 SMTP/TLS），需要重活时入队交给主循环处理。
// ============================================================================

// ---- 中间件 ----
// req：请求对象；next：放行到下一环
void WebPanel::handleAuthMiddleware(AsyncWebServerRequest *req, ArMiddlewareNext next) {
  String url = req->url();
  if (url == "/api/login" || url == "/api/logout" || url == "/api/session") {
    next();  // 登录相关端点无需已认证
    return;
  }
  if (url.startsWith("/api/") && !authorized(req)) {
    sendErr(req, 401, "未授权");
    return;
  }
  next();
}

// ---- 页面 ----
void WebPanel::handleIndex(AsyncWebServerRequest *req) {
  sendFile(req, "/index.html", "text/html; charset=utf-8");
}

void WebPanel::handleLoginPage(AsyncWebServerRequest *req) {
  sendFile(req, "/login.html", "text/html; charset=utf-8");
}

// ---- 认证 ----
void WebPanel::handleLogin(AsyncWebServerRequest *req, JsonVariant &json) {
  if (!json.is<JsonObject>() || !json["user"].is<const char *>() ||
      !json["pass"].is<const char *>()) {
    sendErr(req, 400, "请求格式错误");
    return;
  }
  String user = json["user"].as<String>();
  String pass = json["pass"].as<String>();
  if (!_auth.check(user, pass)) {
    sendErr(req, 401, "用户名或密码错误");
    return;
  }
  String sid = genSecret();
  String token = genSecret();
  if (!storeAdd(_sessions, _sessionCount, sid) ||
      !storeAdd(_tokens, _tokenCount, token)) {
    sendErr(req, 500, "登录会话过多，请稍后再试");
    return;
  }
  String cookie = String(cfg::kCookieName) + "=" + sid +
                  "; HttpOnly; SameSite=Strict; Path=/; Max-Age=" +
                  String(cfg::kCookieMaxAge);
  JsonDocument doc;
  doc["ok"] = true;
  doc["user"] = user;
  doc["token"] = token;
  sendJsonDocWithCookie(req, doc, cookie);
}

void WebPanel::handleLogout(AsyncWebServerRequest *req) {
  String sid = cookieValue(req, cfg::kCookieName);
  if (sid.length()) storeRemove(_sessions, _sessionCount, sid);
  if (req->hasHeader("Authorization")) {
    String a = req->getHeader("Authorization")->value();
    if (a.startsWith("Bearer ")) storeRemove(_tokens, _tokenCount, a.substring(7));
  }
  String cookie = String(cfg::kCookieName) +
                  "=; HttpOnly; SameSite=Strict; Path=/; Max-Age=0";
  JsonDocument doc;
  doc["ok"] = true;
  sendJsonDocWithCookie(req, doc, cookie);
}

void WebPanel::handleSession(AsyncWebServerRequest *req) {
  if (!authorized(req)) {
    sendErr(req, 401, "未授权");
    return;
  }
  JsonDocument doc;
  doc["ok"] = true;
  doc["user"] = _auth.user();
  sendJsonDoc(req, doc);
}

// ---- API 令牌 / 账号 ----
void WebPanel::handleTokenGet(AsyncWebServerRequest *req) {
  if (!authorized(req)) { sendErr(req, 401, "未授权"); return; }
  JsonDocument doc;
  doc["ok"] = true;
  doc["enabled"] = _apiTokenEnabled;
  doc["token"] = _apiTokenEnabled ? _apiToken : String();
  sendJsonDoc(req, doc);
}

void WebPanel::handleTokenPost(AsyncWebServerRequest *req, JsonVariant &json) {
  if (!authorized(req)) { sendErr(req, 401, "未授权"); return; }
  if (!json.is<JsonObject>() || !json["enabled"].is<bool>()) {
    sendErr(req, 400, "请求格式错误"); return;
  }
  _apiTokenEnabled = json["enabled"].as<bool>();
  if (_apiTokenEnabled && _apiToken.length() != 32) _apiToken = genSecret();
  saveApiTokenCfg();
  sendOkFlag(req, "enabled", _apiTokenEnabled);
}

void WebPanel::handleTokenReset(AsyncWebServerRequest *req) {
  if (!authorized(req)) { sendErr(req, 401, "未授权"); return; }
  if (!_apiTokenEnabled) { sendErr(req, 403, "API 访问令牌未启用"); return; }
  String t = resetApiToken();
  JsonDocument doc;
  doc["ok"] = true;
  doc["token"] = t;
  sendJsonDoc(req, doc);
}

void WebPanel::handleAuthGet(AsyncWebServerRequest *req) {
  JsonDocument doc;
  doc["ok"] = true;
  doc["user"] = _auth.user();
  sendJsonDoc(req, doc);
}

void WebPanel::handleAuthPost(AsyncWebServerRequest *req, JsonVariant &json) {
  if (!json.is<JsonObject>() || !json["user"].is<const char *>() ||
      !json["pass"].is<const char *>() ||
      !json["passOld"].is<const char *>()) {
    sendErr(req, 400, "请求格式错误");
    return;
  }
  String user = json["user"].as<String>();
  String pass = json["pass"].as<String>();
  String passOld = json["passOld"].as<String>();
  _log.addf(LogLevel::kDebug, "auth", "修改账号请求：新用户名=%s，当前用户名=%s",
            user.c_str(), _auth.user().c_str());
  if (!_auth.check(_auth.user(), passOld)) {
    _log.addf(LogLevel::kWarn, "auth", "修改账号被拒：当前密码不正确");
    sendErr(req, 403, "当前密码不正确");
    return;
  }
  String err;
  if (!_auth.update(user, pass, &err)) {
    _log.addf(LogLevel::kWarn, "auth", "修改账号失败：%s", err.c_str());
    sendErr(req, 400, err.c_str());
    return;
  }
  invalidateAllSessions();  // 账号变更，全部登录态作废
  sendOk(req);
}

// ---- RGB ----
void WebPanel::handleLedNext(AsyncWebServerRequest *req) {
  _led.next();
  sendJson(req, stateRgbJson(_led));
}

void WebPanel::handleRgbGet(AsyncWebServerRequest *req) {
  sendJson(req, stateRgbJson(_led));
}

void WebPanel::handleRgbPost(AsyncWebServerRequest *req, JsonVariant &json) {
  if (!json.is<JsonObject>()) {
    sendErr(req, 400, "请求格式错误");
    return;
  }
  int c;
  if (jsonInt(json["c"], c)) {
    if (c < 0 || c >= RgbLed::count()) {
      sendErr(req, 400, "颜色预设序号无效");
      return;
    }
    _led.set((uint8_t)c);
  } else {
    int r, g, b;
    if (jsonInt(json["r"], r) && jsonInt(json["g"], g) &&
        jsonInt(json["b"], b)) {
      auto clamp255 = [](int v) -> int {
        return v < 0 ? 0 : (v > 255 ? 255 : v);
      };
      _led.setRaw((uint8_t)clamp255(r), (uint8_t)clamp255(g),
                  (uint8_t)clamp255(b));
    }
  }
  int br;
  if (jsonInt(json["bright"], br)) {
    if (br < 0) br = 0;
    if (br > 100) br = 100;
    _led.setBrightness((uint8_t)br);
  }
  sendJson(req, stateRgbJson(_led));
}

// ---- 模组调试（AT 命令） ----
void WebPanel::handleAtSend(AsyncWebServerRequest *req) {
  uint8_t port = paramInt(req, "port", 1);                 // 模组号，默认 1
  uint32_t baud = paramInt(req, "baud", cfg::kUartBaud);   // 波特率，默认配置值
  uint8_t eol = paramInt(req, "eol", 0);                   // 行尾符，默认 CRLF
  uint32_t timeout = constrain(paramInt(req, "t", 1000), 100, 5000);  // 超时钳制 100..5000ms
  String cmd = stringParam(req, "cmd");                    // 命令文本

  String err;
  auto result = _sms.sendAt(port, baud, cmd, (SerialBridge::Eol)eol, timeout, err);
  if (!result.ok) {
    sendErr(req, 400, err.length() ? err.c_str() : "通信模组序号无效");
    return;
  }
  sendJson(req, _bridge.entryJson(result.id));  // 直接回该条记录 JSON
}

void WebPanel::handleAtHist(AsyncWebServerRequest *req) {
  int32_t after = paramInt(req, "after", -1);   // -1 = 从头拉
  size_t n = (size_t)paramInt(req, "n", 20);    // 默认 20 条
  sendJson(req, _bridge.historyJson(after, n));
}

void WebPanel::handleAtClear(AsyncWebServerRequest *req) {
  _bridge.clearHistory();
  sendOk(req);
}

// AT 历史存储统计（PSRAM/容量等，与 /api/logs/stats 对称）
void WebPanel::handleAtStats(AsyncWebServerRequest *req) {
  sendJson(req, _bridge.historyStats());
}

void WebPanel::handleAtReset(AsyncWebServerRequest *req) {
  uint8_t port = paramInt(req, "port", 0);
  if (port < 1 || port > 2) {
    sendErr(req, 400, "通信模组序号无效");
    return;
  }
  _bridge.reset(port);
  sendOk(req);
}

void WebPanel::handleUartStats(AsyncWebServerRequest *req) {
  sendJson(req, _bridge.statsJson());
}

// ---- 设备日志 ----
void WebPanel::handleLogStats(AsyncWebServerRequest *req) {
  sendJson(req, _log.statsJson());
}

void WebPanel::handleLogList(AsyncWebServerRequest *req) {
  // level 缺省不过滤；前端「仅看警告以上」传 level=2
  int minLevel = req->hasParam("level") ? paramInt(req, "level", -1) : -1;
  String q = stringParam(req, "q");

  // ---- 增量模式：?after=<id>&limit=<n> ----
  // 只取该 ID 之后的新记录（升序），供前端轮询追加，避免整页重绘闪烁
  if (req->hasParam("after")) {
    int afterI = paramInt(req, "after", 0);
    uint32_t after = afterI > 0 ? (uint32_t)afterI : 0;
    size_t limit = (size_t)paramInt(req, "limit", cfg::kLogPageSizeMax);

    std::vector<LogStore::Entry> inc;
    int total = 0;
    _log.tail(after, limit, (int8_t)minLevel, q, inc, total);

    JsonDocument doc;
    doc["ok"] = true;
    doc["last"] = _log.lastId();
    doc["total"] = total;
    doc["count"] = (unsigned)inc.size();
    JsonArray a2 = doc["logs"].to<JsonArray>();
    for (const LogStore::Entry &e : inc) {
      JsonObject o = a2.add<JsonObject>();
      o["id"] = e.id;
      o["ts"] = e.ts;
      o["level"] = e.level;
      o["tag"] = e.tag;
      o["msg"] = e.msg;
    }
    sendJsonDoc(req, doc);
    return;
  }

  // ---- 分页模式：?page=&pageSize= ----
  int page = paramInt(req, "page", 1);
  int pageSize = paramInt(req, "pageSize", 20);
  if (page < 1) page = 1;
  if (pageSize < 1) pageSize = 20;
  if (pageSize > cfg::kLogPageSizeMax) pageSize = cfg::kLogPageSizeMax;

  std::vector<LogStore::Entry> logs;
  int total = 0;
  _log.list(page, pageSize, (int8_t)minLevel, q, logs, total);

  JsonDocument doc;
  doc["ok"] = true;
  doc["total"] = total;
  doc["page"] = page;
  doc["pageSize"] = pageSize;
  JsonArray arr = doc["logs"].to<JsonArray>();
  for (const LogStore::Entry &e : logs) {
    JsonObject o = arr.add<JsonObject>();
    o["id"] = e.id;
    o["ts"] = e.ts;
    o["level"] = e.level;
    o["tag"] = e.tag;
    o["msg"] = e.msg;
  }
  sendJsonDoc(req, doc);
}

void WebPanel::handleLogClear(AsyncWebServerRequest *req) {
  _log.clear();
  _log.add(LogLevel::kInfo, "log", "日志已清空");
  sendOk(req);
}

// ---- 屏幕 ----
void WebPanel::handleDispGet(AsyncWebServerRequest *req) {
  sendJson(req, stateDispJson(_display, _ui));
}

// 屏幕实时预览（方案 B）：抓取当前帧缓冲编码为 JPEG 直接返回
// PSRAM 缓冲区由 shared_ptr（free 删除器）持有，流式发送完成后随响应对象析构释放
void WebPanel::handleScreenGet(AsyncWebServerRequest *req) {
  _display.requestScreenPreview();   // 标记“有人正在看预览”，驱动主循环按需生成 JPEG 缓存
  size_t len = 0;
  uint8_t *buf = _display.takeScreenJpg(len);
  if (!buf || len == 0) {
    req->send(503, "text/plain", "帧缓冲不可用");
    return;
  }
  // shared_ptr + free 删除器：与 ps_malloc 配对；可拷贝以存入 std::function，
  // 随响应析构自动释放，避免内存泄漏（std::function 要求目标可拷贝，故不用 unique_ptr）
  std::shared_ptr<uint8_t> holder(buf, [](uint8_t *p) { free(p); });
  // 流式填充：按 index 从缓冲区拷贝至多 maxLen 字节；index>=len 返回 0 表示结束
  AsyncWebServerResponse *res =
      req->beginResponse("image/jpeg", len,
          [holder, len](uint8_t *out, size_t maxLen, size_t index) -> size_t {
            if (index >= len) return 0;
            size_t n = len - index;
            if (n > maxLen) n = maxLen;
            memcpy(out, holder.get() + index, n);
            return n;
          });
  res->addHeader("Cache-Control", "no-store");  // 实时画面，禁用缓存
  req->send(res);
}

void WebPanel::handleDispPost(AsyncWebServerRequest *req, JsonVariant &json) {
  if (!json.is<JsonObject>()) {
    sendErr(req, 400, "请求格式错误");
    return;
  }
  bool on;
  if (jsonBool(json["on"], on)) {
    _display.setScreenOn(on);
  }
  // 背光亮度：经 UIMenu 落 NVS（重启后沿用），越界钳制
  int b;
  if (jsonInt(json["bright"], b)) {
    if (b < 0) b = 0;
    if (b > 100) b = 100;
    _ui.setBrightness((uint8_t)b);
  }
  // 自动行为（秒）：无操作自动熄屏 / 自动退回实时状态页，0 = 关闭该功能。
  // 越界由 UIMenu 钳制；改完立即生效并落盘，无需重启
  int v;
  if (jsonInt(json["autoOffSec"], v)) _ui.setAutoOffSec((uint16_t)v);
  if (jsonInt(json["idleSec"], v)) _ui.setIdleSec((uint16_t)v);
  sendJson(req, stateDispJson(_display, _ui));
}

// 远程按键注入：把 Web 端的 UP/DOWN/OK/BACK 当作物理按键驱动设备 UI
void WebPanel::handleKeyPost(AsyncWebServerRequest *req, JsonVariant &json) {
  if (!json.is<JsonObject>() || !json["key"].is<const char *>()) {
    sendErr(req, 400, "缺少按键参数");
    return;
  }
  String k = json["key"].as<String>();
  KeyEvent ev = KeyEvent::kClick;   // 默认短按；可显式指定 long/repeat
  if (json["event"].is<const char *>()) {
    String e = json["event"].as<String>();
    if (e == "long") ev = KeyEvent::kLongPress;
    else if (e == "repeat") ev = KeyEvent::kRepeat;
  }
  KeyId key = KeyId::kCount;        // 非法初值
  if (k == "ok") key = KeyId::kOk;
  else if (k == "up") key = KeyId::kUp;
  else if (k == "down") key = KeyId::kDown;
  else if (k == "back") key = KeyId::kBack;
  if (key == KeyId::kCount) { sendErr(req, 400, "未知的按键"); return; }
  KeyInject::instance().push(key, ev);
  sendOk(req);
}

void WebPanel::handleAtBaud(AsyncWebServerRequest *req) {
  JsonDocument doc;
  doc["default"] = cfg::kUartBaud;
  sendJsonDoc(req, doc);
}

// ---- 系统状态 ----
void WebPanel::handleStatus(AsyncWebServerRequest *req) {
  sendJson(req, stateStatusJson(_wifi, _led, _display, _time, _sms.store()));
}

// ---- 系统维护：立即重启 / 出厂复位 ----
// 延迟 2s 重启（与 OTA / 配置导入一致，给响应留出发送时间）
void WebPanel::handleSystemRestart(AsyncWebServerRequest *req) {
  sendOkMsg(req, "设备即将重启");
  xTaskCreate(
      [](void *) {
        vTaskDelay(pdMS_TO_TICKS(2000));
        esp_restart();
        vTaskDelete(nullptr);
      },
      "man_rst", 2048, nullptr, 5, nullptr);
}

// 出厂复位：清空 NVS 全部配置（WiFi/账号/短信/邮件/MQTT/推送/黑名单/屏幕/令牌）
// 并格式化用户数据分区（短信记录 / 日志 / AT 历史），随后重启回到首次启动状态。
void WebPanel::handleSystemReset(AsyncWebServerRequest *req) {
  Preferences prefs;
  if (prefs.begin(cfg::kNvsNamespace, false)) {
    prefs.clear();   // 移除 appcfg 命名空间下所有键
    prefs.end();
  }
  userfs::format();  // 清空 /data 用户数据
  sendOkMsg(req, "已恢复出厂设置，设备即将重启");
  xTaskCreate(
      [](void *) {
        vTaskDelay(pdMS_TO_TICKS(2000));
        esp_restart();
        vTaskDelete(nullptr);
      },
      "fac_rst", 2048, nullptr, 5, nullptr);
}

// 定时重启：仅保存配置（即时生效），不重启设备。
// 请求体可整体为 sched 对象，或 { "sched": {...} }（与通用导入一致）。
void WebPanel::handleSchedPost(AsyncWebServerRequest *req, JsonVariant &json) {
  if (!authorized(req)) { sendErr(req, 401, "未授权"); return; }
  if (!json.is<JsonObject>()) { sendErr(req, 400, "请求格式错误"); return; }
  JsonObject o = json["sched"].is<JsonObject>() ? json["sched"].as<JsonObject>()
                                                : json.as<JsonObject>();
  if (!SystemService::instance().importFrom(o)) {
    sendErr(req, 400, "定时重启配置无效");
    return;
  }
  // importFrom 内已落盘；不重启，下次 tick 起按新计划执行
  sendOkMsg(req, "已保存，定时重启即时生效");
}

// 任务看门狗故障注入自检：让指定受监控任务停止喂狗，其余链路照常运行。
// 约 timeoutMs 后由 TWDT panic 自动重启，下次开机在日志 / NVS / /api/status
// 里留下「任务看门狗超时 + 疑似卡死任务」。仅用于设备验收，会中断服务。
void WebPanel::handleWdtSelfTest(AsyncWebServerRequest *req, JsonVariant &json) {
  if (!json.is<JsonObject>()) { sendErr(req, 400, "请求格式错误"); return; }
  const char *task = json["task"].as<const char *>();
  const uint32_t timeoutMs = json["timeoutMs"].as<uint32_t>();
  if (!task || !task[0]) { sendErr(req, 400, "缺少 task"); return; }
  if (!TaskWatchdog::startSelfTest(task, timeoutMs)) {
    sendErr(req, 400, "任务名无效、超时越界或看门狗未启用");
    return;
  }
  JsonDocument doc;
  doc["ok"] = true;
  doc["message"] = "已停止该任务喂狗，设备将自动重启并记录原因";
  doc["restartInSec"] = TaskWatchdog::timeoutMs() / 1000;
  sendJsonDoc(req, doc);
}

// ---- WiFi 配网 ----
void WebPanel::handleWifiStatus(AsyncWebServerRequest *req) {
  sendJson(req, _wifi.statusJson());
}

void WebPanel::handleWifiScan(AsyncWebServerRequest *req) {
  int16_t state = WiFi.scanComplete();
  if (state == WIFI_SCAN_RUNNING) {
    JsonDocument doc;
  doc["busy"] = 1;
  sendJsonDoc(req, doc);  // 扫描进行中，前端继续轮询
    return;
  }
  if (state == WIFI_SCAN_FAILED) {
    _wifi.startScan();              // 没有进行中的扫描 -> 启动一轮新的
    JsonDocument doc;
  doc["busy"] = 1;
  sendJsonDoc(req, doc);
    return;
  }
  // 结果已就绪，导出并释放底层缓存
  String nets = _wifi.scanJson();
  JsonDocument doc;
  doc["busy"] = 0;
  doc["nets"] = serialized(nets);
  sendJsonDoc(req, doc);
}

void WebPanel::handleWifiList(AsyncWebServerRequest *req) {
  sendJson(req, _wifi.listJson());
}

void WebPanel::handleWifiSave(AsyncWebServerRequest *req, JsonVariant &json) {
  if (!json.is<JsonObject>() || !json["ssid"].is<const char *>()) {
    sendErr(req, 400, "请填写 WiFi 名称");
    return;
  }
  String ssid = json["ssid"].as<String>();
  String pass = json["pass"].is<const char *>()
                    ? json["pass"].as<String>() : String();
  bool connect = true;
  jsonBool(json["connect"], connect);  // 缺省 true
  if (!ssid.length()) {
    sendErr(req, 400, "请填写 WiFi 名称");
    return;
  }
  if (!_wifi.save(ssid, pass, connect)) {
    sendErr(req, 500, "保存失败（最多 5 条配置）");
    return;
  }
  sendOk(req);
}

void WebPanel::handleWifiDelete(AsyncWebServerRequest *req, JsonVariant &json) {
  int index;
  if (!json.is<JsonObject>() || !jsonInt(json["index"], index)) {
    sendErr(req, 400, "序号无效");
    return;
  }
  if (!_wifi.remove((uint8_t)index)) {
    sendErr(req, 400, "序号无效");
    return;
  }
  sendOk(req);
}

void WebPanel::handleWifiMove(AsyncWebServerRequest *req, JsonVariant &json) {
  int from, to;
  if (!json.is<JsonObject>() || !jsonInt(json["from"], from) ||
      !jsonInt(json["to"], to)) {
    sendErr(req, 400, "排序参数无效");
    return;
  }
  if (from < 0 || to < 0 || !_wifi.move((uint8_t)from, (uint8_t)to)) {
    sendErr(req, 400, "排序参数无效");
    return;
  }
  sendOk(req);
}

void WebPanel::handleWifiConnect(AsyncWebServerRequest *req, JsonVariant &json) {
  if (!json.is<JsonObject>() || !json["ssid"].is<const char *>()) {
    sendErr(req, 400, "请填写 WiFi 名称");
    return;
  }
  String ssid = json["ssid"].as<String>();
  String pass = json["pass"].is<const char *>()
                    ? json["pass"].as<String>() : String();
  if (!ssid.length()) {
    sendErr(req, 400, "请填写 WiFi 名称");
    return;
  }
  _wifi.requestConnect(ssid, pass);
  sendOkFlag(req, "connecting", true);
}

// AP 配网热点密码：返回当前是否加密与实际密码（已认证管理员可见）
void WebPanel::handleWifiApGet(AsyncWebServerRequest *req) {
  JsonDocument doc;
  doc["secure"] = _wifi.apSecure();
  doc["pass"] = _wifi.apPass();   // 直接赋值，ArduinoJson 自动转义密码特殊字符
  sendJsonDoc(req, doc);
}

// AP 配网热点密码：修改并落盘；{"pass":""} 设为开放热点
void WebPanel::handleWifiApPost(AsyncWebServerRequest *req, JsonVariant &json) {
  if (!json.is<JsonObject>() || !json["pass"].is<const char *>()) {
    sendErr(req, 400, "请填写密码");
    return;
  }
  String pass = json["pass"].as<String>();
  String err;
  if (!_wifi.setApPass(pass, &err)) {
    sendErr(req, 400, err.c_str());
    return;
  }
  sendOkFlag(req, "secure", _wifi.apSecure());
}

// ---- 短信 ----
void WebPanel::handleSmsStats(AsyncWebServerRequest *req) {
  sendJson(req, _sms.store().statsJson());
}

void WebPanel::handleSmsLatest(AsyncWebServerRequest *req) {
  sendJson(req, _sms.latestJson());
}

void WebPanel::handleSmsPortsGet(AsyncWebServerRequest *req) {
  String modems = _sms.modemsJson();   // 先落本地变量，保证 serialized() 指针在序列化时仍有效
  JsonDocument doc;
  doc["ok"] = true;
  doc["modems"] = serialized(modems);
  sendJsonDoc(req, doc);
}

void WebPanel::handleSmsList(AsyncWebServerRequest *req) {
  int page = paramInt(req, "page", 1);
  int pageSize = paramInt(req, "pageSize", 20);
  String q = stringParam(req, "q");
  int portFilter = req->hasParam("port") ? paramInt(req, "port", -1) : -1;

  SmsList msgs;  // PSRAM 容器：整页最多 100 条
  int total = 0;
  _sms.store().list(page, pageSize, q, (int8_t)portFilter, msgs, total);

  // JSON 内存池也放 PSRAM：整页序列化后可达数十 KB
  JsonDocument doc(psramJsonAllocator());
  doc["ok"] = true;
  doc["total"] = total;
  doc["page"] = page;
  doc["pageSize"] = pageSize;
  JsonArray arr = doc["messages"].to<JsonArray>();
  for (const SmsMessage &m : msgs) {
    JsonObject o = arr.add<JsonObject>();
    o["id"] = m.id;
    o["dir"] = m.incoming ? "in" : "out";
    o["phone"] = m.phone;
    o["content"] = m.content;
    o["ts"] = m.ts;
    o["modem"] = m.modem;
  }
  sendJsonDoc(req, doc);
}

void WebPanel::handleSmsDelete(AsyncWebServerRequest *req) {
  if (req->hasParam("all")) {
    _sms.store().clear();
    sendOk(req);
    return;
  }
  if (!req->hasParam("id")) {
    sendErr(req, 400, "缺少短信 ID");
    return;
  }
  uint32_t id = (uint32_t)paramInt(req, "id", 0);
  if (!_sms.store().deleteOne(id)) {
    sendErr(req, 404, "记录不存在");
    return;
  }
  sendOk(req);
}

void WebPanel::handleSmsPortsPost(AsyncWebServerRequest *req, JsonVariant &json) {
  int port;
  if (!json.is<JsonObject>() || !jsonInt(json["port"], port) ||
      port < 1 || port > cfg::kSmsMaxModems) {
    sendErr(req, 400, "通信模组序号无效");
    return;
  }
  bool changed = false;
  if (json["enabled"].is<bool>()) {
    if (!_sms.setModemEnabled((uint8_t)port, json["enabled"].as<bool>())) {
      sendErr(req, 500, "配置保存失败");
      return;
    }
    changed = true;
  }
  if (json["alias"].is<const char *>()) {
    if (!_sms.setModemAlias((uint8_t)port, json["alias"].as<String>())) {
      sendErr(req, 500, "配置保存失败");
      return;
    }
    changed = true;
  }
  if (!changed) {
    sendErr(req, 400, "缺少启用状态或别名");
    return;
  }
  String modems = _sms.modemsJson();   // 先落本地变量，保证 serialized() 指针在序列化时仍有效
  JsonDocument doc;
  doc["ok"] = true;
  doc["modems"] = serialized(modems);
  sendJsonDoc(req, doc);
}

void WebPanel::handleSmsReinit(AsyncWebServerRequest *req, JsonVariant &json) {
  int port;
  if (!json.is<JsonObject>() || !jsonInt(json["port"], port) ||
      port < 1 || port > cfg::kSmsMaxModems) {
    sendErr(req, 400, "通信模组序号无效");
    return;
  }
  if (!_sms.reinitPort((uint8_t)port)) {
    sendErr(req, 400, "通信模组未启用或不存在");
    return;
  }
  String modems = _sms.modemsJson();   // 先落本地变量，保证 serialized() 指针在序列化时仍有效
  JsonDocument doc;
  doc["ok"] = true;
  doc["modems"] = serialized(modems);
  sendJsonDoc(req, doc);
}

// 经通信模组发送短信（异步）：POST 启动。
// 入队统一发送队列（SmsService::enqueueSend），由专职 worker 任务串行执行阻塞式
// CMGS 握手，不阻塞 HTTP（async_tcp）任务——否则会饿死任务看门狗触发整机复位
// （见崩溃分析）。完成后经 done 回调（tick 主循环上下文）写回 _sendBusy/_sendOk/...，
// 前端轮询 GET /api/sms/send 取回。单飞：UI 一次只允许一个在途发送，避免轮询状态被覆盖。
void WebPanel::handleSmsSend(AsyncWebServerRequest *req, JsonVariant &json) {
  int port;
  if (!json.is<JsonObject>() || !jsonInt(json["port"], port) ||
      !json["phone"].is<const char *>() ||
      !json["content"].is<const char *>()) {
    sendErr(req, 400, "请求格式错误");
    return;
  }
  String phone = json["phone"].as<String>();
  String content = json["content"].as<String>();
  if (port < 1 || port > cfg::kSmsMaxModems) {
    sendErr(req, 400, "通信模组不存在");
    return;
  }

  // 单飞：UI 一次只允许一个在途发送（底层队列可缓冲多个），避免轮询状态被覆盖
  bool busy = false;
  portENTER_CRITICAL(&_sendMux);
  busy = _sendBusy;
  portEXIT_CRITICAL(&_sendMux);
  if (busy) {
    sendErr(req, 409, "短信发送进行中，请稍后再试");
    return;
  }

  // 先置“在途”态，保证单飞；完成后由 done 回调清位
  portENTER_CRITICAL(&_sendMux);
  _sendBusy = true;
  _sendOk = false;
  _sendId = 0;
  _sendStored = false;
  _sendWarn = "";
  _sendErr = "";
  portEXIT_CRITICAL(&_sendMux);

  // 统一经持久化发件箱登记（先落盘再入队），实际阻塞式 CMGS 仍由 SmsService 的
  // 常驻 sms_send worker 执行；done 经 worker -> _sms.tick() -> Outbox 在 loop
  // 任务回传，再写回这里的轮询状态。发件箱不可用时回退原有直投队列，功能不中断。
  auto done = [this, port, phone](uint32_t id, bool stored, const String &err) {
    if (id != 0) {
      _log.addf(LogLevel::kInfo, "sms", "已发送至 %s（通信模组 %u，记录 #%u）",
                phone.c_str(), (unsigned)port, (unsigned)id);
    } else {
      _log.addf(LogLevel::kWarn, "sms",
          "发送失败（通信模组 %u -> %s）：%s", (unsigned)port,
          phone.c_str(), err.c_str());
    }
    portENTER_CRITICAL(&_sendMux);
    _sendOk = (id != 0);
    _sendId = id;
    _sendStored = stored;
    if (id && !stored) {
      _sendWarn = "记录存储失败（userdata 分区异常），本条不会出现在列表中";
    }
    _sendErr = err;
    _sendBusy = false;
    portEXIT_CRITICAL(&_sendMux);
  };
  bool queued = _outbox.mounted()
      ? _outbox.enqueueOutgoingSms((uint8_t)port, phone, content, done)
      : _sms.enqueueSend((uint8_t)port, phone, content, done);
  if (!queued) {
    // 入队缓冲满 / 发件箱不可用：回退在途态，告知客户端稍后重试
    portENTER_CRITICAL(&_sendMux);
    _sendBusy = false;
    portEXIT_CRITICAL(&_sendMux);
    sendErr(req, 503, "发送队列已满或发件箱不可用，请稍后再试");
    return;
  }

  JsonDocument doc;
  doc["ok"] = true;
  doc["started"] = true;
  sendJsonDoc(req, doc);
}

// 短信发送结果轮询：返回当前 busy / 最近一次 ok / id / stored / warn / err。
void WebPanel::handleSmsSendGet(AsyncWebServerRequest *req) {
  bool busy = false, ok = false, stored = false;
  uint32_t id = 0;
  String warn, err;
  portENTER_CRITICAL(&_sendMux);
  busy = _sendBusy;
  ok = _sendOk;
  id = _sendId;
  stored = _sendStored;
  warn = _sendWarn;   // 短字符串拷贝，临界区内完成
  err = _sendErr;
  portEXIT_CRITICAL(&_sendMux);
  JsonDocument doc;
  doc["busy"] = busy;
  doc["ok"] = ok;
  doc["id"] = id;
  doc["stored"] = stored;
  doc["warn"] = warn;
  doc["err"] = err;
  sendJsonDoc(req, doc);
}

// 管理员短信远程控制：读取当前总开关与管理员号码列表
void WebPanel::handleSmsAdminGet(AsyncWebServerRequest *req) {
  JsonDocument doc;
  doc["ok"] = true;
  doc["enabled"] = _sms.adminEnabled();
  JsonArray arr = doc["phones"].to<JsonArray>();
  // adminPhonesJson() 返回 ["..."] 已是合法 JSON；直接反序列化进响应最简洁
  JsonDocument src;
  deserializeJson(src, _sms.adminPhonesJson());
  for (auto v : src.as<JsonArray>()) arr.add(v);
  sendJsonDoc(req, doc);
}

// 设置管理员：JSON {"enabled":bool,"phones":["138...","139..."]}（字段均可选）。
// 任一条号码格式不合法返回 400（号码规则与发送短信一致）；上限 kAdminPhoneMax 条。
// 号码校验优先于开关，避免号码非法却改了开关状态。
void WebPanel::handleSmsAdminPost(AsyncWebServerRequest *req, JsonVariant &json) {
  if (!json.is<JsonObject>()) {
    sendErr(req, 400, "请求格式错误");
    return;
  }
  bool hasEnabled = json["enabled"].is<bool>();
  bool hasPhones = json["phones"].is<JsonArray>();
  if (!hasEnabled && !hasPhones) {
    sendErr(req, 400, "请求格式错误");
    return;
  }
  if (hasPhones) {
    std::vector<String> phones;
    for (JsonVariantConst v : json["phones"].as<JsonArray>()) {
      if (!v.is<const char *>()) {
        sendErr(req, 400, "手机号格式不正确");
        return;
      }
      phones.push_back(v.as<String>());
    }
    if (!_sms.setAdminPhones(phones)) {
      sendErr(req, 400, "手机号格式不正确");
      return;
    }
  }
  if (hasEnabled && !_sms.setAdminEnabled(json["enabled"].as<bool>())) {
    sendErr(req, 500, "保存失败");
    return;
  }
  JsonDocument doc;
  doc["ok"] = true;
  doc["enabled"] = _sms.adminEnabled();
  JsonArray arr = doc["phones"].to<JsonArray>();
  JsonDocument src;
  deserializeJson(src, _sms.adminPhonesJson());
  for (auto v : src.as<JsonArray>()) arr.add(v);
  sendJsonDoc(req, doc);
}

// 经通信模组 ICMP 连通性测试（PING）：POST 启动。
// 复用 SmsService::pingHost（AT+MPING），独立 FreeRTOS 任务异步执行，
// 不阻塞 HTTP（async_tcp）任务；结果经 _pingBusy/_pingOk/_pingResult 暴露，
// 前端轮询 GET /api/sms/ping 取回。host 缺省 8.8.8.8。
void WebPanel::handleSmsPingPost(AsyncWebServerRequest *req, JsonVariant &json) {
  int port;
  if (!json.is<JsonObject>() || !jsonInt(json["port"], port) ||
      port < 1 || port > cfg::kSmsMaxModems) {
    sendErr(req, 400, "通信模组序号无效");
    return;
  }
  String host = json["host"].is<const char *>()
                    ? json["host"].as<String>() : String();

  // 防止并发 PING：读忙标志需与 ping 任务写同临界区，避免读到半更新态
  bool busy = false;
  portENTER_CRITICAL(&_pingMux);
  busy = _pingBusy;
  portEXIT_CRITICAL(&_pingMux);
  if (busy) {
    sendErr(req, 409, "PING 进行中，请稍后再试");
    return;
  }

  // 任务参数（host 按值拷贝，任务内独立持有，不受请求 JSON 生命周期影响）
  struct PingArgs {
    WebPanel *self;
    uint8_t   port;
    String    host;
  };
  PingArgs *a = new PingArgs{this, (uint8_t)port, host};

  portENTER_CRITICAL(&_pingMux);
  _pingBusy = true;
  _pingOk = false;
  _pingResult = "";
  portEXIT_CRITICAL(&_pingMux);

  xTaskCreate(
      [](void *arg) {
        PingArgs *a = static_cast<PingArgs *>(arg);
        String report;
        bool ok = a->self->_sms.pingHost(a->port, a->host, report);
        portENTER_CRITICAL(&a->self->_pingMux);
        a->self->_pingOk = ok;
        a->self->_pingResult = report;
        a->self->_pingBusy = false;
        portEXIT_CRITICAL(&a->self->_pingMux);
        delete a;
        vTaskDelete(nullptr);
      },
      "web_ping", 5120, a, 5, nullptr);

  JsonDocument doc;
  doc["ok"] = true;
  doc["started"] = true;
  sendJsonDoc(req, doc);
}

// PING 结果轮询：返回当前 busy / 最近一次 ok / report。
void WebPanel::handleSmsPingGet(AsyncWebServerRequest *req) {
  bool busy = false, ok = false;
  String result;
  portENTER_CRITICAL(&_pingMux);
  busy = _pingBusy;
  ok = _pingOk;
  result = _pingResult;   // 短字符串拷贝，临界区内完成
  portEXIT_CRITICAL(&_pingMux);
  JsonDocument doc;
  doc["busy"] = busy;
  doc["ok"] = ok;
  doc["report"] = result;
  sendJsonDoc(req, doc);
}

// 主动查询通信模组存储短信：POST 触发（异步，由查询任务执行）。
// JSON {"port":n?}：给出端口则只查该端口，缺省则对所有启用模组各查一次。
void WebPanel::handleSmsQueryPost(AsyncWebServerRequest *req, JsonVariant &json) {
  if (!json.is<JsonObject>()) {
    sendErr(req, 400, "请求格式错误");
    return;
  }
  int port = 0;
  if (json["port"].is<int>()) {
    port = json["port"].as<int>();
    if (port < 1 || port > cfg::kSmsMaxModems) {
      sendErr(req, 400, "通信模组序号无效");
      return;
    }
  }
  bool started = false;
  if (port) {
    started = _sms.requestQuery((uint8_t)port);
    if (!started) {
      sendErr(req, 400, "通信模组未启用或不存在");
      return;
    }
  } else {
    _sms.requestQueryAll();
    started = true;
  }
  JsonDocument doc;
  doc["ok"] = true;
  doc["started"] = started;
  sendJsonDoc(req, doc);
}

// 主动查询状态：各端口待查/执行中标记 + 上次查询发现/处理条数 + SIM 清空状态。
void WebPanel::handleSmsQueryGet(AsyncWebServerRequest *req) {
  sendJson(req, _sms.queryStatusJson());
}

// ---------------- 持久化发件箱（不丢消息转发队列）----------------

// GET /api/outbox?page=&pageSize=&id=
// 返回统计（total/pending/failed/done）与倒序分页记录，每条含各通道投递回执：
// s=pending/sending/ok/submitted/failed/skipped/dead、a=尝试次数、e=最近错误、ms=耗时
void WebPanel::handleOutboxGet(AsyncWebServerRequest *req) {
  int page = paramInt(req, "page", 1);
  int pageSize = paramInt(req, "pageSize", 20);
  uint32_t id = (uint32_t)paramInt(req, "id", 0);
  sendJson(req, _outbox.statusJson(page, pageSize, id));
}

// POST /api/outbox/retry {"id":n}；省略 id 或传 0 表示全部失败通道重新排队
void WebPanel::handleOutboxRetry(AsyncWebServerRequest *req, JsonVariant &json) {
  uint32_t id = 0;
  if (json.is<JsonObject>()) id = (uint32_t)(json["id"] | 0);
  if (!_outbox.retry(id)) {
    sendOkMsg(req, "没有需要重试的通道");
    return;
  }
  sendOkMsg(req, id ? "已重新排队该记录的失败通道" : "已重新排队全部失败通道");
}

// POST /api/outbox/clear：只清已完成回执，未送达记录一律保留
void WebPanel::handleOutboxClear(AsyncWebServerRequest *req) {
  _outbox.clearDone();
  sendOkMsg(req, "已完成回执已清空，未送达记录保留");
}

// POST /api/outbox/delete {"id":n}：删除单条（该记录未送达的通道不再补发）
void WebPanel::handleOutboxDelete(AsyncWebServerRequest *req, JsonVariant &json) {
  if (!json.is<JsonObject>()) {
    sendErr(req, 400, "请求格式错误");
    return;
  }
  uint32_t id = (uint32_t)(json["id"] | 0);
  if (!id || !_outbox.deleteOne(id)) {
    sendErr(req, 404, "记录不存在");
    return;
  }
  sendOkMsg(req, "已删除，该记录未送达通道不再补发");
}

// 清空通信模组 SIM 短信：POST {"port":n} 触发（异步，查询任务执行阻塞式 AT）。
void WebPanel::handleSmsClearPost(AsyncWebServerRequest *req, JsonVariant &json) {
  if (!json.is<JsonObject>()) {
    sendErr(req, 400, "请求格式错误");
    return;
  }
  int port = json["port"].is<int>() ? json["port"].as<int>() : 0;
  if (port < 1 || port > cfg::kSmsMaxModems) {
    sendErr(req, 400, "通信模组序号无效");
    return;
  }
  if (!_sms.requestClear((uint8_t)port)) {
    sendErr(req, 400, "通信模组未就绪或已有清空任务在执行");
    return;
  }
  JsonDocument doc;
  doc["ok"] = true;
  doc["started"] = true;
  sendJsonDoc(req, doc);
}

// 定时主动查询配置：读取开关与模式参数
void WebPanel::handleSmsPollGet(AsyncWebServerRequest *req) {
  JsonDocument doc;
  doc["ok"] = true;
  doc["enabled"] = _sms.pollEnabled();
  doc["mode"] = _sms.pollMode();
  doc["intervalH"] = _sms.pollIntervalH();
  doc["hour"] = (uint8_t)(_sms.pollDailyMin() / 60);
  doc["minute"] = (uint8_t)(_sms.pollDailyMin() % 60);
  doc["safetySec"] = (uint32_t)(_sms.safetyPollMs() / 1000);
  sendJsonDoc(req, doc);
}

// 定时主动查询配置：保存 JSON {"enabled","mode","intervalH","hour","minute","safetySec"}
void WebPanel::handleSmsPollPost(AsyncWebServerRequest *req, JsonVariant &json) {
  if (!json.is<JsonObject>() || !_sms.applyPollConfig(json.as<JsonObjectConst>())) {
    sendErr(req, 500, "配置保存失败");
    return;
  }
  JsonDocument doc;
  doc["ok"] = true;
  doc["enabled"] = _sms.pollEnabled();
  doc["mode"] = _sms.pollMode();
  doc["intervalH"] = _sms.pollIntervalH();
  doc["hour"] = (uint8_t)(_sms.pollDailyMin() / 60);
  doc["minute"] = (uint8_t)(_sms.pollDailyMin() % 60);
  doc["safetySec"] = (uint32_t)(_sms.safetyPollMs() / 1000);
  sendJsonDoc(req, doc);
}

// 短信接收模式（CNMI）：读取当前模式
void WebPanel::handleSmsStoreGet(AsyncWebServerRequest *req) {
  JsonDocument doc;
  doc["ok"] = true;
  doc["store"] = _sms.storeMode();
  sendJsonDoc(req, doc);
}

// 切换短信接收模式：JSON {"store":bool}（true=存储模式，false=直投模式）
void WebPanel::handleSmsStorePost(AsyncWebServerRequest *req, JsonVariant &json) {
  if (!json.is<JsonObject>() || !json["store"].is<bool>()) {
    sendErr(req, 400, "请求格式错误");
    return;
  }
  if (!_sms.setStoreMode(json["store"].as<bool>())) {
    sendErr(req, 500, "配置保存失败");
    return;
  }
  JsonDocument doc;
  doc["ok"] = true;
  doc["store"] = _sms.storeMode();
  sendJsonDoc(req, doc);
}

// ---- 邮件通知 ----
void WebPanel::handleEmailGet(AsyncWebServerRequest *req) {
  sendJson(req, _email.configJson());
}

void WebPanel::handleEmailEnabled(AsyncWebServerRequest *req, JsonVariant &json) {
  bool on = true;
  if (!json.is<JsonObject>() || !jsonBool(json["enabled"], on)) {
    sendErr(req, 400, "请求格式错误");
    return;
  }
  if (!_email.setEnabled(on)) {
    sendErr(req, 500, "配置保存失败");
    return;
  }
  sendJson(req, _email.configJson());
}

void WebPanel::handleEmailTest(AsyncWebServerRequest *req) {
  if (!_email.enabled()) {
    sendErr(req, 400, "邮件通知未启用，请先打开总开关");
    return;
  }
  if (!_email.isConfigured()) {
    sendErr(req, 400, "邮件配置不完整，请先保存服务器/账号/授权码/收件人");
    return;
  }
  if (WiFi.status() != WL_CONNECTED) {
    sendErr(req, 400, "WiFi 未连接，无法发送");
    return;
  }
  if (!_email.requestTest()) {
    sendErr(req, 500, "入队失败，请检查邮件配置与队列状态");
    return;
  }
  JsonDocument doc;
  doc["ok"] = true;
  doc["queued"] = true;
  sendJsonDoc(req, doc);
}

void WebPanel::handleEmailPost(AsyncWebServerRequest *req, JsonVariant &json) {
  if (!json.is<JsonObject>()) {
    sendErr(req, 400, "请求格式错误");
    return;
  }
  String server = json["server"] | "";
  String user = json["user"] | "";
  String pass = json["pass"] | "";
  String sendTo = json["sendTo"] | "";
  int port = 0;
  int notifyTypes = 0;
  int schedMode = 0;
  int schedHour = 0;
  int schedMinute = 0;
  int intervalH = 0;
  bool enabled = json["enabled"] | true;
  jsonInt(json["port"], port);
  jsonInt(json["notifyTypes"], notifyTypes);
  jsonInt(json["schedMode"], schedMode);
  jsonInt(json["schedHour"], schedHour);
  jsonInt(json["schedMinute"], schedMinute);
  jsonInt(json["intervalH"], intervalH);

  MailSchedule sched;
  sched.mode = (schedMode == (int)cfg::kSchedModeDaily)
                   ? cfg::kSchedModeDaily
                   : cfg::kSchedModeInterval;
  sched.hour = (uint8_t)constrain(schedHour, 0, 23);
  sched.minute = (uint8_t)constrain(schedMinute, 0, 59);
  int ih = constrain(intervalH, (int)cfg::kMailIntervalHMin,
                     (int)cfg::kMailOnlineIntervalMaxH);
  sched.intervalH = (uint16_t)(ih > 0 ? ih
                                      : cfg::kMailIntervalHDefault);

  if (!_email.saveConfig(server, (uint16_t)port, user, pass, sendTo,
                         (uint32_t)notifyTypes, sched, enabled)) {
    sendErr(req, 500, "配置保存失败");
    return;
  }
  sendJson(req, _email.configJson());
}

// ---- MQTT ----
void WebPanel::handleMqttGet(AsyncWebServerRequest *req) {
  sendJson(req, _mqtt.configJson());
}

void WebPanel::handleMqttEnabled(AsyncWebServerRequest *req, JsonVariant &json) {
  bool on = false;
  if (!json.is<JsonObject>() || !jsonBool(json["enabled"], on)) {
    sendErr(req, 400, "请求格式错误");
    return;
  }
  if (!_mqtt.setEnabled(on)) {
    sendErr(req, 500, "配置保存失败");
    return;
  }
  sendJson(req, _mqtt.configJson());
}

void WebPanel::handleMqttTest(AsyncWebServerRequest *req) {
  if (!_mqtt.enabled()) {
    sendErr(req, 400, "MQTT 未启用，请先打开总开关");
    return;
  }
  if (!_mqtt.isConfigured()) {
    sendErr(req, 400, "MQTT 配置不完整，请先保存服务器地址");
    return;
  }
  if (WiFi.status() != WL_CONNECTED) {
    sendErr(req, 400, "WiFi 未连接，无法发布");
    return;
  }
  if (!_mqtt.notifyTest()) {
    sendErr(req, 500, "测试消息发布失败，请检查 MQTT 配置与连接状态");
    return;
  }
  JsonDocument doc;
  doc["ok"] = true;
  doc["queued"] = true;
  sendJsonDoc(req, doc);
}

void WebPanel::handleMqttPost(AsyncWebServerRequest *req, JsonVariant &json) {
  if (!json.is<JsonObject>()) {
    sendErr(req, 400, "请求格式错误");
    return;
  }
  String scheme = json["scheme"] | "mqtt";
  String server = json["server"] | "";
  String user = json["user"] | "";
  String pass = json["pass"] | "";
  String clientId = json["clientId"] | "";
  String topicPrefix = json["topicPrefix"] | "smsf";
  int keepAlive = 60;
  int notifyTypes = 0;
  bool subscribeEnabled = true;
  bool enabled = false;
  jsonInt(json["keepAlive"], keepAlive);
  jsonBool(json["subscribeEnabled"], subscribeEnabled);
  jsonBool(json["enabled"], enabled);
  jsonInt(json["notifyTypes"], notifyTypes);

  if (!_mqtt.saveConfig(scheme, server, user, pass, clientId,
                         topicPrefix, (uint16_t)keepAlive, subscribeEnabled,
                         (uint32_t)notifyTypes, enabled)) {
    sendErr(req, 500, "配置保存失败");
    return;
  }
  sendJson(req, _mqtt.configJson());
}

// ---- 推送通道 ----
void WebPanel::handlePushGet(AsyncWebServerRequest *req) {
  sendJson(req, _push.configJson());
}

void WebPanel::handlePushTypes(AsyncWebServerRequest *req) {
  String types = PushService::typesJson();
  JsonDocument doc;
  doc["ok"] = true;
  doc["types"] = serialized(types);
  sendJsonDoc(req, doc);
}

void WebPanel::handlePushEnabled(AsyncWebServerRequest *req, JsonVariant &json) {
  bool on = false;
  if (!json.is<JsonObject>() || !jsonBool(json["enabled"], on)) {
    sendErr(req, 400, "请求格式错误");
    return;
  }
  if (!_push.setEnabled(on)) {
    sendErr(req, 500, "配置保存失败");
    return;
  }
  sendJson(req, _push.configJson());
}

void WebPanel::handlePushTest(AsyncWebServerRequest *req) {
  if (!_push.enabled()) {
    sendErr(req, 400, "推送未启用，请先打开总开关");
    return;
  }
  if (WiFi.status() != WL_CONNECTED) {
    sendErr(req, 400, "WiFi 未连接，无法发送");
    return;
  }
  if (!_push.requestTest()) {
    sendErr(req, 400, "推送未配置任何有效通道");
    return;
  }
  JsonDocument doc;
  doc["ok"] = true;
  doc["queued"] = true;
  sendJsonDoc(req, doc);
}

void WebPanel::handlePushPost(AsyncWebServerRequest *req, JsonVariant &json) {
  if (!json.is<JsonObject>()) {
    sendErr(req, 400, "请求格式错误");
    return;
  }
  if (!_push.saveFromJson(json.as<JsonObject>())) {
    sendErr(req, 500, "配置保存失败");
    return;
  }
  sendJson(req, _push.configJson());
}

// ---- 黑名单 ----
void WebPanel::handleBlackGet(AsyncWebServerRequest *req) {
  sendJson(req, _blacklist.configJson());
}

void WebPanel::handleBlackEnabled(AsyncWebServerRequest *req, JsonVariant &json) {
  bool on = false;
  if (!json.is<JsonObject>() || !jsonBool(json["enabled"], on)) {
    sendErr(req, 400, "请求格式错误");
    return;
  }
  _blacklist.setEnabled(on);
  if (!_blacklist.saveConfig()) {
    sendErr(req, 500, "配置保存失败");
    return;
  }
  sendJson(req, _blacklist.configJson());
}

void WebPanel::handleBlackPost(AsyncWebServerRequest *req, JsonVariant &json) {
  if (!json.is<JsonObject>()) {
    sendErr(req, 400, "请求格式错误");
    return;
  }
  if (!_blacklist.saveFromJson(json.as<JsonObject>())) {
    // 可能是校验失败（空号码/越界）或 NVS 写入失败；统一给 400 便于前端提示
    sendErr(req, 400, "保存失败：规则校验未通过或写入 NVS 出错");
    return;
  }
  sendJson(req, _blacklist.configJson());
}

void WebPanel::handleBlackTest(AsyncWebServerRequest *req) {
  String num = stringParam(req, "num");
  if (!num.length()) {
    sendErr(req, 400, "缺少号码");
    return;
  }
  // 忽略总开关逐条测试（供前端调试规则）
  bool matched = _blacklist.testMatch(num, BlackList::kSms);
  JsonDocument doc;
  doc["ok"] = true;
  doc["matched"] = matched;
  doc["scope"] = matched ? (int)BlackList::kSms : 0;
  doc["note"] = _blacklist.lastNote();
  sendJsonDoc(req, doc);
}

// ---- OTA 升级 ----
// 状态快照：前端轮询此接口渲染进度与「发现新版本」提示
void WebPanel::handleOtaStatus(AsyncWebServerRequest *req) {
  OtaStatusPayload s = Ota::status();
  AsyncJsonResponse *resp = new AsyncJsonResponse();
  auto o = resp->getRoot();
  o["state"]          = (uint8_t)s.state;
  o["progress"]       = s.progress;
  o["message"]        = s.message;
  o["currentVersion"] = s.currentVersion;
  o["latestVersion"]  = s.latestVersion;
  o["available"]      = otaPartitionAvailable();
  resp->setLength();
  req->send(resp);
}

// 触发版本检查（异步），立即返回当前版本；最新版本由 /api/ota/status 轮询得到
void WebPanel::handleOtaVersion(AsyncWebServerRequest *req) {
  OtaStatusPayload s = Ota::status();
  Ota::startVersionCheck();
  AsyncJsonResponse *resp = new AsyncJsonResponse();
  auto o = resp->getRoot();
  o["ok"]            = true;
  o["version"]       = Ota::version();
  o["latestVersion"] = s.latestVersion;
  o["available"]     = otaPartitionAvailable();
  resp->setLength();
  req->send(resp);
}

// POST /api/ota/start 的请求体累积（JSON {"tag":"..."}）
void WebPanel::handleOtaStartBody(AsyncWebServerRequest *req, uint8_t *data, size_t len,
                                  size_t /*index*/, size_t /*total*/) {
  if (!req->_tempObject) req->_tempObject = new String();
  String *s = (String *)req->_tempObject;
  s->concat((char *)data, len);
}

// 启动在线升级：解析 tag，交给后台任务下载并写入 OTA 分区
void WebPanel::handleOtaStart(AsyncWebServerRequest *req) {
  if (!req->_tempObject) {
    sendErr(req, 400, "缺少请求内容");
    return;
  }
  String body = *((String *)req->_tempObject);
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, body);
  delete (String *)req->_tempObject;
  req->_tempObject = nullptr;
  if (err || !doc["tag"].is<const char *>()) {
    sendErr(req, 400, "请求格式错误");
    return;
  }
  String tag = doc["tag"].as<String>();
  if (!Ota::startOnlineUpgrade(tag)) {
    sendErr(req, 400, "升级任务已在运行或参数无效");
    return;
  }
  sendOkFlag(req, "started", true);
}

// 本地固件上传：收尾时返回结果（成功/失败由 Ota 状态决定）
void WebPanel::handleOtaUploadReq(AsyncWebServerRequest *req) {
  OtaStatusPayload s = Ota::status();
  bool ok = (s.state == OtaState::SUCCESS);
  AsyncJsonResponse *resp = new AsyncJsonResponse();
  auto o = resp->getRoot();
  o["ok"] = ok;
  if (ok) {
    o["message"] = s.message.length() ? s.message : "固件上传成功";
  } else {
    o["error"] = s.message.length() ? s.message : "固件写入失败";
  }
  resp->setLength();
  req->send(resp);
}

// 固件分片写入 OTA 分区；失败时立即中止上传（防止后续分片触发重复初始化）
void WebPanel::handleOtaUploadChunk(AsyncWebServerRequest *req, String /*filename*/,
                                    size_t index, uint8_t *data, size_t len, bool final) {
  if (!Ota::handleUploadChunk(data, len, index, final)) {
    req->send(500, "application/json",
              "{\"ok\":false,\"error\":\"固件写入失败，请确认固件与本设备匹配\"}");
  }
}

// 本地 Web UI（LittleFS 镜像）上传：收尾时返回结果
void WebPanel::handleOtaUploadFsReq(AsyncWebServerRequest *req) {
  OtaStatusPayload s = Ota::status();
  bool ok = (s.state == OtaState::SUCCESS);
  AsyncJsonResponse *resp = new AsyncJsonResponse();
  auto o = resp->getRoot();
  o["ok"] = ok;
  if (ok) {
    o["message"] = s.message.length() ? s.message : "Web UI 上传成功";
  } else {
    o["error"] = s.message.length() ? s.message : "Web UI 写入失败";
  }
  resp->setLength();
  req->send(resp);
}

// LittleFS 镜像分片写入 data 分区；totalSize 仅用于进度百分比估算
void WebPanel::handleOtaUploadFsChunk(AsyncWebServerRequest *req, String /*filename*/,
                                      size_t index, uint8_t *data, size_t len, bool final) {
  size_t totalSize = 0;
  if (req->hasHeader("Content-Length")) {
    totalSize = (size_t)req->header("Content-Length").toInt();
  }
  if (!Ota::handleLfsUploadChunk(data, len, index, totalSize, final)) {
    req->send(500, "application/json",
              "{\"ok\":false,\"error\":\"Web UI 写入失败，请确认镜像与本设备匹配\"}");
  }
}

// ---- 配置导入导出 ----
// 导出：各通信模组直接把配置写入主文档的子对象（全程操作 JSON 对象，无字符串往返）。
void WebPanel::handleConfigExport(AsyncWebServerRequest *req) {
  JsonDocument doc;
  doc["ver"] = 1;                  // 配置格式版本，便于后续升级/兼容
  doc["product"] = "sms-forwarder"; // 出品标识，导入时可用于校验
  JsonObject oWifi  = doc["wifi"].to<JsonObject>();
  JsonObject oAuth  = doc["auth"].to<JsonObject>();
  JsonObject oEmail = doc["email"].to<JsonObject>();
  JsonObject oSms   = doc["sms"].to<JsonObject>();
  JsonObject oMqtt  = doc["mqtt"].to<JsonObject>();
  JsonObject oPush  = doc["push"].to<JsonObject>();
  JsonObject oBlack = doc["blacklist"].to<JsonObject>();
  JsonObject oDisp  = doc["disp"].to<JsonObject>();
  _wifi.exportAllTo(oWifi);
  _auth.exportTo(oAuth);
  _email.exportTo(oEmail);
  _sms.exportTo(oSms);
  _mqtt.exportTo(oMqtt);
  _push.exportTo(oPush);
  _blacklist.exportTo(oBlack);
  _ui.exportTo(oDisp);
  JsonObject oSched = doc["sched"].to<JsonObject>();
  SystemService::instance().exportTo(oSched);

  String body;
  serializeJsonPretty(doc, body);

  char filename[40] = "sms_forwarder_config.json";
  time_t now = time(nullptr);
  if (now > 100000) {  // 时间已同步才用日期命名，否则用默认名
    struct tm t;
    localtime_r(&now, &t);
    snprintf(filename, sizeof(filename),
             "sms_forwarder_config_%04d%02d%02d.json",
             t.tm_year + 1900, t.tm_mon + 1, t.tm_mday);
  }

  char disposition[96];
  snprintf(disposition, sizeof(disposition), "attachment; filename=\"%s\"", filename);

  AsyncResponseStream *resp = req->beginResponseStream("application/octet-stream");
  resp->addHeader("Content-Disposition", disposition);
  resp->addHeader("X-Content-Type-Options", "nosniff");
  resp->print(body);
  req->send(resp);

  _log.addf(LogLevel::kInfo, "http", "配置已导出，大小 %u 字节", (unsigned)body.length());
}

// 导入：onBody 分片回调，把原始请求体累积进 req->_tempObject（String*）
void WebPanel::handleConfigImportChunk(AsyncWebServerRequest *req, uint8_t *data,
                                      size_t len, size_t /*index*/,
                                      size_t /*total*/) {
  if (!req->_tempObject) req->_tempObject = new String();
  String *s = (String *)req->_tempObject;
  s->concat((char *)data, len);
}

// 导入：请求收尾后解析整份 JSON，逐节写入各通信模组并落盘，最后延迟重启生效
void WebPanel::handleConfigImportReq(AsyncWebServerRequest *req) {
  if (!req->_tempObject) {
    sendErr(req, 400, "缺少请求内容");
    return;
  }
  String body = *((String *)req->_tempObject);
  delete (String *)req->_tempObject;
  req->_tempObject = nullptr;

  if (body.length() > 65536) {  // 配置不会很大，超限直接拒绝（防内存滥用）
    sendErr(req, 413, "配置文件过大");
    return;
  }

  JsonDocument doc;
  if (deserializeJson(doc, body) != DeserializationError::Ok || !doc.is<JsonObject>()) {
    sendErr(req, 400, "JSON 解析失败，请确认文件格式正确");
    return;
  }

  // 逐节导入，缺失的节跳过（支持部分导入 / 向后兼容）
  bool anyApplied = false;

  if (doc["wifi"].is<JsonObject>()) {
    if (!_wifi.importAllFrom(doc["wifi"].as<JsonObject>())) {
      sendErr(req, 500, "WiFi 配置写入失败");
      return;
    }
    anyApplied = true;
  }
  if (doc["auth"].is<JsonObject>()) {
    if (!_auth.importFrom(doc["auth"].as<JsonObject>())) {
      sendErr(req, 500, "账号配置写入失败");
      return;
    }
    anyApplied = true;
  }
  if (doc["email"].is<JsonObject>()) {
    if (!_email.importFrom(doc["email"].as<JsonObject>())) {
      sendErr(req, 500, "邮件配置写入失败");
      return;
    }
    anyApplied = true;
  }
  if (doc["sms"].is<JsonObject>()) {
    if (!_sms.importFrom(doc["sms"].as<JsonObject>())) {
      sendErr(req, 500, "短信配置写入失败");
      return;
    }
    anyApplied = true;
  }
  if (doc["mqtt"].is<JsonObject>()) {
    if (!_mqtt.importFrom(doc["mqtt"].as<JsonObject>())) {
      sendErr(req, 500, "MQTT 配置写入失败");
      return;
    }
    anyApplied = true;
  }
  if (doc["push"].is<JsonObject>()) {
    if (!_push.importFrom(doc["push"].as<JsonObject>())) {
      sendErr(req, 500, "推送配置写入失败");
      return;
    }
    anyApplied = true;
  }
  if (doc["blacklist"].is<JsonObject>()) {
    if (!_blacklist.importFrom(doc["blacklist"].as<JsonObject>())) {
      sendErr(req, 500, "黑名单配置写入失败");
      return;
    }
    anyApplied = true;
  }
  if (doc["disp"].is<JsonObject>()) {
    if (!_ui.importFrom(doc["disp"].as<JsonObject>())) {
      sendErr(req, 500, "屏幕设置写入失败");
      return;
    }
    anyApplied = true;
  }
  if (doc["sched"].is<JsonObject>()) {
    if (!SystemService::instance().importFrom(doc["sched"].as<JsonObject>())) {
      sendErr(req, 500, "定时重启配置写入失败");
      return;
    }
    anyApplied = true;
  }

  if (!anyApplied) {
    sendErr(req, 400, "配置格式不兼容，未找到可识别的配置节（wifi/auth/email/sms/mqtt）");
    return;
  }

  // 账号可能已被改：导入后立即作废全部会话与令牌，强制重新登录
  invalidateAllSessions();

  _log.addf(LogLevel::kInfo, "http", "配置导入成功，准备重启");

  // 先返回成功，再延迟 2 秒重启（与 OTA 一致，给响应留出发送时间）
  sendOkMsg(req, "配置导入成功，设备将在2秒后自动重启");

  xTaskCreate([](void *) {
    vTaskDelay(pdMS_TO_TICKS(2000));
    esp_restart();
    vTaskDelete(nullptr);
  }, "cfg_rst", 2048, nullptr, 5, nullptr);
}

// ---- 崩溃转储（coredump） ----
// 信息：分区是否有快照 + 崩溃时间 / 版本 + 调试符号链接。
// 分区不存在（分区表被换掉）时按「无数据」处理，前端隐藏整块卡片
void WebPanel::handleCoredumpInfo(AsyncWebServerRequest *req) {
  const esp_partition_t *part = Coredump::partition();
  size_t used = part ? Coredump::usedSize(part) : 0;

  AsyncJsonResponse *resp = new AsyncJsonResponse();
  auto o = resp->getRoot();
  o["hasCoredump"] = (used > 0);
  if (used > 0) {
    o["size"]         = static_cast<uint32_t>(used);
    o["crashTime"]    = static_cast<long long>(Coredump::crashTime());
    o["crashVersion"] = Coredump::crashVersion();
    String elfUrl = Coredump::elfUrl();
    if (elfUrl.length() > 0) o["elfUrl"] = elfUrl;
  }
  resp->setLength();
  req->send(resp);
}

// 导出：分块流式读分区（不把整个快照搬进堆），经 Content-Disposition
// 触发浏览器下载。文件名带崩溃时间（UTC），未知时用 unknown
void WebPanel::handleCoredumpExport(AsyncWebServerRequest *req) {
  const esp_partition_t *part = Coredump::partition();
  if (part == nullptr) {
    sendErr(req, 404, "未找到 coredump 分区");
    return;
  }
  size_t used = Coredump::usedSize(part);
  if (used == 0) {
    sendErr(req, 404, "未发现可导出的 coredump（分区为空）");
    return;
  }

  _log.addf(LogLevel::kInfo, "http", "导出 coredump：已用=%u 字节，分区=%s",
            static_cast<unsigned>(used), part->label);

  AsyncWebServerResponse *resp = req->beginChunkedResponse(
      "application/octet-stream",
      [part, used](uint8_t *buffer, size_t maxLen, size_t index) -> size_t {
        if (index >= used) return 0;
        size_t left  = used - index;
        size_t toRead = left < maxLen ? left : maxLen;
        if (esp_partition_read(part, index, buffer, toRead) != ESP_OK) {
          return 0;  // 读失败即中止传输（前端按不完整的下载处理）
        }
        return toRead;
      });

  char filename[48];
  time_t ct = Coredump::crashTime();
  if (ct > 0) {
    char ts[20];
    struct tm ti;
    gmtime_r(&ct, &ti);
    strftime(ts, sizeof(ts), "%Y%m%dT%H%M%S", &ti);
    snprintf(filename, sizeof(filename), "sms-forwarder-coredump-%s.bin", ts);
  } else {
    snprintf(filename, sizeof(filename), "sms-forwarder-coredump-unknown.bin");
  }
  resp->addHeader("Content-Disposition",
                  String("attachment; filename=") + filename);
  resp->addHeader("Cache-Control", "no-store");
  resp->addHeader("X-CoreDump-Size", String(used));
  req->send(resp);
}

// 清除：整分区擦除。分区起点 0xFE0000、大小 128KB，均按 4KB 扇区对齐，
// 满足 esp_partition_erase_range 的对齐要求，可直接整片擦。
// 只清快照；NVS 里的崩溃时间 / 版本保留（供追溯），下次 panic 覆盖
void WebPanel::handleCoredumpClear(AsyncWebServerRequest *req) {
  const esp_partition_t *part = Coredump::partition();
  if (part == nullptr) {
    sendErr(req, 404, "未找到 coredump 分区");
    return;
  }
  esp_err_t err = esp_partition_erase_range(part, 0, part->size);
  if (err != ESP_OK) {
    char msg[48];
    snprintf(msg, sizeof(msg), "擦除失败: %s", esp_err_to_name(err));
    _log.addf(LogLevel::kError, "http", "清除 coredump 失败: %s",
              esp_err_to_name(err));
    sendErr(req, 500, msg);
    return;
  }
  _log.addf(LogLevel::kInfo, "http", "已清除 coredump 分区: %s (size=%u)",
            part->label, static_cast<unsigned>(part->size));
  sendOkMsg(req, "coredump 已清除");
}

// ---- 兜底 404 ----
void WebPanel::handleNotFound(AsyncWebServerRequest *req) {
  sendErr(req, 404, "接口不存在");
}
