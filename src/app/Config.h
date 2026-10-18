#pragma once

#include <stdint.h>

// 全局配置：所有引脚 / 网络参数集中在这里，模块不写死常量
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

// ---------------- AP 配网模式 ----------------
constexpr const char *kApSsid = "SMS-Forwarder-Setup";  // AP 热点名（开放无密码，仅配网用）
// 管理账号：NVS 无有效记录时的默认值（首次启动会写入 NVS，之后以 NVS 为准）
constexpr const char *kWebAuthUser = "admin";
constexpr const char *kWebAuthPass = "admin123";
constexpr const char *kCookieName = "sid";        // 登录会话 Cookie 名（HttpOnly + SameSite=Strict）
constexpr uint8_t kMaxSessions = 8;               // 内存中同时有效的会话 / 访问令牌数上限
constexpr uint32_t kCookieMaxAge = 604800;        // 会话 Cookie 有效期（秒，7 天）

// ---------------- NVS 配置存储 ----------------
// 按功能分区存放 JSON：WiFi 配置整体是一个 JSON 对象
// {"ver":1,"list":[{"ssid":"...","pass":"..."}, ...]}
constexpr const char *kNvsNamespace = "appcfg";  // NVS 命名空间
constexpr const char *kNvsKeyWifi = "wifi";      // WiFi 配置键（JSON 对象）
constexpr const char *kNvsKeyAuth = "auth";      // 管理账号键（JSON 对象，{"ver":1,"user":"..","pass":".."}）
constexpr const char *kNvsKeySms = "sms";        // 短信配置键（{"ver":1,"max":n,"modems":[{"port":1,"enabled":true},..]}）
constexpr const char *kNvsKeyToken = "apitoken"; // API 访问令牌键（持久化，重启不失效，用于脚本/第三方客户端）

// ---------------- 外部串口（AT 模块）----------------
// 串口1：TX -> 模块 RX，RX <- 模块 TX
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

// ---------------- 其它 ----------------
constexpr uint16_t kHttpPort = 80;      // HTTP 控制台端口
constexpr uint32_t kUiRefreshMs = 500;  // 屏幕刷新周期

// ---------------- SSE 实时推送 ----------------
constexpr const char *kSsePath = "/api/sse";  // SSE 事件流路径（需认证，同 /api/）
constexpr uint32_t kSsePushMs = 1000;         // 状态采集/比对的周期（ms）
constexpr uint8_t kSseMaxClients = 8;         // 并发 SSE 客户端上限（超出顶掉最旧）

// ---------------- 短信（4G 模组） ----------------
constexpr uint8_t kSmsMaxModems = 2;          // 可接模组数（与双串口一一对应）
constexpr uint16_t kSmsStoreMax = 1000;       // 短信存储条数上限（FIFO 淘汰）
constexpr uint8_t kSmsConcatSlots = 5;        // 并发长短信重组槽位
constexpr uint8_t kSmsConcatMaxParts = 20;    // 单条长短信最多分段
constexpr uint32_t kSmsConcatTimeoutMs = 30000;  // 长短信重组超时（收到最后一段后 30s）
constexpr uint32_t kSmsCmgsPromptMs = 5000;      // AT+CMGS= 后等 '>' 提示符的上限
constexpr uint32_t kSmsSendTimeoutMs = 30000;     // 单段 CMGS 发送超时（PDU 响应收集上限）
constexpr uint32_t kSmsCmdTimeoutMs = 1500;       // AT 配置类命令超时
constexpr uint32_t kSmsPageSizeMax = 100;     // 列表分页每页条数上限

// ---------------- 模组初始化状态机（适配 esp32-sms-forwarding 健壮流程）-
// 全部非阻塞：每个状态的动作经 tick() 节流驱动，单个动作最多持端口锁 1.5s，
// 抽数任务对忙端口 try-lock 跳过——故障/未连接模组绝不阻塞另一模组与 Web。
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
// 模组硬复位（EN 引脚）：-1 表示不控制（沿用模组自身上电时序）
constexpr int8_t   kModemEnPin1 = -1;
constexpr int8_t   kModemEnPin2 = -1;
constexpr bool     kModemEnActiveLow = true;   // true: EN 拉低=关机；false: 拉高=关机

}  // namespace cfg
