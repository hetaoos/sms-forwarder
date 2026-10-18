#include "net/WebPanel.h"

#include <ArduinoJson.h>
#include <LittleFS.h>

#include <Arduino.h>

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
// 否则原文件；都不存在返回 404
// req：请求对象；path：LittleFS 内的路径（如 "/index.html"）；mime：MIME 类型
void sendFile(AsyncWebServerRequest *req, const char *path, const char *mime) {
  String gz = String(path) + ".gz";
  if (LittleFS.exists(gz.c_str())) {
    AsyncWebServerResponse *resp = req->beginResponse(LittleFS, gz, mime);
    resp->addHeader("Content-Encoding", "gzip");
    req->send(resp);
    return;
  }
  if (LittleFS.exists(path)) {
    req->send(LittleFS, path, mime);
    return;
  }
  req->send(404, "text/plain", "404 Not Found");
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

// 统一成功响应：{"ok":true}，可附额外字段
void sendOk(AsyncWebServerRequest *req, const char *extra = "") {
  req->send(200, "application/json", String("{\"ok\":true") + extra + "}");
}

}  // namespace

// bridge/led/wifi/display：各模块引用，保存供路由处理器使用
WebPanel::WebPanel(SerialBridge &bridge, RgbLed &led, WifiManager &wifi,
                   Display &display)
    : _bridge(bridge), _led(led), _wifi(wifi), _display(display) {}

