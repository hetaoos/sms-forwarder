#include "app/App.h"

#include <Arduino.h>

#include "app/Config.h"
#include "app/UserFs.h"

// 初始化调试串口与所有子模块；返回 true 表示装配完成
bool App::begin() {
  Serial.begin(115200);

  // 挂载用户数据分区；formatOnFail=true，首次或文件系统损坏时自动格式化。
  // 该分区刷固件后仍保留，短信等业务数据持久化在这里（路径以 /data 开头）
  userfs::begin();

  // 设备日志：PSRAM 环形缓冲，先于其余模块就绪，后续启动日志均可入库。
  // 时间戳供给与短信记录一致（已同步=纪元秒，未同步=开机秒数）
  _log.begin(cfg::kLogRingSize);
  _log.setTimeProvider([this] { return _time.epochOrUptime(); });

  // 时间同步：先恢复 NVS 里的上次已知时间（近似值），随后由 WiFi 连接（NTP）
  // 与模组就绪（SIM NITZ）事件驱动校准
  _time.begin(_bridge, _sms);

  _led.begin(0);                       // RGB 灯默认关闭（索引 0 = OFF）
  _display.begin();                    // 屏幕 + 静态框架
  _bridge.begin();                     // 启动串口抽数任务
  // 短信 URC 消费钩子：抽数任务归档 +CMT 等模组自发数据前交给短信服务
  _bridge.setUnsolicitedHandler([this](uint8_t port, const String &text) {
    return _sms.onUnsolicited(port, text);
  });

  _display.render(collectStatus());    // 先出一帧，避免开机黑屏
  // WiFi 连上（含重连）即启动 NTP 同步；须在 _wifi.begin() 前注册
  _wifi.setOnConnectCb([this] {
    _log.addf(LogLevel::kInfo, "wifi", "已连接 %s，IP %s", _wifi.ssid().c_str(),
              _wifi.ip().c_str());
    _time.syncNTP();
  });
  _wifi.begin();                       // 读 NVS 配置并启动连接状态机（非阻塞）
  _auth.begin();                       // 读 NVS 管理账号（无记录则写默认值）
  _email.begin(_wifi, _sms, _time);    // 邮件：载入 SMTP 配置（NVS）
  _mqtt.begin(_wifi, _sms, _time, _log);  // MQTT：载入配置（NVS）+ 注册日志 sink
  _mqtt.notifyStartup();               // 开机后首次连上 MQTT 时补发启动通知
  _sms.begin();                        // 短信：载配置 + 模组 PDU 模式初始化
  // 模组就绪事件：NTP 未成功时请求一次 SIM NITZ 时间同步（回调仅置标记，
  // 真正的 AT+CCLK? 在 TimeSync::tick（loop 任务）执行，不阻塞抽数任务）
  _sms.setOnModemReadyCb([this](uint8_t port) {
    _log.addf(LogLevel::kInfo, "sms", "模组 %u 初始化完成", (unsigned)port);
    _time.requestSimSync(port);
  });
  // 短信时间戳供给：已同步返回纪元秒，未同步返回开机秒数
  // （前端按 >=1.6e9 区分「真实时间 / 开机时长」，无需改动）
  _sms.setTimeProvider([this] { return _time.epochOrUptime(); });
  // 短信接收 -> 邮件通知入队（回调仅入队，SMTP 会话在 App::tick 执行，
  // 不阻塞串口抽数任务）+ 设备日志入库
  _sms.setOnSmsReceivedCb([this](const String &sender, const String &text,
                                 uint32_t ts, int8_t modem) {
    _email.notifySms(sender, text, ts, modem);
    _mqtt.notifySms(sender, text, ts, modem);
    _log.addf(LogLevel::kInfo, "sms", "收到短信：%s（模组 %d）", sender.c_str(),
              (int)modem);
  });
  // SIM 运行时插拔 -> 邮件通知入队 + 设备日志入库
  _sms.setOnSimChangeCb([this](uint8_t port, bool ready) {
    _email.notifySim(port, ready);
    _mqtt.notifySim(port, ready);
    _log.addf(LogLevel::kInfo, "sim", "模组 %u SIM %s", (unsigned)port,
              ready ? "已插入" : "已拔出");
  });
  // OTA 升级状态初始化（需在 HTTP 路由注册前完成，确保 /api/ota/status 立即可用）
  Ota::init();
  _panel.begin();                      // 启动 HTTP 服务
  // 启动邮件的放行条件：除 WiFi 外还要等模组初始化出结果（就绪或故障），
  // 否则邮件里的模组状态会是「初始化中」。须在 notifyStartup() 前注入
  _email.setStartupGate([this] { return _sms.modemsSettled(); });
  _email.notifyStartup();              // 排入启动邮件（等 WiFi + 模组就绪后发出）

  _log.addf(LogLevel::kInfo, "app",
            "启动完成：heap %u B 空闲，PSRAM %u/%u B，进入主循环",
            (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getFreePsram(),
            (unsigned)ESP.getPsramSize());
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
  s.psramSize = ESP.getPsramSize();   // 0 表示板子无 PSRAM
  s.freePsram = ESP.getFreePsram();
  s.ledName = _led.color().name;
  s.rssi = s.wifiUp ? _wifi.rssi() : 0;

  static String sIp;         // 需要持有字符串，供 const char* 引用
  static String sCmd, sResp, sTime;
  sIp = _wifi.ip();
  s.ip = sIp.c_str();
  sTime = _time.clockStr();  // LCD 标题栏时钟（未同步显示 --:--:--）
  s.time = sTime.c_str();

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
// 2. SSE 状态推送（diff 比对，无客户端时几乎零开销）
// 3. 短信长报文重组超时清理
// 4. 邮件通知：发送队列 / 定时在线通知 / 启动邮件（SMTP 会话同步阻塞，
//    放在屏幕刷新前，保证一轮邮件发送完成后界面立即跟上）
// 5. 每 kUiRefreshMs 毫秒重绘一次屏幕（内容变化才会实际刷新）
// 6. 短暂延时让出 CPU
void App::tick() {
  _wifi.tick();
  _time.tick();                        // NTP 超时判定 / SIM NITZ 重试 / 时间落盘
  _events.tick();
  _sms.tick();
  _email.tick();                       // 邮件通知发送队列（无待发时零开销）
  _mqtt.tick();                        // MQTT 连接维护 / 命令执行 / 事件发布

  if (millis() - _lastUiMs >= cfg::kUiRefreshMs) {
    _lastUiMs = millis();
    _display.render(collectStatus());
  }
  delay(5);
}
