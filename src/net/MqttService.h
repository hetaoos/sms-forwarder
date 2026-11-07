#pragma once

#include <Arduino.h>
#include <stdint.h>

#include <ArduinoJson.h>
#include <mqtt_client.h>

#include "app/Config.h"
#include "utils/TlsTurn.h"   // MQTT-TLS 复用 4 通道计数信号量（按值成员 _tlsGuard）

class WifiManager;
class SmsService;
class TimeSync;
class LogStore;

// MQTT 服务：连接标准 MQTT Broker（基于 esp-idf 内置 esp-mqtt 组件，无需额外库），
// 事件以 JSON 报文发布，并支持订阅命令主题做远程控制。
//
// 设计要点（移植自 esp32-sms-forwarding 的 mqtt 通信模组，并适配本项目架构）：
//  - esp-mqtt 自带网络任务与自动重连，主循环无需调用 client.loop()；
//  - 发布：esp_mqtt_client_enqueue 内部有队列，从任意任务调用均安全，
//    日志采用「LogStore sink 入队 + MqttService::tick() 主循环批量发布」以避免阻塞；
//  - 订阅：仅订阅 前缀/设备ID/command，收到 JSON 命令后入 FreeRTOS 队列，
//    由 tick() 在主循环线程执行（send_sms / send_at / restart 等不应在 MQTT 网络任务里直接调用）；
//  - 事件类型复用 cfg::kMqttNotify* 位掩码（与邮件共用低 4 位定义，LOG 为 MQTT 专属）。
//
// 与参考项目差异：
//  - 本项目无独立 ModemRegistry / OTA / Coredump 等组件，设备信息取自 WifiManager /
//    SmsService / TimeSync；命令也改为适配本项目的端口式 SmsService 接口；
//  - 配置自管 NVS（参考 EmailService 模式），不依赖全局 config 单例。
#if ENABLE_MQTT

// 事件消息类型（仅用于 publishEvent 内部分支）
enum class MqttMsgType : uint8_t {
  kSms = 0,     // 收到短信
  kSim = 1,     // SIM 状态变更
  kBoot = 2,    // 设备启动完成（开机首次连上发布一次）
  kOnline = 3,  // （重新）上线通知（每次连接建立发布）
  kCall = 4,    // 来电提醒（发布到 .../call 主题）
};

class MqttService {
 public:
  // 注入依赖（引用，生命周期由 App 保证）并从 NVS 载入配置；注册日志 sink。
  // wifi：WiFi 管理器（连接状态/IP）；sms：短信服务（通信模组/SIM状态/收发）；
  // time：时间同步（系统时间）；log：设备日志环形存储（提供 sink 入口）。
  void begin(WifiManager &wifi, SmsService &sms, TimeSync &time, LogStore &log);

  // 主循环调用（loop 任务上下文）：等 WiFi 就绪后建连、处理连接跳变
  // （订阅 / 上线状态 / 启动通知）、执行入队命令、批量发布日志。
  void tick();

  // 配置变更后重建客户端（无需重启即可生效）。
  void reconfigure();

  // ---------------- 配置 ----------------
  // MQTT 总开关：false 时一切事件都不发布、命令不订阅，已保存配置保留。
  bool enabled() const;
  // 设置总开关并落盘 NVS；停用时销毁客户端（停止连接），重新开启后 tick 自动重连。
  bool setEnabled(bool on);
  // 四项必填（服务器 / 端口 / 账号 / 密码）是否齐全：
  // 本项目密码可选（匿名 Broker），故「齐全」只校验服务器非空。
  // 声明为 static：onLog / publishEvent 等静态上下文也需调用，且不依赖实例。
  static bool isConfigured();
  // 实时连接状态（供 Web 状态接口 / 仪表盘显示，跨任务安全读取）
  static bool isEnabled();          // 总开关是否开启（= _enabled）
  static bool connected();          // 当前是否真正连上 Broker
  static const String &lastError(); // 最近一次连接错误描述（成功连上后清空，空串=无错误）
  // 某类事件的发布是否开启（mask 取 cfg::kMqttNotify*）
  bool notifyEnabled(uint32_t mask) const;
  // 保存配置并落盘 NVS；pass 为空串表示保持已保存的密码不变；
  // notifyTypes 越界位被过滤；enabled 为 MQTT 总开关。返回是否写入成功。
  // scheme：协议方案（mqtt / mqtts / ws / wss），由前端下拉选择；
  // server：Broker 主机与端口（host[:port]，不含协议前缀）。
  bool saveConfig(const String &scheme, const String &server,
                  const String &user, const String &password,
                  const String &clientId, const String &topicPrefix,
                  uint16_t keepAlive, bool subscribeEnabled,
                  uint32_t notifyTypes, bool enabled);
  // 方案是否需要 TLS（mqtts / wss）；静态，因仅依赖静态配置
  static bool usingTls() { return _scheme == "mqtts" || _scheme == "wss"; }
  // 当前协议方案（供状态展示）
  static String scheme() { return _scheme; }
  // 配置 JSON（GET /api/mqtt 用，password 不回显）。
  String configJson() const;
  // 导出完整配置（含密码，供备份用）到给定 JSON 对象。
  void exportTo(JsonObject &o) const;
  // 从给定 JSON 对象导入配置并落盘；缺失字段保持原值；成功返回 true。
  bool importFrom(const JsonObject &o);

