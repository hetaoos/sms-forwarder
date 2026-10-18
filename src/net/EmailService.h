#pragma once

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <stdint.h>

#include "app/Config.h"

class SmsService;
class TimeSync;
class WifiManager;

// 邮件正文类型：纯文本或 HTML 富文本（本项目所有通知邮件统一走 HTML，
// 纯文本仅作为客户端不支持 HTML 时的兜底）
enum MailBodyType {
  MAIL_BODY_TEXT = 0,
  MAIL_BODY_HTML = 1
};

// 定时在线通知排程：两种触发模式（与 esp32-sms-forwarding 的 Schedule 一致）：
//   kSchedModeInterval —— 固定间隔：每 intervalH 小时一次（基于 millis，开机起算）；
//   kSchedModeDaily    —— 每日定点：每天 hour:minute 一次（基于本地时间，
//                         依赖 NTP / SIM 时间同步，未同步时不触发）。
// 总控开关为通知类型位 cfg::kEmailNotifyOnline（未勾选时排程不生效）。
struct MailSchedule {
  uint8_t  mode = cfg::kSchedModeInterval;  // 触发模式（kSchedMode*）
  uint8_t  hour = 9;                        // 每日模式触发小时 0-23
  uint8_t  minute = 0;                      // 每日模式触发分钟 0-59
  uint16_t intervalH = cfg::kMailIntervalHDefault;  // 间隔模式周期（小时）
};

// EmailService：SMTP 邮件通知（参考 esp32-sms-forwarding 的 Email / MailNotify 实现）。
//
// 能力划分与参考项目一致：
//   - 发送通道：ReadyMail + WiFiClientSecure（隐式 TLS，setInsecure），
//     带退避重试；认证失败属配置错误，不重试；
//   - 模板：所有通知邮件共用一套卡片式 HTML（灰底 + 白色圆角卡片 + 渐变标题栏
//     + 状态徽标 + 公共信息块 + 底部标识），短信邮件额外带验证码高亮块；
//   - 通知类型：位掩码（cfg::kEmailNotify*），每类事件发送前判定开关，
//     保存配置后立即生效。
//
// 与参考项目的差异（适配本项目架构）：
//   - 本项目无独立推送队列任务，改为「非阻塞入队 + 主循环 tick 发送」：
//     短信/事件回调（串口抽数任务上下文）只做入队，真正的 SMTP 会话在
//     App::tick（loop 任务）执行，绝不阻塞抽数任务与 Web；
//   - 定时在线通知用简单的间隔触发（onlineH，0=关闭），替代参考项目的
//     每日定点/固定间隔两种排程。
class EmailService {
 public:
  // 注入依赖（引用，生命周期由 App 保证）并从 NVS 载入配置；
  // wifi：WiFi 管理器（连接状态/IP）；sms：短信服务（模组与 SIM 状态）；
  // time：时间同步（系统时间展示）
  void begin(WifiManager &wifi, SmsService &sms, TimeSync &time);

  // 主循环调用（loop 任务上下文）：发送启动邮件、定时在线通知与队列中的邮件。
  // 单封邮件的发送是同步阻塞的（最长约 3 次尝试），期间屏幕刷新/SSE 会短暂延迟。
  void tick();

  // ---------------- 配置 ----------------
  // SMTP 邮件通知总开关：false 时一切邮件都不发送（短信 / SIM / 启动 /
  // 定时在线通知全部静默跳过，待发队列丢弃），已保存的配置保留。
  // 注意与「配置完整」的区别：isConfigured() 只表示四项参数齐全，
  // 实际能否发邮件由 enabled() && isConfigured() 共同决定。
  bool enabled() const;

  // 设置总开关并落盘 NVS；停用时同时丢弃待发队列与挂起的启动/在线邮件。
  // 返回是否写入成功
  bool setEnabled(bool on);

  // SMTP 四项配置（服务器/账号/授权码/收件人）是否齐全
  bool isConfigured() const;

  // 某类事件的邮件通知是否开启（mask 取 cfg::kEmailNotify*）
  bool notifyEnabled(uint32_t mask) const;

  // 保存配置并落盘 NVS；pass 为空串表示保持已保存的授权码不变；
  // notifyTypes 越界位被过滤；sched 为定时在线通知排程（两种模式）；
  // enabled 为 SMTP 邮件通知总开关。
  // 保存成功后立即补发一封在线通知，便于确认配置生效与设备在线。
  bool saveConfig(const String &server, uint16_t port, const String &user,
                  const String &pass, const String &sendTo,
                  uint32_t notifyTypes, const MailSchedule &sched, bool enabled);

  // 配置 JSON（GET /api/email 用）：
  // {"ok":true,"enabled":bool,"configured":bool,"server":"..","port":n,
  //  "user":"..","sendTo":"..","hasPass":bool,"notifyTypes":n,
  //  "schedMode":0|1,"schedHour":n,"schedMinute":n,"intervalH":n}
  // smtpPass 不回显（hasPass 表示是否已保存），避免泄露
  String configJson() const;

