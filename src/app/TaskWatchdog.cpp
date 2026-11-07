#include "app/TaskWatchdog.h"

#include <Arduino.h>
#include <Preferences.h>
#include <esp_attr.h>
#include <esp_err.h>
#include <esp_system.h>
#include <esp_task_wdt.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <stdio.h>
#include <string.h>

#include "app/Config.h"
#include "log/LogStore.h"
#include "ota/Ota.h"

namespace {

LogStore &_log = devLog();

constexpr uint32_t kMagic = 0x57445431u;  // "WDT1"：RTC 证据有效性标记
constexpr uint8_t kMaxSlots = cfg::kWdtMaxTasks;
constexpr uint8_t kNoSlot = 0xFF;
constexpr size_t kNameLen = 16;

// WDT 超时 ISR 与下次开机之间唯一的信道：RTC_NOINIT 在软件/panic 重启后保留，
// 断电丢失（断电场景由 NVS 里的历史记录兜底）。ISR 只写数值，开机才展开文本。
struct Evidence {
  uint32_t magic;
  uint32_t magicInv;
  uint32_t newestBeatMs;        // 触发时最近一次心跳（近似触发时刻，ms）
  uint32_t staleThresholdMs;    // 判定「心跳落后」的阈值
  uint32_t lastBeatMs[kMaxSlots];
  uint8_t monMask;              // 哪些槽位是受监控任务
  uint8_t staleMask;            // 触发时心跳明显落后的槽位
  char names[kMaxSlots][kNameLen];  // 槽位任务名（重启后 DRAM 里的槽位表已丢）
};
RTC_NOINIT_ATTR static Evidence s_ev;

struct Slot {
  TaskHandle_t handle = nullptr;
  const char *name = nullptr;   // 静态字符串（任务名常量）
  uint32_t lastFeedMs = 0;      // 上次真正喂狗的时刻（节流用）
  bool monitored = false;       // 是否显式登记的关键常驻任务
};
static Slot s_slots[kMaxSlots];
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;

static bool s_enabled = false;
static uint32_t s_timeoutMs = cfg::kWdtTimeoutMs;
static volatile uint8_t s_testSlot = kNoSlot;  // 自检：停止该槽位的喂狗

static String s_resetName, s_resetText;       // 本次开机复位原因
static String s_lastWdtTask, s_lastWdtVersion;  // 最近一次 WDT 事件（NVS 持久）
static uint32_t s_lastWdtUptimeSec = 0;
static uint32_t s_wdtCount = 0;
static bool s_bootWasWdt = false;

// 复位原因枚举名 / 中文说明。ESP_RST_TASK_WDT 由 IDF 在 TWDT panic 时通过
// esp_reset_reason_set_hint() 写入，开机可直接据此区分「看门狗重启」与其它 panic。
void describeReset(esp_reset_reason_t r, const char *&name, const char *&text) {
  switch (r) {
    case ESP_RST_POWERON:    name = "POWERON";    text = "上电复位"; break;
    case ESP_RST_EXT:        name = "EXT";        text = "外部引脚复位"; break;
    case ESP_RST_SW:         name = "SW";         text = "软件重启（主动/OTA）"; break;
    case ESP_RST_PANIC:      name = "PANIC";      text = "panic 崩溃"; break;
    case ESP_RST_INT_WDT:    name = "INT_WDT";    text = "中断看门狗超时"; break;
    case ESP_RST_TASK_WDT:   name = "TASK_WDT";   text = "任务看门狗超时"; break;
    case ESP_RST_WDT:        name = "WDT";        text = "其它看门狗超时"; break;
    case ESP_RST_DEEPSLEEP:  name = "DEEPSLEEP";  text = "深度睡眠唤醒"; break;
    case ESP_RST_BROWNOUT:   name = "BROWNOUT";   text = "欠压复位"; break;
    case ESP_RST_SDIO:       name = "SDIO";       text = "SDIO 复位"; break;
    case ESP_RST_USB:        name = "USB";        text = "USB 复位"; break;
    case ESP_RST_JTAG:       name = "JTAG";       text = "JTAG 复位"; break;
    case ESP_RST_EFUSE:      name = "EFUSE";      text = "eFuse 错误复位"; break;
    case ESP_RST_PWR_GLITCH: name = "PWR_GLITCH"; text = "电源毛刺复位"; break;
    case ESP_RST_CPU_LOCKUP: name = "LOCKUP";     text = "CPU 锁死复位"; break;
    default:                 name = "UNKNOWN";    text = "未知"; break;
  }
}

// 按当前任务句柄找槽位；不存在则分配（优先空槽，其次复用最旧的匿名槽——
// 一次性任务退出后其句柄会失效，不必永久占位置）。短临界区保护槽位表。
int slotFor(TaskHandle_t h, const char *name, bool monitored) {
  int idx = -1;
  portENTER_CRITICAL(&s_mux);
  for (uint8_t i = 0; i < kMaxSlots; i++) {
    if (s_slots[i].handle == h) { idx = i; break; }
  }
  if (idx < 0) {
    for (uint8_t i = 0; i < kMaxSlots; i++) {
      if (s_slots[i].handle == nullptr) { idx = i; break; }
    }
  }
  if (idx < 0) {
    uint32_t oldest = 0xFFFFFFFFu;
    for (uint8_t i = 0; i < kMaxSlots; i++) {
      if (!s_slots[i].monitored && s_slots[i].lastFeedMs < oldest) {
        oldest = s_slots[i].lastFeedMs;
        idx = i;
      }
    }
    if (idx >= 0) s_slots[idx].name = nullptr;
  }
  if (idx >= 0) {
    if (s_slots[idx].handle == nullptr) s_slots[idx].lastFeedMs = 0;
    s_slots[idx].handle = h;
    if (name) s_slots[idx].name = name;
    if (monitored) s_slots[idx].monitored = true;
  }
  portEXIT_CRITICAL(&s_mux);
  return idx;
}

int monitoredSlotByName(const char *task) {
  if (!task) return -1;
  for (uint8_t i = 0; i < kMaxSlots; i++) {
    if (s_slots[i].monitored && s_slots[i].name && strcmp(s_slots[i].name, task) == 0) {
      return i;
    }
  }
  return -1;
}

}  // namespace

