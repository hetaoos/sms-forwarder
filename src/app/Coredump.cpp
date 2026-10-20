#include "app/Coredump.h"

#include <Preferences.h>
#include <esp_system.h>
#include <sys/time.h>
#include <time.h>

#include "app/Config.h"
#include "log/LogStore.h"
#include "ota/Ota.h"

namespace {

LogStore &_log = devLog();

// 最近已知墙上时钟：RTC 内存，panic 重启后保留（断电重启丢失），
// 由 App::tick 在时钟已同步时每 10s 更新一次
RTC_DATA_ATTR static time_t s_rtcLastKnownTime = 0;

// 「时钟真正同步过」的纪元秒下限（2020-01-01），与 TimeSync 的 kEpochFloor
// 同量级：低于该值的 time_t 一律视为未同步，不参与崩溃时刻估算
constexpr time_t kEpochFloor = 1577836800L;

}  // namespace

time_t Coredump::s_crashTime = 0;
String Coredump::s_crashVersion = "";

size_t Coredump::scanUsed(const esp_partition_t *part) {
  uint8_t buf[256];
  for (size_t offset = part->size; offset > 0;) {
    size_t chunk = (offset >= sizeof(buf)) ? sizeof(buf) : offset;
    offset -= chunk;
    if (esp_partition_read(part, offset, buf, chunk) != ESP_OK) {
      return 0;
    }
    for (int i = static_cast<int>(chunk) - 1; i >= 0; --i) {
      if (buf[i] != 0xFF) {
        return offset + static_cast<size_t>(i) + 1;
      }
    }
  }
  return 0;
}

const esp_partition_t *Coredump::partition() {
  return esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
                                  ESP_PARTITION_SUBTYPE_DATA_COREDUMP, nullptr);
}

size_t Coredump::usedSize(const esp_partition_t *part) {
  return part ? scanUsed(part) : 0;
}

bool Coredump::hasData() {
  const esp_partition_t *part = partition();
  return part ? scanUsed(part) > 0 : false;
}

void Coredump::updateLastKnownTime(time_t t) {
  if (t < kEpochFloor) return;  // 时钟未真正同步，不记录
  s_rtcLastKnownTime = t;
}

void Coredump::init() {
  // 断电重启时 RTC 已丢：回落到 TimeSync 每 10 分钟落盘的 NVS lastts
  // （近似值，精度到最近一次落盘，足以把崩溃时刻框定在分钟级）
  if (s_rtcLastKnownTime < kEpochFloor) {
    Preferences prefs;
    if (prefs.begin(cfg::kNvsNamespace, true)) {  // 只读方式打开
      uint32_t saved = prefs.getUInt(cfg::kNvsKeyLastTs, 0);
      prefs.end();
      if (static_cast<time_t>(saved) >= kEpochFloor) {
        s_rtcLastKnownTime = static_cast<time_t>(saved);
      }
    }
  }

  // 恢复 NVS 中记录的上次崩溃信息（跨重启保持，直到被新的 panic 覆盖）
  {
    Preferences prefs;
    if (prefs.begin(cfg::kNvsNamespaceCoredump, true)) {  // 只读方式打开
      long ts = prefs.getLong(cfg::kNvsKeyCdCrashTs, 0);
      s_crashVersion = prefs.getString(cfg::kNvsKeyCdCrashVer, "");
      prefs.end();
      if (ts >= static_cast<long>(kEpochFloor)) {
        s_crashTime = static_cast<time_t>(ts);
      }
    }
  }

  // panic 重启：把 RTC 保留的最近时间戳与当前固件版本记为「上次崩溃」。
  // 分区里的 ELF 快照由 ESP-IDF panic 处理器自动写入，这里只记元信息；
  // 时间未知（RTC 丢且 NVS 无记录）时只记版本，崩溃时间留 0。
  if (esp_reset_reason() == ESP_RST_PANIC) {
    s_crashVersion = Ota::version();
    if (s_rtcLastKnownTime >= kEpochFloor) {
      s_crashTime = s_rtcLastKnownTime;
    }
    Preferences prefs;
    if (prefs.begin(cfg::kNvsNamespaceCoredump, false)) {  // 读写方式打开
      prefs.putLong(cfg::kNvsKeyCdCrashTs, static_cast<long>(s_crashTime));
      prefs.putString(cfg::kNvsKeyCdCrashVer, s_crashVersion);
      prefs.end();
    }
    _log.addf(LogLevel::kWarn, "coredump",
              "检测到 panic 重启，崩溃时间 %ld（近似），版本 %s，"
              "请到工具箱导出 coredump",
              static_cast<long>(s_crashTime), s_crashVersion.c_str());
  }
}

time_t Coredump::crashTime() { return s_crashTime; }

String Coredump::crashVersion() { return s_crashVersion; }

String Coredump::elfUrl() {
  if (s_crashVersion.length() == 0) {
    return "";
  }
  // 与 Ota 的产物命名同源：Release tag 即完整版本标识，调试符号名为
  // firmware-<tag>-<platform>.elf（见 script/build_release.py 与 ota/Ota.h）。
  // 早期 Release tag 只到 VERSION、与带时间戳的产物名对不上，拼出来必然 404，
  // 那时只能不提供链接；现在 tag 与产物名一致，恢复链接。
  return String(OTA_RELEASES_BASE_URL) + "/download/" + s_crashVersion + "/" +
         "firmware-" + s_crashVersion + "-" + OTA_PLATFORM + ".elf";
}
