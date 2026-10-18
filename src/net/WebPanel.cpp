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

// req：请求对象；json：要发送的 JSON 文本
void WebPanel::sendJson(AsyncWebServerRequest *req, const String &json) {
  req->send(200, "application/json", json);
}

// 注册全部路由：
// 1. 全局 Basic Auth 中间件（仅 AP 配网模式启用）；
// 2. 精确路由（API / 首页）先注册，serveStatic 兜底其余静态资源
void WebPanel::setupRoutes() {
  // AP 配网模式下所有请求统一认证；未认证直接 401 挑战，不进入后续处理链
  _server.addMiddleware([this](AsyncWebServerRequest *req,
                               ArMiddlewareNext next) {
    if (!_wifi.apActive() ||
        req->authenticate(cfg::kWebAuthUser, cfg::kWebAuthPass)) {
      next();
      return;
    }
    req->requestAuthentication("ESP32-Setup", false);  // Basic 认证挑战
  });

  // ---- 静态页面（LittleFS）----
  // 首页：控制台单页应用
  _server.on("/", HTTP_GET, [this](AsyncWebServerRequest *req) {
    sendFile(req, "/index.html", "text/html; charset=utf-8");
  });
  // 其余静态资源（css/js/图标等）直接映射到 LittleFS 根目录；
  // serveStatic 会自动探测同名 .gz 文件
  _server.serveStatic("/", LittleFS, "/");

  // ---- RGB ----
  // 切换到下一个颜色，返回新颜色名
  _server.on("/led", HTTP_GET, [this](AsyncWebServerRequest *req) {
    _led.next();
    req->send(200, "text/plain", _led.color().name);
  });

  // 设置颜色：?c=索引 走预设，?r=&g=&b= 走任意 RGB
  _server.on("/set", HTTP_GET, [this](AsyncWebServerRequest *req) {
    if (req->hasParam("c")) {
      _led.set(paramInt(req, "c", 0));
    } else if (req->hasParam("r") && req->hasParam("g") && req->hasParam("b")) {
      _led.setRaw(paramInt(req, "r", 0), paramInt(req, "g", 0),
                  paramInt(req, "b", 0));
    }
    req->send(200, "text/plain", "ok");
  });

  // ---- 串口 AT ----
  // 发送 AT 命令：?port=1|2&baud=115200&eol=0..3&cmd=...&t=超时ms
  _server.on("/at/send", HTTP_GET, [this](AsyncWebServerRequest *req) {
    uint8_t port = paramInt(req, "port", 1);                 // 串口号，默认 1
    uint32_t baud = paramInt(req, "baud", cfg::kUartBaud);   // 波特率，默认配置值
    uint8_t eol = paramInt(req, "eol", 0);                   // 行尾符，默认 CRLF
    uint32_t timeout = constrain(paramInt(req, "t", 1000), 100, 5000);  // 超时钳制在 100..5000ms
    String cmd = stringParam(req, "cmd");                    // 命令文本

    auto result = _bridge.send(port, baud, cmd, (SerialBridge::Eol)eol, timeout);
    if (!result.ok) {
      req->send(400, "text/plain", "invalid port");
      return;
    }
    sendJson(req, _bridge.entryJson(result.id));  // 直接回该条记录 JSON
  });

  // 增量拉取历史：?after=上一页最后ID&n=条数
  _server.on("/at/hist", HTTP_GET, [this](AsyncWebServerRequest *req) {
    int32_t after = paramInt(req, "after", -1);   // -1 = 从头拉
    size_t n = (size_t)paramInt(req, "n", 20);    // 默认 20 条
    sendJson(req, _bridge.historyJson(after, n));
  });

  // 清空历史记录
  _server.on("/at/clear", HTTP_GET, [this](AsyncWebServerRequest *req) {
    _bridge.clearHistory();
    req->send(200, "text/plain", "ok");
  });

  // 串口重新初始化（手动兜底）：?port=1|2
  _server.on("/at/reset", HTTP_GET, [this](AsyncWebServerRequest *req) {
    uint8_t port = paramInt(req, "port", 0);
    if (port < 1 || port > 2) {
      req->send(400, "text/plain", "invalid port");
      return;
    }
    _bridge.reset(port);
    req->send(200, "text/plain", "reset");
  });

  // 串口运行统计 JSON
  _server.on("/uart/stats", HTTP_GET, [this](AsyncWebServerRequest *req) {
    sendJson(req, _bridge.statsJson());
  });

  // ---- 屏幕开关 ----
  // ?on=1 点亮，?on=0 熄灭
  _server.on("/disp", HTTP_GET, [this](AsyncWebServerRequest *req) {
    bool on = paramInt(req, "on", 1) != 0;
    _display.setScreenOn(on);
    req->send(200, "text/plain", _display.screenOn() ? "on" : "off");
  });

  // 查询默认波特率（供前端下拉框初始化）
  _server.on("/at/baud", HTTP_GET, [this](AsyncWebServerRequest *req) {
    req->send(200, "application/json", "{\"default\":" + String(cfg::kUartBaud) + "}");
  });

  // ---- 系统 ----
  // 汇总状态 JSON（页面为静态文件，所有状态由前端从此接口拉取渲染）
  _server.on("/status", HTTP_GET, [this](AsyncWebServerRequest *req) {
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
  _server.on("/wifi/status", HTTP_GET, [this](AsyncWebServerRequest *req) {
    sendJson(req, _wifi.statusJson());
  });

  // 扫描附近 WiFi：扫描中返回 {"busy":1}，完成后返回 {"busy":0,"nets":[...]}
  // scanComplete() 语义：RUNNING=扫描中 / FAILED=无扫描且无结果 / >=0=结果就绪
  _server.on("/wifi/scan", HTTP_GET, [this](AsyncWebServerRequest *req) {
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
  _server.on("/wifi/list", HTTP_GET, [this](AsyncWebServerRequest *req) {
    sendJson(req, _wifi.listJson());
  });

  // 保存配置：?ssid=&pass=&connect=1
  // SSID 已存在则覆盖密码；成功后按需立即尝试连接
  _server.on("/wifi/save", HTTP_POST, [this](AsyncWebServerRequest *req) {
    String ssid = stringParam(req, "ssid");
    String pass = stringParam(req, "pass");
    if (!ssid.length()) {
      req->send(400, "text/plain", "ssid required");
      return;
    }
    bool connect = paramInt(req, "connect", 1) != 0;
    if (!_wifi.save(ssid, pass, connect)) {
      req->send(500, "text/plain", "save failed (max 5)");
      return;
    }
    req->send(200, "text/plain", "ok");
  });

  // 删除配置：?index=列表下标
  _server.on("/wifi/delete", HTTP_POST, [this](AsyncWebServerRequest *req) {
    uint8_t index = (uint8_t)paramInt(req, "index", -1);
    if (!_wifi.remove(index)) {
      req->send(400, "text/plain", "invalid index");
      return;
    }
    req->send(200, "text/plain", "ok");
  });

  // 调整顺序：?from=原下标&to=新下标
  _server.on("/wifi/move", HTTP_POST, [this](AsyncWebServerRequest *req) {
    int from = paramInt(req, "from", -1);
    int to = paramInt(req, "to", -1);
    if (from < 0 || to < 0 || !_wifi.move((uint8_t)from, (uint8_t)to)) {
      req->send(400, "text/plain", "invalid move");
      return;
    }
    req->send(200, "text/plain", "ok");
  });

  // 试连（不保存）：?ssid=&pass=
  _server.on("/wifi/connect", HTTP_POST, [this](AsyncWebServerRequest *req) {
    String ssid = stringParam(req, "ssid");
    String pass = stringParam(req, "pass");
    if (!ssid.length()) {
      req->send(400, "text/plain", "ssid required");
      return;
    }
    _wifi.requestConnect(ssid, pass);
    req->send(200, "text/plain", "connecting");
  });

  // 未匹配路径统一返回 404
  _server.onNotFound([](AsyncWebServerRequest *req) {
    req->send(404, "text/plain", "404 Not Found");
  });
}
