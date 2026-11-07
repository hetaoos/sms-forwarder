#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include <functional>
#include <vector>

#include "app/Config.h"

class EmailService;
class MqttService;
class PushService;
class SmsService;
class WifiManager;
class LogStore;

// 持久化发件箱（不丢消息转发队列）。
//
// 支持两类记录：
//  - 收到的短信（kRecv）：先落盘事件 + 当时启用且有效的通道快照，再逐通道转发；
//  - 主动发出的短信（kSend）：只生成一个 sms:<port> 通道，交给 SmsService 的
//    常驻 sms_send worker 执行 CMGS，结果异步回写。**不生成 email/mqtt/push
//    通道**，否则会经「SMS 短信备份」push 通道形成「发一条 -> 再发一条」回环。
//
// 断电/重启后扫描 /data/outbox/<id>.json 恢复未完成通道：转发类按退避续投；
// 发出类若卡在「已交给 worker、结果未回传」，标记为结果未知而**不自动重发**
// （短信可能已经发出，自动重投会重复扣费），用户可在发件箱页手动重试。
//
// 并发模型：
//  - enqueueSms()/enqueueOutgoingSms() 可在后台任务或 HTTP 任务调用，仅浅入队；
//  - 所有 FS 操作只在 tick() / 发送完成回调（均为 loop 任务）内串行执行；
//  - Web API 只读写内存并置标记，由 tick() 统一落盘；
//  - 实际投递前必须释放互斥锁（SMTP 最长 15s）；短信发送更慢，绝不在 loop
//    任务里同步做 CMGS，始终经 SmsService::enqueueSend 交给专职 worker。
class Outbox {
 public:
  // 与 SmsService::SendDoneFn 同签名：id=0 失败，>0 已发出（含 kIdSentUnstored）；
  // stored=是否写入短信库；err=错误或提示
  using SendDoneFn = std::function<void(uint32_t id, bool stored, const String &err)>;

  void begin(EmailService &email, MqttService &mqtt, PushService &push,
             SmsService &sms, WifiManager &wifi, LogStore &log);

  // 主循环调用：落盘新事件 -> 淘汰旧回执 -> 投递一个到期通道 -> 回写状态
  void tick();

  // 短信到达（回调上下文安全：只做浅入队）
  void enqueueSms(const String &sender, const String &text, uint32_t ts,
                  int8_t modem);

  // 网页主动发出短信（HTTP 上下文安全：只做浅入队）
  // 返回 false = 入队缓冲已满或发件箱不可用（调用方应回退到原有发送队列）
  bool enqueueOutgoingSms(uint8_t port, const String &phone,
                          const String &content, SendDoneFn done = nullptr);

  // ---------------- Web API ----------------
  // GET /api/outbox：统计 + 分页记录；id 非 0 时只返回该条
  String statusJson(int page, int pageSize, uint32_t id) const;
  // POST /api/outbox/retry：把失败/放弃/结果未知的通道重置为待投；id=0 表示全部
  bool retry(uint32_t id);
  // POST /api/outbox/clear：清空已完成回执（未完成记录一律保留）
  void clearDone();
  // POST /api/outbox/delete：删除单条（含文件）
  bool deleteOne(uint32_t id);

  // 待投 / 已失败（可重试）通道计数，供状态页与菜单使用
  int pendingCount() const;
  int failedCount() const;
  bool mounted() const { return _fsOk; }

 private:
  static constexpr uint8_t kRecv = 0;  // 收到的短信 -> 转发
  static constexpr uint8_t kSend = 1;  // 主动发出的短信

  // 通道投递状态。kSubmitted 用于「已交给底层队列、无端到端确认」的通道
  // （MQTT QoS1 入队 / SMS 备份短信入发送队列），与 kOk 区分开。
  // kSending 对 sms: 通道还有「已交给 worker、等待 done 回传」的语义（会落盘），
  // 此时 recDone() 视为未完成、pickDue() 不再挑它。
  enum class ChState : uint8_t {
    kPending = 0,
    kSending = 1,
    kOk = 2,
    kSubmitted = 3,
    kFailed = 4,
    kSkipped = 5,
    kDead = 6,
  };

