#include "time/TimeSync.h"

#include "serial/SerialBridge.h"
#include "sms/SmsService.h"

#include <Arduino.h>
#include <Preferences.h>
#include <esp_sntp.h>
#include <time.h>

#include "log/LogStore.h"

namespace {

// 本模组日志出口：全局日志环（串口由 LogStore 统一镜像输出）
LogStore &_log = devLog();

}  // namespace

TimeSync *TimeSync::sSelf = nullptr;

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
  // POSIX TZ 的偏移符号与 UTC 偏移相反：UTC+8 写 "UTC-8"，UTC-5 写 "UTC+5"。
  // 格式 <name>[+|-]hh[:mm]；不带 DST 规则（设备不做夏令时切换），分钟数
  // 支持 +05:30 / +05:45 这类半时区与 45 分钟时区。
  int posix = -_tzOffsetMin;
  int mag = posix < 0 ? -posix : posix;
  int h = mag / 60, m = mag % 60;
  char tz[24];
  if (posix == 0) snprintf(tz, sizeof(tz), "UTC0");
  else if (m) snprintf(tz, sizeof(tz), "UTC%c%d:%02d", posix > 0 ? '+' : '-', h, m);
  else snprintf(tz, sizeof(tz), "UTC%c%d", posix > 0 ? '+' : '-', h);
  setenv("TZ", tz, 1);
  tzset();
}

String TimeSync::timezoneText() const {
  int off = _tzOffsetMin;
  int mag = off < 0 ? -off : off;
  char buf[20];
  snprintf(buf, sizeof(buf), "UTC%c%02d:%02d", off >= 0 ? '+' : '-', mag / 60, mag % 60);
  return String(buf);
}

void TimeSync::loadTimezone() {
  // 首次启动（NVS 无 tz 键）或值越界时，把默认东八区显式写回 NVS，
  // 而不是只留在内存里：配置导出 / 外部工具读 NVS 都能看到确定的值，
  // Web 设置页一开始也有明确的选中项。写入失败不致命，内存默认值仍生效。
  Preferences prefs;
  int v = cfg::kTzOffsetMinDefault;
  if (prefs.begin(cfg::kNvsNamespace, false)) {
    bool missing = !prefs.isKey(cfg::kNvsKeyTz);
    v = prefs.getInt(cfg::kNvsKeyTz, cfg::kTzOffsetMinDefault);
    if (missing || v < cfg::kTzOffsetMinMin || v > cfg::kTzOffsetMinMax) {
      v = cfg::kTzOffsetMinDefault;
      prefs.putInt(cfg::kNvsKeyTz, v);
    }
    prefs.end();
  }
  _tzOffsetMin = (int16_t)v;
  applyTimezone();
}

bool TimeSync::saveTimezone() const {
  Preferences prefs;
  if (!prefs.begin(cfg::kNvsNamespace, false)) return false;
  size_t n = prefs.putInt(cfg::kNvsKeyTz, _tzOffsetMin);
  prefs.end();
  return n > 0;
}

bool TimeSync::setTimezone(int minutes) {
  int v = minutes;
  if (v < cfg::kTzOffsetMinMin) v = cfg::kTzOffsetMinMin;
  if (v > cfg::kTzOffsetMinMax) v = cfg::kTzOffsetMinMax;
  _tzOffsetMin = (int16_t)v;
  applyTimezone();       // 立即生效：所有 localtime_r / strftime 改用新时区
  bool ok = saveTimezone();
  String t = timezoneText();
  _log.addf(LogLevel::kInfo, "time", "时区已设为 %s（UTC 偏移 %d 分钟）%s",
            t.c_str(), (int)_tzOffsetMin, ok ? "" : "，NVS 写入失败");
  return ok;
}

void TimeSync::exportTo(JsonObject &o) const {
  o["tzOffsetMin"] = _tzOffsetMin;
  o["tzText"] = timezoneText();
}

bool TimeSync::importFrom(const JsonObject &o) {
  // 旧备份没有时区节、或字段类型异常时保持当前值（默认东八区），不视为导入失败
  JsonVariant v = o["tzOffsetMin"];
  if (v.isNull() || !v.is<int>()) return true;
  return setTimezone(v.as<int>());
}

uint32_t TimeSync::stamp() {
  return sSelf ? sSelf->epochOrUptime() : (uint32_t)(millis() / 1000);
}

