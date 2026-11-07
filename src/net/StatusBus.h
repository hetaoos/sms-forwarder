#pragma once

#include <ESPAsyncWebServer.h>

#include <Arduino.h>

#include <vector>

#include "app/Config.h"
#include "app/UIMenu.h"   // 屏幕设置（自动熄屏 / 自动返回状态页）
#include "hal/Display.h"
#include "hal/RgbLed.h"
#include "net/WifiManager.h"
#include "serial/SerialBridge.h"
#include "sms/SmsService.h"
#include "time/TimeSync.h"
#include "app/Outbox.h"     // SSE 增量推送发件箱变更（脏信号）
#include "log/LogStore.h"   // SSE 增量推送运行日志

// 状态推送总线：通过 SSE（cfg::kSsePath）向所有已连接浏览器实时广播
// 设备状态，取代前端对各状态接口的定时轮询。
//
// 设计要点：
//   1. diff 推送：tick() 每 kSsePushMs 采集各通信模组状态，与上次快照做
//      字符串比对，只发送发生变化的事件；无客户端时重置基线并跳过采集；
//   2. AT 历史增量：记录已推送的 lastHistoryId，只推新记录（断线期间的
//      记录由前端重连后的 REST 兜底拉取补齐）；
//   3. 新客户端连接时立即收到一份全量快照（onConnect）；
//   4. 客户端上限 kSseMaxClients，超出时关闭最旧连接；
//   5. 认证复用 WebPanel 注册的全局中间件（Cookie / Bearer），本类
//      不另做鉴权；事件 id 递增，为 Last-Event-ID 续传预留。
class StatusBus {
 public:
  // wifi/bridge/led/display/ui/sms/time：各通信模组引用（不持有所有权）
  // ui：取屏幕设置（自动熄屏 / 自动返回状态页）随屏幕状态一并推送
  StatusBus(WifiManager &wifi, SerialBridge &bridge, RgbLed &led, Display &display,
            UIMenu &ui, SmsService &sms, TimeSync &time, LogStore &log, Outbox &outbox);

  // 把 SSE handler 注册到服务器（由 WebPanel::begin 调用，需在 server.begin 前）
  void begin(AsyncWebServer &server);

  // 周期调用（App::tick）：采集、比对、推送
  void tick();

 private:
  // 新客户端接入：超限先关最旧连接，然后向全部客户端推一份全量快照
  void onClientConnect(AsyncEventSourceClient *client);

  // 客户端断开：从维护的客户端列表中移除
  void onClientDisconnect(AsyncEventSourceClient *client);

  // 向全部客户端发送一个事件（id 自动递增）
  void publish(const char *event, const String &json);

  // 向单个客户端发送一个事件（新连接快照用）
  void publishTo(AsyncEventSourceClient *client, const char *event, const String &json);

  // 推送全部六类状态的全量快照（to 为空表示广播）
  void publishSnapshot(AsyncEventSourceClient *client);

  WifiManager &_wifi;      // WiFi 管理器引用
  SerialBridge &_bridge;   // AT 串口桥引用
  RgbLed &_led;            // RGB 灯引用
  Display &_display;       // 屏幕引用
  UIMenu &_ui;             // 菜单状态机（屏幕设置的持有者）
  SmsService &_sms;        // 短信服务引用
  TimeSync &_time;         // 时间同步引用
  LogStore &_log;          // 设备日志环形存储（SSE 日志增量推送用）
  Outbox  &_outbox;        // 持久化发件箱（SSE 变更脏信号用）

  AsyncEventSource _events{cfg::kSsePath};
  std::vector<AsyncEventSourceClient *> _clients;  // 客户端列表（按接入顺序）
  uint32_t _eventId = 0;       // SSE 事件序号（Last-Event-ID 续传用）
  uint32_t _lastPushMs = 0;    // 上次推送时间戳
  uint32_t _lastHistId = 0;    // 已推送的 AT 历史最大 ID
  bool _baseline = false;      // 是否已建立 diff 基线

  // 各状态上次已发送的内容（diff 用）
  String _lastStatus;
  String _lastStats;
  String _lastWifi;
  String _lastSaved;
  String _lastRgb;
  String _lastDisp;
  uint32_t _lastSmsId = 0;   // 上次已推送的最新短信 ID
  String _lastSmsPorts;      // 上次已推送的通信模组配置 JSON
  String _lastSmsStats;      // 上次已推送的短信存储统计 JSON
  uint32_t _lastLogId = 0;     // 已推送的最新日志 ID（0 = 下次连接重新基线）
  uint32_t _lastOutboxRev = 0;  // 已推送的发件箱变更修订号（见 Outbox::revision）
};
