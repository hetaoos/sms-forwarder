#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <stdint.h>

#include "app/Config.h"
#include "push/PushTypes.h"
#include "push/MsgContext.h"
#include "push/PushChannels.h"

class SmsService;  // 前向声明：SMS 备份通道的发送入口（App 装配时注入）

// PushService：统一的 webhook 推送通道框架（参考 esp32-sms-forwarding 的 push 模组，
// 适配本项目「按功能自管 NVS」架构）。
//
// 能力：
//  - 多通道数组（Bark/钉钉/Telegram/Mailgun 等），每通道可独立启停、配置、失败重试；
//  - 推送策略：广播（全部通道都发）/ 故障转移（按顺序首个成功即停止后续）；
//  - 非阻塞入队 + 主循环 tick 发送（与 EmailService 一致，绝不阻塞串口抽数任务）。
//
// 与参考项目的差异：
//  - 邮件（SMTP）/ MQTT 保持各自独立的通知通道，本框架只调度 webhook 类通道；
//  - 配置自管 NVS（appcfg/push），不依赖全局 config 单例。
class PushService {
 public:
  // 载入 NVS 配置并初始化队列（App::begin 调用）
  void begin();

  // 注入短信服务（SMS 备份推送通道的发送入口）。App 装配时调用；
  // 未注入时 SMS 备份通道一律判失败（其余通道不受影响）。
  void setSms(SmsService *sms) { _sms = sms; }

  // 主循环调用（loop 任务上下文）：发送队列中的推送任务与重试项
  void tick();

  // ---------------- 通知入口（非阻塞，回调上下文安全）----------------
  // 收到短信：入队一条推送任务（未启用或配置为空时静默跳过）
  void notifySms(const String &sender, const String &message, uint32_t ts,
                 int8_t modem);

  // 来电提醒：入队一条推送任务（number 为主叫号码，可能为「未知号码」/
  // 「号码保密」）。短信备份通道对来电同样生效（SIM/启动/在线事件除外）。
  void notifyCall(const String &number, uint32_t ts, int8_t modem);

  // 立即排入一条测试推送（立即返回，由 tick 在 loop 任务执行）
  bool requestTest();

  // ---------------- 配置 ----------------
  bool enabled() const;

  // 有效推送通道数（启用且字段齐全，与 configJson 的 configured 同一判定）。
  // 加锁读取，供 LCD 状态页展示「N 个通道」
  int validChannelCount() const;

  // 单条通道简报（POD，供 LCD 菜单「推送通道」页逐条展示）：
  // label 为用户名称，无名称时取通道类型中文名
  struct ChannelBrief {
    char label[20] = "";
    bool on = false;     // 是否启用
    bool valid = false;  // 字段是否齐全（有效才参与发送）
  };
  // 填充前 max 条通道简报（含未启用者），返回实际条数；加锁读取
  int channelBriefs(ChannelBrief *out, int max) const;

  // 当前推送策略（广播 / 故障转移）；加锁读取
  PushStrategy strategy() const;

  // ---------------- 单次同步投递（持久化发件箱专用）----------------
  // 绕过内部队列与重试表，只投一个通道一次并返回结果；内部复用 executeChannel
  // （模板渲染、跳过判断、HTTP/TLS 会话全部与原有链路一致）。
  // skipped=true 表示通道已停用/配置不全，调用方应直接结单而不是重试。
  // 只能在 loop 任务上下文调用（内部含最长 8s 的 HTTP 会话）。
  bool deliverOnce(int channelIdx, const String &sender, const String &message,
                   uint32_t ts, int8_t modem, MsgType type, bool &skipped,
                   String &err);
  // 该通道是否依赖 WiFi：SMS 备份通道不依赖，发件箱据此在网络未就绪时仍放行
  bool channelNeedsNet(int channelIdx) const;

  // 设置总开关并落盘 NVS
  bool setEnabled(bool on);

  // 配置读取 JSON：{"ok":true,"enabled":bool,"strategy":int,"configured":bool,
  //   "channels":[{"type":n,"name":"..","url":"..","hasKey1":bool,"key2":"..",
  //   "key3":"..","key4":"..","key5":"..","customBody":"..","retryOnFail":bool,
  //   "valid":bool},...]}
  // key1（常为 Token/API Key/签名密钥）不回显，仅返回 hasKey1 标记
  String configJson() const;

