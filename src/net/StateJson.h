#pragma once

#include <ArduinoJson.h>

#include <Arduino.h>

#include "hal/Display.h"
#include "hal/RgbLed.h"
#include "net/WifiManager.h"
#include "serial/SerialBridge.h"
#include "time/TimeSync.h"

// 各状态快照的 JSON 序列化：WebPanel 的 REST 接口与 StatusBus 的 SSE
// 推送共用这些函数，确保同一状态在两条通道上的数据完全一致。
// 均为纯函数（不持有状态），可在任意任务上下文调用。

// 汇总状态：与 GET /api/status 响应一致
inline String stateStatusJson(WifiManager &wifi, RgbLed &led, Display &display,
                              TimeSync &time) {
  const RgbLed::Color &c = led.color();
  JsonDocument doc;
  doc["ssid"] = wifi.ssid();
  doc["ip"] = wifi.ip();
  doc["rssi"] = wifi.rssi();
  doc["uptime"] = millis() / 1000;
  doc["heap"] = ESP.getFreeHeap();
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
  String out;
  serializeJson(doc, out);
  return out;
}

// 屏幕状态：与 GET /api/disp 响应一致
inline String stateDispJson(Display &display) {
  return String("{\"ok\":true,\"on\":") + (display.screenOn() ? "true" : "false") +
         ",\"bright\":" + String(display.brightness()) + "}";
}

// RGB 状态：与 GET /api/rgb 响应一致
inline String stateRgbJson(RgbLed &led) {
  const RgbLed::Color &c = led.color();
  String rgb = "[" + String(c.r) + "," + String(c.g) + "," + String(c.b) + "]";
  return String("{\"ok\":true,\"color\":\"") + c.name + "\",\"rgb\":" + rgb +
         ",\"index\":" + String(led.index()) +
         ",\"bright\":" + String(led.brightness()) + "}";
}
