#include "app/SystemService.h"

#include <esp_system.h>           // esp_restart()
#include <freertos/FreeRTOS.h>    // xTaskCreate / vTaskDelay
#include <freertos/task.h>
#include <time.h>                 // time / localtime_r / mktime

SystemService &SystemService::instance() {
  static SystemService s;
  return s;
}

void SystemService::begin() {
  load();
  _intervalBaseMs = millis();    // 间隔模式以「开机」为基准
}

void SystemService::load() {
  Preferences prefs;
  if (!prefs.begin(cfg::kNvsNamespace, true)) return;
  String json = prefs.getString(cfg::kNvsKeySched, "");
  prefs.end();
  if (json.length()) {
    JsonDocument doc;
    if (deserializeJson(doc, json) == DeserializationError::Ok) {
      importFrom(doc.as<JsonObject>());   // 复用校验/钳制逻辑
      if (doc["lastDay"].is<uint32_t>())
        _lastDay = doc["lastDay"].as<uint32_t>();
    }
  }
}

void SystemService::save() {
  JsonDocument doc;
  exportTo(doc.to<JsonObject>());
  doc["lastDay"] = _lastDay;      // 运行时状态一并持久化
  String out;
  serializeJson(doc, out);
  Preferences prefs;
  if (prefs.begin(cfg::kNvsNamespace, false)) {
    prefs.putString(cfg::kNvsKeySched, out);
    prefs.end();
  }
}

bool SystemService::importFrom(JsonObjectConst o) {
  if (!o) return false;
  if (o["enabled"].is<bool>()) _enabled = o["enabled"].as<bool>();
  if (o["mode"].is<uint8_t>()) {
    uint8_t m = o["mode"].as<uint8_t>();
    _mode = (m == cfg::kSchedModeDaily) ? cfg::kSchedModeDaily
                                        : cfg::kSchedModeInterval;
  }
  if (o["intervalH"].is<uint16_t>()) {
    uint16_t h = o["intervalH"].as<uint16_t>();
    _intervalH = (uint16_t)constrain((int32_t)h, (int32_t)cfg::kSchedIntervalHMin,
                                     (int32_t)cfg::kSchedIntervalHMax);
  }
  if (o["daily"].is<const char *>()) {
    int hh = 0, mm = 0;
    if (sscanf(o["daily"].as<const char *>(), "%d:%d", &hh, &mm) == 2)
      _dailyMin = (uint16_t)constrain(hh * 60 + mm, 0, 1439);
  }
  if (o["weekdays"].is<uint8_t>()) _weekdays = o["weekdays"].as<uint8_t>() & 0x7F;
  save();   // 与各模组一致：importFrom 内自行落盘
  return true;
}

void SystemService::exportTo(JsonObject o) const {
  o["enabled"] = _enabled;
  o["mode"] = _mode;
  char buf[8];
  snprintf(buf, sizeof(buf), "%02u:%02u", (unsigned)(_dailyMin / 60),
           (unsigned)(_dailyMin % 60));
  o["daily"] = buf;
  o["intervalH"] = _intervalH;
  o["weekdays"] = _weekdays;
}

void SystemService::tick(TimeSync &ts) {
  if (!_enabled) return;

  // ---- 固定间隔：基于开机时长，无需时间同步 ----
  if (_mode == cfg::kSchedModeInterval) {
    uint32_t ivMs = (uint32_t)_intervalH * 3600UL * 1000UL;
    if (millis() - _intervalBaseMs >= ivMs) triggerRestart();
    return;
  }

  // ---- 每日定点：需时间已同步 ----
  if (!ts.isSynced()) return;
  time_t now = time(nullptr);
  if (now < (time_t)cfg::kEpochFloorSec) return;
  struct tm t;
  localtime_r(&now, &t);

  // 星期过滤：0=每天；否则看对应位（tm_wday: 0=周日）
  if (_weekdays != 0 && !(_weekdays & (1u << t.tm_wday))) return;

  uint32_t curSec = (uint32_t)(t.tm_hour * 3600 + t.tm_min * 60 + t.tm_sec);
  uint32_t targetSec = (uint32_t)_dailyMin * 60UL;
  if (curSec < targetSec || curSec >= targetSec + cfg::kSchedDailyWindowSec)
    return;   // 不在触发窗口内

  // 本地天序号（午夜 epoch / 86400）：跨天才允许再次触发
  struct tm tl = t;
  tl.tm_hour = 0; tl.tm_min = 0; tl.tm_sec = 0;
  uint32_t day = (uint32_t)mktime(&tl) / 86400UL;
  if (_lastDay != day) {
    _lastDay = day;
    save();            // 持久化，防重启后同日重复触发
    triggerRestart();
  }
}

void SystemService::triggerRestart() {
  static bool fired = false;
  if (fired) return;
  fired = true;        // 同一次运行只安排一次重启
  xTaskCreate(
      [](void *) {
        vTaskDelay(pdMS_TO_TICKS(2000));
        esp_restart();
        vTaskDelete(nullptr);
      },
      "sched_rst", 2048, nullptr, 5, nullptr);
}
