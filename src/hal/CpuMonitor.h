#pragma once

#include <Arduino.h>

// CPU 使用率估算：基于 FreeRTOS 运行时间统计（需在 sdkconfig 中启用
// CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS 与 RUN_TIME_STATS_USING_ESP_TIMER，
// 见项目根 sdkconfig.defaults）。遍历各任务 ulRunTimeCounter，空闲任务
// （IDLE0 / IDLE1）累计占比即空闲率，使用率 = 100 - 空闲率。
// 未启用统计时返回 -1（前端展示「不可用」）。
class CpuMonitor {
 public:
  // 当前整机 CPU 使用率（%），范围 0~100；不可用时返回 -1.0f
  static float usage();
};