// TWDT 超时 ISR 的用户回调（覆盖 IDF 的弱符号）。运行在中断上下文：
// 只做数值比较与 RTC DRAM 写入，不调用日志 / NVS / FreeRTOS / 浮点运算。
// 判据：所有受监控任务里，心跳落后「最新心跳」达到阈值者即疑似未喂狗的任务；
// 若全部心跳同时停止（CPU 整体挂死）则 staleMask 为 0，开机显示「未能定位」。
extern "C" void IRAM_ATTR esp_task_wdt_isr_user_handler(void) {
  uint8_t mask = s_ev.monMask;
  uint32_t newest = 0;
  for (uint8_t i = 0; i < kMaxSlots; i++) {
    if ((mask & (1u << i)) && s_ev.lastBeatMs[i] > newest) newest = s_ev.lastBeatMs[i];
  }
  const uint32_t threshold = s_ev.staleThresholdMs ? s_ev.staleThresholdMs
                                                   : cfg::kWdtStaleMs;
  uint8_t stale = 0;
  for (uint8_t i = 0; i < kMaxSlots; i++) {
    if (!(mask & (1u << i))) continue;
    if (newest - s_ev.lastBeatMs[i] >= threshold) stale |= (uint8_t)(1u << i);
  }
  s_ev.newestBeatMs = newest;
  s_ev.staleMask = stale;
  s_ev.magic = kMagic;
  s_ev.magicInv = ~kMagic;
}

