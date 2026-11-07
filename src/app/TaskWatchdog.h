#pragma once

#include <Arduino.h>

#include <stdint.h>

// 任务看门狗（ESP-IDF TWDT，底层为硬件定时器）统一接入层。
//
// 目标：把「靠约定不写死循环」变成可验证的保护——关键常驻任务显式订阅
// TWDT 并周期性喂狗；任何任务卡死超过 cfg::kWdtTimeoutMs 即由 panic 自动
// 重启（框架已配置 CONFIG_ESP_TASK_WDT_PANIC 且 panic = print + reboot）。
//
// 触发原因分两级留痕：
//   1. TWDT 超时 ISR 里覆盖弱符号 esp_task_wdt_isr_user_handler()，把各任务
//      最近心跳写入 RTC_NOINIT 内存（软重启保留），推断疑似卡死的任务；
//      该回调在中断上下文，只做 O(1) 数值运算 + RTC 内存写，禁止日志/NVS。
//   2. 下次开机 TaskWatchdog::begin() 结合 esp_reset_reason() 读回，
//      写一条运行日志 + 持久化到 NVS 的 wdt 命名空间（断电也不丢），
//      Web /api/status 的 wdt 字段可查。
//
// 用法：
//   - App::begin() 早期（任何常驻任务创建前）调 begin()：读回原因、重配置
//     TWDT 超时、把 Arduino loopTask 注册进去；
//   - 其余常驻任务入口调 registerCurrent("任务名")；
//   - 各任务循环体、以及 readResponse / 等 '>' / 等 CMGS / 等端口锁这类
//     长阻塞等待处调 feed()（内部按 1s 节流，未订阅 TWDT 的任务调用无副作用）。
class TaskWatchdog {
 public:
  // 开机初始化：读回上次复位/卡死原因、重配置 TWDT、订阅调用者（loopTask）。
  // 须在日志环就绪之后、其它常驻任务创建之前调用一次。
  static void begin();

  // 把当前运行的任务登记为受监控任务（入口处调用一次）。
  // name 必须是生命周期为静态的字符串（通常直接传任务名常量）。
  static bool registerCurrent(const char *name);

  // 喂狗：当前任务已订阅 TWDT 时重置看门狗并更新心跳。
  // 未订阅的任务调用是 no-op，可安全放在多任务共用的阻塞等待循环里。
  static void feed();

  // ---- 状态查询（供 StateJson.h / Web 使用，均为静态数据，无锁读）----
  static bool enabled();                 // TWDT 是否成功重配置并启用
  static uint32_t timeoutMs();           // 当前生效的超时（自检可临时缩短）
  static String monitoredTasks();        // 已登记的常驻任务名，逗号分隔
  static const String &resetName();      // 本次开机复位原因枚举名（TASK_WDT / SW / ...）
  static const String &resetText();      // 本次开机复位原因中文说明
  static bool bootWasWdt();              // 本次开机是否由看门狗超时触发
  static uint32_t wdtResetCount();       // 累计看门狗重启次数（NVS 持久）
  static const String &lastWdtTask();    // 最近一次 WDT 疑似卡死任务（空 = 未能定位）
  static uint32_t lastWdtUptimeSec();    // 最近一次 WDT 触发时的开机秒数
  static const String &lastWdtVersion(); // 最近一次 WDT 时的固件版本

  // 故障注入自检：让指定受监控任务停止喂狗，验证「卡死 -> 自动重启 -> 记原因」。
  // timeoutMs=0 沿用当前超时；非 0 时临时缩短（下限 cfg::kWdtSelfTestMinMs）。
  // !! 会触发整机重启，仅供设备验收使用，Web 端需认证 + 二次确认。
  static bool startSelfTest(const char *task, uint32_t timeoutMs);
};
