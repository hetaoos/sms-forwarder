#include "net/WifiManager.h"

#include <ArduinoJson.h>

#include <Arduino.h>

// 读取 NVS 中的 JSON 配置，失败或不存在时按空列表处理
void WifiManager::loadFromNvs() {
  _entryCount = 0;

  Preferences prefs;
  if (!prefs.begin(cfg::kNvsNamespace, true)) return;  // 只读方式打开
  String json = prefs.getString(cfg::kNvsKeyWifi, "");
  prefs.end();
  if (!json.length()) return;

  JsonDocument doc;
  if (deserializeJson(doc, json) != DeserializationError::Ok) {
    Serial.println("[wifi] nvs config parse failed, ignore");
    return;
  }
  // 逐条读取，超出容量或 SSID 为空的条目跳过
  for (JsonObject o : doc["list"].as<JsonArray>()) {
    if (_entryCount >= kMaxEntries) break;
    const char *s = o["ssid"] | "";
    if (!*s) continue;
    _entries[_entryCount].ssid = s;
    _entries[_entryCount].pass = o["pass"] | "";
    _entryCount++;
  }
  Serial.printf("[wifi] nvs: %u saved network(s)\r\n", (unsigned)_entryCount);
}

// 把当前配置列表序列化为 JSON 写入 NVS
bool WifiManager::persistToNvs() const {
  JsonDocument doc;
  doc["ver"] = 1;  // 配置格式版本，便于后续升级
  JsonArray arr = doc["list"].to<JsonArray>();
  for (uint8_t i = 0; i < _entryCount; i++) {
    JsonObject o = arr.add<JsonObject>();
    o["ssid"] = _entries[i].ssid;
    o["pass"] = _entries[i].pass;
  }

  String out;
  serializeJson(doc, out);

  Preferences prefs;
  if (!prefs.begin(cfg::kNvsNamespace, false)) return false;  // 读写方式打开
  size_t written = prefs.putString(cfg::kNvsKeyWifi, out);
  prefs.end();
  return written == out.length();
}

// 候选列表 = 编译期默认账号（置顶）+ NVS 配置；同 SSID 只保留先出现的一条
void WifiManager::rebuildCandidates() {
  _candCount = 0;

  auto pushUnique = [&](const String &ssid, const String &pass) {
    if (!ssid.length()) return;
    for (uint8_t i = 0; i < _candCount; i++) {
      if (_cand[i].ssid == ssid) return;  // 已存在同 SSID，跳过
    }
    if (_candCount >= kMaxCandidates) return;
    _cand[_candCount].ssid = ssid;
    _cand[_candCount].pass = pass;
    _candCount++;
  };

  pushUnique(cfg::kWifiSsidDefault, cfg::kWifiPassDefault);
  for (uint8_t i = 0; i < _entryCount; i++) {
    pushUnique(_entries[i].ssid, _entries[i].pass);
  }
}

