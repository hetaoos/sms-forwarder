#pragma once

#include <Arduino.h>
#include <stddef.h>
#include <stdint.h>

#include "app/Config.h"

// 环形缓冲：保存最近若干条 AT 记录（含串口自发上报）。
// 与设备日志 _log 同思路：条目缓冲区整块分配在 PSRAM（PSRAM 优先），与 WiFi/
// LwIP/SSL 等内部堆使用者隔离；板子无 PSRAM 或分配失败时自动退化到内部堆，
// 并按 kAtHistoryRingSizeNoPsram 收紧条数，功能不变（psramOk() 可查落点）。
// 条目为定长结构（cmd/resp 为内嵌字符数组，超长截断），写入只做 memcpy，
// 运行期零堆分配、无碎片，抽数任务 / HTTP 任务等任意上下文可安全调用。
class AtHistory {
 public:
  // 一条 AT 交互/上报记录（定长结构，按槽位原地写入）
  struct Entry {
    uint32_t id;                           // 全局自增 ID（不回绕，取模映射到槽位）
    uint8_t port;                          // 来源串口号（1/2）
    char cmd[cfg::kAtHistoryCmdMax];       // 命令内容；空 = 串口自发上报
    char resp[cfg::kAtHistoryRespMax];     // 响应内容
  };

  // 分配环形缓冲：max 为条数上限（0 或越界取 cfg::kAtHistoryRingSize）。
  // PSRAM 优先（按上限给足）；失败退化为内部堆并按 kAtHistoryRingSizeNoPsram
  // 收紧条数；两者都分配失败则缓冲不可用（add 静默丢弃，串口交互不受影响）
  void begin(uint16_t max);

  // 追加一条记录（port：串口号；cmd：命令，空串=自发上报；resp：响应内容；
  // 二者均超长自动截断到对应字段长度）
  void add(uint8_t port, const String &cmd, const String &resp);

  // 按 ID 取记录，不存在返回 nullptr
  const Entry *get(uint32_t id) const;

  // 导出 afterId 之后（不含）的若干条记录为 JSON 数组
  // afterId：-1 表示从头开始；limit：最多导出条数，0 表示不限
  String toJson(int32_t afterId, size_t limit) const;

  // 导出单条记录为 JSON 对象，不存在返回 "{}"
  String entryJson(uint32_t id) const;

  // 清空所有记录（保留 ID 计数不回退）
  void clear();

  // 最新记录的 ID（无记录时返回 -1）
  uint32_t lastId() const { return _end == 0 ? -1 : _end - 1; }

  // 当前记录条数
  uint32_t size() const { return _end - _start; }

  // 是否为空
  bool empty() const { return _end == _start; }

  // 容量上限 / 缓冲是否位于 PSRAM / 缓冲是否就绪（begin 成功分配）
  uint16_t max() const { return _max; }
  bool psramOk() const { return _psram; }
  bool ready() const { return _buf != nullptr; }

  // 统计 JSON：
  // {"ok":true,"total":n,"max":n,"psram":bool,"bytes":n,
  //  "psramTotal":n,"psramFree":n}
  // psram=false 表示缓冲未落在 PSRAM（无 PSRAM 或分配失败，已退化内部堆）；
  // 此时 psramTotal/psramFree 可区分两种原因（0 = 设备无 PSRAM）
  String statsJson() const;

 private:
  Entry *_buf = nullptr;   // 环形缓冲（PSRAM 优先，begin 时整块分配）
  uint16_t _max = 0;       // 容量上限（条）
  uint32_t _start = 0;     // 最旧条目 id
  uint32_t _end = 0;       // 下一个可用 id（即已写入总数）
  bool _psram = false;     // 缓冲是否成功分配到 PSRAM
};
