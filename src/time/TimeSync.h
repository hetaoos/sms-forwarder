#pragma once

#include <Arduino.h>
#include <stdint.h>
#include <sys/time.h>
#include <time.h>

#include "app/Config.h"

class SerialBridge;
class SmsService;

// 时间来源（同步源优先级：NTP > SIM NITZ > NVS 恢复）
enum class TimeSrc : uint8_t { kNone = 0, kNvs, kSim, kNtp };

// 时间同步模块：NTP 为主，SIM NITZ（AT+CCLK?）兜底。
//
// - WiFi 连接成功（含重连）时由 WifiManager 回调触发 syncNTP()：启动 SNTP，
//   非阻塞；首次成功与之后的周期校准都由 SNTP 回调通知；
// - NTP 等待超时（kNtpTimeoutMs）或始终无法连接时，转 SIM NITZ 兜底：
//   模组就绪事件触发一次即时尝试，之后每 kSimTimeRetryMs 周期重试；
// - 时间写入系统时钟后统一经 applyTimezone() 固定显示时区 UTC+8；
// - 已同步的时间按 kTimePersistMs（10 分钟）落盘 NVS，重启时恢复近似值，
//   避免重启后一段时间内短信记录全是「开机秒数」。
//
// 短信时间戳经 epochOrUptime() 获取：已同步返回纪元秒，未同步返回开机秒数
// （沿用前端「>= 1.6e9 视为真实纪元时间」的约定）。
class TimeSync {
 public:
  // 注入依赖（引用，生命周期由 App 保证）并从 NVS 恢复上次已知时间
  void begin(SerialBridge &bridge, SmsService &sms);

  // 周期调用（App::tick，loop 任务上下文）：NTP 超时判定、SIM NITZ 重试、
  // 时间落盘。同步用的阻塞 AT 也在此执行，避免拖住抽数任务。
  void tick();

  // WiFi 已连接时调用（重连可重复调用，幂等）：启动 SNTP，非阻塞
  void syncNTP();

  // 模组就绪事件（抽数任务上下文）：请求立即尝试一次 SIM 时间同步。
  // 只置标记，真正的 AT 交互在 tick() 执行，不阻塞抽数任务。
  void requestSimSync(uint8_t port);

  // 时钟是否可用：NTP / SIM 同步过，或开机时从 NVS 恢复过
  bool isSynced() const { return _synced; }

  // NTP 是否至少成功过一次（权威源；此后 SIM 不再覆盖系统时钟）
  bool ntpOk() const { return _ntpOk; }

  // 当前生效的时间来源名称（前端映射中文）：none / nvs / sim / ntp
  const char *srcName() const;

  // 当前纪元秒（已同步）；未同步返回开机秒数——短信时间戳直接使用
  uint32_t epochOrUptime() const {
    return _synced ? (uint32_t)time(nullptr) : (uint32_t)(millis() / 1000);
  }

  // 本地时间字符串 "YYYY-MM-DD HH:mm:ss"；时钟不可用返回 "未知"
  String dateStr() const;

  // 时钟短串 "HH:MM:SS"（24 小时制，供 LCD 标题栏显示）；
  // 时钟不可用返回 "--:--:--"
  String clockStr() const;

  // 重新应用固定显示时区（UTC+8）。ESP-IDF 的 settimeofday() 在某些版本会
  // 重置 libc 的 TZ 状态，导致 localtime_r 退回到默认 UTC；SNTP 完成时底层
  // 也会调用 settimeofday，因此凡是写时钟之后都应再调一次。
  static void applyTimezone();

 private:
  // 经 AT+CCLK? 从模组获取 NITZ 时间并写入系统时钟（NTP 已成功后跳过）
  void syncFromSIM(uint8_t port);

  // 从 NVS 恢复上次落盘的时间（近似值，不改变 _ntpOk）
  void restoreFromNvs();

  // 把当前时间写入 NVS（仅时钟真正同步后）
  void persistToNvs();

  // SNTP 同步完成回调：首次成功与之后每次周期校准都会触发
  void onNtpSynced(struct timeval *tv);

  // 挑一个就绪模组（时间只需要一个可信来源）；无则返回 0
  uint8_t pickReadyModem() const;

  // C 回调跳板（esp_sntp 注册的是函数指针）
  static void sntpThunk(struct timeval *tv);
  static TimeSync *sSelf;  // 供跳板取回实例

  SerialBridge *_bridge = nullptr;  // AT 串口桥（SIM 时间查询）
  SmsService *_sms = nullptr;      // 模组就绪状态查询

  bool _synced = false;        // 时钟可用（任意源，含 NVS 恢复）
  bool _ntpOk = false;         // NTP 至少成功过一次（权威源，SIM 不再覆盖）
  bool _ntpPending = false;    // NTP 已启动、等待 SNTP 回调
  bool _ntpFailed = false;     // NTP 本次启动等待超时（转 SIM 兜底）
  uint32_t _ntpStartMs = 0;    // NTP 启动时刻（超时判定）
  uint32_t _simRetryNext = 0;  // 下次 SIM NITZ 重试时刻
  uint8_t _simReqPort = 0;     // 模组就绪请求的端口（0 = 无待处理请求）
  uint32_t _lastPersistMs = 0; // 上次落盘时刻
  bool _persistDue = false;    // 首次同步后需立即落盘
  TimeSrc _src = TimeSrc::kNone;  // 当前生效的时间来源
};