// 用当前候选发起连接（非阻塞，结果在 tick 中观察）
void WifiManager::startNextAttempt() {
  if (_candCount == 0 || _candPos >= _candCount) {
    enterApMode();  // 没有可用候选，直接进配网模式
    return;
  }
  const WifiEntry &c = _cand[_candPos];
  // 配网模式下保持 AP 在线，用双栈让 STA 也能连
  WiFi.mode(_apUp ? WIFI_AP_STA : WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.begin(c.ssid.c_str(), c.pass.c_str());
  _attemptMs = millis();
  Serial.printf("[wifi] try '%s' ...\r\n", c.ssid.c_str());
}

// 显式试连失败后的回退：回到 NVS 候选重新开始，而不是直接跳 AP
void WifiManager::restartAttempts() {
  _explicitTry = false;
  rebuildCandidates();
  _candPos = 0;
  _round = 0;
  if (_candCount == 0) {
    enterApMode();
    return;
  }
  _mode = WifiMode::kConnecting;
  startNextAttempt();
}

// 进入 AP 配网模式：开放热点 + AP_STA 双栈，网页开启认证
void WifiManager::enterApMode() {
  if (_mode == WifiMode::kAp && _apUp) return;  // 已是 AP 模式，幂等返回
  _mode = WifiMode::kAp;
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAPConfig(IPAddress(192, 168, 4, 1), IPAddress(192, 168, 4, 1),
                    IPAddress(255, 255, 255, 0));
  WiFi.softAP(cfg::kApSsid);  // 开放热点，无密码
  _apUp = true;
  Serial.printf("[wifi] AP mode '%s' ip=192.168.4.1\r\n", cfg::kApSsid);
}

// 初始化：读 NVS -> 组候选 -> 启动连接状态机
void WifiManager::begin() {
  if (!_mutex) _mutex = xSemaphoreCreateMutex();

  lock();
  loadFromNvs();
  rebuildCandidates();
  if (_candCount == 0) {
    // 无任何配置（编译期为空且 NVS 为空）-> 直接 AP 配网
    enterApMode();
    unlock();
    return;
  }
  _mode = WifiMode::kConnecting;
  _candPos = 0;
  _round = 0;
  startNextAttempt();
  unlock();
}

// 状态机推进：
// kConnecting: 等待当前候选，超时切下一个；整轮失败 kWifiAttemptRounds 次 -> AP
// kSta:        监测掉线，掉线超过 kWifiLostRetryMs 后从头重试
// kAp:         无后台重试，等待网页保存/试连
void WifiManager::tick() {
  lock();
  switch (_mode) {
    case WifiMode::kConnecting:
      if (connected()) {
        _mode = WifiMode::kSta;
        _lostMs = millis();
        Serial.printf("[wifi] up, ip=%s\r\n", ip().c_str());
        unlock();
        if (_onConnect) _onConnect();  // 锁外触发：回调可能启动 NTP 等耗时操作
        return;
      }
      if (millis() - _attemptMs >= cfg::kWifiConnectTimeoutMs) {
        Serial.println("[wifi] attempt timeout");
        _candPos++;
        if (_candPos >= _candCount) {  // 一轮结束
          _candPos = 0;
          _round++;
        }
        if (_round >= cfg::kWifiAttemptRounds) {
          if (_explicitTry) {
            // 网页显式试连失败：回退到已保存候选，不轻易丢弃当前环境
            restartAttempts();
          } else {
            enterApMode();  // 连续多轮全部失败，进入配网模式
          }
          unlock();
          return;
        }
        startNextAttempt();
      }
      break;

    case WifiMode::kSta:
      if (!connected()) {
        if (millis() - _lostMs >= cfg::kWifiLostRetryMs) {
          Serial.println("[wifi] link lost, retry candidates");
          rebuildCandidates();
          _candPos = 0;
          _round = 0;
          _mode = WifiMode::kConnecting;
          startNextAttempt();
        }
      } else {
        _lostMs = millis();  // 仍在线，刷新掉线计时
      }
      break;

    case WifiMode::kAp:
      // AP 模式下不做后台重试；用户经网页保存配置时显式发起
      break;
  }
  unlock();
}

// 当前 IP 字符串
String WifiManager::ip() const {
  if (connected()) return WiFi.localIP().toString();
  if (_apUp) return "192.168.4.1";
  return "0.0.0.0";
}

// 当前网络名
String WifiManager::ssid() const {
  if (connected()) return WiFi.SSID();
  if (_apUp) return cfg::kApSsid;
  return "";
}

// 已保存配置 JSON（不回传密码，只回 hasPass 标记）
String WifiManager::listJson() const {
  lock();
  JsonDocument doc;
  doc["count"] = _entryCount;
  JsonArray arr = doc["list"].to<JsonArray>();
  for (uint8_t i = 0; i < _entryCount; i++) {
    JsonObject o = arr.add<JsonObject>();
    o["i"] = i;                            // 列表序号（增删排序用）
    o["ssid"] = _entries[i].ssid;
    o["hasPass"] = _entries[i].pass.length() > 0;
  }
  String out;
  serializeJson(doc, out);
  unlock();
  return out;
}

// 新增/更新配置；ssid 为空或超出容量返回 false
bool WifiManager::save(const String &ssid, const String &pass,
                       bool connectNow) {
  if (!ssid.length()) return false;

  lock();
  // 同 SSID 已存在 -> 覆盖密码并保持原位；否则追加到末尾
  int8_t found = -1;
  for (uint8_t i = 0; i < _entryCount; i++) {
    if (_entries[i].ssid == ssid) {
      found = (int8_t)i;
      break;
    }
  }
  if (found >= 0) {
    _entries[found].pass = pass;
  } else {
    if (_entryCount >= kMaxEntries) {
      unlock();
      return false;  // 超出上限
    }
    _entries[_entryCount].ssid = ssid;
    _entries[_entryCount].pass = pass;
    _entryCount++;
  }

  bool persisted = persistToNvs();
  if (persisted) {
    Serial.printf("[wifi] saved '%s' (total %u)\r\n", ssid.c_str(),
                  (unsigned)_entryCount);
  }
  if (persisted && connectNow) requestConnectLocked(ssid, pass);
  unlock();

  if (!persisted) Serial.println("[wifi] persist failed");
  return persisted;
}

// 按序号删除；index 越界返回 false
bool WifiManager::remove(uint8_t index) {
  lock();
  if (index >= _entryCount) {
    unlock();
    return false;
  }
  for (uint8_t i = index; i + 1 < _entryCount; i++) {
    _entries[i] = _entries[i + 1];  // 后续条目整体前移
  }
  _entryCount--;
  bool persisted = persistToNvs();
  unlock();
  if (!persisted) return false;
  Serial.printf("[wifi] removed #%u (total %u)\r\n", (unsigned)index,
                (unsigned)_entryCount);
  return true;
}

// 把 from 处条目移动到 to 处（调整启动尝试顺序）
bool WifiManager::move(uint8_t from, uint8_t to) {
  lock();
  if (from >= _entryCount || to >= _entryCount || from == to) {
    unlock();
    return false;
  }

  WifiEntry tmp = _entries[from];
  if (from < to) {
    for (uint8_t i = from; i < to; i++) _entries[i] = _entries[i + 1];
  } else {
    for (uint8_t i = from; i > to; i--) _entries[i] = _entries[i - 1];
  }
  _entries[to] = tmp;

  bool persisted = persistToNvs();
  unlock();
  if (!persisted) return false;
  Serial.printf("[wifi] moved #%u -> #%u\r\n", (unsigned)from, (unsigned)to);
  return true;
}

// 立即用给定账号试连（不落盘）；失败后自动回退到已保存候选，
// 避免把已有可用连接拖入 AP 模式
void WifiManager::requestConnect(const String &ssid, const String &pass) {
  lock();
  requestConnectLocked(ssid, pass);
  unlock();
}

// requestConnect 的持锁实现：只放一个候选，失败由 tick 回退
void WifiManager::requestConnectLocked(const String &ssid,
                                       const String &pass) {
  _candCount = 1;
  _cand[0].ssid = ssid;
  _cand[0].pass = pass;
  _candPos = 0;
  _round = 0;
  _explicitTry = true;  // 标记显式试连，失败时走 restartAttempts 回退
  _mode = WifiMode::kConnecting;
  startNextAttempt();
}

// 启动异步扫描；已有扫描在进行时返回 false
bool WifiManager::startScan() {
  if (scanBusy()) return false;
  // 参数：异步 / 不显示隐藏 / 主动扫描 / 每信道 300ms
  WiFi.scanNetworks(true, false, false, 300);
  return true;
}

// 导出扫描结果并释放底层缓存
// 同一 SSID 可能出现多次（多个 AP / mesh / 多信道广播相同名称），
// 这里按名称去重，每个 SSID 只保留信号最强（RSSI 数值最大）的那一条
String WifiManager::scanJson() {
  int n = WiFi.scanComplete();
  if (n <= 0) {
    if (n == 0) WiFi.scanDelete();  // 0 条结果也要释放，否则无法再次扫描
    return "[]";
  }

  // 去重表：记录每个唯一 SSID 对应的最佳结果下标（下标引用扫描结果，
  // 需在 scanDelete() 之前用完；64 条唯一 SSID 对选择界面足够）
  static const int kMaxUnique = 64;
  int bestIdx[kMaxUnique];
  int unique = 0;

  for (int i = 0; i < n; i++) {
    String ssid = WiFi.SSID(i);
    if (!ssid.length()) continue;  // 隐藏网络不展示

    int slot = -1;
    for (int j = 0; j < unique; j++) {
      if (WiFi.SSID(bestIdx[j]) == ssid) {  // 已见过同名 SSID
        slot = j;
        break;
      }
    }
    if (slot < 0) {
      if (unique >= kMaxUnique) continue;   // 超出容量的唯一 SSID 忽略
      bestIdx[unique++] = i;
    } else if (WiFi.RSSI(i) > WiFi.RSSI(bestIdx[slot])) {
      bestIdx[slot] = i;                    // 同名保留信号更强的条目
    }
  }

  JsonDocument doc;
  JsonArray arr = doc.to<JsonArray>();
  for (int j = 0; j < unique; j++) {
    int i = bestIdx[j];
    JsonObject o = arr.add<JsonObject>();
    o["ssid"] = WiFi.SSID(i);
    o["rssi"] = WiFi.RSSI(i);
    o["secure"] = WiFi.encryptionType(i) != WIFI_AUTH_OPEN;
  }
  WiFi.scanDelete();  // 释放结果，便于下一轮扫描
  String out;
  serializeJson(doc, out);
  return out;
}

// 运行状态 JSON（供前端轮询）
String WifiManager::statusJson() const {
  lock();
  JsonDocument doc;
  switch (_mode) {
    case WifiMode::kConnecting: doc["mode"] = "connecting"; break;
    case WifiMode::kSta:        doc["mode"] = "sta"; break;
    case WifiMode::kAp:         doc["mode"] = "ap"; break;
  }
  doc["connected"] = connected();
  doc["ssid"] = ssid();
  doc["ip"] = ip();
  doc["rssi"] = rssi();
  doc["ap"] = _apUp;
  doc["apSsid"] = cfg::kApSsid;
  doc["count"] = _entryCount;
  String out;
  serializeJson(doc, out);
  unlock();
  return out;
}
