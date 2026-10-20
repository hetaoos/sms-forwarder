#include "time/TimeSync.h"

#include "serial/SerialBridge.h"
#include "sms/SmsService.h"

#include <Arduino.h>
#include <Preferences.h>
#include <esp_sntp.h>
#include <time.h>

#include "log/LogStore.h"

namespace {

// 本通信模组日志出口：全局日志环（串口由 LogStore 统一镜像输出）
LogStore &_log = devLog();

}  // namespace

TimeSync *TimeSync::sSelf = nullptr;

// 纪元时间下限：低于此值视为「未真正同步」（1970 附近的系统时间）
static constexpr uint32_t kEpochFloor = 1600000000u;

// 把「已是 UTC 的墙钟时间」换算成 time_t：纯算术、不依赖全局 TZ 环境变量，
// 因此与其它任务并发调用也安全（不会像 mktime/timegm 那样受 TZ 状态影响）。
static time_t utcWallClockToEpoch(int yy, int mo, int dd, int hh, int mi, int ss) {
  int year = 2000 + yy;  // CCLK 使用两位年 00-99
  static const int dim[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  long days = 0;
  for (int y = 1970; y < year; ++y) {
    days += (y % 4 == 0 && (y % 100 != 0 || y % 400 == 0)) ? 366 : 365;
  }
  bool leap = (year % 4 == 0 && (year % 100 != 0 || year % 400 == 0));
  for (int m = 1; m < mo; ++m) {
    int d = dim[m - 1];
    if (m == 2 && leap) d = 29;
    days += d;
  }
  days += dd - 1;
  return (time_t)(days * 86400LL + hh * 3600LL + mi * 60LL + ss);
}

void TimeSync::applyTimezone() {
  // 固定显示时区 UTC+8（POSIX TZ 符号相反，"UTC-8" 即 UTC+8）
  setenv("TZ", "UTC-8", 1);
  tzset();
}

void TimeSync::begin(SerialBridge &bridge, SmsService &sms) {
  _bridge = &bridge;
  _sms = &sms;
  sSelf = this;  // SNTP 回调跳板取回实例（全局仅一份，由 App 单例持有）

  restoreFromNvs();

  _lastPersistMs = millis();
  // 先给 WiFi/NTP 一个优先窗口（kSimTimeRetryMs），期间若 NTP 成功则不碰
  // 系统时钟；窗口内通信模组就绪事件仍可即时触发一次 SIM 同步
  _simRetryNext = millis() + cfg::kSimTimeRetryMs;
  _log.addf(LogLevel::kInfo, "time", "时间通信模组已初始化（NTP 为主，SIM NITZ 兜底，UTC+8）");
}

void TimeSync::syncNTP() {
  configTime(cfg::kNtpGmtOffsetSec, 0, "ntp.aliyun.com", "ntp.ntsc.ac.cn",
             "pool.ntp.org");
  applyTimezone();
  // 后台周期校准：首次成功后 SNTP 任务按此间隔自动重新对时，系统时钟长期精准
  esp_sntp_set_sync_interval(cfg::kNtpResyncMs);
  // 注册同步回调：首次成功与之后每次周期校准都会触发
  esp_sntp_set_time_sync_notification_cb(sntpThunk);
  // 仅启动 SNTP，不在此处阻塞等待；完成判定由 onNtpSynced 负责，
  // tick() 另做 kNtpTimeoutMs 超时兜底（转 SIM NITZ）
  _ntpPending = true;
  _ntpFailed = false;
  _ntpStartMs = millis();
  _log.addf(LogLevel::kInfo, "time", "NTP 同步已启动，等待 SNTP 回调");
}

void TimeSync::sntpThunk(struct timeval *tv) {
  if (sSelf) sSelf->onNtpSynced(tv);
}

void TimeSync::onNtpSynced(struct timeval *tv) {
  bool first = !_synced;
  _ntpPending = false;
  _ntpOk = true;
  _synced = true;
  _src = TimeSrc::kNtp;
  // SNTP 内部通过 settimeofday 写时钟，可能重置 libc TZ 状态，必须补调
  applyTimezone();
  if (first) _persistDue = true;  // 首次成功立即落盘，保证重启后有个较新的近似值
  if (first) {
    _log.addf(LogLevel::kInfo, "time", "NTP 首次同步成功（UTC+8）：%ld",
              (long)tv->tv_sec);
  } else {
    _log.addf(LogLevel::kInfo, "time", "NTP 周期同步成功（UTC+8）：%ld",
              (long)tv->tv_sec);
  }
}

void TimeSync::requestSimSync(uint8_t port) {
  if (port < 1 || port > cfg::kSmsMaxModems) return;
  if (_ntpOk) return;    // NTP 权威源已就绪，无需 SIM 兜底
  _simReqPort = port;    // 仅置标记；真正的 AT+CCLK? 在 tick()（loop 任务）执行
}

void TimeSync::tick() {
  uint32_t now = millis();

  // NTP 等待超时：放弃本次启动，转 SIM NITZ 兜底（WiFi 重连时会再次启动 NTP）
  if (_ntpPending && now - _ntpStartMs >= cfg::kNtpTimeoutMs) {
    _ntpPending = false;
    _ntpFailed = true;
    _log.addf(LogLevel::kWarn, "time", "NTP 同步超时，转 SIM NITZ 兜底");
  }

  // 通信模组就绪事件的即时请求（NTP 未成功时执行一次）
  if (_simReqPort) {
    uint8_t port = _simReqPort;
    _simReqPort = 0;
    if (!_ntpOk) syncFromSIM(port);
  }

  // SIM NITZ 周期重试：NTP 未成功且不在等待中时，每 kSimTimeRetryMs 挑一个
  // 就绪通信模组试一次（NTP 成功后由 _ntpOk 终结重试；WiFi 重连会重启 NTP）
  if (!_ntpOk && !_ntpPending && now >= _simRetryNext) {
    _simRetryNext = now + cfg::kSimTimeRetryMs;
    uint8_t port = pickReadyModem();
    if (port) syncFromSIM(port);
  }

  // 落盘：首次同步后立即一次，之后每 kTimePersistMs（10 分钟）一次
  if (_persistDue || (_synced && now - _lastPersistMs >= cfg::kTimePersistMs)) {
    persistToNvs();
    _persistDue = false;
    _lastPersistMs = now;
  }
}

uint8_t TimeSync::pickReadyModem() const {
  if (!_sms) return 0;
  for (uint8_t p = 1; p <= cfg::kSmsMaxModems; p++) {
    if (_sms->modemReady(p)) return p;
  }
  return 0;
}

void TimeSync::syncFromSIM(uint8_t port) {
  if (_ntpOk) return;  // NTP 权威，SIM 不覆盖
  if (!_bridge) return;
  if (port < 1 || port > cfg::kSmsMaxModems) return;

  // quiet=true：内部流量不进 AT 历史（控制台只展示用户主动命令）
  SerialBridge::Result r =
      _bridge->send(port, cfg::kUartBaud, "AT+CCLK?", SerialBridge::kCrlf,
                    cfg::kSimTimeCmdMs, true);
  if (!r.ok) return;

  int cclkIdx = r.resp.indexOf("+CCLK:");
  if (cclkIdx < 0) {
    _log.addf(LogLevel::kWarn, "time", "通信模组 %u 的 AT+CCLK? 无响应", (unsigned)port);
    return;
  }
  int q1 = r.resp.indexOf('"', cclkIdx);
  int q2 = r.resp.indexOf('"', q1 + 1);
  if (q1 < 0 || q2 <= q1) {
    _log.addf(LogLevel::kWarn, "time", "CCLK 响应格式解析失败");
    return;
  }
  String ts = r.resp.substring(q1 + 1, q2);

  int yy, mo, dd, hh, mi, ss, tz = 0;
  int parsed =
      sscanf(ts.c_str(), "%d/%d/%d,%d:%d:%d%d", &yy, &mo, &dd, &hh, &mi, &ss, &tz);
  if (parsed < 6) {
    _log.addf(LogLevel::kWarn, "time", "CCLK 时间字段解析失败：%s", ts.c_str());
    return;
  }
  if (yy < 0 || yy > 99 || mo < 1 || mo > 12 || dd < 1 || dd > 31 || hh < 0 ||
      hh > 23 || mi < 0 || mi > 59 || ss < 0 || ss > 60) {
    _log.addf(LogLevel::kWarn, "time", "CCLK 时间字段越界：%s", ts.c_str());
    return;
  }
  if (tz < -56 || tz > 56) tz = 0;  // 时区字段异常按 0 处理（不参与换算）

  // 本设备通信模组（SIMCOM 系列）在 +CCLK 里给出的墙钟字段其实是 UTC，tz 字段
  // （如 +32 即 UTC+8）只是本地偏移，并非「墙钟=本地时间」的 3GPP 标准形式。
  // 因此把墙钟直接当作 UTC 转成 epoch（手算，不碰全局 TZ）。
  time_t utcTime = utcWallClockToEpoch(yy, mo, dd, hh, mi, ss);
  if (utcTime < 0) {
    _log.addf(LogLevel::kWarn, "time", "CCLK 时间换算失败，放弃本次同步");
    return;
  }

  struct timeval tv = {utcTime, 0};
  settimeofday(&tv, nullptr);
  applyTimezone();  // settimeofday 可能重置 libc TZ

  bool first = !_synced;
  _synced = true;
  _src = TimeSrc::kSim;
  if (first) _persistDue = true;
  _log.addf(LogLevel::kInfo, "time", "已从通信模组 %u 的 NITZ 同步时间：%s",
            (unsigned)port, ts.c_str());
}

void TimeSync::restoreFromNvs() {
  Preferences prefs;
  if (!prefs.begin(cfg::kNvsNamespace, true)) return;  // 只读方式打开
  uint32_t ts = prefs.getUInt(cfg::kNvsKeyLastTs, 0);
  prefs.end();
  if (ts < kEpochFloor) return;  // 无记录或异常值，不恢复

  struct timeval tv = {(time_t)ts, 0};
  settimeofday(&tv, nullptr);
  applyTimezone();
  _synced = true;  // 近似时间即可用（NTP/SIM 成功后会被校准覆盖）
  _src = TimeSrc::kNvs;
  _log.addf(LogLevel::kInfo, "time", "已从 NVS 恢复上次已知时间：%u", (unsigned)ts);
}

void TimeSync::persistToNvs() {
  if (!_synced) return;
  time_t now = time(nullptr);
  if (now < 0 || (uint32_t)now < kEpochFloor) return;  // 时钟未真正同步，不写

  Preferences prefs;
  if (!prefs.begin(cfg::kNvsNamespace, false)) return;  // 读写方式打开
  prefs.putUInt(cfg::kNvsKeyLastTs, (uint32_t)now);
  prefs.end();
}

const char *TimeSync::srcName() const {
  switch (_src) {
    case TimeSrc::kNvs: return "nvs";
    case TimeSrc::kSim: return "sim";
    case TimeSrc::kNtp: return "ntp";
    default: return "none";
  }
}

String TimeSync::dateStr() const {
  time_t now = time(nullptr);
  if (now < 1000000) return "未知";
  struct tm t;
  localtime_r(&now, &t);
  char buf[32];
  // 统一输出 "yyyy-MM-dd HH:mm:ss"（不含时区偏移）
  strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &t);
  return String(buf);
}

String TimeSync::clockStr() const {
  time_t now = time(nullptr);
  if (!_synced || now < 1000000) return "--:--:--";
  struct tm t;
  localtime_r(&now, &t);
  char buf[16];
  strftime(buf, sizeof(buf), "%H:%M:%S", &t);
  return String(buf);
}