void TaskWatchdog::begin() {
  const esp_reset_reason_t rr = esp_reset_reason();
  const char *rn = nullptr, *rt = nullptr;
  describeReset(rr, rn, rt);
  s_resetName = rn;
  s_resetText = rt;

  // RTC 证据：magic + 反码双校验，避免断电后 RTC 内容随机被误判为 WDT 事件。
  const bool evValid = (s_ev.magic == kMagic && s_ev.magicInv == ~kMagic);
  s_bootWasWdt = evValid || rr == ESP_RST_TASK_WDT || rr == ESP_RST_INT_WDT ||
                 rr == ESP_RST_WDT;

  uint32_t uptimeSec = 0;
  String staleTasks;
  if (evValid) {
    uptimeSec = s_ev.newestBeatMs / 1000;
    for (uint8_t i = 0; i < kMaxSlots; i++) {
      if (!(s_ev.staleMask & (1u << i))) continue;
      s_ev.names[i][kNameLen - 1] = '\0';
      if (!s_ev.names[i][0]) continue;
      if (staleTasks.length()) staleTasks += ",";
      staleTasks += s_ev.names[i];
    }
  }

  // 历史（最近一次 WDT 事件 / 累计次数）先读回，再按本次复位原因更新。
  // 独立命名空间不随出厂复位清除，便于追溯。
  {
    Preferences prefs;
    if (prefs.begin(cfg::kNvsNamespaceWdt, true)) {
      s_wdtCount = prefs.getUInt(cfg::kNvsKeyWdtCount, 0);
      s_lastWdtTask = prefs.getString(cfg::kNvsKeyWdtTask, "");
      s_lastWdtUptimeSec = prefs.getUInt(cfg::kNvsKeyWdtUptime, 0);
      s_lastWdtVersion = prefs.getString(cfg::kNvsKeyWdtVersion, "");
      prefs.end();
    }
  }
  if (s_bootWasWdt) {
    s_wdtCount++;
    s_lastWdtTask = staleTasks;   // 空串 = 未能定位（心跳同时停止 / 非本模块触发）
    s_lastWdtUptimeSec = uptimeSec;
    s_lastWdtVersion = Ota::version();
  }
  {
    Preferences prefs;
    if (prefs.begin(cfg::kNvsNamespaceWdt, false)) {
      prefs.putString(cfg::kNvsKeyWdtReset, s_resetName);
      prefs.putString(cfg::kNvsKeyWdtReason, s_resetText);
      if (s_bootWasWdt) {
        prefs.putUInt(cfg::kNvsKeyWdtCount, s_wdtCount);
        prefs.putString(cfg::kNvsKeyWdtTask, s_lastWdtTask);
        prefs.putUInt(cfg::kNvsKeyWdtUptime, s_lastWdtUptimeSec);
        prefs.putString(cfg::kNvsKeyWdtVersion, s_lastWdtVersion);
      }
      prefs.end();
    }
  }

  if (s_bootWasWdt) {
    _log.addf(LogLevel::kWarn, "wdt",
              "上次由看门狗重启：%s；卡死任务 %s；开机 %u 秒；累计 %u 次",
              s_resetText.c_str(),
              s_lastWdtTask.length() ? s_lastWdtTask.c_str() : "未能定位",
              (unsigned)uptimeSec, (unsigned)s_wdtCount);
  } else {
    _log.addf(LogLevel::kInfo, "wdt", "上次复位原因：%s（%s）", s_resetText.c_str(),
              s_resetName.c_str());
  }

  // 清掉本次证据，避免下次普通重启重复报同一次 WDT 事件。
  memset(&s_ev, 0, sizeof(s_ev));
  s_ev.staleThresholdMs = cfg::kWdtStaleMs;
  s_testSlot = kNoSlot;
  s_timeoutMs = cfg::kWdtTimeoutMs;

  // 框架开机已初始化 TWDT（默认 5s / panic / 订阅 Idle0+Idle1）。这里只做
  // 运行期重配置：5s 会误杀 SMTP/TLS、发件箱单次投递、CMGS 握手等合法阻塞；
  // 保持 panic=true 才能由 IDF 自动重启并记录 ESP_RST_TASK_WDT 复位提示。
  esp_task_wdt_config_t twdt{};
  twdt.timeout_ms = cfg::kWdtTimeoutMs;
  twdt.idle_core_mask = (1u << portNUM_PROCESSORS) - 1u;
  twdt.trigger_panic = true;
  const esp_err_t err = esp_task_wdt_reconfigure(&twdt);
  if (err != ESP_OK) {
    // 降级：不订阅任何项目任务，保留框架默认的 Idle 监控，绝不误重启。
    s_enabled = false;
    _log.addf(LogLevel::kError, "wdt", "TWDT 重配置失败：%s，显式任务监控未启用",
              esp_err_to_name(err));
    return;
  }
  s_enabled = true;

  // App::begin() 运行在 Arduino 的 loopTask 上下文，先把它登记为受监控任务。
  // 其余常驻任务（uart-pump / sms_send / sms_query）在各自入口自行登记。
  if (!registerCurrent("loopTask")) {
    s_enabled = false;
    return;
  }
  _log.addf(LogLevel::kInfo, "wdt", "任务看门狗已启用：超时 %u 秒，监控 %s",
            (unsigned)(cfg::kWdtTimeoutMs / 1000), monitoredTasks().c_str());
}