  // 支持的通道类型清单及字段 schema：前端据此动态渲染表单
  // [{"type":n,"label":"..","required":["url"],"optional":["key1","name"],
  //   "body":bool,"secretField":"key1"},...]
  static String typesJson();

  // 从 JSON 对象保存配置并落盘 NVS：
  //   enabled / strategy / channels（数组，每项含 type/enabled/name/url/
  //   key1..key5/customBody/retryOnFail）。key1 为空串表示保持已存值（掩码字段）。
  // 返回是否写入成功
  bool saveFromJson(const JsonObject &o);

  // 导出完整配置（含 key1，供备份用）到给定 JSON 对象
  void exportTo(JsonObject &o) const;

  // 从给定 JSON 对象导入配置并落盘；缺失字段保持原值；成功返回 true
  bool importFrom(const JsonObject &o);

 private:
  // 一条待推送任务（仅存原始事件）
  struct PushJob {
    String  sender;     // 短信发件人
    String  message;    // 短信正文
    String  timestamp;  // 时间戳（字符串，便于直接下发）
    int8_t  modem = -1; // 接收模组串口号（<0 未知）
    MsgType type = MSG_TYPE_SMS;
  };

  // 失败重试项：channelIdx 指向要重发的通道下标
  struct RetryItem {
    int      channelIdx = -1;
    PushJob  job;
    uint8_t  attempts = 0;      // 已尝试次数（等待网络期间不计入）
    uint32_t nextAt = 0;
    bool     needsNet = false;  // HTTP 通道：WiFi 未连接时推迟，不消耗尝试次数
  };

  // 单条推送任务执行（广播 / 故障转移编排）
  // attempts：整链重试的累计尝试次数（用于退避与封顶判断）
  void executeChain(const PushJob &job, uint8_t attempts = 0);

  // 单通道推送（含跳过判断、构建上下文），供重试回调
  bool executeChannel(int channelIdx, const PushJob &job);

  // 重试退避时长（毫秒）：2s,4s,8s... 封顶 kRetryBackoffMax
  static uint32_t retryBackoffMs(uint8_t attempts);

  // 内部辅助：按通道配置分发单次推送（不含跳过判断）
  bool _sendOneChannel(const PushChannel &ch, const MessageContext &ctx,
                       const String &sender, const String &message,
                       const String &renderedBody, const String &timestamp,
                       MsgType type);

  // 构建消息上下文（内部辅助）
  MessageContext buildMsgContext(const PushJob &job, const String &messageType);

  // 渲染自定义消息格式（空模板返回空串）
  String renderChannelBody(const PushChannel &ch, const MessageContext &ctx);

  // 入队主任务（持锁）
  void enqueueJobLocked(const PushJob &job);
  // 入队重试项（持锁）
  void enqueueRetryLocked(const RetryItem &item);

  // 配置读写（NVS 落地）
  bool loadConfig();
  bool persistConfig();

  // 设备标识
  static String deviceId();
  static String deviceName();
  // 本地时间字符串；未同步返回空串
  static String localDateStr();

  mutable SemaphoreHandle_t _mutex = nullptr;  // 保护配置、队列与重试表（递归锁）

  SmsService *_sms = nullptr;   // 短信服务（SMS 备份通道发送入口；未注入时该通道判失败）

  bool     _enabled = false;                   // 推送总开关（NVS 无记录默认关）
  String   _remark;                            // 设备备注（独立 NVS 键 cfg::kNvsKeyRemark，模板 {remark}）
  PushStrategy _strategy = PUSH_STRATEGY_BROADCAST;
  PushChannel  _channels[MAX_PUSH_CHANNELS];
  int          _count = 0;                     // 有效通道数（遍历以此为界）

  static constexpr uint8_t kQueueSize = 6;     // 待发队列深度（满时丢最旧）
  static constexpr uint8_t kRetrySize = 32;    // 重试表容量
  static constexpr uint8_t kMaxAttempts = 8;   // 单条重试最大次数（含首次）
  static constexpr uint32_t kRetryBackoffBase = 2000;  // 退避基值（2s,4s,...）
  static constexpr uint32_t kRetryBackoffMax = 300000; // 退避封顶（5min）

  PushJob    _queue[kQueueSize];
  uint8_t    _queueCount = 0;
  RetryItem  _retry[kRetrySize];
  uint8_t    _retryCount = 0;
  uint32_t   _nextRetryMs = 0;                 // 重试节流时刻
};
