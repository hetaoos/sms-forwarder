#include "app/ModemHw.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <Preferences.h>

#include "log/LogStore.h"

namespace modemhw {

namespace {
// 运行期配置（下标 = 模组端口号 1..kSmsMaxModems）
PortPins s_ports[cfg::kSmsMaxModems + 1];

// 默认 RX/TX/EN 引脚：沿用编译期常量；EN 模式由 en 引脚号与 kModemEnActiveLow 推导
void fillDefaults() {
  // 端口 1
  s_ports[1].rx = cfg::kUart1Rx;
  s_ports[1].tx = cfg::kUart1Tx;
  s_ports[1].en = cfg::kModemEnPin1;
  s_ports[1].enMode = (cfg::kModemEnPin1 >= 0)
                           ? (cfg::kModemEnActiveLow ? kEnHighEnable : kEnLowEnable)
                           : kEnInvalid;
  // 端口 2
  s_ports[2].rx = cfg::kUart2Rx;
  s_ports[2].tx = cfg::kUart2Tx;
  s_ports[2].en = cfg::kModemEnPin2;
  s_ports[2].enMode = (cfg::kModemEnPin2 >= 0)
                           ? (cfg::kModemEnActiveLow ? kEnHighEnable : kEnLowEnable)
                           : kEnInvalid;
}

// 合法 GPIO 范围（ESP32-S3）：0..48。-1 仅用于 EN 的「未接」语义
bool gpioValid(int v) { return v >= 0 && v <= 48; }
}  // namespace

const PortPins &get(uint8_t port) {
  if (port < 1 || port > cfg::kSmsMaxModems) return s_ports[0];
  return s_ports[port];
}

EnMode enMode(uint8_t port) {
  if (port < 1 || port > cfg::kSmsMaxModems) return kEnInvalid;
  const PortPins &p = s_ports[port];
  if (p.enMode == kEnInvalid || p.en < 0) return kEnInvalid;
  return static_cast<EnMode>(p.enMode);
}

void load() {
  fillDefaults();  // 先填编译期默认，NVS 无记录时即为生效值
  Preferences prefs;
  String json;
  if (prefs.begin(cfg::kNvsNamespace, true)) {
    json = prefs.getString(cfg::kNvsKeyModemHw, "");
    prefs.end();
  }
  if (!json.length()) return;  // 无记录：使用默认

  JsonDocument doc;
  if (deserializeJson(doc, json) != DeserializationError::Ok) return;
  for (JsonObject o : doc["ports"].as<JsonArray>()) {
    int port = o["port"] | 0;
    if (port < 1 || port > cfg::kSmsMaxModems) continue;
    if (o["rx"].is<int>()) s_ports[port].rx = (int8_t)o["rx"].as<int>();
    if (o["tx"].is<int>()) s_ports[port].tx = (int8_t)o["tx"].as<int>();
    if (o["en"].is<int>()) s_ports[port].en = (int8_t)o["en"].as<int>();
    if (o["enMode"].is<int>()) {
      int m = o["enMode"].as<int>();
      s_ports[port].enMode = (m >= kEnInvalid && m <= kEnLowEnable)
                                 ? (uint8_t)m
                                 : (uint8_t)kEnInvalid;
    }
    // en 引脚号失效（<0）时强制 EN 无效，避免悬空控制
    if (s_ports[port].en < 0) s_ports[port].enMode = kEnInvalid;
  }
  devLog().addf(LogLevel::kInfo, "modemhw",
                "模组引脚已载入：1 RX%d/TX%d EN%d(mode%d)  2 RX%d/TX%d EN%d(mode%d)",
                (int)s_ports[1].rx, (int)s_ports[1].tx, (int)s_ports[1].en,
                (int)s_ports[1].enMode, (int)s_ports[2].rx, (int)s_ports[2].tx,
                (int)s_ports[2].en, (int)s_ports[2].enMode);
}

bool save(const PortPins ports[], size_t n) {
  JsonDocument doc;
  doc["ver"] = 1;
  JsonArray arr = doc["ports"].to<JsonArray>();
  for (size_t i = 0; i < n && i < cfg::kSmsMaxModems; i++) {
    // 同步运行期内存配置：s_ports 是 GET /api/modemhw 与即时响应的数据来源，
    // 若不更新，保存后页面读取到的仍是旧值（多为出厂默认），表现为「保存后还显示默认」。
    s_ports[i + 1] = ports[i];
    JsonObject o = arr.add<JsonObject>();
    const PortPins &p = ports[i];
    o["port"] = (uint8_t)(i + 1);
    o["rx"] = p.rx;
    o["tx"] = p.tx;
    o["en"] = p.en;
    o["enMode"] = p.enMode;
  }
  String out;
  serializeJson(doc, out);

  Preferences prefs;
  if (!prefs.begin(cfg::kNvsNamespace, false)) return false;
  size_t written = prefs.putString(cfg::kNvsKeyModemHw, out);
  prefs.end();
  return written == out.length();
}

void applyBootEn() {
  for (uint8_t port = 1; port <= cfg::kSmsMaxModems; port++) {
    int8_t pin = enPin(port);
    if (pin < 0) continue;
    int8_t lvl = enEnableLevel(port);
    pinMode(pin, OUTPUT);
    digitalWrite(pin, lvl == HIGH ? HIGH : LOW);
  }
}

int8_t enPin(uint8_t port) {
  EnMode m = enMode(port);
  if (m == kEnInvalid) return -1;
  return s_ports[port].en;
}

int8_t enEnableLevel(uint8_t port) {
  EnMode m = enMode(port);
  if (m == kEnHighEnable) return HIGH;
  if (m == kEnLowEnable) return LOW;
  return -1;
}

const std::vector<int8_t> &availablePins() {
  static std::vector<int8_t> pins;
  if (!pins.empty()) return pins;
  // 真正不可用于模组 RX/TX/EN 的引脚（其余如屏幕 TFT、按键、RGB 等板载外设
  // 引脚一律释放——部分机型未焊接屏幕/按键，这些 GPIO 完全可改作模组引脚；是否占用
  // 由用户在 Web 引脚配置里自行决定，冲突风险自负）：
  //   0 启动 Strapping；3/43/44 控制台 UART0（RX/TX，烧录/日志用）；
  //   45/46 启动 Strapping（46 仅输入）；48 板载 RGB 灯（多数机型必接）。
  // Flash/PSRAM 占 GPIO 26..32（物理存在但不可作 GPIO，下方循环跳过）。
  static const int8_t kReserved[] = {0, 3, 43, 44, 45, 46, 48};
  for (int p = 0; p <= 48; p++) {
    if (p >= 22 && p <= 25) continue;   // ESP32-S3 不存在该区间 GPIO
    if (p >= 26 && p <= 32) continue;   // 内部 Flash/PSRAM，不可作 GPIO
    bool ok = true;
    for (int r : kReserved) {
      if (p == r) { ok = false; break; }
    }
    if (ok) pins.push_back((int8_t)p);
  }
  return pins;
}

}  // namespace modemhw