  struct Channel {
    String   key;        // "email" / "mqtt" / "push:<idx>" / "sms:<port>"
    String   label;      // 通道显示名
    String   err;        // 最近一次错误 / 提示
    ChState  state = ChState::kPending;
    uint8_t  attempts = 0;
    uint32_t costMs = 0;     // 最近一次投递耗时
    uint32_t nextAtMs = 0;   // 运行时字段，不落盘（millis 重启归零）
  };

  struct Rec {
    uint32_t id = 0;
    uint8_t  kind = kRecv;
    String   sender;         // kRecv：发件人；kSend：目标号码
    String   phone;          // kSend：目标号码
    uint8_t  port = 0;       // kSend：通信模组串口号
    String   text;           // 短信正文 / 待发内容
    uint32_t ts = 0;         // 短信时间戳（纪元秒或开机秒数）
    int8_t   modem = -1;
    uint32_t created = 0;    // 记录生成时间（展示用）
    bool     failover = false;  // 入队时的推送策略快照
    bool     dirty = false;     // 需要回写文件
    bool     removed = false;   // 待删除
    SendDoneFn done;            // kSend 的外部结果回调（不落盘，重启后为空）
    uint32_t inflightAtMs = 0;  // kSending 起始时刻（运行时，不落盘）
    std::vector<Channel> ch;
  };

  struct Incoming {
    uint8_t kind = kRecv;
    String sender, text, phone;
    uint32_t ts = 0;
    int8_t modem = -1;
    uint8_t port = 0;
    SendDoneFn done;
  };

  // 目录扫描恢复：损坏文件跳过；转发类 sending 回到 pending 重投，
  // 发出类 sending 视为结果未知（不自动重发，可手动重试）
  void load();
  // 单条读写
  bool writeRec(const Rec &r) const;
  bool readRec(uint32_t id, Rec &out) const;
  String filePath(uint32_t id) const;
  static uint32_t parseId(const String &name);

  // 由当前配置生成通道快照（kSend 只生成单通道）
  void snapshotChannels(Rec &r) const;
  // 记录是否已完成（所有通道都处于终态）
  static bool recDone(const Rec &r);
  static const char *stateName(ChState s);
  static uint32_t backoffMs(uint8_t attempts);

  // 单次投递到指定通道（调用方不得持锁；sms: 通道只入队，不阻塞）
  bool deliver(const Rec &r, const Channel &c, bool &submitted, bool &skipped,
               String &err);
  // 挑一个到期的待投通道，标记 sending 并把待发内容拷到 out（持锁内调用）
  bool pickDue(Rec &out, Channel &outCh);
  // 发送完成后回写状态并落盘
  void applyResult(uint32_t id, const String &key, bool ok, bool submitted,
                   bool skipped, bool noNet, const String &err, uint32_t costMs);
  // sms_send worker 的 done 回调（loop 任务上下文）
  void onSendDone(uint32_t recId, const String &key, uint32_t sid, bool stored,
                  const String &err);
  // done 回调丢失（_doneQ 满）时的兜底，避免通道永久停在 kSending
  void checkSendTimeout();
  // 故障转移：某个 push 通道成功后，把同记录其余 push 通道标记跳过
  void markFailoverSkipped(uint32_t id);
  // 淘汰最旧的已完成回执、处理 removed 记录
  void sweep();

  void lock() const { xSemaphoreTake(_mutex, portMAX_DELAY); }
  void unlock() const { xSemaphoreGive(_mutex); }

  EmailService *_email = nullptr;
  MqttService  *_mqtt = nullptr;
  PushService  *_push = nullptr;
  SmsService   *_sms = nullptr;
  WifiManager  *_wifi = nullptr;
  LogStore     *_log = nullptr;

  bool _fsOk = false;
  uint32_t _maxId = 0;
  uint32_t _lastTickMs = 0;
  std::vector<Rec> _recs;
  std::vector<Incoming> _incoming;
  mutable SemaphoreHandle_t _mutex = nullptr;
};