void TimeSync::begin(SerialBridge &bridge, SmsService &sms) {
  _bridge = &bridge;
  _sms = &sms;
  sSelf = this;  // SNTP 回调跳板取回实例（全局仅一份，由 App 单例持有）

  loadTimezone();    // 必须先于 restoreFromNvs()：后者写时钟后会重新应用 TZ
  restoreFromNvs();

  _lastPersistMs = millis();
  // 先给 WiFi/NTP 一个优先窗口（kSimTimeRetryMs），期间若 NTP 成功则不碰
  // 系统时钟；窗口内模组就绪事件仍可即时触发一次 SIM 同步
  _simRetryNext = millis() + cfg::kSimTimeRetryMs;
  String t = timezoneText();
  _log.addf(LogLevel::kInfo, "time", "时间模组已初始化（NTP 为主，SIM NITZ 兜底，%s）",
            t.c_str());
}

void TimeSync::syncNTP() {
  configTime((long)_tzOffsetMin * 60, 0, "ntp.aliyun.com", "ntp.ntsc.ac.cn",
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
  String t = timezoneText();
  _log.addf(LogLevel::kInfo, "time", "NTP %s同步成功（%s）：%ld",
            first ? "首次" : "周期", t.c_str(), (long)tv->tv_sec);
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

  // 模组就绪事件的即时请求（NTP 未成功时执行一次）
  if (_simReqPort) {
    uint8_t port = _simReqPort;
    _simReqPort = 0;
    if (!_ntpOk) syncFromSIM(port);
  }

  // SIM NITZ 周期重试：NTP 未成功且不在等待中时，每 kSimTimeRetryMs 挑一个
  // 就绪模组试一次（NTP 成功后由 _ntpOk 终结重试；WiFi 重连会重启 NTP）
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

  // 内部流量以 kBg 类型记入 AT 历史（控制台可见但仍与用户命令区分）。
  // 用 trySend 非阻塞取锁：若端口正忙（如正在发送短信）立即跳过本次同步，下个
  // 周期再试——避免阻塞主循环（否则会与统一发送队列的 worker 抢端口锁数十秒，
  // 触发任务看门狗复位）。
  SerialBridge::Result r;
  if (!_bridge->trySend(port, cfg::kUartBaud, "AT+CCLK?", SerialBridge::kCrlf,
                        cfg::kSimTimeCmdMs, AtHistory::Type::kBg, r)) {
    return;
  }
  if (!r.ok) return;

  int cclkIdx = r.resp.indexOf("+CCLK:");
  if (cclkIdx < 0) {
    _log.addf(LogLevel::kWarn, "time", "模组 %u 的 AT+CCLK? 无响应", (unsigned)port);
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

  // 本设备模组（SIMCOM 系列）在 +CCLK 里给出的墙钟字段其实是 UTC，tz 字段
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
  _log.addf(LogLevel::kInfo, "time", "已从模组 %u 的 NITZ 同步时间：%s",
            (unsigned)port, ts.c_str());
}

void TimeSync::restoreFromNvs() {
  Preferences prefs;
  if (!prefs.begin(cfg::kNvsNamespace, true)) return;  // 只读方式打开
  uint32_t ts = prefs.getUInt(cfg::kNvsKeyLastTs, 0);
  prefs.end();
  if (ts < cfg::kEpochFloorSec) return;  // 无记录或异常值，不恢复

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
  if (now < 0 || (uint32_t)now < cfg::kEpochFloorSec) return;  // 时钟未真正同步，不写

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
  // 时钟未同步 / 仍是 1970 附近的小值时，不要拼出一个看似真实的 1970 日期
  if (!_synced || now < 0 || (uint32_t)now < cfg::kEpochFloorSec) return "未知";
  struct tm t;
  localtime_r(&now, &t);
  char buf[32];
  // 统一输出 "yyyy-MM-dd HH:mm:ss"（不含时区偏移）
  strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &t);
  return String(buf);
}

String TimeSync::clockStr() const {
  time_t now = time(nullptr);
  if (!_synced || now < 0 || (uint32_t)now < cfg::kEpochFloorSec) return "--:--:--";
  struct tm t;
  localtime_r(&now, &t);
  char buf[16];
  strftime(buf, sizeof(buf), "%H:%M:%S", &t);
  return String(buf);
}
