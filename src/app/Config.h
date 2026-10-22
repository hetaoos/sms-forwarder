#pragma once

#include <stdint.h>

// 全局配置：所有引脚 / 网络参数集中在这里，通信模组不写死常量
namespace cfg {

// ---------------- WiFi 默认账号（编译期可注入）----------------
// 在 platformio.ini 的 build_flags 里用 -DWIFI_SSID_DEFAULT=\"xxx\" 覆盖；
// 未设置时默认为空串，表示不内置账号，完全依赖 NVS 中保存的配置
#ifndef WIFI_SSID_DEFAULT
#define WIFI_SSID_DEFAULT ""
#endif
#ifndef WIFI_PASS_DEFAULT
#define WIFI_PASS_DEFAULT ""
#endif
constexpr const char *kWifiSsidDefault = WIFI_SSID_DEFAULT;  // 内置默认 SSID
constexpr const char *kWifiPassDefault = WIFI_PASS_DEFAULT;  // 内置默认密码

// ---------------- WiFi 连接策略 ----------------
constexpr uint8_t kWifiMaxNetworks = 5;        // NVS 最多保存的 WiFi 配置组数
constexpr uint8_t kWifiAttemptRounds = 3;      // 全部候选都连失败的轮数，超过则进 AP 模式
constexpr uint32_t kWifiConnectTimeoutMs = 8000;  // 单个候选的连接等待上限（ms）
constexpr uint32_t kWifiLostRetryMs = 5000;    // STA 掉线后多久开始重连（ms）
constexpr uint32_t kWifiApRetryMs = 30000;     // AP 模式下后台静默重试 STA 的间隔（ms）

// ---------------- Web 管理账号（编译期可注入）----------------
// 在 platformio.ini 的 build_flags 里用 -DWEB_AUTH_USER_DEFAULT=\"xxx\" 覆盖；
// 未设置时为空串，表示未注入。
// 生效规则（见 AdminStore::begin 的 NVS 初始化）：用户名与密码必须同时非空才
// 采用编译期值，否则整体回落出厂默认 admin / admin123，避免出现无法登录的账号。
// 仅在 NVS 无有效账号记录时生效（首次启动写入 NVS，之后以 NVS 为准，网页可改）
#ifndef WEB_AUTH_USER_DEFAULT
#define WEB_AUTH_USER_DEFAULT ""
#endif
#ifndef WEB_AUTH_PASS_DEFAULT
#define WEB_AUTH_PASS_DEFAULT ""
#endif
constexpr const char *kWebAuthUserDefault = WEB_AUTH_USER_DEFAULT;  // 编译期注入的用户名（可为空）
constexpr const char *kWebAuthPassDefault = WEB_AUTH_PASS_DEFAULT;  // 编译期注入的密码（可为空）
// 出厂兜底账号：编译期注入不完整（任一为空/超长）时使用
constexpr const char *kWebAuthUserFallback = "admin";
constexpr const char *kWebAuthPassFallback = "admin123";

// ---------------- AP 配网模式 ----------------
constexpr const char *kApSsid = "SMS-Forwarder-Setup";  // AP 热点名（仅配网用）

// AP 配网热点密码：运行时 NVS 可改，编译期可注入默认（与 WEB_AUTH_*_DEFAULT 同套路）。
// 在 platformio.ini 用 -DAP_PASS_DEFAULT=\"xxx\" 覆盖；未设置（空串）表示开放热点。
// 非空必须是 8~63 字符（WPA2 要求），否则回落开放并告警（避免 softAP 静默失败）。
// 首次启动（NVS 无记录）采用编译期值并落盘；之后以 NVS 为准，可在网页/设置修改。
#ifndef AP_PASS_DEFAULT
#define AP_PASS_DEFAULT ""
#endif
constexpr const char *kApPassDefault = AP_PASS_DEFAULT;  // AP 热点密码（可为空）
constexpr size_t kApPassMaxLen = 63;                    // WPA2 口令长度上限
constexpr const char *kCookieName = "sid";        // 登录会话 Cookie 名（HttpOnly + SameSite=Strict）
constexpr uint8_t kMaxSessions = 8;               // 内存中同时有效的会话 / 访问令牌数上限
constexpr uint32_t kCookieMaxAge = 604800;        // 会话 Cookie 有效期（秒，7 天）

// ---------------- NVS 配置存储 ----------------
// 按功能分区存放 JSON：WiFi 配置整体是一个 JSON 对象
// {"ver":1,"list":[{"ssid":"...","pass":"..."}, ...]}
constexpr const char *kNvsNamespace = "appcfg";  // NVS 命名空间
constexpr const char *kNvsKeyWifi = "wifi";      // WiFi 配置键（JSON 对象）
constexpr const char *kNvsKeyApPass = "appass";   // AP 配网热点密码键（{"ver":1,"pass":".."}）
constexpr const char *kNvsKeyAuth = "auth";      // 管理账号键（JSON 对象，{"ver":1,"user":"..","pass":".."}）
constexpr const char *kNvsKeySms = "sms";        // 短信配置键（{"ver":1,"max":n,"modems":[{"port":1,"enabled":true},..]}）
constexpr const char *kNvsKeyToken = "apitoken"; // API 访问令牌键（持久化，重启不失效，用于脚本/第三方客户端）
constexpr const char *kNvsKeyEmail = "email";    // 邮件配置键（{"ver":1,"server":"..","port":465,"user":"..","pass":"..","sendTo":"..","notifyTypes":n,"onlineH":n}）
constexpr const char *kNvsKeyMqtt = "mqtt";      // MQTT 配置键（{"ver":1,"server":"..","port":1883,"useTls":bool,"user":"..","pass":"..","clientId":"..","topicPrefix":"..","keepAlive":n,"subscribeEnabled":bool,"notifyTypes":n,"enabled":bool}）
constexpr const char *kNvsKeyPush = "push";      // 推送通道配置键（{"ver":1,"enabled":bool,"strategy":n,"channels":[...]}）
constexpr const char *kNvsKeyBlacklist = "blacklist";  // 黑名单配置键（{"ver":1,"enabled":bool,"action":n,"list":[...]}）
constexpr const char *kNvsKeyDisp = "disp";        // 屏幕设置键（{"ver":1,"bright":n,"autoOffSec":n,"idleSec":n}）
constexpr uint8_t kBlacklistMax = 100;           // 黑名单规则条数上限
constexpr uint8_t kAdminPhoneMax = 5;            // 管理员号码个数上限（短信远程控制）

// ---------------- 崩溃转储（coredump）信息 ----------------
// 崩溃元信息独立命名空间，不随出厂复位清除（appcfg 会被 handleSystemReset
// 整表清空）——崩溃证据需要在复位配置后仍可追溯；coredump 分区里的快照
// 同样只在用户主动清除时擦除。分区由 ESP-IDF panic 处理器自动写入（ELF
// 格式），这里只记录时间与版本，供 Web「工具箱」查询 / 导出 / 清除，
// 详见 app/Coredump。
constexpr const char *kNvsNamespaceCoredump = "coredump";  // 独立命名空间
constexpr const char *kNvsKeyCdCrashTs  = "cdCrashTs";   // 上次崩溃时间戳（纪元秒）
constexpr const char *kNvsKeyCdCrashVer = "cdCrashVer";  // 上次崩溃时固件版本（Release tag）

// ---------------- 分区表 label ----------------
// 分区表（partitions/esp32s3_16mb.csv）中各分区的 label。
// 多个 data 分区 subtype 同为 spiffs（userdata / spiffs），按 label 查找分区时
// 必须传精确 label，严禁传 nullptr（会返回地址最小的 userdata，误把短信分区当
// Web UI 分区擦除，导致升级后短信全丢，见 ota/Ota.cpp）。所有按 label 查找 /
// 挂载分区的地方统一引用此处常量，禁止再硬编码字符串。
constexpr const char *kPartLabelSpiffs   = "spiffs";    // Web UI 资源分区（LittleFS 镜像）
constexpr const char *kPartLabelUserdata = "userdata";  // 用户数据分区（短信等，挂载 /data）
constexpr const char *kPartLabelCoredump = "coredump";  // 崩溃转储分区（按 subtype 查找，label 仅供参考）

// ---------------- SMTP 邮件通知（参考 esp32-sms-forwarding）----------------
// 通知类型位掩码：每个位代表一类「会单独发一封 HTML 邮件」的事件。
// 关闭后该事件不再发邮件，短信存储 / SSE / Web 等其余功能不受影响。
// 位定义与 esp32-sms-forwarding 的 EmailNotifyType 保持一致，便于两个项目对照；
// 新增类型时：这里加一位 → EmailService 加一个构建分支 → Web 表单加一个复选框。
constexpr uint32_t kEmailNotifyStartup = 1u << 0;  // 设备启动完成
constexpr uint32_t kEmailNotifySim     = 1u << 1;  // SIM 状态变更（运行时插入/拔出）
constexpr uint32_t kEmailNotifySms     = 1u << 2;  // 收到短信
constexpr uint32_t kEmailNotifyOnline  = 1u << 3;  // 定时在线通知（确认设备仍在线）
constexpr uint32_t kEmailNotifyCall    = 1u << 4;  // 来电提醒（+CLIP/+CLCC 检测，号码可能未知）
// 全部已知位（保存配置时过滤越界位用）
constexpr uint32_t kEmailNotifyAll = kEmailNotifyStartup | kEmailNotifySim |
                                     kEmailNotifySms | kEmailNotifyOnline |
                                     kEmailNotifyCall;
// 出厂默认：五类全开
constexpr uint32_t kEmailNotifyDefault = kEmailNotifyAll;

// ---------------- MQTT 事件上报（与 esp32-sms-forwarding 复用同一套位定义）----------------
// 通知类型位掩码：每个位代表一类「会发布到 MQTT」的事件。
// 低 5 位与邮件 kEmailNotify* 完全一致（语义相同），便于 Web 表单与固件对照；
// LOG 为 MQTT 专属位（运行日志实时推送，邮件不支持）。
// 新增类型时：这里加一位 → MqttService 加发布分支 → Web 表单加一个复选框。
constexpr uint32_t kMqttNotifyStartup = 1u << 0;  // 设备启动完成
constexpr uint32_t kMqttNotifySim     = 1u << 1;  // SIM 状态变更（插入/拔出）
constexpr uint32_t kMqttNotifySms     = 1u << 2;  // 收到短信
constexpr uint32_t kMqttNotifyOnline  = 1u << 3;  // （重新）上线通知（确认设备在线）
constexpr uint32_t kMqttNotifyCall    = 1u << 4;  // 来电提醒（发布到 .../call 主题）
constexpr uint32_t kMqttNotifyLog     = 1u << 16; // 运行日志实时推送（仅 MQTT）
// 全部已知位（保存配置时过滤越界位用）
constexpr uint32_t kMqttNotifyAll = kMqttNotifyStartup | kMqttNotifySim |
                                    kMqttNotifySms | kMqttNotifyOnline |
                                    kMqttNotifyCall | kMqttNotifyLog;
// 出厂默认：除「运行日志」外全部开启（日志推送默认关闭，避免日志风暴；
// 用户可在页面单独勾选开启）。注意：运行日志为 MQTT 专属位（1<<16），
// 不纳入默认，与邮件通知类型默认全开区分。
constexpr uint32_t kMqttNotifyDefault = kMqttNotifyStartup | kMqttNotifySim |
                                        kMqttNotifySms | kMqttNotifyOnline |
                                        kMqttNotifyCall;

// SMTP 默认端口：465 为隐式 TLS（握手即加密，多数服务商推荐）。
// 587（STARTTLS 明文起连后升级）不适用——底层直接用 WiFiClientSecure 连接。
constexpr uint16_t kMailSmtpPortDefault = 465;
constexpr uint16_t kMailOnlineIntervalMaxH = 720;    // 定时在线通知最大间隔（小时，30 天）
constexpr uint16_t kMailIntervalHMin = 1;            // 间隔模式周期下限（小时）
constexpr uint16_t kMailIntervalHDefault = 24;       // 间隔模式默认周期（小时）
constexpr uint8_t  kMailQueueSize = 6;               // 待发邮件队列深度（超出丢弃新通知）
constexpr uint8_t  kMailMaxAttempts = 3;             // 单封邮件最大尝试次数（含首次）
constexpr uint32_t kMailSocketTimeoutMs = 15000;     // SMTP 单次 socket 读写上限（ms）
constexpr uint32_t kMailBootDelayMs = 15000;         // 启动邮件延迟（等 WiFi 连上再发）
constexpr uint32_t kMailBootWaitMaxMs = 300000;      // 启动邮件等 WiFi 的最长等待（5 分钟，超时放弃）
// WiFi 连上后留给通信模组初始化的宽限：期间通信模组一有结果（就绪/故障）就发；
// 到期仍未出结果也照发（正文的通信模组状态会显示未就绪），避免通信模组异常时
// 启动邮件一直发不出去。总时长仍受 kMailBootWaitMaxMs 之外的这段限制。
constexpr uint32_t kMailBootModemWaitMs = 20000;     // WiFi 连上后最多再等通信模组 20s

// ---------------- 定时在线通知排程模式（与 esp32-sms-forwarding 的 Schedule.mode 一致）-
// 总控开关为通知类型位 kEmailNotifyOnline（Web「通知类型」里的「定时在线通知」）；
// 勾选后按这里的模式触发：间隔模式基于 millis()（开机起算），
// 每日模式基于本地时间（依赖 NTP / SIM 时间同步，未同步时不触发）。
constexpr uint8_t kSchedModeDaily    = 0;  // 每日定点（hour:minute）
constexpr uint8_t kSchedModeInterval = 1;  // 固定间隔（intervalH 小时）
// 每日模式触发窗口（秒）：tick 被邮件发送等阻塞时可能错过触发分，
// 窗口内的迟到触发仍算有效（配合 NVS 记录的上次触发时刻，不会重复发）
constexpr uint32_t kMailDailyWindowSec = 600;

// ---------------- 外部串口（AT 通信模组）----------------
// 串口1：TX -> 通信模组 RX，RX <- 通信模组 TX
constexpr int8_t kUart1Tx = 17;    // 串口1 TX 引脚
constexpr int8_t kUart1Rx = 18;    // 串口1 RX 引脚
// 串口2
constexpr int8_t kUart2Tx = 41;    // 串口2 TX 引脚
constexpr int8_t kUart2Rx = 40;    // 串口2 RX 引脚
constexpr uint32_t kUartBaud = 115200;  // 串口默认波特率

// ---------------- ST7789 240x320 ----------------
constexpr int8_t kTftCs  = 15;     // 片选 CS
constexpr int8_t kTftRst = 5;      // 复位 RST
constexpr int8_t kTftDc  = 14;     // 数据/命令选择 DC
constexpr int8_t kTftMosi = 13;    // SPI 数据 SDA(MOSI)
constexpr int8_t kTftSclk = 12;    // SPI 时钟 SCL(SCLK)
constexpr int8_t kTftBlk = 8;      // 背光控制
constexpr uint16_t kTftWidth  = 240;   // 面板宽（像素）
constexpr uint16_t kTftHeight = 320;   // 面板高（像素）
constexpr uint8_t  kTftRotation = 0;   // 屏幕旋转方向
constexpr uint32_t kTftSpiFreq = 40000000;  // SPI 写入频率 40MHz
constexpr uint8_t  kTftBacklight = 80;      // 默认亮度 %

// ---------------- 按键（LCD UI 导航）----------------
// 4 键：OK / UP / DOWN / BACK，低电平有效（GPIO 接上拉，按键到 GND）。
// 仅作导航输入，不参与任何输出；GPIO2 为 Strapping 引脚但只作输入读取，无副作用。
constexpr int8_t  kBtnOk   = 42;   // OK（确认 / 进入）
constexpr int8_t  kBtnUp   = 47;   // UP（上移 / 增加）
constexpr int8_t  kBtnDown = 38;   // Down（下移 / 减少）
constexpr int8_t  kBtnBack = 2;    // Back（返回 / 取消；长按熄屏/唤醒）
constexpr uint16_t kBtnLongMs    = 600;     // 长按阈值（ms）
constexpr uint16_t kBtnRepeatMs  = 180;     // 长按连发间隔（ms，UP/DOWN/OK 调值用）

// ---------------- 屏幕设置（NVS 持久化，见 app/UIMenu）----------------
// 两项超时均基于「最后一次有效输入」计时，0 表示对应功能关闭。
// 语义：先到点的先执行——菜单无操作到 IdleReturn 秒退回实时状态页；
// 无操作到 AutoOff 秒自动熄屏（内容保留，任意键唤醒）。
constexpr uint16_t kUiSecMin = 5;               // 非零取值下限（秒，防误设成 1-4s 抖动）
constexpr uint16_t kUiIdleReturnSecMax = 600;   // 自动返回状态页上限（秒，10 分钟）
constexpr uint16_t kUiAutoOffSecMax = 3600;     // 自动熄屏上限（秒，1 小时）
constexpr uint16_t kUiIdleReturnDefaultSec = 10;  // 默认 10s 无操作退回状态页
constexpr uint16_t kUiAutoOffDefaultSec = 0;      // 默认不自动熄屏（需用户显式开启）
// 菜单里长按 +/- 连调亮度后的落盘防抖：连发期间只改内存，停手后才写 NVS，
// 避免一次长按产生几十次 NVS 写入
constexpr uint32_t kDispSaveDebounceMs = 2000;
constexpr uint32_t kUiIdleSleepMs = 10000;  // 菜单无操作自动退回状态页（ms）
// 新短信弹窗自动停留时长（ms）：到点自动收起回到原页面，按任意键可立即收起
constexpr uint32_t kUiSmsPopupMs = 30000;
// 菜单「短信」页最多预览的条数（受菜单槽位限制，取小值；区域可滚动，8 条够翻一阵）
constexpr uint8_t kUiSmsPreviewMax = 8;

// ---------------- LCD 工具箱（检查更新 / 重启 / 恢复出厂，见 app/UIMenu）----------------
// 「重置」是破坏性操作：必须连按 OK 达 kToolResetConfirm 次才执行，且每按一下
// 都会刷新计时——停手超过 kToolResetTimeoutMs 还没按满则计数清零，防止误触。
constexpr uint8_t  kToolResetConfirm = 5;        // 恢复出厂需连按 OK 的次数
constexpr uint32_t kToolResetTimeoutMs = 8000;   // 连按中断多久未续按则计数清零（ms）
// 确认重启 / 重置后的延迟：给屏幕留出提示时间，期间按 BACK 可取消
// （与 Web「系统维护」的 2s 延迟一致）
constexpr uint32_t kToolActDelayMs = 2000;
// 发起 OTA 检查/升级后等待其真正启动的宽限：Ota 内部有 30s 防抖与占用判定，
// 被挡下时不会进入非 IDLE 状态，超过宽限即回报「操作未开始」
constexpr uint32_t kToolOtaStartGraceMs = 3000;

// ---------------- LVGL UI（src/ui）----------------
// LVGL 的心跳间隔：lv_timer_handler() 会返回下一次应调用的毫秒数，返回值异常小
// （忙时可能为 0）时用它兜底，避免在主循环里空转烧 CPU
constexpr uint32_t kLvglMinTimerGapMs = 1;
// 中文字库：运行时由 LVGL 的 TinyTTF 从该文件渲染（文件放 data/ -> LittleFS 资源分区）
// GB2312 全集，约 1.2MB；已加进 script/uploadfs_gz.py 的不压缩名单，不能 gzip
constexpr const char *kUiFontPath = "/ui_cn.ttf";
constexpr int32_t kUiFontTextPx = 16;    // 正文/菜单/状态值字号
constexpr int32_t kUiFontTitlePx = 20;   // 标题栏字号
// TinyTTF 每个字号的字形缓存条数（命中才能不重新光栅化字形）
constexpr uint32_t kUiFontGlyphCacheCnt = 256;

// ---------------- 其它 ----------------
constexpr uint16_t kHttpPort = 80;      // HTTP 控制台端口
constexpr uint32_t kUiRefreshMs = 500;  // 屏幕刷新周期
// 开机页（Logo + 进度条）展示时长：这段时间里只画开机页，不去采集业务状态，
// 让中文字库加载与各通信模组初始化都发生在「不取数」的窗口内，避免初始化期冲突
constexpr uint32_t kBootScreenMs = 2500;
constexpr uint32_t kBootBarStepMs = 100;  // 进度条推进间隔

// ---------------- SSE 实时推送 ----------------
constexpr const char *kSsePath = "/api/sse";  // SSE 事件流路径（需认证，同 /api/）
constexpr uint32_t kSsePushMs = 1000;         // 状态采集/比对的周期（ms）
constexpr uint8_t kSseMaxClients = 8;         // 并发 SSE 客户端上限（超出顶掉最旧）

// ---------------- 设备日志（PSRAM 环形存储）----------------
// 运行日志环形缓冲（log/LogStore）：条目缓冲区整块分配在 PSRAM，与 WiFi/
// LwIP 等内部堆使用者隔离，定长条目 + FIFO 淘汰最旧，运行期零堆分配。
// PSRAM 容量充裕（8MB 板型）：2048 条 x 228B ≈ 467KB，可回溯较长一段时间的
// 运行记录；无 PSRAM / PSRAM 分配失败退化到内部堆时按 kLogRingSizeNoPsram
// 收紧条数，避免大缓冲挤占本就紧张的 DRAM（功能不变，只保留最近若干条）。
// 正文 192B（含 '\0'）足以放下一条完整日志（邮件主题、短信正文片段等），
// 超长自动截断——串口镜像输出的是同一份截断后的正文，两者不会不一致。
constexpr uint16_t kLogRingSize = 2048;       // PSRAM 下的环形缓冲条数上限（FIFO 淘汰最旧）
constexpr uint16_t kLogRingSizeNoPsram = 256; // 退化到内部堆时的条数上限
constexpr uint8_t  kLogTagMax = 24;           // 标签字段长度（含 '\0'，超出截断）
constexpr uint8_t  kLogMsgMax = 192;          // 正文字段长度（含 '\0'，超出截断）
constexpr uint8_t  kLogPageSizeMax = 100;     // 分页查询每页条数上限

// ---------------- AT 历史（串口日志，PSRAM 环形缓冲）----------------
// 与设备日志 _log 同思路：整块缓冲优先分配在 PSRAM，与 WiFi/LwIP/SSL 等
// 内部堆使用者隔离；有 PSRAM 时容量放宽（kAtHistoryRingSize），无 PSRAM 或
// 分配失败退化到内部堆并按 kAtHistoryRingSizeNoPsram 收紧，功能不变。
// 条目为定长结构（cmd/resp 内嵌字符数组，超长截断），写入只做 memcpy，
// 运行期零堆分配、无碎片，抽数任务 / HTTP 任务等任意上下文可安全调用。
// 单条目约 4+1+256+1024 = 1285B：PSRAM 下 512 条 ≈ 658KB，足够长时段回溯；
// 无 PSRAM 时收紧到 30 条 ≈ 38KB，避免挤占本就紧张的内部堆。
constexpr uint16_t kAtHistoryRingSize = 512;       // PSRAM 下环形缓冲条数上限（FIFO 淘汰最旧）
constexpr uint16_t kAtHistoryRingSizeNoPsram = 30; // 退化到内部堆时的条数上限
constexpr uint16_t kAtHistoryCmdMax = 256;         // 命令字段长度（含 '\0'，超长截断）
constexpr uint16_t kAtHistoryRespMax = 1024;       // 响应字段长度（含 '\0'，超长截断）

// ---------------- 短信（通信模组） ----------------
constexpr uint8_t kSmsMaxModems = 2;          // 可接通信模组数（与双串口一一对应）
constexpr uint16_t kSmsStoreMax = 1000;       // 短信存储条数上限（FIFO 淘汰）
constexpr uint8_t kSmsConcatSlots = 5;        // 并发长短信重组槽位
constexpr uint8_t kSmsConcatMaxParts = 20;    // 单条长短信最多分段
constexpr uint32_t kSmsConcatTimeoutMs = 30000;  // 长短信重组超时（收到最后一段后 30s）
constexpr uint32_t kSmsCmgsPromptMs = 5000;      // AT+CMGS= 后等 '>' 提示符的上限
constexpr uint32_t kSmsSendTimeoutMs = 30000;     // 单段 CMGS 发送超时（PDU 响应收集上限）
constexpr uint32_t kSmsCmdTimeoutMs = 1500;       // AT 配置类命令超时
constexpr uint32_t kSmsPageSizeMax = 100;     // 列表分页每页条数上限

// ---------------- 时间同步（NTP 为主，SIM NITZ 兜底）----------------
constexpr int32_t  kNtpGmtOffsetSec = 8 * 3600;                 // 时区偏移：UTC+8
constexpr uint32_t kNtpTimeoutMs    = 10000;                    // NTP 启动后等待首次同步的上限
constexpr uint32_t kNtpResyncMs     = 24UL * 3600UL * 1000UL;   // SNTP 后台周期校准间隔
constexpr uint32_t kSimTimeRetryMs  = 30000;                    // NTP 不可用时 SIM NITZ 重试间隔
constexpr uint32_t kSimTimeCmdMs    = 3000;                     // AT+CCLK? 响应收集超时
constexpr uint32_t kTimePersistMs   = 10UL * 60UL * 1000UL;     // 时间落盘周期（10 分钟）
constexpr const char *kNvsKeyLastTs = "lastts";                 // NVS 键：最后已知纪元时间（秒）

// ---------------- 通信模组初始化状态机（适配 esp32-sms-forwarding 健壮流程）-
// 全部非阻塞：每个状态的动作经 tick() 节流驱动，单个动作最多持端口锁 1.5s，
// 抽数任务对忙端口 try-lock 跳过——故障/未连接通信模组绝不阻塞另一通信模组与 Web。
constexpr uint32_t kModemAtPollMs = 1000;       // 等 AT 就绪轮询间隔
constexpr uint8_t  kModemAtReadyMax = 20;      // AT 就绪最大尝试(~20s)后转故障
constexpr uint32_t kModemCpinPollMs = 1500;    // 等待 SIM 就绪轮询间隔
constexpr uint8_t  kModemCpinMax = 15;         // SIM 就绪最大尝试(~22s)后转故障
constexpr uint32_t kModemRegPollMs = 2000;     // 网络注册轮询间隔
constexpr uint8_t  kModemRegMaxAttempts = 45;  // 注册最大尝试(~90s)后转故障
constexpr uint32_t kModemConfigStepMs = 400;   // 配置逐条下发间隔（避免一次持锁过久）
constexpr uint32_t kModemLivenessMs = 30000;   // 就绪后心跳间隔（探活）
constexpr uint8_t  kModemLivenessMaxFail = 3;  // 连续心跳失败转重初始化
constexpr uint32_t kModemFaultRetryMs = 15000; // 故障态重试间隔
constexpr uint32_t kModemTeleMs = 4000;        // 就绪后遥测轮询周期（信号/运营商/注册/IMEI）
// 通信模组硬复位（EN 引脚）：-1 表示不控制（沿用通信模组自身上电时序）
constexpr int8_t   kModemEnPin1 = -1;
constexpr int8_t   kModemEnPin2 = -1;
constexpr bool     kModemEnActiveLow = true;   // true: EN 拉低=关机；false: 拉高=关机

// ---------------- 定时重启（系统保活）----------------
// 配置以 JSON 存于 NVS（appcfg / sched）；模式复用 kSchedModeDaily(0=每日定点)
// 与 kSchedModeInterval(1=固定间隔)；默认禁用。
constexpr const char *kNvsKeySched = "sched";        // 定时重启配置键（JSON）
constexpr uint16_t kSchedIntervalHMin = 1;           // 间隔模式周期下限（小时）
constexpr uint16_t kSchedIntervalHMax = 720;         // 上限（30 天）
constexpr uint16_t kSchedIntervalHDefault = 24;      // 默认 24h
constexpr uint16_t kSchedDailyWindowSec = 600;       // 每日模式触发窗口（秒，容忍迟到 tick）

}  // namespace cfg
