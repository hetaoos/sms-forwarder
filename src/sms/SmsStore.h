#pragma once

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include <vector>

#include "app/Config.h"
#include "app/UserFs.h"
#include "sms/SmsRecord.h"

// 短信存储：每条一个文件，存放于用户数据分区 /data/sms/<id>.txt，
// 刷固件与 OTA 均保留。与参考实现一致的单文件格式（省 flash、无字段冗余）：
//   第 1 行: <id>,<dir>,<phone>,<ts>,<modem>     dir: in=收到 out=发出
//   第 2 行: 空行
//   第 3 行起: 短信正文（UTF-8，可含换行）
// 列表无内存索引，每次请求扫描目录逐条读取（与参考实现同策略）；
// 所有 FS 操作在互斥锁内，抽数任务（写入）与 HTTP 任务（查询）并发安全。
class SmsStore {
 public:
  // 确保目录存在；max 为条数上限（FIFO 淘汰）
  void begin(uint16_t max);

  // 分区是否可用
  bool enabled() const { return _enabled; }

  // 当前条数（内存计数，启动时扫描校准）
  int count() const { return _count; }

  // 写入一条记录，返回记录 ID（0 = 失败）
  uint32_t add(const SmsMessage &msg);

  // 分页 + 关键字 + 模组过滤；out 填充本页记录，total 为匹配总数
  // page/pageSize 越界自动钳制；q 为空不筛；modem < 0 不筛
  void list(int page, int pageSize, const String &q, int8_t modem,
            std::vector<SmsMessage> &out, int &total);

  // 读取单条；找到返回 true
  bool detail(uint32_t id, SmsMessage &out);

  // 删除单条；返回是否确实存在
  bool deleteOne(uint32_t id);

  // 清空全部
  void clear();

  // 当前最大记录 ID（SSE diff 基线用）
  uint32_t maxId() const { return _maxId; }

  // 存储统计 JSON：{"ok":true,"total":n,"max":n,"used":bytes,"fsTotal":bytes}
  String statsJson();

 private:
  // 文件路径
  String filePath(uint32_t id) const;

  // 从文件名解析 ID（非纯数字返回 0）
  static uint32_t parseId(const String &name);

  // 只读首行头（不读正文），不加锁；调用方自行管理互斥。找不到返回 false
  bool readHeader(uint32_t id, SmsMessage &out);

  // 读头 + 正文，不加锁；调用方（detail）自行加锁。找不到返回 false
  bool readFull(uint32_t id, SmsMessage &out);

  // 取/放互斥锁
  void lock() const { xSemaphoreTake(_mutex, portMAX_DELAY); }
  void unlock() const { xSemaphoreGive(_mutex); }

  String _dir = "/data/sms";      // 存储目录（挂载点下）
  uint16_t _max = 100;            // 条数上限
  bool _enabled = false;          // 分区是否可用
  int _count = 0;                 // 当前条数（内存计数）
  uint32_t _maxId = 0;            // 当前最大 ID
  uint32_t _minId = 0;            // 当前最小 ID（FIFO 淘汰用，id 单调递增）
  mutable SemaphoreHandle_t _mutex = nullptr;  // 保护 FS 操作与计数
};
