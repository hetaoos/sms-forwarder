#pragma once

#include <Arduino.h>

#include "app/Psram.h"  // PsBuffer：toJson 直发 PSRAM（与方案 #2 一致）

// 内存量化诊断：把内部 DRAM / DMA / PSRAM 的占用按能力拆分，
// 并汇总各 FreeRTOS 任务栈的 high-water（剩余未用栈），供分析「为何内部堆只剩 43KB」。
//
// 用法：
//   - memdiag::printAll()        开机时打印到串口（最详细，含每片 heap 区域表）
//   - memdiag::heapSummary()     紧凑文本：internal/DMA/PSRAM 总量/剩余/最大连续块 + 历史最小空闲堆
//   - memdiag::taskStackSummary() 各任务 high-water（字节）
//   - memdiag::toJson()          合成为 JSON，供 /api/debug/mem 远程读取

namespace memdiag {

// 内部 DRAM（8BIT+INTERNAL）占用拆分 + 历史最小空闲堆 + PSRAM/DMA，多行文本
String heapSummary();

// 各任务栈 high-water（剩余未用栈字节），多行文本
String taskStackSummary();

// 综合 JSON（heap + tasks）：
//   - toJson(PsBuffer&)  直接序列化进 PSRAM 缓冲（供服务端免 DRAM 直发，见方案 #2）
//   - toJson()           兼容旧用法，返回 String（由前者转来，阈值下也落 PSRAM）
void toJson(PsBuffer &out);
String toJson();

// 一次性打印到串口：heap 区域详细表 + 紧凑摘要 + 任务栈；开机诊断用
void printAll();

}  // namespace memdiag