bool TaskWatchdog::registerCurrent(const char *name) {
  if (!s_enabled || !name) return false;
  const int idx = slotFor(xTaskGetCurrentTaskHandle(), name, true);
  if (idx < 0) {
    _log.addf(LogLevel::kError, "wdt", "任务 %s 无可用看门狗槽位", name);
    return false;
  }
  // 名字写进 RTC 证据区：panic 重启后 DRAM 槽位表已丢，开机仍要能报出任务名。
  snprintf(s_ev.names[idx], kNameLen, "%s", name);
  s_ev.names[idx][kNameLen - 1] = '\0';
  s_ev.lastBeatMs[idx] = millis();
  portENTER_CRITICAL(&s_mux);
  s_ev.monMask |= (uint8_t)(1u << idx);
  portEXIT_CRITICAL(&s_mux);

  esp_err_t err = esp_task_wdt_status(nullptr);
  if (err != ESP_OK) err = esp_task_wdt_add(nullptr);  // 已订阅则跳过
  if (err != ESP_OK) {
    _log.addf(LogLevel::kError, "wdt", "任务 %s 订阅 TWDT 失败：%s", name,
              esp_err_to_name(err));
    return false;
  }
  _log.addf(LogLevel::kInfo, "wdt", "任务 %s 已注册看门狗", name);
  return true;
}

void TaskWatchdog::feed() {
  if (!s_enabled) return;
  const int idx = slotFor(xTaskGetCurrentTaskHandle(), nullptr, false);
  if (idx < 0) return;
  const uint32_t now = millis();
  // 节流：readResponse 等循环每 2ms 调一次，但 1s 喂一次对 60s 超时绰绰有余。
  if ((uint32_t)(now - s_slots[idx].lastFeedMs) < cfg::kWdtFeedGapMs) return;
  s_slots[idx].lastFeedMs = now;
  if (s_testSlot == idx) return;   // 自检：模拟该任务卡死，不喂狗也不更新心跳
  if (esp_task_wdt_status(nullptr) != ESP_OK) return;  // 未订阅的任务（一次性任务等）
  esp_task_wdt_reset();
  if (s_slots[idx].monitored) s_ev.lastBeatMs[idx] = now;
}

bool TaskWatchdog::enabled() { return s_enabled; }
uint32_t TaskWatchdog::timeoutMs() { return s_timeoutMs; }
const String &TaskWatchdog::resetName() { return s_resetName; }
const String &TaskWatchdog::resetText() { return s_resetText; }
bool TaskWatchdog::bootWasWdt() { return s_bootWasWdt; }
uint32_t TaskWatchdog::wdtResetCount() { return s_wdtCount; }
const String &TaskWatchdog::lastWdtTask() { return s_lastWdtTask; }
uint32_t TaskWatchdog::lastWdtUptimeSec() { return s_lastWdtUptimeSec; }
const String &TaskWatchdog::lastWdtVersion() { return s_lastWdtVersion; }

String TaskWatchdog::monitoredTasks() {
  String out;
  for (uint8_t i = 0; i < kMaxSlots; i++) {
    if (!s_slots[i].monitored || !s_slots[i].name) continue;
    if (out.length()) out += ",";
    out += s_slots[i].name;
  }
  return out;
}

bool TaskWatchdog::startSelfTest(const char *task, uint32_t timeoutMs) {
  if (!s_enabled) return false;
  const int idx = monitoredSlotByName(task);
  if (idx < 0) return false;
  if (timeoutMs) {
    if (timeoutMs < cfg::kWdtSelfTestMinMs || timeoutMs > cfg::kWdtTimeoutMs) return false;
    esp_task_wdt_config_t twdt{};
    twdt.timeout_ms = timeoutMs;
    twdt.idle_core_mask = (1u << portNUM_PROCESSORS) - 1u;
    twdt.trigger_panic = true;
    if (esp_task_wdt_reconfigure(&twdt) != ESP_OK) return false;
    s_timeoutMs = timeoutMs;
    s_ev.staleThresholdMs = timeoutMs * 3 / 4;
  }
  s_testSlot = (uint8_t)idx;
  _log.addf(LogLevel::kWarn, "wdt",
            "看门狗自检：任务 %s 停止喂狗，设备将在约 %u 秒后自动重启",
            task, (unsigned)(s_timeoutMs / 1000));
  return true;
}
