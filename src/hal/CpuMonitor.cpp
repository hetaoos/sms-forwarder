#include "hal/CpuMonitor.h"

#include <string.h>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

float CpuMonitor::usage() {
#if defined(configUSE_TRACE_FACILITY) && configUSE_TRACE_FACILITY == 1 && \
    defined(configGENERATE_RUN_TIME_STATS) && configGENERATE_RUN_TIME_STATS == 1
  static TaskStatus_t buf[64];
  UBaseType_t n = uxTaskGetNumberOfTasks();
  if (n > 64) n = 64;
  UBaseType_t got = uxTaskGetSystemState(buf, n, nullptr);
  if (got == 0) return -1.0f;
  uint64_t idle = 0, total = 0;
  for (UBaseType_t i = 0; i < got; ++i) {
    total += (uint64_t)buf[i].ulRunTimeCounter;
    if (strncmp(buf[i].pcTaskName, "IDLE", 4) == 0)
      idle += (uint64_t)buf[i].ulRunTimeCounter;
  }
  if (total == 0) return -1.0f;
  float cpu = (float)(100.0 * (1.0 - (double)idle / (double)total));
  if (cpu < 0) cpu = 0;
  if (cpu > 100) cpu = 100;
  return cpu;
#else
  return -1.0f;  // 运行时间统计未启用
#endif
}
