#include "app/App.h"

#include <Arduino.h>
#include <WiFi.h>

#include "app/Config.h"
#include "app/KeyInject.h"   // 远程按键注入队列：把 Web /api/key 事件交给 UI 状态机
#include "app/UserFs.h"
#include "hal/CpuMonitor.h"  // CPU 使用率（FreeRTOS 运行时间统计）
#include "ui/Ui.h"           // LVGL 页面层（Display 只负责面板与 LVGL 端口）

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
  _display.begin();                    // 面板 + LVGL 显示端口
  ui::instance().begin();              // LVGL 页面树（状态页 / 菜单 / 屏幕显示）
  _keypad.begin();                     // 4 键输入（OK/UP/DOWN/BACK）
  _ui.begin();                         // 菜单导航状态机（默认停在状态页）
  _ui.setStatusProvider([this](StatusInfo &s) { collectStatus(s); });  // 屏幕显示页数据源
  _bridge.begin();                     // 启动串口抽数任务
  // 短信 URC 消费钩子：抽数任务归档 +CMT 等模组自发数据前交给短信服务
  _bridge.setUnsolicitedHandler([this](uint8_t port, const String &text) {
    return _sms.onUnsolicited(port, text);
  });
  // 开机先出 Logo 页（纯图标 + ASCII，不取任何业务数据），避免初始化期黑屏；
  // 展示时长由 tick 里的开机窗口控制，期间不采集状态，彻底避开「模块还没 begin
  // 就被取数」这一类冲突（互斥量句柄为 NULL 会直接断言重启）。
  ui::instance().bootUpdate(0);

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
  _push.begin();                       // 推送通道：载入 NVS 配置 + 初始化队列
  _mqtt.notifyStartup();               // 开机后首次连上 MQTT 时补发启动通知
  _blacklist.begin();                  // 黑名单：载入 NVS 配置（默认总开关关）
  _sms.begin();                        // 短信：载配置 + 模组 PDU 模式初始化
  _sms.setBlackList(&_blacklist);     // 注入黑名单拦截器（handleCmt 内先于落盘/转发判定）
  // 至此邮件 / MQTT / 推送 / 短信都已持有各自的互斥量，状态快照可以放心读它们了
  _svcReady = true;
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
    _push.notifySms(sender, text, ts, modem);  // webhook 推送通道（广播/故障转移）
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

// 聚合屏幕渲染所需的全部状态字段，填充到 out（out 的生命周期由调用方持有）
// 注意：ip / lastCmd / lastResp 为 const char*，必须指向生命周期超出本函数的
// 存储，这里使用 static String 持有副本
void App::collectStatus(StatusInfo &out) {
  out.wifiUp = _wifi.connected();
  out.apMode = _wifi.apMode();
  out.uptimeSec = millis() / 1000;
  out.freeHeap = ESP.getFreeHeap();
  out.psramSize = ESP.getPsramSize();   // 0 表示板子无 PSRAM
  out.freePsram = ESP.getFreePsram();
  out.ledName = _led.color().name;
  out.rssi = out.wifiUp ? _wifi.rssi() : 0;

  static String sIp;         // 需要持有字符串，供 const char* 引用
  static String sCmd, sResp, sTime;
  static String sSsid, sPass;
  sIp = _wifi.ip();
  out.ip = sIp.c_str();
  sTime = _time.clockStr();  // LCD 标题栏时钟（未同步显示 --:--:--）
  out.time = sTime.c_str();

  // 网络名：STA 已连显示 SSID，AP 模式显示热点名，未连接显示配置名
  sSsid = _wifi.ssid();
  out.ssid = sSsid.length() ? sSsid.c_str() : "-";
  // AP 为开放热点（无密码），这里直接给可读文本而非空串
  sPass = out.apMode ? "开放无密码" : "-";
  out.apPass = sPass.c_str();

  // ---- 现场拼装：本机信息 / 资源 / 模组 / 服务 ----
  WiFi.macAddress().toCharArray(out.mac, sizeof(out.mac));
  fmtUptime(out.uptimeSec, out.uptime, sizeof(out.uptime));

  // 内存：内部堆剩余 / PSRAM 剩余（无 PSRAM 只显示前者）
  if (out.psramSize) {
    snprintf(out.mem, sizeof(out.mem), "%s / %s", fmtBytes(out.freeHeap),
             fmtBytes(out.freePsram));
  } else {
    snprintf(out.mem, sizeof(out.mem), "%s", fmtBytes(out.freeHeap));
  }

  // 负载：CPU 使用率（不可用时用 -）与芯片温度
  const float cpu = CpuMonitor::usage();
  const float t = temperatureRead();
  snprintf(out.load, sizeof(out.load), "%s %s",
           cpu >= 0 ? String(cpu, 1).c_str() : "-",
           isnan(t) ? "-" : (String(t, 1) + "℃").c_str());

  if (_svcReady) {
    for (uint8_t i = 0; i < 2; i++) {
      String b = _sms.modemBrief((uint8_t)(i + 1));   // 串口号 1/2
      snprintf(out.modem[i], sizeof(out.modem[i]), "%s",
               b.length() ? b.c_str() : "-");
    }

#if defined(ENABLE_MQTT) && ENABLE_MQTT
    if (!MqttService::isEnabled()) {
      snprintf(out.mqtt, sizeof(out.mqtt), "已关闭");
    } else if (!MqttService::isConfigured()) {
      snprintf(out.mqtt, sizeof(out.mqtt), "未配置");
    } else {
      snprintf(out.mqtt, sizeof(out.mqtt), "%s",
               MqttService::connected() ? "已连接" : "未连接");
    }
#else
    snprintf(out.mqtt, sizeof(out.mqtt), "未编译");
#endif

    if (!_email.enabled()) snprintf(out.mail, sizeof(out.mail), "已关闭");
    else snprintf(out.mail, sizeof(out.mail), "%s",
                  _email.isConfigured() ? "已配置" : "未配置");

    if (!_push.enabled()) {
      snprintf(out.push, sizeof(out.push), "已关闭");
    } else {
      const int n = _push.validChannelCount();
      snprintf(out.push, sizeof(out.push), n > 0 ? "%d 个通道" : "未配置", n);
    }
  }
  // 未就绪（开机首帧）时上述字段保持结构体默认的 "-"，下一帧就会补上

  uint8_t port = 0;
  if (_bridge.lastEntry(port, sCmd, sResp)) {
    out.lastPort = port;
    out.lastCmd = sCmd.c_str();
    out.lastResp = sResp.c_str();
  }
}

