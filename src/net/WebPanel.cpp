#include "net/WebPanel.h"

#include <ArduinoJson.h>
#include <LittleFS.h>
#include <Preferences.h>

#include <Arduino.h>

#include "net/StateJson.h"

namespace {

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
  req->send(404, "application/json", "{\"ok\":false,\"error\":\"not found\"}");
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
// req：请求对象；code：HTTP 状态码；msg：错误描述
void sendErr(AsyncWebServerRequest *req, int code, const char *msg) {
  req->send(code, "application/json",
            String("{\"ok\":false,\"error\":\"") + msg + "\"}");
}

// 统一成功响应：{"ok":true}，可附额外字段（如 ",\"id\":12"）
void sendOk(AsyncWebServerRequest *req, const String &extra = "") {
  req->send(200, "application/json", String("{\"ok\":true") + extra + "}");
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

// bridge/led/wifi/display/auth/events/sms/time/email/log：各模块引用，保存供路由处理器使用
WebPanel::WebPanel(SerialBridge &bridge, RgbLed &led, WifiManager &wifi,
                   Display &display, AdminStore &auth, StatusBus &events,
                   SmsService &sms, TimeSync &time, EmailService &email,
                   LogStore &log)
    : _bridge(bridge), _led(led), _wifi(wifi), _display(display), _auth(auth),
      _events(events), _sms(sms), _time(time), _email(email), _log(log) {}

// 挂载 LittleFS 专有分区，注册路由与认证中间件，启动 HTTP 服务
void WebPanel::begin() {
  ensureApiToken();   // 载入/生成持久化 API 令牌（NVS）
  // 挂载失败时格式化一次重试；仍失败则仅 API 可用，页面返回 404
  if (!LittleFS.begin(true)) {
    Serial.println("[http] LittleFS mount failed, web page unavailable");
  } else {
    Serial.println("[http] LittleFS mounted");
  }

  setupRoutes();
  _events.begin(_server);   // 注册 SSE 事件流（/api/sse，复用上面的认证中间件）
  _server.begin();
  Serial.printf("[http] server on :%u\r\n", cfg::kHttpPort);
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
      if (t.length() && (storeHas(_tokens, _tokenCount, t) || t == _apiToken)) return true;
    }
  }
  return false;
}

// 作废全部会话与访问令牌（账号密码变更后调用，所有客户端需重新登录）
void WebPanel::invalidateAllSessions() {
  _sessionCount = 0;
  _tokenCount = 0;
  Serial.println("[http] all sessions and tokens invalidated");
}

// 载入/生成持久化 API 令牌：NVS 已有且长度合法则复用，否则生成并写回
void WebPanel::ensureApiToken() {
  Preferences prefs;
  String t;
  if (prefs.begin(cfg::kNvsNamespace, true)) {
    t = prefs.getString(cfg::kNvsKeyToken, "");
    prefs.end();
  }
  if (t.length() != 32) {  // genSecret 生成 32 位十六进制；非法则重新生成
    t = genSecret();
    if (prefs.begin(cfg::kNvsNamespace, false)) {
      prefs.putString(cfg::kNvsKeyToken, t);
      prefs.end();
    }
    Serial.println("[http] API 令牌缺失，已自动生成并写入 NVS");
  } else {
    Serial.println("[http] 已从 NVS 载入 API 令牌");
  }
  _apiToken = t;
}

