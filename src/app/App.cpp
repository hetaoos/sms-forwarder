#include "app/App.h"

#include <Arduino.h>
#include <WiFi.h>

#include "app/Config.h"
#include "app/Coredump.h"  // 崩溃转储信息：panic 记录 + coredump 分区查询（工具箱导出用）
#include "app/KeyInject.h"   // 远程按键注入队列：把 Web /api/key 事件交给 UI 状态机
#include "app/SystemService.h"  // 定时重启（系统保活）
#include "app/TaskWatchdog.h"   // 任务看门狗：读回卡死原因 / 重配置 / 注册任务
#include "app/UserFs.h"
#include "hal/CpuMonitor.h"  // CPU 使用率（FreeRTOS 运行时间统计）
#include "ota/Ota.h"         // 工具箱：检查新版本并更新（Ota 状态机）
#include "ui/Ui.h"           // LVGL 页面层（Display 只负责面板与 LVGL 端口）
#include "utils/tls_mem.h"   // TLS 内存重定向到 PSRAM（须在任何 mbedTLS 分配前生效）

#include <Preferences.h>
#include <esp_system.h>          // esp_restart()
#include <freertos/FreeRTOS.h>   // xTaskCreate / vTaskDelay
#include <freertos/task.h>

// 初始化调试串口与所有子通信模组；返回 true 表示装配完成
bool App::begin() {
  Serial.begin(115200);

  // TLS 内存重定向到 PSRAM（大块缓冲）必须在任何 mbedTLS 分配之前生效，
  // 以支撑「最多 4 个并发 TLS 上下文」而不撑爆内部 DRAM。无 PSRAM 时内部自动跳过。
  tlsMemInit();

  // 挂载用户数据分区；formatOnFail=true，首次或文件系统损坏时自动格式化。
  // 该分区刷固件后仍保留，短信等业务数据持久化在这里（路径以 /data 开头）
  userfs::begin();

  // 设备日志：PSRAM 环形缓冲，先于其余通信模组就绪，后续启动日志均可入库。
  // 时间戳供给与短信记录一致（已同步=纪元秒，未同步=开机秒数）
  _log.begin(cfg::kLogRingSize);
  _log.setTimeProvider([this] { return _time.epochOrUptime(); });

  // 时间同步：先恢复 NVS 里的上次已知时间（近似值），随后由 WiFi 连接（NTP）
  // 与通信模组就绪（SIM NITZ）事件驱动校准
  _time.begin(_bridge, _sms);

  // 崩溃转储信息：panic 重启时把 RTC 保留的最近时间戳与当前固件版本写入 NVS，
  // 供 Web「工具箱」展示与导出。须在 _time.begin() 之后（NVS lastts 的兜底
  // 读取以其恢复的时间为准），分区快照由 ESP-IDF panic 处理器自动写入
  Coredump::init();

  // 任务看门狗：须在日志环就绪之后、任何常驻任务创建之前完成——读回上次
  // WDT 卡死原因（RTC 证据 + NVS）、把 TWDT 超时重配到能覆盖合法长阻塞的
  // 宽度（默认 5s 会误杀 SMTP/CMGS），并把当前 loopTask 订阅进去。
  // 其余常驻任务（uart-pump / sms_send / sms_query）在各自入口自行注册。
  TaskWatchdog::begin();

  _led.begin(0);                       // RGB 灯默认关闭（索引 0 = OFF）
  _display.begin();                    // 面板 + LVGL 显示端口
  ui::instance().begin();              // LVGL 页面树（状态页 / 菜单 / 屏幕显示）
  // setup 阶段 loopTask 已订阅 TWDT 却还没进 App::tick()，长初始化之间补一次心跳，
  // 避免中文字库加载 / 用户数据扫描等步骤把 60s 超时耗光导致开机即重启。
  TaskWatchdog::feed();
  _keypad.begin();                     // 4 键输入（OK/UP/DOWN/BACK）
  _ui.begin();                         // 菜单导航状态机（默认停在状态页）
  _ui.setStatusProvider([this](StatusInfo &s) { collectStatus(s); });  // 动态页数据源
  // LCD「短信」页数据源：只在菜单页可见时被调用（内部另有 3 秒节流）
  _ui.setSmsListProvider([this](SmsPreview *out, int max, int &total) {
    return smsPreview(out, max, total);
  });
  // RGB 灯控制（菜单「RGB 灯」页切换颜色 / 调亮度）
  _ui.setRgbControl([this](uint8_t cmd) {
    if (cmd == 0) _led.next();
    else if (cmd == 1) _led.setBrightness((uint8_t)(_led.brightness() + 10));
    else if (cmd == 2) _led.setBrightness((uint8_t)(_led.brightness() - 10));
  });
  // LCD「工具箱」页动作（检查新版本并更新 / 重启 / 恢复出厂）。
  // 回调只负责「发起」：耗时动作在 Ota 后台任务或延迟重启任务里执行，不阻塞主循环。
  // 恢复出厂与 Web「系统维护」的出厂复位同语义：清空 NVS 配置 + 格式化用户数据分区
  // ENABLE_FIRMWARE_UPGRADE=0 时不注册 OTA 动作（LCD 工具箱点了也不会误触发）
#if ENABLE_FIRMWARE_UPGRADE
  _ui.setOtaCheckFn([]() { Ota::startVersionCheck(); });
  _ui.setOtaUpgradeFn([](const String &tag) { Ota::startOnlineUpgrade(tag); });
#endif
  _ui.setRestartFn([]() {
    xTaskCreate(
        [](void *) {
          vTaskDelay(pdMS_TO_TICKS(2000));
          esp_restart();
          vTaskDelete(nullptr);
        },
        "ui_rst", 2048, nullptr, 5, nullptr);
  });
  _ui.setResetFn([]() {
    Preferences prefs;
    if (prefs.begin(cfg::kNvsNamespace, false)) {
      prefs.clear();   // 移除 appcfg 命名空间下所有键（WiFi/账号/短信/邮件/MQTT/推送/黑名单/屏幕）
      prefs.end();
    }
    userfs::format();  // 清空 /data 用户数据（短信记录等），随后重启回到首次启动状态
    xTaskCreate(
        [](void *) {
          vTaskDelay(pdMS_TO_TICKS(2000));
          esp_restart();
          vTaskDelete(nullptr);
        },
        "ui_fac", 2048, nullptr, 5, nullptr);
  });
  // Web 账号：LCD 查看用户名 / 重置登录密码为随机密码
  _ui.setAccountUserFn([this]() { return _panel.webUser(); });
  _ui.setAccountResetFn([this]() { return _panel.resetWebPassword(); });
  _bridge.begin();                     // 启动串口抽数任务
  // AT 历史带上时间：已同步用纪元秒，未同步退回开机秒数（与短信/运行日志一致）
  _bridge.setTimeProvider([this] { return _time.epochOrUptime(); });
  // 短信 URC 消费钩子：抽数任务归档 +CMT 等通信模组自发数据前交给短信服务
  _bridge.setUnsolicitedHandler([this](uint8_t port, const String &text) {
    return _sms.onUnsolicited(port, text);
  });
  // 开机先出 Logo 页（纯图标 + ASCII，不取任何业务数据），避免初始化期黑屏；
  // 展示时长由 tick 里的开机窗口控制，期间不采集状态，彻底避开「通信模组还没 begin
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
  _sms.begin();                        // 短信：载配置 + 通信模组 PDU 模式初始化
  // 持久化发件箱：扫描 /data/outbox 恢复未送达通道（须在转发服务与 _sms.begin 之后，
  // 发出类记录要靠 _sms.enqueueSend 交给 sms_send worker 执行）
  _outbox.begin(_email, _mqtt, _push, _sms, _wifi, _log);
  TaskWatchdog::feed();               // 发件箱目录扫描后补一次心跳（同上）
  _sms.setBlackList(&_blacklist);     // 注入黑名单拦截器（handleCmt 内先于落盘/转发判定）
  SystemService::instance().begin();  // 定时重启：载入 NVS 配置（默认禁用）
  // 至此邮件 / MQTT / 推送 / 短信都已持有各自的互斥量，状态快照可以放心读它们了
  _svcReady = true;
  // 通信模组就绪事件：NTP 未成功时请求一次 SIM NITZ 时间同步（回调仅置标记，
  // 真正的 AT+CCLK? 在 TimeSync::tick（loop 任务）执行，不阻塞抽数任务）
  _sms.setOnModemReadyCb([this](uint8_t port) {
    _log.addf(LogLevel::kInfo, "sms", "通信模组 %u 初始化完成", (unsigned)port);
    _time.requestSimSync(port);
  });
  // 短信时间戳供给：已同步返回纪元秒，未同步返回开机秒数
  // （前端按 >=1.6e9 区分「真实时间 / 开机时长」，无需改动）
  _sms.setTimeProvider([this] { return _time.epochOrUptime(); });
  // 短信接收 -> 持久化发件箱登记（回调仅做浅入队，落盘与投递在 App::tick，
  // 不阻塞串口抽数任务）。用户数据分区不可用时回退到原有即时转发队列，
  // 保证转发功能本身不因发件箱不可用而中断。
  _sms.setOnSmsReceivedCb([this](const String &sender, const String &text,
                                 uint32_t ts, int8_t modem) {
    if (_outbox.mounted()) {
      _outbox.enqueueSms(sender, text, ts, modem);
    } else {
      _email.notifySms(sender, text, ts, modem);
      _mqtt.notifySms(sender, text, ts, modem);
      _push.notifySms(sender, text, ts, modem);
    }
    _log.addf(LogLevel::kInfo, "sms", "收到短信：%s（通信模组 %d）", sender.c_str(),
              (int)modem);
  });
  // SIM 运行时插拔 -> 邮件通知入队 + 设备日志入库
  _sms.setOnSimChangeCb([this](uint8_t port, bool ready) {
    _email.notifySim(port, ready);
    _mqtt.notifySim(port, ready);
    _log.addf(LogLevel::kInfo, "sim", "通信模组 %u SIM %s", (unsigned)port,
              ready ? "已插入" : "已拔出");
  });
  // 来电（+CLIP/+CLCC 检测）-> 邮件 / MQTT / webhook 推送入队 + 设备日志入库。
  // 回调运行于 tick 主循环（SmsService 派发点），各 notify 均为浅入队，安全。
  _sms.setOnCallReceivedCb([this](const String &number, uint32_t ts, int8_t modem) {
    _email.notifyCall(number, ts, modem);
    _mqtt.notifyCall(number, ts, modem);
    _push.notifyCall(number, ts, modem);
  });
  _push.setSms(&_sms);  // SMS 备份推送通道的发送入口（无注入时该通道判失败）
  // 管理员 PING 命令的系统状态摘要：复用 collectStatus（与 LCD/Web 同源），
  // 运行于 tick 主循环（pollAdminCmd），与状态页刷新同上下文，安全。
  _sms.setSystemStatusFn([this]() -> String {
    StatusInfo s;
    collectStatus(s);
    String out;
    out += s.wifiUp ? "在线(STA)" : (s.apMode ? "AP配网模式" : "WiFi未连接");
    out += " RSSI " + String(s.rssi) + "dBm\n";
    out += "IP: " + String(s.ip) + "\n";
    out += "运行: " + String(s.uptime) + "  内存: " + String(s.mem) + "\n";
    out += "模组1: " + String(s.modem[0]) + "\n";
    out += "模组2: " + String(s.modem[1]) + "\n";
    out += "MQTT: " + String(s.mqtt) + "  邮件: " + String(s.mail) +
           "  推送: " + String(s.push) + "\n";
    out += "时间: " + String(s.time) + " (" + String(s.timeSrc) + ")\n";
    out += "固件: " + Ota::version();
    return out;
  });
  // OTA 升级状态初始化（需在 HTTP 路由注册前完成，确保 /api/ota/status 立即可用）
  Ota::init();
  _panel.begin();                      // 启动 HTTP 服务
  // 启动邮件的放行条件：除 WiFi 外还要等通信模组初始化出结果（就绪或故障），
  // 否则邮件里的通信模组状态会是「初始化中」。须在 notifyStartup() 前注入
  _email.setStartupGate([this] { return _sms.modemsSettled(); });
  _email.notifyStartup();              // 排入启动邮件（等 WiFi + 通信模组就绪后发出）

  // 新短信弹窗的开机基线：此前存的短信不弹（只弹运行期新收到的）
  _lastSmsId = _sms.lastSmsId();
  _smsBaseReady = true;

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
  // AP 配网热点信息（无论当前是否处于 AP 模式都在 LCD WIFI 页展示）
  out.apSsid = cfg::kApSsid;
  if (_wifi.apSecure()) sPass = _wifi.apPass();       // 加密：展示实际密码
  else sPass = "开放无密码";                           // 开放：可读文本
  out.apPass = sPass.c_str();

  // ---- 现场拼装：本机信息 / 资源 / 通信模组 / 服务 ----
  WiFi.macAddress().toCharArray(out.mac, sizeof(out.mac));
  fmtUptime(out.uptimeSec, out.uptime, sizeof(out.uptime));

  // 内存：内部堆剩余 / PSRAM 剩余（无 PSRAM 只显示前者）
  if (out.psramSize) {
    snprintf(out.mem, sizeof(out.mem), "%s / %s", fmtBytes(out.freeHeap),
             fmtBytes(out.freePsram));
  } else {
    snprintf(out.mem, sizeof(out.mem), "%s", fmtBytes(out.freeHeap));
  }

  // ---- SOC / 时间 ----
  // 芯片型号 + 步进（同时作为硬件版本展示）
  {
    String chip = ESP.getChipModel();
    if (chip.length()) chip += " rev " + String((int)ESP.getChipRevision());
    snprintf(out.chip, sizeof(out.chip), "%s", chip.length() ? chip.c_str() : "-");
  }
  snprintf(out.cpuFreq, sizeof(out.cpuFreq), "%uMHz", (unsigned)ESP.getCpuFreqMHz());
  snprintf(out.cores, sizeof(out.cores), "%u", (unsigned)ESP.getChipCores());
  snprintf(out.flash, sizeof(out.flash), "%s", fmtBytes(ESP.getFlashChipSize()));

  // 负载：CPU 使用率（不可用时用 -）与芯片温度
  const float cpu = CpuMonitor::usage();
  const float t = temperatureRead();
  if (cpu >= 0) snprintf(out.cpu, sizeof(out.cpu), "%.1f%%", cpu);
  else snprintf(out.cpu, sizeof(out.cpu), "-");
  if (!isnan(t)) snprintf(out.temp, sizeof(out.temp), "%.1f°C", t);
  else snprintf(out.temp, sizeof(out.temp), "-");
  snprintf(out.load, sizeof(out.load), "%s %s", out.cpu, out.temp);

  snprintf(out.heapSize, sizeof(out.heapSize), "%s", fmtBytes(ESP.getHeapSize()));
  snprintf(out.psramTotal, sizeof(out.psramTotal), "%s",
           out.psramSize ? fmtBytes(out.psramSize) : "-");

  out.timeSynced = _time.isSynced();
  snprintf(out.timeSrc, sizeof(out.timeSrc), "%s", _time.srcName());
  String dt = _time.dateStr();   // "YYYY-MM-DD HH:mm:ss" / "未知"
  snprintf(out.dateTime, sizeof(out.dateTime), "%s", dt.length() ? dt.c_str() : "-");

  // ---- WiFi 补充（菜单「WIFI」页）----
  if (out.wifiUp) snprintf(out.wifiMode, sizeof(out.wifiMode), "STA 已连接");
  else if (out.apMode) snprintf(out.wifiMode, sizeof(out.wifiMode), "AP 配网");
  else snprintf(out.wifiMode, sizeof(out.wifiMode), "连接中");
  out.wifiSaved = _wifi.savedCount();

  // ---- RGB 灯（菜单「RGB 灯」页）----
  snprintf(out.rgbName, sizeof(out.rgbName), "%s",
           out.ledName ? out.ledName : "-");
  out.rgbBright = _led.brightness();
  out.rgbIdx = _led.index();

  if (_svcReady) {
    for (uint8_t i = 0; i < 2; i++) {
      String b = _sms.modemBrief((uint8_t)(i + 1));   // 模组号 1/2
      snprintf(out.modem[i], sizeof(out.modem[i]), "%s",
               b.length() ? b.c_str() : "-");
      // 完整遥测快照（菜单「通信模组」页）
      _sms.modemSnapshot((uint8_t)(i + 1), out.modems[i]);
    }

    // ---- 邮件通知 ----
    out.mailOn = _email.enabled();
    out.mailCfg = _email.isConfigured();
    snprintf(out.mailServer, sizeof(out.mailServer), "%s", _email.server().c_str());
    out.mailPort = _email.port();
    snprintf(out.mailUser, sizeof(out.mailUser), "%s", _email.user().c_str());
    snprintf(out.mailTo, sizeof(out.mailTo), "%s", _email.sendTo().c_str());
    out.mailQueue = _email.queueCount();
    snprintf(out.mailSched, sizeof(out.mailSched), "%s", _email.scheduleText().c_str());
    out.mailTypes = _email.notifyTypes();

    // ---- 推送通道 ----
    out.pushOn = _push.enabled();
    out.pushCh = (uint8_t)_push.validChannelCount();
    snprintf(out.pushStrategy, sizeof(out.pushStrategy), "%s",
             _push.strategy() == PUSH_STRATEGY_FAILOVER ? "故障转移" : "广播");
    {
      PushService::ChannelBrief cb[4];
      const int cn = _push.channelBriefs(cb, 4);
      out.pushListCount = (uint8_t)(cn > 0 ? cn : 0);
      for (int i = 0; i < cn; i++) {
        snprintf(out.pushList[i].label, sizeof(out.pushList[i].label), "%s", cb[i].label);
        out.pushList[i].on = cb[i].on;
        out.pushList[i].valid = cb[i].valid;
      }
    }

#if ENABLE_MQTT
    out.mqttOn = MqttService::isEnabled();
    out.mqttCfg = MqttService::isConfigured();
    out.mqttConn = MqttService::connected();
    out.mqttSub = MqttService::_subscribeEnabled;
    {
      static String sMqttSrv;
      sMqttSrv = MqttService::scheme() + "://" + MqttService::_server;
      const char *src = sMqttSrv.length() ? sMqttSrv.c_str() : "-";
      strncpy(out.mqttServer, src, sizeof(out.mqttServer) - 1);
      out.mqttServer[sizeof(out.mqttServer) - 1] = '\0';
    }
    snprintf(out.mqttClient, sizeof(out.mqttClient), "%s",
             MqttService::deviceId().c_str());
    snprintf(out.mqttTopic, sizeof(out.mqttTopic), "%s/#",
             MqttService::_topicPrefix.c_str());
    {
      const String &err = MqttService::lastError();
      snprintf(out.mqttErr, sizeof(out.mqttErr), "%s",
               err.length() ? err.c_str() : "-");
    }

    if (!out.mqttOn) snprintf(out.mqtt, sizeof(out.mqtt), "已关闭");
    else if (!out.mqttCfg) snprintf(out.mqtt, sizeof(out.mqtt), "未配置");
    else snprintf(out.mqtt, sizeof(out.mqtt), "%s", out.mqttConn ? "已连接" : "未连接");
#else
    snprintf(out.mqtt, sizeof(out.mqtt), "未编译");
#endif

    if (!out.mailOn) snprintf(out.mail, sizeof(out.mail), "已关闭");
    else snprintf(out.mail, sizeof(out.mail), "%s", out.mailCfg ? "已配置" : "未配置");

    if (!out.pushOn) {
      snprintf(out.push, sizeof(out.push), "已关闭");
    } else {
      snprintf(out.push, sizeof(out.push), out.pushCh > 0 ? "%d 个通道" : "未配置",
               (int)out.pushCh);
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

// LCD「短信」页数据源：取最近 max 条（时间倒序），total 为存储总条数。
// 扫文件（每条一次 open/read）有成本，菜单页每 500ms 就会问一次，
// 因此这里做 3 秒节流：窗口内直接回缓存。
int App::smsPreview(SmsPreview *out, int max, int &total) {
  const uint32_t now = millis();
  if (now - _smsCacheMs >= 3000 || _smsCacheMs == 0) {
    SmsList list;
    const int n = max > 0 ? _sms.store().latest(max, list) : 0;
    for (int i = 0; i < n && i < (int)cfg::kUiSmsPreviewMax; i++) {
      fillPreview(list[i], _smsCache[i]);
    }
    _smsCacheCount = n;
    _smsCacheTotal = _sms.store().count();
    _smsCacheMs = now;
  }
  total = _smsCacheTotal;
  if (max > _smsCacheCount) max = _smsCacheCount;
  for (int i = 0; i < max; i++) out[i] = _smsCache[i];
  return max > 0 ? max : 0;
}

// 单条记录 -> 预览条目。时间沿用前端约定：>= 1.6e9 为真实纪元时间，
// 否则是开机秒数（格式化成时长）。
void App::fillPreview(const SmsMessage &m, SmsPreview &out) {
  out.id = m.id;
  out.incoming = m.incoming;
  out.modem = m.modem;
  out.ts = m.ts;
  snprintf(out.phone, sizeof(out.phone), "%s", m.phone.c_str());
  snprintf(out.text, sizeof(out.text), "%s", m.content.c_str());
  // 通信模组别名（备注）：按串口号查快照，仅展示用，无别名则留空
  out.alias[0] = 0;
  if (m.modem >= 1 && m.modem <= (int)cfg::kSmsMaxModems) {
    ModemSnapshot ms;
    if (_sms.modemSnapshot((uint8_t)m.modem, ms) && ms.alias[0]) {
      snprintf(out.alias, sizeof(out.alias), "%s", ms.alias);
    }
  }
  if (m.ts >= 1600000000u) {
    time_t tv = (time_t)m.ts;
    struct tm ti;
    localtime_r(&tv, &ti);
    snprintf(out.time, sizeof(out.time), "%02d-%02d %02d:%02d", ti.tm_mon + 1,
             ti.tm_mday, ti.tm_hour, ti.tm_min);
  } else {
    const uint32_t s = m.ts;
    snprintf(out.time, sizeof(out.time), "%02u:%02u:%02u", (unsigned)(s / 3600),
             (unsigned)((s % 3600) / 60), (unsigned)(s % 60));
  }
}

// 新短信检测：SmsService 落盘后 lastSmsId 会推进，主循环据此弹窗。
// 放在 loop 任务里做（而不是短信回调里）：弹窗要点屏、要拷贝正文，
// 这些都不该发生在串口抽数任务上下文。
void App::checkNewSms() {
  const uint32_t id = _sms.lastSmsId();
  if (id == _lastSmsId) return;
  const bool hadBase = _smsBaseReady;
  _lastSmsId = id;
  if (!hadBase || id == 0) return;   // 开机基线：不弹历史短信

  SmsMessage m;
  if (!_sms.store().detail(id, m)) return;
  if (!m.incoming) return;           // 只弹「收到」的短信（发出不算新短信）

  SmsPreview p;
  fillPreview(m, p);
  _ui.showSms(p);
  // 顺带让「短信」页下次进入时立刻能看到这条（缓存 3 秒节流会挡住新数据）
  _smsCacheMs = 0;
  _log.addf(LogLevel::kInfo, "ui", "新短信已弹屏：%s（通信模组 %d）", m.phone.c_str(),
            (int)m.modem);
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
  TaskWatchdog::feed();  // loopTask 心跳（内部 1s 节流；长阻塞由下层等待循环续喂）
  _wifi.tick();
  _time.tick();                        // NTP 超时判定 / SIM NITZ 重试 / 时间落盘
  // 崩溃时刻估算：时钟已同步时每 10s 把最新已知时间写入 RTC 内存（panic 重启
  // 保留、断电丢失；断电场景由 TimeSync 落盘的 NVS lastts 兜底，见 Coredump::init）
  {
    static uint32_t s_lastCdMs = 0;
    if (_time.isSynced() && millis() - s_lastCdMs >= 10000) {
      s_lastCdMs = millis();
      Coredump::updateLastKnownTime(time(nullptr));
    }
  }
  SystemService::instance().tick(_time);  // 定时重启检测（到点延迟 2s 重启）
  _events.tick();
  _sms.tick();
  _email.tick();                       // 邮件通知发送队列（无待发时零开销）
  _mqtt.tick();                        // MQTT 连接维护 / 命令执行 / 事件发布
  _push.tick();                        // 推送通道队列与重试（loop 任务内执行）
  _outbox.tick();                      // 持久化发件箱：落盘新事件 + 逐通道投递/续投

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

  // 新短信检测：有新的收到短信时弹出到屏幕（loop 任务上下文）
  checkNewSms();

  // 无操作超时：由 UI 状态机按屏幕设置处理（自动退回状态页 / 自动熄屏 / 弹窗超时，
  // 三项均可关闭）；兼管菜单连调亮度后的落盘防抖
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
    // 开机窗口内不渲染其它页面，避免与初始化中的通信模组抢数据
    ui::instance().tick();
    _display.lvglTick();
    delay(5);
    return;
  }

  if (_ui.inPopup()) {
    // 新短信弹窗：优先级最高，内容固定（Ui 层按指纹跳过重复重排）
    _ui.renderPopup();
  } else if (_ui.inView()) {
    // 可滚动文本页（屏幕显示 / 短信详情）：按固定周期重绘，滚动由按键驱动
    if (millis() - _lastUiMs >= cfg::kUiRefreshMs) {
      _lastUiMs = millis();
      _ui.renderView();
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
