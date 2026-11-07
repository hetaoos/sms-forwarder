#pragma once

#include <ArduinoJson.h>

#include <Arduino.h>

#include <WiFi.h>

#include "app/Config.h"  // ENABLE_MQTT / ENABLE_FIRMWARE_UPGRADE（编译期开关）
#include "app/UIMenu.h"   // 屏幕设置的持有者（亮度 / 自动熄屏 / 自动返回状态页）
#include "app/TaskWatchdog.h"  // 任务看门狗状态与上次卡死原因
#include "hal/CpuMonitor.h"
#include "hal/Display.h"
#include "hal/RgbLed.h"
#include "net/WifiManager.h"
#include "ota/Ota.h"  // otaPartitionAvailable()：运行时探测是否支持 OTA 双槽
#include "serial/SerialBridge.h"
#include "sms/SmsStore.h"        // 今日短信统计
#include "app/SystemService.h"  // 定时重启配置回显（单例）
#include "time/TimeSync.h"
#include "utils/DeviceId.h"   // Device::id()：与 MQTT / 推送 / 邮件一致的设备唯一 ID

// MQTT 推送连接状态（仅编译期开启 MQTT 时引用其类定义）
#if ENABLE_MQTT
#include "net/MqttService.h"
#endif

// 各状态快照的 JSON 序列化：WebPanel 的 REST 接口与 StatusBus 的 SSE
// 推送共用这些函数，确保同一状态在两条通道上的数据完全一致。
// 均为纯函数（不持有状态），可在任意任务上下文调用。

