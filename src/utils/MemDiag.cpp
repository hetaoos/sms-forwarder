#include "utils/MemDiag.h"

#include <ArduinoJson.h>
#include <esp_heap_caps.h>
#include <esp_system.h>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "app/Psram.h"  // PsBuffer / psramJsonAllocator()：诊断 JSON 也走 PSRAM（方案 #2）

namespace memdiag {

namespace {

// 字节紧凑格式：>=1MB 显示 "7.80M"，>=1KB 显示 "123.4K"，否则 "512B"
String fmtKb(size_t bytes) {
  char buf[24];
  if (bytes >= (1u << 20)) {
    snprintf(buf, sizeof(buf), "%.2fM", bytes / 1024.0f / 1024.0f);
  } else if (bytes >= 1024) {
    snprintf(buf, sizeof(buf), "%.1fK", bytes / 1024.0f);
  } else {
    snprintf(buf, sizeof(buf), "%uB", (unsigned)bytes);
  }
  return String(buf);
}

}  // namespace

String heapSummary() {
  const size_t total8 = heap_caps_get_total_size(MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL);
  const size_t free8 = heap_caps_get_free_size(MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL);
  const size_t largest8 = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL);
  const size_t dmaFree = heap_caps_get_free_size(MALLOC_CAP_DMA);
  const size_t psTotal = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
  const size_t psFree = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
  const size_t minFree = esp_get_minimum_free_heap_size();

  String s;
  s += "HEAP internal(8BIT): total=" + fmtKb(total8) + " free=" + fmtKb(free8) +
       " used=" + fmtKb(total8 > free8 ? total8 - free8 : 0) +
       " largest=" + fmtKb(largest8) + "\n";
  s += "HEAP DMA free=" + fmtKb(dmaFree) + "\n";
  s += "PSRAM total=" + fmtKb(psTotal) + " free=" + fmtKb(psFree) + "\n";
  s += "min-ever free internal heap=" + fmtKb(minFree) + "\n";
  return s;
}

String taskStackSummary() {
#if (configUSE_TRACE_FACILITY == 1)
  UBaseType_t n = uxTaskGetNumberOfTasks();
  if (n == 0) return String("TASKS: none\n");
  // 用 DRAM 暂存（仅诊断瞬间，释放即回收）；失败则降级返回
  TaskStatus_t *st =
      (TaskStatus_t *)heap_caps_malloc(n * sizeof(TaskStatus_t), MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL);
  if (!st) return String("TASKS: stat alloc failed\n");
  n = uxTaskGetSystemState(st, n, nullptr);

  String s = "TASKS (high-water = 未用栈, 越小越危险):\n";
  for (UBaseType_t i = 0; i < n; i++) {
    const uint32_t hwmBytes = (uint32_t)st[i].usStackHighWaterMark * sizeof(StackType_t);
    s += "  " + String(st[i].pcTaskName) + " prio=" + String(st[i].uxCurrentPriority) +
         " hwm=" + String(hwmBytes) + "B\n";
  }
  heap_caps_free(st);
  return s;
#else
  return String("TASKS: configUSE_TRACE_FACILITY 未开启，无法枚举\n");
#endif
}

void toJson(PsBuffer &out) {
  const size_t total8 = heap_caps_get_total_size(MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL);
  const size_t free8 = heap_caps_get_free_size(MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL);
  const size_t largest8 = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL);
  const size_t dmaFree = heap_caps_get_free_size(MALLOC_CAP_DMA);
  const size_t psTotal = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
  const size_t psFree = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
  const size_t minFree = esp_get_minimum_free_heap_size();

  // 内存池放 PSRAM：tasks 数组可达数十项（方案 #2）
  JsonDocument doc(psramJsonAllocator());
  JsonObject h = doc["heap"].to<JsonObject>();
  h["internalTotal"] = (unsigned)total8;
  h["internalFree"] = (unsigned)free8;
  h["internalUsed"] = (unsigned)(total8 > free8 ? total8 - free8 : 0);
  h["internalLargest"] = (unsigned)largest8;
  h["dmaFree"] = (unsigned)dmaFree;
  h["psramTotal"] = (unsigned)psTotal;
  h["psramFree"] = (unsigned)psFree;
  h["minEverFree"] = (unsigned)minFree;

  JsonArray arr = doc["tasks"].to<JsonArray>();
#if (configUSE_TRACE_FACILITY == 1)
  UBaseType_t n = uxTaskGetNumberOfTasks();
  if (n > 0) {
    // 仅诊断瞬间暂存任务表，用 DRAM（容量小、释放即回收）
    TaskStatus_t *st =
        (TaskStatus_t *)heap_caps_malloc(n * sizeof(TaskStatus_t), MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL);
    if (st) {
      n = uxTaskGetSystemState(st, n, nullptr);
      for (UBaseType_t i = 0; i < n; i++) {
        const uint32_t hwmBytes = (uint32_t)st[i].usStackHighWaterMark * sizeof(StackType_t);
        JsonObject o = arr.add<JsonObject>();
        o["name"] = st[i].pcTaskName;
        o["prio"] = st[i].uxCurrentPriority;
        o["hwm"] = (unsigned)hwmBytes;
      }
      heap_caps_free(st);
    }
  }
#endif
  out.serialize(doc);
}

String toJson() {
  PsBuffer b;
  toJson(b);
  return String(b.c_str());
}

void printAll() {
  Serial.println("===== MEM DIAG (heap_caps) =====");
  // 详细区域表：先内部堆（含 DMA/IRAM 等子区域），再 PSRAM
  heap_caps_print_heap_info(MALLOC_CAP_INTERNAL);
  heap_caps_print_heap_info(MALLOC_CAP_SPIRAM);
  Serial.println("---- summary ----");
  Serial.print(heapSummary());
  Serial.print(taskStackSummary());
  Serial.println("===== MEM DIAG END =====");
}

}  // namespace memdiag