  // ---- 配置（NVS: appcfg/mqtt，自管，不依赖全局 config 单例）----
  static bool     _enabled;          // MQTT 总开关
  static String   _server;           // Broker 地址（host[:port]，不含协议前缀）
  static String   _scheme;           // 协议方案：mqtt / mqtts / ws / wss
  static String   _user;             // 用户名（匿名可空）
  static String   _pass;             // 密码（GET 不回显）
  static String   _clientId;         // 客户端 ID（空则用 SMSFwd-<设备ID>）
  static String   _topicPrefix;      // 主题前缀（默认 smsf）
  static uint16_t _keepAlive;        // 保活秒数
  static bool     _subscribeEnabled; // 是否订阅命令主题
  static uint32_t _notifyTypes;      // 事件类型位掩码（cfg::kMqttNotify*）
  // 配置读写（NVS 落地）
  static bool loadConfig();
  static bool persistConfig();

  // ---------------- 事件上报（非阻塞，调用上下文安全）----------------
  // 收到短信（sender：发件人；message：正文；ts：纪元秒或开机秒数；modem：接收通信模组串口号）
  void notifySms(const String &sender, const String &message, uint32_t ts,
                 int8_t modem);

  // ---------------- 单次同步投递（持久化发件箱专用）----------------
  // 与 publishEvent(kSms) 同款报文，但同步返回「是否成功入队」，供发件箱记账。
  // skipped=true 表示总开关关闭 / 配置不全 / 短信上报位关闭，应直接结单；
  // 未连上 Broker 返回可重试失败。QoS1 只代表已交给 esp-mqtt 队列，
  // 无 Broker 端到端确认，故发件箱记为 submitted 而非 ok。
  bool publishSmsOnce(const String &sender, const String &message, uint32_t ts,
                      int8_t modem, bool &skipped, String &err);
  // SIM 状态变更（port：串口号；ready：true=已插入 false=已拔出）
  void notifySim(uint8_t port, bool ready);
  // 来电提醒（number：主叫号码，可能为「未知号码」/「号码保密」；modem：串口号）
  void notifyCall(const String &number, uint32_t ts, int8_t modem);
  // 启动完成（开机后首次连上发布一次）
  void notifyStartup();
  // 上线通知（每次（重）连接发布一次）
  void notifyOnline();
  // 发布一条测试事件（立即返回，由 tick 批量发布）
  bool notifyTest();

  // ---------------- 设备标识 ----------------
  // 设备唯一 ID：完整芯片 eFuse MAC，12 位十六进制（与邮件 mailDeviceId() 一致）
  static String deviceId();
  // 设备名：SMS-Forwarder-<deviceId>
  static String deviceName();

  // 拼接完整主题：前缀/设备ID/<sub>
  static String fullTopic(const String &sub);

 public:
  // 以下为内部实现辅助（声明为 public 以便文件作用域的 esp-mqtt 事件回调 /
  // 命令响应等自由函数直接访问通信模组级状态；外部仍只应使用上方公开 API）
  // LogStore sink 回调（任意任务上下文，须轻量）：结构化字段 -> MQTT 日志发布
  static void onLog(uint8_t level, const char *tag, const char *msg, uint32_t ts);
  // 待执行命令处理（在主循环线程执行）
  static void handleCommand(const String &json);
  // 客户端构建 / 销毁（按当前配置创建 / 释放 esp-mqtt 客户端）
  static void buildClient();
  static void destroyClient();
  // 发布到 前缀/设备ID/<sub>
  static bool publish(const String &subtopic, const String &payload, int qos,
                      bool retain);
  // 按消息类型发布事件（供 notifyXxx 调用），内部按 notifyTypes 位掩码判定。
  // ts 为事件时间戳（纪元秒或开机秒数，>=1.6e9 视为真实时间）。
  static void publishEvent(MqttMsgType type, const String &sender,
                           const String &message, uint32_t ts,
                           const String &typeLabel, const String &modemLabel,
                           int8_t modemSlot);
  // 发布设备状态（online/offline，后者由 LWT 遗嘱发出）
  static void publishStatus(const String &status);

