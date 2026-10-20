#pragma once

#include <Arduino.h>
#include <esp_partition.h>
#include <time.h>

// Coredump 模块：崩溃转储（dumpcore）的记录与查询，流程参照 esp32-sms-forwarding。
//
// panic（断言 / 非法访问等）发生时，ESP-IDF 的 panic 处理器会把 ELF 格式的
// 内存快照自动写入分区表里的 coredump 分区（platform-espressif32 预编译库
// 默认开启，分区见 partitions/esp32s3_16mb.csv 的 0xFE0000 ~ 0xFFFFFF），
// 本模块负责快照之外的「元信息」：
//   - 崩溃时刻：RTC_DATA_ATTR 保留最近已知墙上时钟（panic 重启不丢、断电丢），
//     断电场景回落到 TimeSync 每 10 分钟落盘的 NVS lastts（近似值）；
//   - 崩溃版本：panic 重启时把当前固件版本（Release tag）记入 NVS；
//   - 调试符号链接：按 Release tag 拼出 firmware-<tag>-<platform>.elf 的下载地址，
//     与 OTA 产物命名同源，供离线解析（esp-coredump 工具）。
// 崩溃记录独立命名空间存放，不随出厂复位清除；分区快照只在用户主动清除时擦除。
class Coredump {
 public:
  // 初始化：从 NVS 恢复上次崩溃信息；若本次为 panic 重启，则把 RTC 保留的
  // 最近时间戳与当前固件版本写入 NVS。须在 TimeSync::begin() 之后调用
  // （NVS lastts 由 TimeSync 维护，恢复的系统时钟以它为准）。
  static void init();

  // 周期调用（App::tick，loop 任务上下文）：记录最新已知墙上时钟，
  // 用于崩溃时刻估算。仅写 RTC 内存，无 NVS 开销；断电兜底见 init()。
  static void updateLastKnownTime(time_t t);

  // coredump 分区是否包含数据（即是否存在未导出的崩溃快照）
  static bool hasData();

  // 上一次崩溃的时间戳（0 表示未知）
  static time_t crashTime();

  // 上一次崩溃时运行的固件版本（空串表示未知）
  static String crashVersion();

  // 基于崩溃版本构造 ELF 调试符号下载 URL（版本未知时返回空串）
  static String elfUrl();

  // coredump 分区句柄（找不到分区返回 nullptr）
  static const esp_partition_t *partition();

  // 扫描分区已使用的字节数（带缓存语义由调用方掌握，本函数不做缓存）
  static size_t usedSize(const esp_partition_t *part);

 private:
  // 分区扫描实现：从尾部反向找第一个非 0xFF 字节，其偏移 + 1 即已用大小。
  // 空分区（全 0xFF）会扫完整分区，故调用方应节制频率（仅 Web 查询时用）。
  static size_t scanUsed(const esp_partition_t *part);

  static time_t s_crashTime;     // 上次崩溃时间（NVS 持久化）
  static String s_crashVersion;  // 上次崩溃时固件版本（NVS 持久化）
};