  // ---------------- 通知入队（非阻塞，回调上下文安全）----------------
  // 收到短信：构建短信邮件并入队（未配置或该类已关闭时静默跳过）
  // sender：发件人号码；message：正文；ts：纪元秒（未同步时为开机秒数）；
  // modem：接收模组串口号（1..kSmsMaxModems，<0 未知）
  void notifySms(const String &sender, const String &message, uint32_t ts,
                 int8_t modem);

  // SIM 状态变更（运行时插卡/拔卡，由 SmsService 状态切换触发）
  void notifySim(uint8_t port, bool ready);

  // 排入启动邮件：延迟 kMailBootDelayMs 等待 WiFi 连接，连不上最多再等
  // kMailBootWaitMaxMs，超时放弃（App::begin 调用一次）
  void notifyStartup();

  // 立即排入一封「设备在线」通知邮件
  void notifyOnline();

  // 发送一封测试邮件（同步、单次尝试，供 Web「测试」按钮调用）。
  // 运行于 HTTP 任务上下文，最长阻塞约一次 socket 超时（15s）。
  bool sendTest();

  // ---------------- 统一 HTML 邮件模板工具 ----------------
  // 模板约定（与 esp32-sms-forwarding 一致）：所有通知邮件共用 buildHtml()
  // 生成的卡片式正文；各邮件只提供标题与卡片主体片段。
  // 公共信息块（系统时间 + 各模组本机号码，无号码回退设备 IP）由
  // buildHtml() 自动追加，新增邮件类型也自动带上。

  // HTML 转义（& < > " 与换行）。所有动态文本进入邮件前都必须过这一层。
  String escape(const String &s) const;

  // 一行「键: 值」表格行（键值均已转义，可直接拼接）
  String row(const String &key, const String &val) const;

  // 一行「键: 链接」表格行。用自绘 <a> 而不是裸文本 URL：多数邮件客户端
  // 会把裸 URL 自动转成链接，且转写时常把纯数字主机名改坏，自绘锚点可锁定显示文本。
  String linkRow(const String &key, const String &url) const;

  // 把若干 row() 结果包成一个带内边距的表格块
  String table(const String &rows) const;

  // 带小标题的表格块（用于把多个 table() 分组展示）
  String section(const String &caption, const String &rows) const;

  // 状态徽标：main 为醒目主文案，sub 为补充说明，ok 决定配色（绿/橙）
  String badge(const String &main, const String &sub, bool ok) const;

  // 纯文字说明段（正文里唯一允许出现换行渲染的地方）
  String note(const String &text) const;

  // 完整 HTML 正文：样式表 + 卡片 + 标题栏 + 主体 + 公共信息块 + 底部标识。
  // title 可带 emoji，需调用方自行保证已转义。
  String buildHtml(const String &title, const String &inner,
                   int maxWidth = 560) const;

  // HTML 正文上限：urlEncode/编码后体积会膨胀数倍，ESP32 堆有限，
  // 超出时只发纯文本兜底。
  static constexpr int kMaxHtmlChars = 6000;

 private:
  // 一封待发邮件（队列单元）
  struct MailJob {
    String subject;
    String html;
    String text;
    uint8_t attempts = 0;  // 已失败次数（退避重试计数）
  };

  // 递归互斥锁守卫：配置读写、入队与模板构建都可能嵌套加锁。
  // 直接持有信号量句柄，const 方法亦可使用（_mutex 为 mutable）。
  struct Guard {
    SemaphoreHandle_t m;
    explicit Guard(SemaphoreHandle_t m_) : m(m_) {
      xSemaphoreTakeRecursive(m, portMAX_DELAY);
    }
    ~Guard() { xSemaphoreGiveRecursive(m); }
  };

  // NVS 配置读写；载入失败时保持出厂默认
  bool loadConfig();
  bool persistConfig();

  // 已配置判定（持锁调用，避免读配置时与 Web 保存竞争）
  bool configuredLocked() const;

  // 实际是否应发邮件：总开关开启 且 四项配置齐全（持锁调用）
  bool activeLocked() const {
    return _enabled && configuredLocked();
  }

  // 队列操作（持锁调用）
  void enqueueLocked(const MailJob &job);
  void requeueFrontLocked(const MailJob &job);

  // 单次发送尝试（同步阻塞，最长约一次 socket 超时）。
  // configErr 出参：true = 配置类错误（认证失败等，重试无意义，直接丢弃）。
  bool sendOnce(const String &subject, const String &html, const String &text,
                bool &configErr);

  // ---------------- 各事件邮件的构建 ----------------
  void buildStartupMail(String &subject, String &html, String &text) const;
  void buildOnlineMail(String &subject, String &html, String &text) const;
  void buildSimMail(uint8_t port, bool ready, String &subject, String &html,
                    String &text) const;
  void buildSmsMail(const String &sender, const String &message, uint32_t ts,
                    int8_t modem, String &subject, String &html,
                    String &text) const;
  void buildTestMail(String &subject, String &html, String &text) const;

