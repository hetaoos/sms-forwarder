#pragma once

#include <Arduino.h>
#include <Preferences.h>

#include <ArduinoJson.h>

#include "app/Config.h"
#include "time/TimeSync.h"

// 定时重启（系统保活）：默认禁用，支持两种模式——
//   · 固定间隔（kSchedModeInterval）：基于开机时长（millis），到点即重启；
//   · 每日定点（kSchedModeDaily）：基于本地时间（依赖 NTP/SIM 同步），
//     可指定星期（位掩码，0=每天），配合触发窗口 + 持久化「上次触发天」防重复。
// 单例：App 与 WebPanel 共用同一份配置与触发状态。
class SystemService {
 public:
  static SystemService &instance();

  // 读 NVS 配置（App::begin 调用一次）
  void begin();

  // 每轮主循环调用；到点（且条件满足）后延迟 2s 重启
  void tick(TimeSync &ts);

  // 配置导入/导出（纳入 /api/config/import|export）
  bool importFrom(JsonObjectConst o);
  void exportTo(JsonObject o) const;

  // 状态回显（供 Web 状态 JSON）
  bool enabled() const { return _enabled; }
  uint8_t mode() const { return _mode; }
  uint16_t dailyMin() const { return _dailyMin; }
  uint16_t intervalH() const { return _intervalH; }
  uint8_t weekdays() const { return _weekdays; }

 private:
  void save();
  void load();
  void triggerRestart();

  bool _enabled = false;                          // 总开关（默认关）
  uint8_t _mode = cfg::kSchedModeInterval;         // 默认固定间隔
  uint16_t _intervalH = cfg::kSchedIntervalHDefault;// 默认 24h
  uint16_t _dailyMin = 0;        // 每日模式触发时刻（分钟，0..1439，即 00:00）
  uint8_t _weekdays = 0;         // 位掩码 bit0=周日…bit6=周六；0=每天
  uint32_t _intervalBaseMs = 0;  // 间隔模式基线（开机 millis）
  uint32_t _lastDay = 0;         // 上次每日触发所在「本地天序号」（持久化防重复）
};
