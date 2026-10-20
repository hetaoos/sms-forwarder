#pragma once

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <stdint.h>
#include <ArduinoJson.h>

#include "app/Config.h"
#include "app/Psram.h"

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
  // wifi：WiFi 管理器（连接状态/IP）；sms：短信服务（通信模组与 SIM 状态）；
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

  // ---------------- 配置只读快照（LCD 菜单「邮件通知」页用）----------------
  // 一律持锁读取；返回副本，调用方无需关心锁的生命周期。
  String server() const;             // SMTP 服务器
  uint16_t port() const;             // SMTP 端口
  String user() const;               // 发件邮箱
  String sendTo() const;             // 收件邮箱
  uint8_t queueCount() const;        // 当前待发邮件数
  uint32_t notifyTypes() const;      // 通知类型位掩码（cfg::kEmailNotify*）
  String scheduleText() const;       // 定时在线通知排程描述（如「每 24 小时」）

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

  // 导出完整配置（含授权码，供备份用）到给定 JSON 对象（字段同 configJson 但含 pass）
  void exportTo(JsonObject &o) const;

  // 从给定 JSON 对象导入配置并落盘；缺失字段保持原值；成功返回 true
  // 字段：enabled/server/port/user/pass/sendTo/notifyTypes/
  // schedMode/schedHour/schedMinute/intervalH
  bool importFrom(const JsonObject &o);

  // ---------------- 通知入队（非阻塞，回调上下文安全）----------------
  // 以下方法运行于串口抽数任务（SerialBridge 抽数任务）等后台上下文，
  // **只允许做浅入队**（互斥锁 + 少量字符串拷贝）。HTML 模板构建（JSON 解析 +
  // 大量字符串拼接，约 2-3KB 栈）一律推迟到 App::tick（loop 任务）发送前进行，
  // 否则会撑爆抽数任务栈导致其死亡——短信收发全部停止。
  // 与 esp32-sms-forwarding 一致：推送链（含邮件构建与 SMTP）只在 loop 任务执行。
  //
  // 收到短信：入队一封短信邮件（未配置或该类已关闭时静默跳过）
  // sender：发件人号码；message：正文；ts：纪元秒（未同步时为开机秒数）；
  // modem：接收通信模组串口号（1..kSmsMaxModems，<0 未知）
  void notifySms(const String &sender, const String &message, uint32_t ts,
                 int8_t modem);

  // SIM 状态变更（运行时插卡/拔卡，由 SmsService 状态切换触发）
  void notifySim(uint8_t port, bool ready);

  // 来电提醒（+CLIP/+CLCC 检测，由 SmsService 来电状态机派发时触发）。
  // number：主叫号码（未获取到时为「未知号码」/「号码保密」）；
  // modem：收到来电的通信模组串口号（1..kSmsMaxModems，<0 未知）
  void notifyCall(const String &number, uint32_t ts, int8_t modem);

  // 排入启动邮件（App::begin 调用一次）。发送时机：
  //   1. 先等 kMailBootDelayMs（WiFi 通常此时还没连上）；
  //   2. WiFi 未连：每 15s 探一次，超过 kMailBootWaitMaxMs 放弃（无网发不出）；
  //   3. WiFi 已连：给通信模组初始化 kMailBootModemWaitMs 的宽限，期间通信模组一有
  //      结果（就绪 / 故障）就立即发；到期仍未出结果也照发（正文的通信模组状态会
  //      显示为未就绪），不会因通信模组异常而永远不发。
  void notifyStartup();

  // 启动邮件的额外放行条件（除 WiFi 外的等待项，如「通信模组初始化已完成」）。
  // 返回 true 表示可以发；未注入时只等 WiFi。
  // 由 App 注入（SmsService::modemsSettled），原因：启动邮件正文含通信模组状态，
  // 通信模组还在初始化时发出会显示「初始化中」，故需等其出结果（就绪或故障）。
  using StartupGateFn = std::function<bool()>;
  void setStartupGate(StartupGateFn fn) { _startupGate = fn; }

  // 立即排入一封「设备在线」通知邮件
  void notifyOnline();

  // 请求发送一封测试邮件：只入队并立即返回，真正的构建与发送由 tick() 在
  // loop 任务上下文完成（与短信/SIM/在线通知同一条链路）。
  // 不可在 HTTP 任务里同步发送：本方法运行于 async_tcp 任务，其栈远小于
  // SMTP + TLS 握手所需，同步发送会栈溢出导致设备重启；且最长 15s 的阻塞
  // 会拖死整个 Web 服务。返回 false 表示当前不该发（总开关未开或配置不全）
  bool requestTest();

  // ---------------- 统一 HTML 邮件模板工具 ----------------
  // 模板约定（与 esp32-sms-forwarding 一致）：所有通知邮件共用 buildHtml()
  // 生成的卡片式正文；各邮件只提供标题与卡片主体片段。
  // 公共信息块（系统时间 + 各通信模组本机号码，无号码回退设备 IP）由
  // buildHtml() 自动追加，新增邮件类型也自动带上。
  //
  // 内存约定：正文一律拼进 PsBuffer（优先 PSRAM），不产生中间 String。
  // 单封正文可达数 KB~十几 KB，若走 Arduino String 则全部压在内部 DRAM 上，
  // 与 LWIP 缓冲、任务栈抢空间；改用 PSRAM 后 DRAM 峰值只剩 subject/text 等短串。
  // 拼接过程中任一步分配失败都会置 ok()=false，调用方据此丢弃本次通知。

  // HTML 转义（& < > " 与换行）。所有动态文本进入邮件前都必须过这一层。
  // 仅用于主题等短文本（返回 String，落 DRAM）；正文请用 appendEscaped()。
  String escape(const String &s) const;

  // 把转义结果直接追加进缓冲（正文路径专用，不产生中间 String）
  void appendEscaped(PsBuffer &out, const String &s) const;

  // 一行「键: 值」表格行（键值均已转义）
  void appendRow(PsBuffer &out, const String &key, const String &val) const;

  // 一行「键: 链接」表格行。用自绘 <a> 而不是裸文本 URL：多数邮件客户端
  // 会把裸 URL 自动转成链接，且转写时常把纯数字主机名改坏，自绘锚点可锁定显示文本。
  void appendLinkRow(PsBuffer &out, const String &key, const String &url) const;

  // 带内边距的表格块：appendTableOpen() ... 若干行 ... appendTableClose()
  void appendTableOpen(PsBuffer &out) const;
  void appendTableClose(PsBuffer &out) const;

  // 带小标题的表格块（用于把多组行分组展示），开闭用法同上
  void appendSectionOpen(PsBuffer &out, const String &caption) const;
  void appendSectionClose(PsBuffer &out) const;

  // 状态徽标：main 为醒目主文案，sub 为补充说明，ok 决定配色（绿/橙）
  void appendBadge(PsBuffer &out, const String &main, const String &sub,
                   bool ok) const;

  // 纯文字说明段（正文里唯一允许出现换行渲染的地方）
  void appendNote(PsBuffer &out, const String &text) const;

  // 完整 HTML 正文：样式表 + 卡片 + 标题栏 + 主体 + 公共信息块 + 底部标识。
  // 写入 out（PSRAM）；inner 为卡片主体片段；title 可带 emoji，
  // 需调用方自行保证已转义。
  void buildHtml(PsBuffer &out, const String &title, const PsBuffer &inner,
                 int maxWidth = 560) const;

  // HTML 正文上限：urlEncode/编码后体积会膨胀数倍，ESP32 堆有限，
  // 超出时只发纯文本兜底。
  static constexpr int kMaxHtmlChars = 6000;

 private:
  // 队列单元的事件类型
  enum Kind : uint8_t {
    kSms = 0,    // 收到短信
    kSim,        // SIM 状态变更
    kStartup,    // 设备启动完成
    kOnline,     // 定时在线通知
    kCall,       // 来电提醒
    kTest,       // 测试邮件
  };

  // 一封待发邮件（队列单元）——只存「原始事件」，不存构建好的 HTML。
  // 原因：入队路径运行于串口抽数任务（4KB 栈），在那里构建 HTML 会栈溢出；
  // HTML 在 loop 任务发送前由 buildMail() 现构（约 2-3KB 栈，loop 任务 8KB）。
  struct MailJob {
    Kind kind = kSms;        // 事件类型
    String sender;           // 短信：发件人号码
    String message;          // 短信：正文
    uint32_t ts = 0;         // 短信：时间戳（纪元秒；未同步时为开机秒数）
    int8_t modem = -1;       // 短信：接收通信模组串口号（<0 未知）
    uint8_t port = 0;        // SIM 事件：通信模组串口号
    bool ready = false;      // SIM 事件：true=已插入 false=已拔出
    uint8_t attempts = 0;    // 已失败次数（退避重试计数）
  };

  // 递归互斥锁守卫：配置读写、入队与模板构建都可能嵌套加锁。
  // 直接持有信号量句柄，const 方法亦可使用（_mutex 为 mutable）。
  struct Guard {
    SemaphoreHandle_t m;
    explicit Guard(SemaphoreHandle_t m_) : m(m_) {
      // 允许 NULL 句柄：本通信模组 begin() 之前（如开机首帧状态采集）也可能被读取，
      // xSemaphoreTakeRecursive(NULL) 会触发断言并导致无限重启
      if (m) xSemaphoreTakeRecursive(m, portMAX_DELAY);
    }
    ~Guard() { if (m) xSemaphoreGiveRecursive(m); }
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
  // html 为 PSRAM 正文缓冲，直接以「静态内容」形式交给 ReadyMail
  // （body(const char*, size_t)），全程不复制回 DRAM。
  // configErr 出参：true = 配置类错误（认证失败等，重试无意义，直接丢弃）。
  bool sendOnce(const String &subject, const PsBuffer &html, const String &text,
                bool &configErr);

  // 由原始事件构建邮件正文（loop 任务调用，可做重模板拼接）。
  // 返回 false 表示「当前不该发」：服务未启用/未配置，或该类通知已关闭
  // （入队后配置可能被改动，发送前再判定一次），或正文缓冲分配失败。
  bool buildMail(const MailJob &job, String &subject, PsBuffer &html,
                 String &text) const;

  // ---------------- 各事件邮件的构建 ----------------
  void buildStartupMail(String &subject, PsBuffer &html, String &text) const;
  void buildOnlineMail(String &subject, PsBuffer &html, String &text) const;
  void buildSimMail(uint8_t port, bool ready, String &subject, PsBuffer &html,
                    String &text) const;
  void buildSmsMail(const String &sender, const String &message, uint32_t ts,
                    int8_t modem, String &subject, PsBuffer &html,
                    String &text) const;
  void buildCallMail(const String &number, uint32_t ts, int8_t modem,
                     String &subject, PsBuffer &html, String &text) const;
  void buildTestMail(String &subject, PsBuffer &html, String &text) const;

  // 判定开关与配置后入队（tick 调用，loop 任务上下文；只做浅入队）
  void enqueueStartupLocked();
  void enqueueOnlineLocked();

  // 每日定点模式是否到点（持锁调用）：本地时间进入触发分窗口、
  // 且晚于 NVS 记录的上次触发时刻时返回 true 并落盘更新。
  // 时间未同步时返回 false（每日模式依赖 NTP / SIM 时间）。
  bool dailyDueLocked();

  // 记录每日模式的触发时刻并落盘（防重启后补发当天已发的通知；持锁调用）
  void markDailyFiredLocked();

  // 排程的人类可读描述（日志 / 测试邮件用，如「每日 09:00」「每 24 小时」）
  String schedText() const;

  // ---------------- 模板辅助（均追加进 PSRAM 缓冲）----------------
  // 公共信息块：系统时间 + 各启用通信模组本机号码（无号码回退设备 IP）
  void appendInfoBlock(PsBuffer &out) const;
  // 设备信息行（IP / MAC / 设备名 / WiFi 信号 / 运行时长 / 芯片温度）
  void appendDeviceRows(PsBuffer &out) const;
  // 芯片型号与片内温度（传感器未使能时展示「不支持」）
  void appendChipRows(PsBuffer &out) const;
  // 按通信模组分组的「通信模组信息 + SIM/信号」两节
  void appendModemGroupSection(PsBuffer &out) const;
  // 所有启用通信模组的 SIM/信号行汇总
  void appendSimRows(PsBuffer &out) const;
  // SIM 整体是否就绪（未启用任何通信模组时视为健康，不因无卡报警）
  bool simOverallReady() const;
  // 徽章副标题用的状态文本
  String simOverallStateText() const;
  String simStateText(uint8_t port) const;
  // 通信模组展示名：有别名用别名，否则「通信模组N」；modem<0 返回空串
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
  SmsService *_sms = nullptr;    // 短信服务（通信模组与 SIM 状态）
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
  uint32_t _startupWifiMs = 0;          // 启动邮件等待期间 WiFi 首次连上的时刻（0=尚未连上）
  uint8_t _startupTries = 0;            // 启动邮件已等待次数（仅用于等 WiFi 的放弃判定）
  bool _startupPending = false;         // 启动邮件待发送
  StartupGateFn _startupGate;           // 启动邮件额外放行条件（未注入则只等 WiFi）
  bool _onlineImmediate = false;        // 保存配置后补发一封在线通知（确认用）

  mutable SemaphoreHandle_t _mutex = nullptr;  // 保护配置与队列（递归锁）
};
