#pragma once

#include <Arduino.h>
#include <stddef.h>
#include <stdint.h>

// 环形缓冲：保存最近若干条 AT 记录（含串口自发上报）
class AtHistory {
 public:
  // 一条 AT 交互/上报记录
  struct Entry {
    uint32_t id;      // 全局自增 ID（不回绕，取模映射到槽位）
    uint8_t port;     // 来源串口号（1/2）
    String cmd;       // 命令内容；空 = 串口自发上报
    String resp;      // 响应内容
  };

  // 追加一条记录
  // port：串口号；cmd：命令（自发上报传空串）；resp：响应内容
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

 private:
  static const size_t kMax = 30;  // 环形缓冲容量（最多保留条数）
  Entry _buf[kMax];               // 记录槽位数组，按 id % kMax 定位
  uint32_t _start = 0;            // 最旧条目 id
  uint32_t _end = 0;              // 下一个可用 id（即已写入总数）
};