// 汇总状态：与 GET /api/status 响应一致
inline String stateStatusJson(WifiManager &wifi, RgbLed &led, Display &display,
                              TimeSync &time, SmsStore &store) {
  const RgbLed::Color &c = led.color();
  JsonDocument doc;
  doc["ssid"] = wifi.ssid();
  doc["ip"] = wifi.ip();
  doc["rssi"] = wifi.rssi();
  doc["uptime"] = millis() / 1000;
  doc["heap"] = ESP.getFreeHeap();
  doc["heapSize"] = ESP.getHeapSize();   // 内部堆总容量（与 heap 配对展示 剩余/总量）
  // PSRAM：总量 0 表示板子无 PSRAM；注意 heap 只统计内部 DRAM，
  // >4KB 的分配会自动走 PSRAM（CONFIG_SPIRAM_USE_MALLOC），故两者需分别展示
  doc["psram"] = ESP.getFreePsram();
  doc["psramSize"] = ESP.getPsramSize();
  // ROM（Flash）总容量，用于仪表盘展示设备存储规模
  doc["romSize"] = ESP.getFlashChipSize();
  doc["color"] = c.name;
  JsonArray rgb = doc["rgb"].to<JsonArray>();
  rgb.add(c.r);
  rgb.add(c.g);
  rgb.add(c.b);
  doc["disp"] = display.screenOn();
  doc["dispBright"] = display.brightness();
  doc["rgbBright"] = led.brightness();
  doc["connected"] = wifi.connected();
  doc["mode"] = wifi.apMode() ? "ap" : (wifi.connected() ? "sta" : "connecting");
  // 时间同步：本地时间串（"YYYY-MM-DD HH:mm:ss" / "未知"）、是否可用、来源
  // （ntp / sim / nvs / none）
  doc["time"] = time.dateStr();
  doc["timeSynced"] = time.isSynced();
  doc["timeSrc"] = time.srcName();
  // 设备时区：前端列表按此偏移把 epoch 秒格式化为设备本地时间，
  // 避免浏览器所在的时区与设备不一致时显示错乱（旧固件无此字段时前端默认 +480）
  doc["tzOffsetMin"] = time.timezoneMin();
  doc["tzText"] = time.timezoneText();
  // OTA 是否可用（编译期开关 + 设备分区表是否具备双槽）。前端据此决定是否展示升级入口
  doc["otaCompiled"] = (ENABLE_FIRMWARE_UPGRADE != 0);
  doc["otaAvailable"] = otaPartitionAvailable();
  // LCD / RGB 灯是否编译进固件：ENABLE_LCD / ENABLE_LED=0 时前端隐藏对应 UI 入口
  doc["lcdCompiled"] = (ENABLE_LCD != 0);
  doc["ledCompiled"] = (ENABLE_LED != 0);

  // ---- MQTT 推送连接状态（仪表盘 / 配置页共用，SSE status 事件实时推送）----
  // compiled 供前端决定 MQTT 菜单是否出现：ENABLE_MQTT=0 时不能再让配置页露出来。
#if ENABLE_MQTT
  JsonObject mq = doc["mqtt"].to<JsonObject>();
  mq["compiled"]   = true;
  mq["enabled"]    = MqttService::isEnabled();    // 总开关是否开启
  mq["configured"] = MqttService::isConfigured(); // 服务器是否已配置
  mq["connected"]  = MqttService::connected();    // 当前是否真正连上 Broker
  mq["error"]      = MqttService::lastError();    // 最近一次连接错误（空串=无错误）
#else
  JsonObject mq = doc["mqtt"].to<JsonObject>();
  mq["compiled"]   = false;
  mq["enabled"]    = false;
  mq["configured"] = false;
  mq["connected"]  = false;
  mq["error"]      = String("");
#endif

  // ---- 设备硬件/固件信息 ----
  // CPU 使用率（%）：基于 FreeRTOS 运行时间统计；不可用时 cpuAvail=false
  float cpu = CpuMonitor::usage();
  doc["cpuAvail"] = cpu >= 0;
  doc["cpu"] = cpu >= 0 ? (float)(round(cpu * 10) / 10.0) : 0.0f;
  // 芯片内部温度传感器（ESP32-S3 内置）；读数异常（NaN）时 tempAvail=false
  float tempC = temperatureRead();
  doc["tempAvail"] = !isnan(tempC);
  doc["temp"] = isnan(tempC) ? 0.0f : (float)(round(tempC * 10) / 10.0);
  // 芯片型号 + 步进（如 "ESP32-S3 rev 2"），同时作为硬件版本展示
  String chip = ESP.getChipModel();
  if (chip.length() > 0) chip += " rev " + String((int)ESP.getChipRevision());
  doc["chip"] = chip.length() > 0 ? chip : String("未知");
  // 硬件版本：直接采用芯片型号 + 步进（随实际硅片自动变化）
  doc["hwVer"] = chip.length() > 0 ? chip : String("未知");
  // 软件版本（= 固件所属 Release tag，由 CI 注入 OTA_RELEASE_TAG）
  doc["swVer"] = Ota::version();
  // 本机 MAC 地址（STA 接口，大写冒号分隔）
  doc["mac"] = WiFi.macAddress();
  // 设备 ID（= 完整芯片 MAC 48 位，12 位小写十六进制；与 MQTT ClientID / 主题前缀一致）
  doc["deviceId"] = Device::id();

  // 今日短信统计（收/发），按本地日期（午夜为界）
  int recv = 0, sent = 0;
  store.todayCount(recv, sent);
  JsonObject tsms = doc["todaySms"].to<JsonObject>();
  tsms["recv"] = recv;
  tsms["sent"] = sent;

  // 定时重启配置回显（默认禁用）
  {
    const SystemService &sys = SystemService::instance();
    JsonObject sc = doc["sched"].to<JsonObject>();
    sc["enabled"] = sys.enabled();
    sc["mode"] = sys.mode();
    char buf[8];
    snprintf(buf, sizeof(buf), "%02u:%02u", (unsigned)(sys.dailyMin() / 60),
             (unsigned)(sys.dailyMin() % 60));
    sc["daily"] = buf;
    sc["intervalH"] = sys.intervalH();
    sc["weekdays"] = sys.weekdays();
  }

  // 任务看门狗：本次开机复位原因 + 最近一次 WDT 卡死记录（NVS 持久）。
  // REST /api/status 与 SSE status 事件共用本函数，字段天然一致。
  {
    JsonObject w = doc["wdt"].to<JsonObject>();
    w["enabled"] = TaskWatchdog::enabled();
    w["timeoutMs"] = TaskWatchdog::timeoutMs();
    w["tasks"] = TaskWatchdog::monitoredTasks();
    w["reset"] = TaskWatchdog::resetName();
    w["resetText"] = TaskWatchdog::resetText();
    w["bootWasWdt"] = TaskWatchdog::bootWasWdt();
    w["wdtCount"] = TaskWatchdog::wdtResetCount();
    w["lastWdtTask"] = TaskWatchdog::lastWdtTask();
    w["lastWdtUptimeSec"] = TaskWatchdog::lastWdtUptimeSec();
    w["lastWdtVersion"] = TaskWatchdog::lastWdtVersion();
  }

  String out;
  serializeJson(doc, out);
  return out;
}

// 屏幕状态：与 GET /api/disp 响应一致
// ui 提供持久化的自动熄屏 / 自动返回状态页时间（秒）
inline String stateDispJson(Display &display, UIMenu &ui) {
  JsonDocument doc;
  doc["ok"] = true;
  doc["on"] = display.screenOn();
  doc["bright"] = display.brightness();
  doc["autoOffSec"] = ui.autoOffSec();
  doc["idleSec"] = ui.idleSec();
  String out;
  serializeJson(doc, out);
  return out;
}

// RGB 状态：与 GET /api/rgb 响应一致
inline String stateRgbJson(RgbLed &led) {
  const RgbLed::Color &c = led.color();
  JsonDocument doc;
  doc["ok"] = true;
  doc["color"] = c.name;
  JsonArray rgb = doc["rgb"].to<JsonArray>();
  rgb.add(c.r);
  rgb.add(c.g);
  rgb.add(c.b);
  doc["index"] = led.index();
  doc["bright"] = led.brightness();
  String out;
  serializeJson(doc, out);
  return out;
}
