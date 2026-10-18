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
constexpr const char *kApSsid = "SMS-FWD-Setup";  // AP 热点名（开放无密码，仅配网用）
constexpr const char *kWebAuthUser = "admin";     // AP 模式下网页管理认证用户名
constexpr const char *kWebAuthPass = "admin123";  // AP 模式下网页管理认证密码

// ---------------- NVS 配置存储 ----------------
// 按功能分区存放 JSON：WiFi 配置整体是一个 JSON 对象
// {"ver":1,"list":[{"ssid":"...","pass":"..."}, ...]}
constexpr const char *kNvsNamespace = "appcfg";  // NVS 命名空间
constexpr const char *kNvsKeyWifi = "wifi";      // WiFi 配置键（JSON 对象）

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

}  // namespace cfg