// 挂载 LittleFS 专有分区，注册路由与认证中间件，启动 HTTP 服务
void WebPanel::begin() {
  // 挂载失败时格式化一次重试；仍失败则仅 API 可用，页面返回 404
  if (!LittleFS.begin(true)) {
    Serial.println("[http] LittleFS mount failed, web page unavailable");
  } else {
    Serial.println("[http] LittleFS mounted");
  }

  setupRoutes();
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
      if (t.length() && storeHas(_tokens, _tokenCount, t)) return true;
    }
  }
  return false;
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
// 2. 精确路由（API / 首页）先注册，serveStatic 兜底其余静态资源
void WebPanel::setupRoutes() {
  _server.addMiddleware([this](AsyncWebServerRequest *req,
                               ArMiddlewareNext next) {
    String url = req->url();
    if (url == "/api/login" || url == "/api/logout" || url == "/api/session") {
      next();  // 登录相关端点无需已认证
      return;
    }
    if (url.startsWith("/api/") && !authorized(req)) {
      req->send(401, "application/json", "{\"error\":\"unauthorized\"}");
      return;
    }
    next();
  });

  // ---- 页面与静态资源（LittleFS，公开）----
  // 首页：控制台单页应用（左侧菜单 + 右侧内容，hash 路由）
  _server.on("/", HTTP_GET, [this](AsyncWebServerRequest *req) {
    sendFile(req, "/index.html", "text/html; charset=utf-8");
  });
  // 独立登录页（已登录时页面脚本自行跳回 /）
  _server.on("/login", HTTP_GET, [this](AsyncWebServerRequest *req) {
    sendFile(req, "/login.html", "text/html; charset=utf-8");
  });
  // 其余静态资源（css/js/图标等）直接映射到 LittleFS 根目录；
  // serveStatic 会自动探测同名 .gz 文件
  _server.serveStatic("/", LittleFS, "/");

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
               if (user != cfg::kWebAuthUser || pass != cfg::kWebAuthPass) {
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
      req->send(401, "application/json", "{\"error\":\"unauthorized\"}");
      return;
    }
    sendJson(req, "{\"ok\":1,\"user\":\"" + String(cfg::kWebAuthUser) + "\"}");
  });

  // ---- RGB ----
  // 切换到下一个颜色，返回 {"color":"颜色名"}
  _server.on("/api/led", HTTP_GET, [this](AsyncWebServerRequest *req) {
    _led.next();
    sendJson(req, String("{\"color\":\"") + _led.color().name + "\"}");
  });

  // 设置颜色：?c=索引 走预设，?r=&g=&b= 走任意 RGB
  _server.on("/api/set", HTTP_GET, [this](AsyncWebServerRequest *req) {
    if (req->hasParam("c")) {
      _led.set(paramInt(req, "c", 0));
    } else if (req->hasParam("r") && req->hasParam("g") && req->hasParam("b")) {
      _led.setRaw(paramInt(req, "r", 0), paramInt(req, "g", 0),
                  paramInt(req, "b", 0));
    }
    sendOk(req);
  });

  // ---- 串口 AT ----
  // 发送 AT 命令：?port=1|2&baud=115200&eol=0..3&cmd=...&t=超时ms
  _server.on("/api/at/send", HTTP_GET, [this](AsyncWebServerRequest *req) {
    uint8_t port = paramInt(req, "port", 1);                 // 串口号，默认 1
    uint32_t baud = paramInt(req, "baud", cfg::kUartBaud);   // 波特率，默认配置值
    uint8_t eol = paramInt(req, "eol", 0);                   // 行尾符，默认 CRLF
    uint32_t timeout = constrain(paramInt(req, "t", 1000), 100, 5000);  // 超时钳制在 100..5000ms
    String cmd = stringParam(req, "cmd");                    // 命令文本

    auto result = _bridge.send(port, baud, cmd, (SerialBridge::Eol)eol, timeout);
    if (!result.ok) {
      sendErr(req, 400, "invalid port");
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

  // ---- 屏幕开关 ----
  // ?on=1 点亮，?on=0 熄灭；返回 {"on":bool}
  _server.on("/api/disp", HTTP_GET, [this](AsyncWebServerRequest *req) {
    bool on = paramInt(req, "on", 1) != 0;
    _display.setScreenOn(on);
    sendJson(req, String("{\"on\":") + (_display.screenOn() ? "true" : "false") + "}");
  });

  // 查询默认波特率（供前端下拉框初始化）
  _server.on("/api/at/baud", HTTP_GET, [this](AsyncWebServerRequest *req) {
    req->send(200, "application/json", "{\"default\":" + String(cfg::kUartBaud) + "}");
  });

  // ---- 系统 ----
  // 汇总状态 JSON（页面为静态文件，所有状态由前端从此接口拉取渲染）
  _server.on("/api/status", HTTP_GET, [this](AsyncWebServerRequest *req) {
    const RgbLed::Color &c = _led.color();
    JsonDocument doc;
    doc["ssid"] = _wifi.ssid();        // 当前网络名（AP 模式为 AP 名）
    doc["ip"] = _wifi.ip();            // 本机 IP
    doc["rssi"] = _wifi.rssi();        // 信号强度 dBm
    doc["uptime"] = millis() / 1000;   // 运行秒数
    doc["heap"] = ESP.getFreeHeap();   // 剩余堆内存
    doc["color"] = c.name;             // RGB 颜色名
    JsonArray rgb = doc["rgb"].to<JsonArray>();
    rgb.add(c.r);                      // RGB 三通道（前端画色块）
    rgb.add(c.g);
    rgb.add(c.b);
    doc["disp"] = _display.screenOn();          // 屏幕开关
    doc["connected"] = _wifi.connected();       // STA 是否已连接
    doc["mode"] = _wifi.apMode() ? "ap" : (_wifi.connected() ? "sta" : "connecting");
    String out;
    serializeJson(doc, out);
    sendJson(req, out);
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
               bool connect = json["connect"].is<bool>()
                                  ? json["connect"].as<bool>() : true;
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
               if (!json.is<JsonObject>() || !json["index"].is<int>()) {
                 sendErr(req, 400, "invalid index");
                 return;
               }
               uint8_t index = (uint8_t)json["index"].as<int>();
               if (!_wifi.remove(index)) {
                 sendErr(req, 400, "invalid index");
                 return;
               }
               sendOk(req);
             });

  // 调整顺序：JSON {"from":原下标,"to":新下标}
  _server.on("/api/wifi/move", HTTP_POST,
             [this](AsyncWebServerRequest *req, JsonVariant &json) {
               if (!json.is<JsonObject>() || !json["from"].is<int>() ||
                   !json["to"].is<int>()) {
                 sendErr(req, 400, "invalid move");
                 return;
               }
               int from = json["from"].as<int>();
               int to = json["to"].as<int>();
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

  // 未匹配路径统一返回 404（JSON，与全站一致）
  _server.onNotFound([](AsyncWebServerRequest *req) {
    req->send(404, "application/json", "{\"ok\":false,\"error\":\"not found\"}");
  });
}