  // 判定开关与配置后入队（tick / 通知入口共用；持锁调用）
  void queueStartupLocked();
  void queueOnlineLocked();

  // 每日定点模式是否到点（持锁调用）：本地时间进入触发分窗口、
  // 且晚于 NVS 记录的上次触发时刻时返回 true 并落盘更新。
  // 时间未同步时返回 false（每日模式依赖 NTP / SIM 时间）。
  bool dailyDueLocked();

  // 记录每日模式的触发时刻并落盘（防重启后补发当天已发的通知；持锁调用）
  void markDailyFiredLocked();

  // 排程的人类可读描述（日志 / 测试邮件用，如「每日 09:00」「每 24 小时」）
  String schedText() const;

  // ---------------- 模板辅助 ----------------
  // 公共信息块：系统时间 + 各启用模组本机号码（无号码回退设备 IP）
  String infoBlock() const;
  // 设备信息行（IP / MAC / 设备名 / WiFi 信号 / 运行时长 / 芯片温度）
  String deviceInfoRows() const;
  // 芯片型号与片内温度（传感器未使能时展示「不支持」）
  String chipInfoRows() const;
  // 按模组分组的「模组信息 + SIM/信号」两节
  String modemGroupSection() const;
  // 所有启用模组的 SIM/信号行汇总
  String simSectionRows() const;
  // SIM 整体是否就绪（未启用任何模组时视为健康，不因无卡报警）
  bool simOverallReady() const;
  // 徽章副标题用的状态文本
  String simOverallStateText() const;
  String simStateText(uint8_t port) const;
  // 模组展示名：有别名用别名，否则「模组N」；modem<0 返回空串
  String modemLabelOf(int8_t modem) const;
  // 信号强度文本（csq 0..31，99/未知为 -1）
  static String signalText(int csq);
  // 运营商短代码中文名
  static String operCn(const String &code);
  // 纪元秒 -> "YYYY-MM-DD HH:MM:SS"；未同步（小于 1.6e9）返回「时间未同步」
  static String formatTs(uint32_t ts);
  // millis() 时长 -> 中文可读串（移植自参考实现 MsgContext::formatUptime）
  static String formatUptime(uint32_t ms);

  // ---------------- 短信元数据解析（移植自参考实现）----------------
  // 从短信正文解析发送者名称与验证码：
  //   发送者名称：扫描全部 【...】/[[...]]/[...]，优先取最靠近开头或结尾的括号内容
  //   验证码：优先取关键词附近的 4~6 位数字串，无关键词时取首个 4~6 位数字串
  // （关键词匹配、边界判定等辅助函数为 .cpp 内部自由函数，不进头文件）
  static void parseSmsMeta(const String &message, String &senderName,
                           String &verifyCode);

  WifiManager *_wifi = nullptr;  // WiFi 管理器（连接状态 / IP）
  SmsService *_sms = nullptr;    // 短信服务（模组与 SIM 状态）
  TimeSync *_time = nullptr;     // 时间同步（系统时间）

  // ---- SMTP 配置（NVS: appcfg/email；持 _mutex 访问）----
  String _server;                              // SMTP 服务器地址
  uint16_t _port = cfg::kMailSmtpPortDefault;  // SMTP 端口（仅隐式 TLS）
  String _user;                                // 发件邮箱（SMTP 登录账号）
  String _pass;                                // SMTP 授权码（不回显）
  String _sendTo;                              // 收件邮箱
  bool _enabled = true;                        // SMTP 邮件通知总开关（NVS 无记录时默认开启，兼容旧配置）
  uint32_t _notifyTypes = cfg::kEmailNotifyDefault;  // 通知类型位掩码
  MailSchedule _sched;                         // 定时在线通知排程（两种模式）
  uint32_t _lastDailyEpoch = 0;                // 每日模式上次触发时刻（纪元秒，落盘防重启重复）

  // ---- 运行时状态 ----
  MailJob _queue[cfg::kMailQueueSize];  // 待发邮件队列（FIFO，满时丢新的）
  uint8_t _queueCount = 0;              // 队列有效条数
  uint32_t _nextRetryMs = 0;            // 下次退避重试时刻
  uint32_t _onlineDueMs = 0;            // 间隔模式下次触发时刻
  uint32_t _startupDueMs = 0;           // 启动邮件计划发送时刻
  uint8_t _startupTries = 0;            // 启动邮件已等待次数
  bool _startupPending = false;         // 启动邮件待发送
  bool _onlineImmediate = false;        // 保存配置后补发一封在线通知（确认用）

  mutable SemaphoreHandle_t _mutex = nullptr;  // 保护配置与队列（递归锁）
};
