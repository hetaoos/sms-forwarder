#pragma once

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include <ArduinoJson.h>

#include "app/Config.h"

// 黑名单：拦截命中规则的短信与来电（来电由 SmsService 的 +CLIP/+CLCC 检测
// 在派发前判定），命中后阻止转发（邮件 / MQTT / 推送），并支持「仅记录不推送」
// 或「静默丢弃」。
//
// 匹配在 SmsService::handleCmt（短信）/ dispatchCall（来电）内、落盘与转发回调
// 之前完成——这样「静默丢弃」才能做到真的既不入库也不推送；「仅记录不推送」则
// 照常入库但跳过 _onSms 回调。
// 抽数任务上下文（handleCmt）与 HTTP 任务（配置读写）并发访问 _entries，故加互斥锁。
class BlackList {
 public:
  // 匹配作用域（位掩码，便于单条规则同时覆盖短信与来电）
  enum Scope : uint8_t {
    kSms  = 1 << 0,   // 短信接收
    kCall = 1 << 1,   // 来电提醒（SmsService 派发前判定，命中即不通知）
  };

  // 命中动作
  enum Action : uint8_t {
    kRecord = 0,   // 仅记录不推送（默认）：短信照常入库，但不转发
    kDrop   = 1,   // 静默丢弃：不入库、不转发
  };

  // 匹配模式
  enum Mode : uint8_t {
    kExact    = 0,   // 精确号码
    kPrefix   = 1,   // 前缀（目标以规则开头）
    kWildcard = 2,   // 通配符（* 任意串 / ? 单字符）
  };

  // 单条规则
  struct Entry {
    String  pattern;   // 号码 / 通配串（去空白与分隔符后匹配）
    uint8_t mode   = kExact;
    uint8_t scope  = kSms;
    String  note;      // 备注名
    bool    enabled = true;
  };

  // 载入 NVS 配置（无记录则用默认：总开关关、动作=仅记录、空表）
  void begin();

  // 总开关（是否启用拦截）
  bool enabled() const { return _enabled; }
  void setEnabled(bool on) { _enabled = on; }

  // 命中动作
  Action action() const { return _action; }

  // 当前已匹配到的备注（match 命中时写入，供日志使用）
  const String &lastNote() const { return _lastNote; }

  // 匹配：target 为原始发件人号码，scope 为本次事件作用域。
  // 返回是否命中；命中时写入 _lastNote（规则备注）。未启用或总开关关时直接返回 false。
  bool match(const String &target, uint8_t scope);

  // 测试匹配：忽略总开关，逐条比对（供前端调试规则）；命中写入 _lastNote。
  bool testMatch(const String &target, uint8_t scope);

  // 规则条数
  size_t count() const { return _count; }

  // 配置读取 JSON：{"ok":true,"enabled":bool,"action":n,"count":n,
  //   "list":[{"pattern":"..","mode":n,"scope":n,"note":"..","enabled":bool}]}
  String configJson() const;

  // 从 JSON 对象整体保存（校验 + 去重 + 落盘 NVS）；o 同 configJson 结构。
  // 成功返回 true（已写入），失败（校验不通过/写入失败）返回 false。
  bool saveFromJson(const JsonObject &o);

  // 导出完整配置（含备注）到给定 JSON 对象，供配置备份
  void exportTo(JsonObject &o) const;

  // 从给定 JSON 对象导入并落盘；缺失字段保持原值；成功返回 true
  bool importFrom(const JsonObject &o);

  // 将当前内存中的配置落盘 NVS（saveFromJson 内部与总开关翻转时共用）
  bool saveConfig();

  // 归一化号码：去掉空格 / - / ( ) / . / / 等分隔符，仅保留数字与前导 +。
  // 空串返回空串（调用方应据此拒绝）。
  static String normalize(const String &s);

 private:
  // 单条匹配（已归一化的目标 vs 单条规则；规则已假定 enabled 且 scope 命中）
  bool matchEntry(const String &normTarget, const Entry &e) const;

  // 通配符匹配：* 匹配任意长度（含空），? 匹配单个字符
  static bool wildcardMatch(const char *pat, const char *txt);

  // 取/放互斥锁（保护 _entries / _enabled / _action）
  void lock() const { xSemaphoreTake(_mutex, portMAX_DELAY); }
  void unlock() const { xSemaphoreGive(_mutex); }

  // 从 NVS 载入配置（begin 内部调用）
  bool loadConfig();

  static constexpr uint8_t kMax = cfg::kBlacklistMax;  // 规则上限
  Entry _entries[kMax];
  uint8_t _count = 0;
  bool    _enabled = false;
  Action  _action = kRecord;
  String  _lastNote;                          // 最近一次命中规则的备注
  mutable SemaphoreHandle_t _mutex = nullptr; // 保护配置读写
};