  // ---- 通信模组级状态 ----
  static WifiManager *_wifi;   // WiFi 管理器（连接状态 / IP）
  static SmsService *_sms;     // 短信服务（通信模组与 SIM 状态）
  static TimeSync *_time;      // 时间同步（系统时间展示）
  static LogStore *_log;       // 设备日志（sink 入口，仅用于日志标签）

  static String s_uri, s_cid, s_will, s_willMsg, s_user, s_pass;
  static esp_mqtt_client_handle_t s_client;
  static bool s_connected;       // 当前连接状态

  // TLS 轮次守卫：仅当 usingTls() 时，建连前取、断连/销毁前还（从末尾槽分配）。
  // 与 OTA/推送/邮件共用 4 通道计数信号量；release() 幂等，明文连接不占通道。
  // 与此类其余状态一致，均声明为 static（buildClient/destroyClient 为静态函数）。
  static TlsTurn _tlsGuard;
  static unsigned long _mqttTlsNextTryMs;  // TLS 通道满时的重试退避时刻（避免主循环刷日志）
  static String s_lastError;     // 最近一次连接错误描述（CONNECTED 时清空，空串=无错误）
  static bool s_wasConnected;    // 上一 tick 连接状态（检测刚连上的跳变）
  static bool s_bootNotified;    // 本次开机是否已发启动通知
  static bool s_pendingBoot;     // 待发启动通知（连接建立后于主循环补发）

  // 待执行命令队列（MQTT 网络任务收到命令后入队，由主循环 tick 执行）；
  // 队列元素类型 MqttCmdMsg 定义在 MqttService.cpp 文件作用域（供事件回调直接构造）。
  static QueueHandle_t s_cmdQueue;

  // 客户端生命周期互斥锁：buildClient / destroyClient / publish 等跨任务访问
  // s_client 的操作均须持此锁，防止「网页任务停用」与「主循环 tick」并发对同一
  // handle 重复销毁（esp_mqtt_client_destroy 重复调用会双重释放导致异常）。
  // 注意：destroyClient 在持锁期间只做「快照 handle + 置空 s_client」，真正的
  // esp_mqtt_client_destroy 在释放锁之后执行，避免阻塞 MQTT 内部任务造成死锁。
  static SemaphoreHandle_t s_lifecycleMux;

  // 日志推送的轻量限流（只做速率保护，不做消息缓存）
  static constexpr int LOG_THROTTLE_PER_SEC = 20;
  static unsigned long s_logLastSec;
  static int s_logSecCount;

  // 日志发布缓冲（sink 入队，tick 批量发布；避免任意任务 context 直接 enqueue 风暴）
  // 这里采用简单策略：sink 直接调用 publish（esp-mqtt enqueue 内部队列已同步），
  // 限流只控制入队速率，可靠性由 esp-mqtt outbox 保证。
};

#else  // !ENABLE_MQTT —— 编译期关闭 MQTT

// 空实现替身：让 App / WebPanel / Outbox 的装配代码无需到处写 #if，
// 同时 MqttService.cpp 整个实现（esp-mqtt 客户端、TLS、命令队列）不会进固件。
// 只覆盖「未被 #if ENABLE_MQTT 保护」的全部调用点，全部 inline、无状态、无副作用。
// 配置接口返回失败 / compiled:false，避免旧配置导入或误调用时静默假装成功。
class MqttService {
 public:
  void begin(WifiManager &, SmsService &, TimeSync &, LogStore &) {}
  void tick() {}
  void notifyStartup() {}
  void notifySms(const String &, const String &, uint32_t, int8_t) {}
  void notifySim(uint8_t, bool) {}
  void notifyCall(const String &, uint32_t, int8_t) {}

  // 发件箱里若残留旧固件的 mqtt 通道，判为 skipped 直接结单，不重试
  // （Outbox 始终编译，故该方法必须存在于替身）
  bool publishSmsOnce(const String &, const String &, uint32_t, int8_t,
                      bool &skipped, String &err) {
    skipped = true;
    err = "MQTT 未编译进固件";
    return false;
  }
};

#endif // ENABLE_MQTT