// 重新生成 API 令牌（写 NVS），返回新令牌
String WebPanel::resetApiToken() {
  _apiToken = genSecret();
  Preferences prefs;
  if (prefs.begin(cfg::kNvsNamespace, false)) {
    prefs.putString(cfg::kNvsKeyToken, _apiToken);
    prefs.end();
  }
  Serial.println("[http] API 令牌已重置");
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

// 注册全部路由：
// 1. 认证中间件：/api/ 前缀统一要求登录（Cookie 或 Bearer 令牌），
//    登录/登出/会话探测三个端点放行；页面与静态资源公开（不含敏感数据）；
// 2. 精确路由（API / 首页）先注册，serveStatic 兜底其余静态资源；
// 3. 父子路径顺序：ESPAsyncWebServer 对普通 URI 采用 BackwardCompatible
//    匹配（精确 或 前缀 + "/"，见库 WebServer.cpp 的 AsyncURIMatcher::matches），
//    且按注册顺序取第一个匹配，故子路径必须注册在父路径之前，否则会被父路由
//    吞掉（返回父路由的响应，字段对不上）。例：/api/logs/stats 必须先于
//    /api/logs，/api/sms/stats|latest|ports(GET) 必须先于 /api/sms，
//    /api/email/enabled|test(POST) 必须先于 POST /api/email。
//    仅父路由未注册该方法时（如 /api/sms 无 POST），子路由才可放在后面。
void WebPanel::setupRoutes() {
  _server.addMiddleware([this](AsyncWebServerRequest *req,
                               ArMiddlewareNext next) {
    String url = req->url();
    if (url == "/api/login" || url == "/api/logout" || url == "/api/session") {
      next();  // 登录相关端点无需已认证
      return;
    }
    if (url.startsWith("/api/") && !authorized(req)) {
      sendErr(req, 401, "unauthorized");
      return;
    }
    next();
  });

  // ---- 页面与静态资源（LittleFS，公开）----
  // 首页：短信转发器 Web 控制台单页应用（左侧菜单 + 右侧内容，hash 路由）
  _server.on("/", HTTP_GET, [this](AsyncWebServerRequest *req) {
    sendFile(req, "/index.html", "text/html; charset=utf-8");
  });
  // 独立登录页（已登录时页面脚本自行跳回 /）
  _server.on("/login", HTTP_GET, [this](AsyncWebServerRequest *req) {
    sendFile(req, "/login.html", "text/html; charset=utf-8");
  });
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
             [this](AsyncWebServerRequest *req, JsonVariant &json) {
               if (!json.is<JsonObject>() || !json["user"].is<const char *>() ||
                   !json["pass"].is<const char *>()) {
                 sendErr(req, 400, "invalid json");
                 return;
               }
               String user = json["user"].as<String>();
               String pass = json["pass"].as<String>();
               if (!_auth.check(user, pass)) {
                 sendErr(req, 401, "bad credentials");
                 return;
               }
               String sid = genSecret();
               String token = genSecret();
               if (!storeAdd(_sessions, _sessionCount, sid) ||
                   !storeAdd(_tokens, _tokenCount, token)) {
                 sendErr(req, 500, "too many sessions");
                 return;
               }
               String cookie = String(cfg::kCookieName) + "=" + sid +
                               "; HttpOnly; SameSite=Strict; Path=/; Max-Age=" +
                               String(cfg::kCookieMaxAge);
               sendJsonWithCookie(req,
                                  "{\"ok\":true,\"user\":\"" + user +
                                      "\",\"token\":\"" + token + "\"}",
                                  cookie);
             });

  // 登出：作废当前会话 Cookie 与 Bearer 令牌，并清除浏览器 Cookie
  _server.on("/api/logout", HTTP_POST, [this](AsyncWebServerRequest *req) {
    String sid = cookieValue(req, cfg::kCookieName);
    if (sid.length()) storeRemove(_sessions, _sessionCount, sid);
    if (req->hasHeader("Authorization")) {
      String a = req->getHeader("Authorization")->value();
      if (a.startsWith("Bearer ")) storeRemove(_tokens, _tokenCount, a.substring(7));
    }
    String cookie = String(cfg::kCookieName) +
                    "=; HttpOnly; SameSite=Strict; Path=/; Max-Age=0";
    sendJsonWithCookie(req, "{\"ok\":true}", cookie);
  });

  // 会话探测：前端据此判断是否已登录（未认证返回 401）
  _server.on("/api/session", HTTP_GET, [this](AsyncWebServerRequest *req) {
    if (!authorized(req)) {
      sendErr(req, 401, "unauthorized");
      return;
    }
    sendJson(req, "{\"ok\":true,\"user\":\"" + _auth.user() + "\"}");
  });

  // ---- API 访问令牌（持久化，重启不失效）----
  // 查询当前令牌（需登录）；UI 默认掩码展示，点按后才显示完整
  _server.on("/api/token", HTTP_GET, [this](AsyncWebServerRequest *req) {
    if (!authorized(req)) { sendErr(req, 401, "unauthorized"); return; }
    sendJson(req, "{\"ok\":true,\"token\":\"" + _apiToken + "\"}");
  });
  // 重置令牌：生成新令牌并写 NVS，旧令牌立即失效
  _server.on("/api/token", HTTP_POST, [this](AsyncWebServerRequest *req) {
    if (!authorized(req)) { sendErr(req, 401, "unauthorized"); return; }
    String t = resetApiToken();
    sendJson(req, "{\"ok\":true,\"token\":\"" + t + "\"}");
  });

  // ---- 管理账号 ----
  // 查询当前用户名（密码不回传）
  _server.on("/api/auth", HTTP_GET, [this](AsyncWebServerRequest *req) {
    sendJson(req, "{\"ok\":true,\"user\":\"" + _auth.user() + "\"}");
  });

  // 修改用户名 / 密码：JSON {"user":"新用户","pass":"新密码","passOld":"当前密码"}
  // 需验证当前密码（防登录态被冒用）；成功后作废全部会话与令牌，
  // 所有客户端（含当前浏览器）需用新账号重新登录
  _server.on("/api/auth", HTTP_POST,
             [this](AsyncWebServerRequest *req, JsonVariant &json) {
               if (!json.is<JsonObject>() || !json["user"].is<const char *>() ||
                   !json["pass"].is<const char *>() ||
                   !json["passOld"].is<const char *>()) {
                 sendErr(req, 400, "invalid json");
                 return;
               }
               String user = json["user"].as<String>();
               String pass = json["pass"].as<String>();
               String passOld = json["passOld"].as<String>();
               Serial.printf("[auth] change: new user=%s, stored user=%s\r\n",
                             user.c_str(), _auth.user().c_str());
               if (!_auth.check(_auth.user(), passOld)) {
                 Serial.println("[auth] change rejected: current password mismatch");
                 sendErr(req, 403, "当前密码不正确");
                 return;
               }
               String err;
               if (!_auth.update(user, pass, &err)) {
                 Serial.printf("[auth] change failed: %s\r\n", err.c_str());
                 sendErr(req, 400, err.c_str());
                 return;
               }
               invalidateAllSessions();  // 账号变更，全部登录态作废
               sendOk(req);
             });

  // ---- RGB ----
  // 切换到下一个预设颜色，返回最新 RGB 状态
  _server.on("/api/led", HTTP_GET, [this](AsyncWebServerRequest *req) {
    _led.next();
    sendJson(req, stateRgbJson(_led));
  });

  // 查询 RGB 状态：{"ok":true,"color":名,"rgb":[r,g,b],"index":n,"bright":0-100}
  _server.on("/api/rgb", HTTP_GET, [this](AsyncWebServerRequest *req) {
    sendJson(req, stateRgbJson(_led));
  });

  // 设置 RGB：JSON，三个维度可任意组合（都可选）：
  //   {"c":索引}        走预设（0-4，越界 400）
  //   {"r":n,"g":n,"b":n}  任意颜色（0-255，越界钳制）
  //   {"bright":0-100}  亮度百分比（越界钳制，实时折算输出）
  _server.on("/api/rgb", HTTP_POST,
             [this](AsyncWebServerRequest *req, JsonVariant &json) {
               if (!json.is<JsonObject>()) {
                 sendErr(req, 400, "invalid json");
                 return;
               }
               int c;
               if (jsonInt(json["c"], c)) {
                 if (c < 0 || c >= RgbLed::count()) {
                   sendErr(req, 400, "invalid preset index");
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
             });

  // ---- 串口 AT ----
  // 发送 AT 命令：?port=1|2&baud=115200&eol=0..3&cmd=...&t=超时ms
  // 经短信模块发送（与短信收发共用端口锁、自动还原 CMGF/CNMI），避免与短信冲突
  _server.on("/api/at/send", HTTP_GET, [this](AsyncWebServerRequest *req) {
    uint8_t port = paramInt(req, "port", 1);                 // 串口号，默认 1
    uint32_t baud = paramInt(req, "baud", cfg::kUartBaud);   // 波特率，默认配置值
    uint8_t eol = paramInt(req, "eol", 0);                   // 行尾符，默认 CRLF
    uint32_t timeout = constrain(paramInt(req, "t", 1000), 100, 5000);  // 超时钳制在 100..5000ms
    String cmd = stringParam(req, "cmd");                    // 命令文本

    String err;
    auto result = _sms.sendAt(port, baud, cmd, (SerialBridge::Eol)eol, timeout, err);
    if (!result.ok) {
      sendErr(req, 400, err.length() ? err.c_str() : "invalid port");
      return;
    }
    sendJson(req, _bridge.entryJson(result.id));  // 直接回该条记录 JSON
  });

  // 增量拉取历史：?after=上一页最后ID&n=条数
  _server.on("/api/at/hist", HTTP_GET, [this](AsyncWebServerRequest *req) {
    int32_t after = paramInt(req, "after", -1);   // -1 = 从头拉
    size_t n = (size_t)paramInt(req, "n", 20);    // 默认 20 条
    sendJson(req, _bridge.historyJson(after, n));
  });

  // 清空历史记录
  _server.on("/api/at/clear", HTTP_GET, [this](AsyncWebServerRequest *req) {
    _bridge.clearHistory();
    sendOk(req);
  });

  // 串口重新初始化（手动兜底）：?port=1|2
  _server.on("/api/at/reset", HTTP_GET, [this](AsyncWebServerRequest *req) {
    uint8_t port = paramInt(req, "port", 0);
    if (port < 1 || port > 2) {
      sendErr(req, 400, "invalid port");
      return;
    }
    _bridge.reset(port);
    sendOk(req);
  });

  // 串口运行统计 JSON
  _server.on("/api/uart/stats", HTTP_GET, [this](AsyncWebServerRequest *req) {
    sendJson(req, _bridge.statsJson());
  });

  // ================= 设备日志（PSRAM 环形缓冲）=================
  // 路由顺序注意：ESPAsyncWebServer 对普通 URI 采用「精确 或 前缀 + /」匹配
  // （AsyncURIMatcher 的 BackwardCompatible），handler 按注册顺序取第一个匹配，
  // 故子路径（/api/logs/stats）必须先于父路径（/api/logs）注册，否则会被父路由
  // 吞掉——表现为返回父路由的 JSON（字段对不上，页面出现「-」）。

  // 存储统计：{"ok":true,"total":n,"max":n,"psram":bool,"bytes":n,
  //  "psramTotal":n,"psramFree":n}
  // （psram 表示环形缓冲实际是否落在 PSRAM，bytes 为占用字节数）
  _server.on("/api/logs/stats", HTTP_GET, [this](AsyncWebServerRequest *req) {
    sendJson(req, _log.statsJson());
  });

  // 分页查询：?page=1&pageSize=20&level=2&q=关键字
  // page 从 1 起；pageSize 越界钳制（上限 kLogPageSizeMax）；
  // level 为「最低级别」过滤（0 调试 / 1 信息 / 2 警告 / 3 错误，缺省不过滤）；
  // q 对 tag/msg 做大小写不敏感的子串匹配；结果按 ID 倒序（最新在前）：
  // {"ok":true,"total":n,"page":p,"pageSize":ps,
  //  "logs":[{id,ts,level,tag,msg}...]}
  _server.on("/api/logs", HTTP_GET, [this](AsyncWebServerRequest *req) {
    // level 缺省不过滤；前端「仅看警告以上」传 level=2
    int minLevel = req->hasParam("level") ? paramInt(req, "level", -1) : -1;
    String q = stringParam(req, "q");

    // ---- 增量模式：?after=<id>&limit=<n> ----
    // 只取该 ID 之后的新记录（升序），供前端轮询追加，避免整页重绘闪烁。
    // 过滤条件与分页模式共用；last 为服务端最新 ID（前端据此推进基线）
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
      String outInc;
      serializeJson(doc, outInc);
      sendJson(req, outInc);
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
    String out;
    serializeJson(doc, out);
    sendJson(req, out);
  });

  // 清空全部日志（逻辑清空，ID 计数保留，便于前端区分新旧）；
  // 清空动作本身补记一条日志
  _server.on("/api/logs", HTTP_DELETE, [this](AsyncWebServerRequest *req) {
    _log.clear();
    _log.add(LogLevel::kInfo, "log", "日志已清空");
    sendOk(req);
  });

  // ---- 屏幕开关 / 亮度 ----
  // 查询当前状态：{"ok":true,"on":bool,"bright":0-100}
  _server.on("/api/disp", HTTP_GET, [this](AsyncWebServerRequest *req) {
    sendJson(req, stateDispJson(_display));
  });

  // 设置屏幕：JSON {"on":bool?,"bright":0-100?}，两个字段都可选
  // （只传其一即可；bright 越界自动钳制）；返回最新状态
  _server.on("/api/disp", HTTP_POST,
             [this](AsyncWebServerRequest *req, JsonVariant &json) {
               if (!json.is<JsonObject>()) {
                 sendErr(req, 400, "invalid json");
                 return;
               }
               bool on;
               if (jsonBool(json["on"], on)) {
                 _display.setScreenOn(on);
               }
               int b;
               if (jsonInt(json["bright"], b)) {
                 if (b < 0) b = 0;
                 if (b > 100) b = 100;
                 _display.setBrightness((uint8_t)b);
               }
               sendJson(req, stateDispJson(_display));
             });

  // 查询默认波特率（供前端下拉框初始化）
  _server.on("/api/at/baud", HTTP_GET, [this](AsyncWebServerRequest *req) {
    req->send(200, "application/json", "{\"default\":" + String(cfg::kUartBaud) + "}");
  });

  // ---- 系统 ----
  // 汇总状态 JSON（页面为静态文件，所有状态由前端从此接口拉取渲染；
  // 浏览器端同时经 /api/sse 实时推送，序列化共用 StateJson.h）
  _server.on("/api/status", HTTP_GET, [this](AsyncWebServerRequest *req) {
    sendJson(req, stateStatusJson(_wifi, _led, _display, _time));
  });

  // ================= WiFi 配网 =================
  // 状态查询：模式 / 连接 / AP 信息 / 已保存数量
  _server.on("/api/wifi/status", HTTP_GET, [this](AsyncWebServerRequest *req) {
    sendJson(req, _wifi.statusJson());
  });

  // 扫描附近 WiFi：扫描中返回 {"busy":1}，完成后返回 {"busy":0,"nets":[...]}
  // scanComplete() 语义：RUNNING=扫描中 / FAILED=无扫描且无结果 / >=0=结果就绪
  _server.on("/api/wifi/scan", HTTP_GET, [this](AsyncWebServerRequest *req) {
    int16_t state = WiFi.scanComplete();
    if (state == WIFI_SCAN_RUNNING) {
      sendJson(req, "{\"busy\":1}");  // 扫描进行中，前端继续轮询
      return;
    }
    if (state == WIFI_SCAN_FAILED) {
      _wifi.startScan();              // 没有进行中的扫描 -> 启动一轮新的
      sendJson(req, "{\"busy\":1}");
      return;
    }
    // 结果已就绪，导出并释放底层缓存
    sendJson(req, "{\"busy\":0,\"nets\":" + _wifi.scanJson() + "}");
  });

  // 已保存的 WiFi 配置列表（启动时按顺序尝试）
  _server.on("/api/wifi/list", HTTP_GET, [this](AsyncWebServerRequest *req) {
    sendJson(req, _wifi.listJson());
  });

  // 保存配置：JSON {"ssid":"..","pass":"..","connect":true}
  // SSID 已存在则覆盖密码；成功后按需立即尝试连接
  _server.on("/api/wifi/save", HTTP_POST,
             [this](AsyncWebServerRequest *req, JsonVariant &json) {
               if (!json.is<JsonObject>() || !json["ssid"].is<const char *>()) {
                 sendErr(req, 400, "ssid required");
                 return;
               }
               String ssid = json["ssid"].as<String>();
               String pass = json["pass"].is<const char *>()
                                 ? json["pass"].as<String>() : String();
               bool connect = true;
               jsonBool(json["connect"], connect);  // 缺省 true
               if (!ssid.length()) {
                 sendErr(req, 400, "ssid required");
                 return;
               }
               if (!_wifi.save(ssid, pass, connect)) {
                 sendErr(req, 500, "save failed (max 5)");
                 return;
               }
               sendOk(req);
             });

  // 删除配置：JSON {"index":列表下标}
  _server.on("/api/wifi/delete", HTTP_POST,
             [this](AsyncWebServerRequest *req, JsonVariant &json) {
               int index;
               if (!json.is<JsonObject>() || !jsonInt(json["index"], index)) {
                 sendErr(req, 400, "invalid index");
                 return;
               }
               if (!_wifi.remove((uint8_t)index)) {
                 sendErr(req, 400, "invalid index");
                 return;
               }
               sendOk(req);
             });

  // 调整顺序：JSON {"from":原下标,"to":新下标}
  _server.on("/api/wifi/move", HTTP_POST,
             [this](AsyncWebServerRequest *req, JsonVariant &json) {
               int from, to;
               if (!json.is<JsonObject>() || !jsonInt(json["from"], from) ||
                   !jsonInt(json["to"], to)) {
                 sendErr(req, 400, "invalid move");
                 return;
               }
               if (from < 0 || to < 0 || !_wifi.move((uint8_t)from, (uint8_t)to)) {
                 sendErr(req, 400, "invalid move");
                 return;
               }
               sendOk(req);
             });

  // 试连（不保存）：JSON {"ssid":"..","pass":".."}
  _server.on("/api/wifi/connect", HTTP_POST,
             [this](AsyncWebServerRequest *req, JsonVariant &json) {
               if (!json.is<JsonObject>() || !json["ssid"].is<const char *>()) {
                 sendErr(req, 400, "ssid required");
                 return;
               }
               String ssid = json["ssid"].as<String>();
               String pass = json["pass"].is<const char *>()
                                 ? json["pass"].as<String>() : String();
               if (!ssid.length()) {
                 sendErr(req, 400, "ssid required");
                 return;
               }
               _wifi.requestConnect(ssid, pass);
               sendOk(req, ",\"connecting\":true");
             });

  // ================= 短信 =================
  // 路由顺序注意（同设备日志段）：子路径必须先于父路径注册，否则父路由按
  // BackwardCompatible（精确 或 前缀 + /）匹配会吞掉 /api/sms/stats、
  // /api/sms/latest 与 GET /api/sms/ports。父路由 /api/sms 未注册 POST，
  // 故 POST 子路由（ports/reinit/send）不受影响，保持原位即可。

  // 存储统计：{"ok":true,"total":n,"max":n,"used":bytes,"fsTotal":bytes}
  _server.on("/api/sms/stats", HTTP_GET, [this](AsyncWebServerRequest *req) {
    sendJson(req, _sms.store().statsJson());
  });

  // 最新一条短信摘要（仪表盘用；与 SSE sms 事件同格式），无记录返回 {}
  _server.on("/api/sms/latest", HTTP_GET, [this](AsyncWebServerRequest *req) {
    sendJson(req, _sms.latestJson());
  });

  // 模组列表：{"ok":true,"modems":[{"port":1,"enabled":true},...]}
  _server.on("/api/sms/ports", HTTP_GET, [this](AsyncWebServerRequest *req) {
    sendJson(req, String("{\"ok\":true,\"modems\":") + _sms.modemsJson() + "}");
  });

  // 列表：?page=&pageSize=&q=&port= -> {"ok":true,"total":n,"page":p,
  // "pageSize":ps,"messages":[{id,dir,phone,content,ts,modem}],...}
  _server.on("/api/sms", HTTP_GET, [this](AsyncWebServerRequest *req) {
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
    String out;
    serializeJson(doc, out);
    sendJson(req, out);
  });

  // 删除：?id=n 删单条（不存在 404）；?all=1 清空
  _server.on("/api/sms", HTTP_DELETE, [this](AsyncWebServerRequest *req) {
    if (req->hasParam("all")) {
      _sms.store().clear();
      sendOk(req);
      return;
    }
    if (!req->hasParam("id")) {
      sendErr(req, 400, "missing id or all");
      return;
    }
    uint32_t id = (uint32_t)paramInt(req, "id", 0);
    if (!_sms.store().deleteOne(id)) {
      sendErr(req, 404, "record not found");
      return;
    }
    sendOk(req);
  });

  // 配置模组：JSON {"port":1,"enabled":true,"alias":"客厅卡槽"}（enabled/alias 可缺省其一）
  _server.on("/api/sms/ports", HTTP_POST,
             [this](AsyncWebServerRequest *req, JsonVariant &json) {
               int port;
               if (!json.is<JsonObject>() || !jsonInt(json["port"], port) ||
                   port < 1 || port > cfg::kSmsMaxModems) {
                 sendErr(req, 400, "invalid port");
                 return;
               }
               bool changed = false;
               if (json["enabled"].is<bool>()) {
                 if (!_sms.setModemEnabled((uint8_t)port, json["enabled"].as<bool>())) {
                   sendErr(req, 500, "config save failed");
                   return;
                 }
                 changed = true;
               }
               if (json["alias"].is<const char *>()) {
                 if (!_sms.setModemAlias((uint8_t)port, json["alias"].as<String>())) {
                   sendErr(req, 500, "config save failed");
                   return;
                 }
                 changed = true;
               }
               if (!changed) {
                 sendErr(req, 400, "missing enabled or alias");
                 return;
               }
               sendJson(req, String("{\"ok\":true,\"modems\":") + _sms.modemsJson() + "}");
             });

  // 重新初始化模组：JSON {"port":1}（仅启用态有效；置位 needReinit 由抽数任务重跑）
  _server.on("/api/sms/reinit", HTTP_POST,
             [this](AsyncWebServerRequest *req, JsonVariant &json) {
               int port;
               if (!json.is<JsonObject>() || !jsonInt(json["port"], port) ||
                   port < 1 || port > cfg::kSmsMaxModems) {
                 sendErr(req, 400, "invalid port");
                 return;
               }
               if (!_sms.reinitPort((uint8_t)port)) {
                 sendErr(req, 400, "模组未启用或不存在");
                 return;
               }
               sendJson(req, String("{\"ok\":true,\"modems\":") + _sms.modemsJson() + "}");
             });

  // 发送短信：JSON {"port":1,"phone":"..","content":".."}
  // 成功返回 {"ok":true,"id":记录ID}；失败 {"ok":false,"error":"原因"}。
  // 模组已发出但记录未入库（userdata 异常）时仍算成功，附带 warn 提示。
  _server.on("/api/sms/send", HTTP_POST,
             [this](AsyncWebServerRequest *req, JsonVariant &json) {
               int port;
               if (!json.is<JsonObject>() || !jsonInt(json["port"], port) ||
                   !json["phone"].is<const char *>() ||
                   !json["content"].is<const char *>()) {
                 sendErr(req, 400, "invalid json");
                 return;
               }
               String phone = json["phone"].as<String>();
               String content = json["content"].as<String>();
               String err;
               bool stored = true;
               uint32_t id = _sms.send((uint8_t)port, phone, content, err, &stored);
               if (!id) {
                 _log.addf(LogLevel::kWarn, "sms", "发送失败（模组 %d -> %s）：%s",
                           port, phone.c_str(), err.c_str());
                 sendErr(req, 400, err.c_str());
                 return;
               }
               _log.addf(LogLevel::kInfo, "sms", "已发送至 %s（模组 %d，记录 #%u）",
                         phone.c_str(), port, (unsigned)id);
               if (!stored) {
                 sendJson(req,
                          "{\"ok\":true,\"stored\":false,\"warn\":"
                          "\"记录存储失败（userdata 分区异常），本条不会出现在列表中\"}");
                 return;
               }
               sendOk(req, String(",\"id\":") + id);
             });

  // ================= 邮件通知（SMTP）=================
  // 配置读取：{"ok":true,"configured":bool,"server":"..","port":n,"user":"..",
  //   "sendTo":"..","hasPass":bool,"notifyTypes":n,"onlineH":n}
  // SMTP 授权码不回显（hasPass 表示是否已保存），避免泄露
  _server.on("/api/email", HTTP_GET, [this](AsyncWebServerRequest *req) {
    sendJson(req, _email.configJson());
  });

  // 路由顺序注意（同设备日志段）：POST 子路径必须先于 POST /api/email 注册，
  // 否则父路由按 BackwardCompatible（精确 或 前缀 + /）匹配会吞掉下面两条；
  // 被吞时「总开关/测试发送」会走到配置保存分支。GET /api/email 无此问题
  // （子路径均为 POST，方法不匹配）。

  // 总开关：JSON {"enabled":true|false} —— 只翻转启用位，不动其余配置
  // （停用时丢弃待发队列，已保存的 SMTP 参数与通知类型保留）
  _server.on("/api/email/enabled", HTTP_POST,
             [this](AsyncWebServerRequest *req, JsonVariant &json) {
               bool on = true;
               if (!json.is<JsonObject>() || !jsonBool(json["enabled"], on)) {
                 sendErr(req, 400, "invalid json");
                 return;
               }
               if (!_email.setEnabled(on)) {
                 sendErr(req, 500, "config save failed");
                 return;
               }
               sendJson(req, _email.configJson());
             });

  // 测试发送：立即同步发一封测试邮件（单次尝试，最长阻塞约 15s）。
  // 用户主动触发、频率极低，可接受；队列中的通知邮件不受影响。
  _server.on("/api/email/test", HTTP_POST, [this](AsyncWebServerRequest *req) {
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
    if (!_email.sendTest()) {
      sendErr(req, 500, "发送失败，请检查 SMTP 配置与网络");
      return;
    }
    sendOk(req);
  });

  // 配置保存：JSON {"server":"..","port":465,"user":"..","pass":"..","sendTo":"..",
  //   "enabled":true,"notifyTypes":n,"schedMode":0|1,"schedHour":n,"schedMinute":n,
  //   "intervalH":n}
  // pass 为空串表示保持已保存的授权码不变（表单不回显）；enabled 为 SMTP
  // 邮件通知总开关（缺省 true，兼容不带该字段的旧客户端）；notifyTypes 为位掩码
  // （bit0=设备启动 bit1=SIM状态 bit2=收到短信 bit3=定时在线）；
  // 定时在线通知排程两种模式：schedMode=1 固定间隔（intervalH 小时）/
  // schedMode=0 每日定点（schedHour:schedMinute，依赖时间同步）。
  _server.on("/api/email", HTTP_POST,
             [this](AsyncWebServerRequest *req, JsonVariant &json) {
               if (!json.is<JsonObject>()) {
                 sendErr(req, 400, "invalid json");
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
                 sendErr(req, 500, "config save failed");
                 return;
               }
               sendJson(req, _email.configJson());
             });

  // 未匹配路径统一返回 404（JSON，与全站一致）
  _server.onNotFound([](AsyncWebServerRequest *req) {
    req->send(404, "application/json", "{\"ok\":false,\"error\":\"not found\"}");
  });
}