// 秒 -> "1天02:03:04" / "02:03:04"（超过一天加天数前缀，便于长时间运行读数）
void App::fmtUptime(uint32_t sec, char *out, size_t n) {
  const uint32_t d = sec / 86400;
  const uint32_t h = (sec % 86400) / 3600;
  const uint32_t m = (sec % 3600) / 60;
  const uint32_t s = sec % 60;
  if (d) snprintf(out, n, "%u天%02u:%02u:%02u", (unsigned)d, (unsigned)h,
                  (unsigned)m, (unsigned)s);
  else snprintf(out, n, "%02u:%02u:%02u", (unsigned)h, (unsigned)m, (unsigned)s);
}

// 字节数紧凑格式：>=1MB 显示 "7.80M"，>=1KB 显示 "123.4K"，否则 "512B"
const char *App::fmtBytes(uint32_t bytes) {
  static char buf[4][16];   // 一次调用可能用到多个（内存行拼两个值）
  static uint8_t idx = 0;
  char *b = buf[idx];
  idx = (idx + 1) & 3;
  if (bytes >= 1024u * 1024u) snprintf(b, 16, "%.2fM", bytes / 1048576.0);
  else if (bytes >= 1024u) snprintf(b, 16, "%.1fK", bytes / 1024.0);
  else snprintf(b, 16, "%uB", (unsigned)bytes);
  return b;
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
  _push.tick();                        // 推送通道队列与重试（loop 任务内执行）

  // 轮询 4 键，事件交给菜单状态机处理（状态页下任意键切入菜单）
  KeyId key;
  KeyEvent ev;
  if (_keypad.poll(millis(), key, ev)) {
    _bootDone = true;   // 开机页期间有按键：立即结束开机页，按正常流程走
    _ui.handle(key, ev);
  }

  // 远程注入的按键（Web /api/key）与物理按键走同一处理路径
  KeyId ikey; KeyEvent iev;
  while (KeyInject::instance().pop(ikey, iev)) {
    _bootDone = true;
    _ui.handle(ikey, iev);
  }

  // 无操作超时：由 UI 状态机按屏幕设置处理（自动退回状态页 / 自动熄屏，
  // 两项均可关闭）；兼管菜单连调亮度后的落盘防抖
  _ui.tick();

  // 开机窗口：只画开机页（Logo + 进度条），不采集任何业务状态。
  // 计时从首轮 tick 起算，保证「Logo 实际在屏上停留 kBootScreenMs」——
  // 若从 begin() 起算，初始化耗时可能把这段窗口吃掉。
  if (!_bootDone) {
    if (_bootMs == 0) _bootMs = millis();
    const uint32_t el = millis() - _bootMs;
    if (el >= cfg::kBootScreenMs) {
      _bootDone = true;                 // 下一轮起进入常规渲染（状态页）
      _lastUiMs = 0;                    // 让状态页立刻出首帧，不等 500ms 节流
    } else if (el - _lastBootBarMs >= cfg::kBootBarStepMs) {
      _lastBootBarMs = el;
      ui::instance().bootUpdate((uint8_t)(el * 100 / cfg::kBootScreenMs));
    }
    // 开机窗口内不渲染其它页面，避免与初始化中的模块抢数据
    ui::instance().tick();
    _display.lvglTick();
    delay(5);
    return;
  }

  if (_ui.inScreenView()) {
    // 「屏幕显示」页：实时跟随，按固定周期重绘（滚动/刷新由按键驱动）
    if (millis() - _lastUiMs >= cfg::kUiRefreshMs) {
      _lastUiMs = millis();
      _ui.renderScreenView();
    }
  } else if (_ui.inStatusMode()) {
    // 实时状态页：按固定周期刷新（LVGL 自带脏区比对，内容没变不会推屏）
    if (millis() - _lastUiMs >= cfg::kUiRefreshMs) {
      _lastUiMs = millis();
      StatusInfo s;
      collectStatus(s);
      _ui.renderStatus(s);
    }
  } else if (_display.screenOn() && _ui.dirty()) {
    // 菜单页：由按键事件驱动，导航变化立即更新（不受 500ms 节流限制）
    _ui.render();
  }

  // LVGL 心跳：推进动画/定时器并把脏区 flush 到面板（内部自带节流）
  ui::instance().tick();
  _display.lvglTick();

  // 屏幕实时预览：主循环预编码 JPEG 缓存（自节流约 1.4fps）；HTTP 处理器只复制返回
  _display.refreshScreenJpg();

  delay(5);
}
