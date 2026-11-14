#include "net/WifiManager.h"

#include "log/LogStore.h"

#include <ArduinoJson.h>

#include <Arduino.h>

namespace {

// 本模组日志出口：全局日志环（串口由 LogStore 统一镜像输出）
LogStore &_log = devLog();

}  // namespace

// 读取 NVS 中的 JSON 配置，失败或不存在时按空列表处理
void WifiManager::loadFromNvs() {
  _entryCount = 0;
  _nvsHadKey = false;

  Preferences prefs;
  if (!prefs.begin(cfg::kNvsNamespace, true)) return;  // 只读方式打开
  // 区分「NVS 无任何 WiFi 配置键」（首次启动）与「有键但为空/解析失败」（可能用户清空）。
  // 仅首次启动才触发编译期默认账号回填（P3），避免覆盖用户主动清空后的意图（进 AP 配网）。
  if (!prefs.isKey(cfg::kNvsKeyWifi)) {
    prefs.end();
    return;
  }
  _nvsHadKey = true;
  String json = prefs.getString(cfg::kNvsKeyWifi, "");
  prefs.end();
  if (!json.length()) return;

  JsonDocument doc;
  if (deserializeJson(doc, json) != DeserializationError::Ok) {
    _log.addf(LogLevel::kWarn, "wifi", "NVS 中的 WiFi 配置解析失败，按空列表处理");
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
  _log.addf(LogLevel::kInfo, "wifi", "已从 NVS 载入 %u 个 WiFi 配置",
            (unsigned)_entryCount);
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

// 候选列表 = 编译期默认账号（置顶）+ NVS 配置；同 SSID 只保留先出现的一条。
// 若记录过"上次成功连上的"网络，则把它前置：瞬时断线（同一路由器恢复）
// 可 1 次试探内恢复，不必从头遍历所有候选。
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

  // 优先前置上次成功的网络（仅在列表里确实存在该 SSID 时才生效）
  if (_lastGoodSsid.length()) {
    if (cfg::kWifiSsidDefault[0] && _lastGoodSsid == cfg::kWifiSsidDefault) {
      pushUnique(cfg::kWifiSsidDefault, cfg::kWifiPassDefault);
    }
    for (uint8_t i = 0; i < _entryCount; i++) {
      if (_entries[i].ssid == _lastGoodSsid) {
        pushUnique(_entries[i].ssid, _entries[i].pass);
        break;
      }
    }
  }

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
  _log.addf(LogLevel::kInfo, "wifi", "正在连接 '%s'", c.ssid.c_str());
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

// 进入 AP 配网模式：按 _apPass 决定开放或加密热点 + AP_STA 双栈，网页开启认证
void WifiManager::enterApMode() {
  if (_mode == WifiMode::kAp && _apUp) return;  // 已是 AP 模式，幂等返回
  _mode = WifiMode::kAp;
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAPConfig(IPAddress(192, 168, 4, 1), IPAddress(192, 168, 4, 1),
                    IPAddress(255, 255, 255, 0));
  applyAp();
  _apRetryMs = millis();      // 重置后台重试计时
  if (_apPass.length() >= 8)
    _log.addf(LogLevel::kInfo, "wifi",
              "已进入 AP 配网模式 '%s'（WPA2 加密，IP 192.168.4.1）", cfg::kApSsid);
  else
    _log.addf(LogLevel::kInfo, "wifi",
              "已进入 AP 配网模式 '%s'（开放无密码，IP 192.168.4.1）", cfg::kApSsid);
}

// （重）创建 AP 热点：_apPass 合规则加密，否则开放
void WifiManager::applyAp() {
  if (_apPass.length() >= 8)
    WiFi.softAP(cfg::kApSsid, _apPass.c_str());  // WPA2 加密热点
  else
    WiFi.softAP(cfg::kApSsid);                   // 开放热点
  _apUp = true;
}

// 从 NVS 读取 AP 密码；无记录时用编译期默认（合规才采用）并落盘
void WifiManager::loadApPass() {
  Preferences prefs;
  if (!prefs.begin(cfg::kNvsNamespace, true)) {  // 只读
    _apPass = "";
    return;
  }
  String p = prefs.getString(cfg::kNvsKeyApPass, "");
  prefs.end();
  if (!p.length()) {  // 首次启动：用编译期默认初始化并落盘
    _apPass = validApPass(cfg::kApPassDefault) ? String(cfg::kApPassDefault) : String();
    if (!validApPass(cfg::kApPassDefault) && cfg::kApPassDefault[0])
      _log.addf(LogLevel::kWarn, "wifi",
                "编译期 AP 密码长度不足 8 字符，WPA2 不支持，回落开放热点");
    persistApPass();
    return;
  }
  // NVS 已有记录：persistApPass 写入的是 JSON 封装（{"ver":1,"pass":"..."}），
  // 需解析取出真实密码，否则会把整段 JSON 当作密码（热点/LCD/网页均异常）。
  String pass = "";
  JsonDocument doc;
  if (deserializeJson(doc, p) == DeserializationError::Ok &&
      doc["pass"].is<const char *>()) {
    pass = doc["pass"].as<const char *>();   // 复制出真实密码
  } else {
    _log.addf(LogLevel::kWarn, "wifi", "NVS 中 AP 密码解析失败，回落开放热点");
  }
  // 长度合规才采用，否则回落开放（不覆盖 NVS，避免反复告警）
  if (validApPass(pass)) {
    _apPass = pass;
  } else {
    _apPass = "";
    _log.addf(LogLevel::kWarn, "wifi", "NVS 中 AP 密码长度异常，回落开放热点");
  }
}

// 序列化为 JSON 写入 NVS
bool WifiManager::persistApPass() const {
  JsonDocument doc;
  doc["ver"] = 1;
  doc["pass"] = _apPass;
  String out;
  serializeJson(doc, out);
  Preferences prefs;
  if (!prefs.begin(cfg::kNvsNamespace, false)) return false;  // 读写
  size_t written = prefs.putString(cfg::kNvsKeyApPass, out);
  prefs.end();
  return written == out.length();
}

// AP 密码是否合规：空串（开放）或 8~kApPassMaxLen 字符（WPA2）
bool WifiManager::validApPass(const String &p) {
  return p.length() == 0 || (p.length() >= 8 && p.length() <= cfg::kApPassMaxLen);
}

// 设置 AP 热点密码并落盘；AP 已启动则立即重建热点使新密码生效
bool WifiManager::setApPass(const String &pass, String *err) {
  if (!validApPass(pass)) {
    if (err)
      *err = "AP 密码需为 " + String(cfg::kApPassMaxLen) +
             " 字符以内且空或≥8 字符（WPA2），或留空设为开放";
    return false;
  }
  String old = _apPass;
  _apPass = pass;
  if (!persistApPass()) {
    _apPass = old;
    if (err) *err = "NVS 写入失败";
    return false;
  }
  if (_apUp) {  // 重建热点使新密码生效（先断开再按当前 _apPass 重建）
    WiFi.softAPdisconnect(false);
    _apUp = false;
    applyAp();
    _log.addf(LogLevel::kInfo, "wifi", "AP 热点密码已更新（%s）",
              _apPass.length() >= 8 ? "加密" : "开放");
  }
  return true;
}

// 初始化：读 NVS -> 组候选 -> 启动连接状态机
void WifiManager::begin() {
  if (!_mutex) _mutex = xSemaphoreCreateMutex();

  lock();
  loadFromNvs();
  loadApPass();   // AP 热点密码：NVS 优先于编译期默认
  // P3：首次启动（NVS 中完全没有 WiFi 配置键）且编译期内置了默认账号时，把该默认
  // 账号作为一条保存项写入 NVS（入库），使其持久化、在网页可见并可编辑，而非每次
  // 启动仅临时拼进候选。已写过的设备（NVS 有键）不再回填，尊重用户后续清空/修改。
  if (!_nvsHadKey && cfg::kWifiSsidDefault[0] && _entryCount == 0) {
    if (_entryCount < kMaxEntries) {
      _entries[_entryCount].ssid = cfg::kWifiSsidDefault;
      _entries[_entryCount].pass = cfg::kWifiPassDefault;
      _entryCount++;
      if (persistToNvs())
        _log.addf(LogLevel::kInfo, "wifi",
                  "NVS 无 WiFi 配置，已将编译期默认账号 '%s' 入库",
                  cfg::kWifiSsidDefault);
      else
        _log.addf(LogLevel::kWarn, "wifi",
                  "编译期默认账号入库失败（写入 NVS 失败）");
    }
  }
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
// kAp:         每 kWifiApRetryMs 后台静默重试 STA，连上即关闭热点自愈；否则等待网页干预
void WifiManager::tick() {
  lock();
  switch (_mode) {
    case WifiMode::kConnecting:
      if (connected()) {
        if (_apUp) {  // 可能来自 AP 后台重试 / 网页显式试连：连上即关闭配网热点
          if (WiFi.softAPgetStationNum() == 0) {
            WiFi.softAPdisconnect(false);  // 关闭 AP，保留 STA
            _apUp = false;
            _log.addf(LogLevel::kInfo, "wifi", "已关闭配网热点");
          } else {
            // 仍有客户端在 AP 上配置，暂保留热点；待其离开后由 kSta 分支关闭
            _log.addf(LogLevel::kInfo, "wifi",
                      "AP 仍被占用（有客户端在配置），暂保留热点");
          }
        }
        _mode = WifiMode::kSta;
        _lostMs = millis();
        _lastGoodSsid = WiFi.SSID();  // 记录上次成功网络，优先重连
        _log.addf(LogLevel::kInfo, "wifi", "WiFi 已连接，IP %s", ip().c_str());
        unlock();
        if (_onConnect) _onConnect();  // 锁外触发：回调可能启动 NTP 等耗时操作
        return;
      }
      if (millis() - _attemptMs >= cfg::kWifiConnectTimeoutMs) {
        _log.addf(LogLevel::kWarn, "wifi", "连接 '%s' 超时",
                  _cand[_candPos].ssid.c_str());
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
      // 若此前因 AP 被占用而保留了热点（用户配置中途 STA 重连成功），
      // 待客户端离开后在此关闭热点，回收 AP 资源。
      if (_apUp && WiFi.softAPgetStationNum() == 0) {
        WiFi.softAPdisconnect(false);
        _apUp = false;
        _log.addf(LogLevel::kInfo, "wifi", "客户端已离开，已关闭配网热点");
      }
      if (!connected()) {
        if (millis() - _lostMs >= cfg::kWifiLostRetryMs) {
          _log.addf(LogLevel::kWarn, "wifi", "WiFi 链路断开，重新尝试候选网络");
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

    case WifiMode::kAp: {
      // AP 配网模式：后台静默重试 STA，连上即关闭热点自愈回到 kSta；
      // 失败则回到 AP 等待下一轮，避免路由器恢复后设备卡在开放热点。
      // 若本机热点上有客户端在配置，暂停后台重试与自动关热点，避免打断会话。
      uint8_t staNum = WiFi.softAPgetStationNum();
      if (connected()) {  // 网页显式试连已成功，或后台重试命中
        if (staNum > 0) {
          // 用户还在 AP 上配置：暂不自愈关闭热点，刷新计时等其离开后再处理
          _apRetryMs = millis();
          break;
        }
        WiFi.softAPdisconnect(false);  // 关闭 AP，保留 STA
        _apUp = false;
        _mode = WifiMode::kSta;
        _lostMs = millis();
        _lastGoodSsid = WiFi.SSID();
        _log.addf(LogLevel::kInfo, "wifi",
                  "AP 后台重连成功，已关闭配网热点，IP %s", ip().c_str());
        unlock();
        if (_onConnect) _onConnect();  // 锁外触发：回调可能启动 NTP 等耗时操作
        return;
      }
      if (staNum == 0 && millis() - _apRetryMs >= cfg::kWifiApRetryMs) {
        _apRetryMs = millis();
        _explicitTry = false;  // 后台重试按普通失败处理：失败则回到 AP 等待
        rebuildCandidates();
        _candPos = 0;
        _round = 0;
        _mode = WifiMode::kConnecting;
        startNextAttempt();  // AP_STA 双栈，重试时 AP 仍对外提供服务
      }
      break;
    }
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

// 导出全部 WiFi 配置（含密码，供备份用）到给定 JSON 对象
void WifiManager::exportAllTo(JsonObject &o) const {
  lock();
  o["count"] = _entryCount;
  JsonArray arr = o["list"].to<JsonArray>();
  for (uint8_t i = 0; i < _entryCount; i++) {
    JsonObject e = arr.add<JsonObject>();
    e["ssid"] = _entries[i].ssid;
    e["pass"] = _entries[i].pass;
  }
  o["apPass"] = _apPass;  // AP 热点密码（导出含明文，供备份/迁移）
  unlock();
}

// 批量导入（覆盖式）：清空现有列表，按 JSON 对象重建并落盘
bool WifiManager::importAllFrom(const JsonObject &o) {
  lock();
  _entryCount = 0;
  for (JsonObject e : o["list"].as<JsonArray>()) {
    if (_entryCount >= kMaxEntries) break;   // 超容量忽略
    const char *s = e["ssid"] | "";
    if (!*s) continue;                        // 空 SSID 跳过
    _entries[_entryCount].ssid = s;
    _entries[_entryCount].pass = e["pass"] | "";
    _entryCount++;
  }
  // AP 热点密码（导入同处理：合规才采用，否则保持原值/开放）
  const char *ap = o["apPass"] | "";
  if (ap[0] && validApPass(ap)) _apPass = ap;
  bool persisted = persistToNvs() && persistApPass();
  unlock();
  if (persisted) {
    _log.addf(LogLevel::kInfo, "wifi", "已从备份导入 %u 个 WiFi 配置",
              (unsigned)_entryCount);
  } else {
    _log.addf(LogLevel::kError, "wifi", "WiFi 配置导入落盘失败");
  }
  return persisted;
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
    _log.addf(LogLevel::kInfo, "wifi", "已保存 '%s'（共 %u 个）", ssid.c_str(),
              (unsigned)_entryCount);
  }
  if (persisted && connectNow) requestConnectLocked(ssid, pass);
  unlock();

  if (!persisted) _log.addf(LogLevel::kError, "wifi", "WiFi 配置保存失败");
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
  _log.addf(LogLevel::kInfo, "wifi", "已删除 #%u（剩余 %u 个）", (unsigned)index,
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
  _log.addf(LogLevel::kInfo, "wifi", "已调整优先级：#%u -> #%u", (unsigned)from,
            (unsigned)to);
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
  doc["apSecure"] = apSecure();              // 热点是否加密（供前端提示）
  doc["apPass"] = _apPass;                   // AP 密码（限已认证管理员可见）
  doc["apSta"] = WiFi.softAPgetStationNum();  // 当前连在热点上的客户端数（用于网页提示）
  doc["count"] = _entryCount;
  String out;
  serializeJson(doc, out);
  unlock();
  return out;
}
