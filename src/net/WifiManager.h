#pragma once

#include <Arduino.h>
#include <Preferences.h>
#include <WiFi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <stdint.h>

#include "app/Config.h"

// WiFi 管理器工作模式
enum class WifiMode {
  kConnecting,  // 正在按候选列表依次尝试连接
  kSta,         // 已连接（STA）
  kAp,          // 候选全部失败，进入 AP 配网模式
};

// 一条 WiFi 配置（NVS 中的最小存储单元）
struct WifiEntry {
  String ssid;  // 热点名
  String pass;  // 密码（开放网络为空串）
};

// WiFi 管理器：
// 1. 启动时按顺序尝试「编译期默认账号 + NVS 中保存的配置」，失败
//    kWifiAttemptRounds 轮后切换 AP 配网模式（网页用 admin/admin123 保护）；
// 2. NVS 中以 JSON 对象按功能保存 WiFi 配置，最多 kWifiMaxNetworks 组；
// 3. 提供扫描 / 增删改排序 / 立即试连能力，全部走非阻塞状态机；
// 4. 公开方法可能被 Web 任务调用，内部用互斥量与 loop 任务的 tick() 互斥。
class WifiManager {
 public:
  // 读取 NVS 配置并启动连接状态机（非阻塞，立即返回）
  void begin();

  // 推进连接/重连状态机；由主循环周期调用
  void tick();

  // ---------------- 状态查询 ----------------
  WifiMode mode() const { return _mode; }

  // 是否处于 AP 配网模式（此时网页需要认证）
  bool apMode() const { return _mode == WifiMode::kAp; }

  // AP 接口是否正在工作（决定网页是否开启认证）
  bool apActive() const { return _apUp; }

  // STA 是否已连接
  bool connected() const { return WiFi.status() == WL_CONNECTED; }

  // 当前 IP：已连返回 STA IP，AP 模式返回 192.168.4.1，否则 0.0.0.0
  String ip() const;

  // 当前网络名：已连返回 SSID，AP 模式返回 AP 名，否则空串
  String ssid() const;

  // 信号强度 dBm；未连接返回 0
  int8_t rssi() const { return connected() ? WiFi.RSSI() : 0; }

  // ---------------- 配置管理（含 NVS 持久化） ----------------
  // 已保存配置的 JSON：{"count":n,"list":[{"i":序号,"ssid":"..","hasPass":true},..]}
  // 密码不回传前端，避免泄露
  String listJson() const;

  // 新增或更新一条配置（同 SSID 覆盖密码）并落盘；
  // ssid：热点名；pass：密码；connectNow：true 表示立即尝试连接
  // 保存成功返回 true
  bool save(const String &ssid, const String &pass, bool connectNow = true);

  // 按序号删除一条配置并落盘；index：列表下标（0 起）；成功返回 true
  bool remove(uint8_t index);

  // 调整启动尝试顺序：把 from 处条目移动到 to；成功返回 true
  bool move(uint8_t from, uint8_t to);

  // 用给定账号立即尝试连接（不落盘），用于"先试连再保存"
  // ssid/pass：账号密码；结果通过 mode()/connected() 观察
  void requestConnect(const String &ssid, const String &pass);

  // ---------------- 扫描 ----------------
  // 启动一次异步扫描；返回 false 表示已有扫描在进行
  bool startScan();

  // 扫描是否仍在进行
  bool scanBusy() const { return WiFi.scanComplete() == WIFI_SCAN_RUNNING; }

  // 扫描结果 JSON 数组：[{"ssid":"..","rssi":n,"secure":true},..]
  // 按 SSID 去重（保留信号最强的一条）；调用后释放扫描结果缓存，
  // 无结果返回 []
  String scanJson();

  // ---------------- 状态 JSON ----------------
  // {"mode":"connecting|sta|ap","connected":bool,"ssid":"..","ip":"..",
  //  "rssi":n,"ap":bool,"apSsid":"..","count":保存的配置数}
  String statusJson() const;

 private:
  // 从 NVS 读取 JSON 配置填充 _entries；读取/解析失败按空处理
  void loadFromNvs();

  // 把 _entries 序列化为 JSON 写入 NVS；写入成功返回 true
  bool persistToNvs() const;

  // 组装候选连接列表：编译期默认账号置顶，其后为 NVS 配置，按 SSID 去重
  void rebuildCandidates();

  // 用当前候选 _candPos 发起一次连接尝试（非阻塞）
  void startNextAttempt();

  // 显式试连失败后的回退：回到 NVS 候选重新开始，而不是直接跳 AP
  void restartAttempts();

  // 进入 AP 配网模式：AP+STA 双栈，开放热点，网页需认证
  void enterApMode();

  // requestConnect 的加锁内部实现（save 持锁调用，避免可重入死锁）
  void requestConnectLocked(const String &ssid, const String &pass);

  // 取/放成员互斥锁（保护 Web 任务与 loop 任务之间的共享状态）
  // 声明为 const：允许 listJson()/statusJson() 等只读方法持锁
  void lock() const { xSemaphoreTake(_mutex, portMAX_DELAY); }
  void unlock() const { xSemaphoreGive(_mutex); }

  static constexpr uint8_t kMaxEntries = cfg::kWifiMaxNetworks;  // 保存配置上限
  static constexpr uint8_t kMaxCandidates = kMaxEntries + 1;     // 候选上限 = 保存数 + 编译期默认

  WifiEntry _entries[kMaxEntries];    // NVS 保存的配置（有序，启动按序尝试）
  uint8_t _entryCount = 0;            // 有效配置条数

  WifiEntry _cand[kMaxCandidates];    // 候选连接列表（默认账号 + NVS 配置）
  uint8_t _candCount = 0;             // 候选条数
  uint8_t _candPos = 0;               // 当前候选下标
  uint8_t _round = 0;                 // 已失败的整轮数
  uint32_t _attemptMs = 0;            // 当前候选尝试的起始时间戳
  uint32_t _lostMs = 0;               // STA 掉线时刻（用于重连节流）

  WifiMode _mode = WifiMode::kConnecting;  // 当前工作模式
  bool _apUp = false;                      // AP 接口是否已启动
  bool _explicitTry = false;               // 是否为网页显式试连（失败回退而非进 AP）

  SemaphoreHandle_t _mutex = nullptr;      // 共享状态互斥量
};
