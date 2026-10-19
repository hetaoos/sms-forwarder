#pragma once
#include <Arduino.h>

// OTA 状态机
enum class OtaState : uint8_t {
  IDLE        = 0,    // 空闲
  CHECKING    = 1,    // 正在查询远端 Release 最新版本
  DOWNLOADING = 2,    // 正在下载固件
  WRITING     = 3,    // 正在写入 OTA 分区
  SUCCESS     = 4,    // 成功（等待重启）
  FAILED      = 5,    // 失败（message 携带原因）
  FLASHING_FS = 6,    // 正在烧录 LittleFS（替换 HTML 资源）
};

// 给前端的状态快照（JSON 序列化后通过 /api/ota/status 返回）
struct OtaStatusPayload {
  OtaState state          = OtaState::IDLE;
  uint8_t  progress       = 0;     // 0~100
  String   message        = "";    // 失败原因或当前阶段说明
  String   currentVersion = "";    // 当前固件版本（= Release tag）
  String   latestVersion  = "";    // 远端最新 Release tag（仅 CHECKING 完成后）
};

// ── 发布源与版本标识 ──────────────────────────────────────────────
//
// 发布产物由 CNB 流水线构建，托管在 CNB 的 Release 上。多硬件平台共用同一
// 个 Release，靠**产物文件名里的平台后缀**区分（见 CI 的 APP_PLATFORM）。
//   ota-<Release tag>-<platform>.bin      仅固件（写入 ota_0/ota_1）
//   littlefs-<Release tag>-<platform>.bin Web UI 文件系统镜像
// OTA 下载时拼接的 URL 形如：
//   <OTA_RELEASES_BASE_URL>/download/<Release tag>/ota-<Release tag>-<platform>.bin
//
// **Release tag 就是完整版本标识** <VERSION>-<构建时间>-<短 SHA>，如
// 1.0.0-20261004T121442-f2b513a。设备自报的版本、下载地址里的 tag、产物文件名
// 里的 tag、比较用的 tag 都是同一个值，同源才可能一致 —— 曾经 tag 只到
// VERSION（1.0.0）而产物名带时间戳，地址就必然 404。别再把它们拆开。
//
// 地址与 Release tag 都由 CI 注入（见 script/inject_app_version.py）。
// 曾经这里是两个硬编码常量，代价是：仓库一改名/迁移，旧固件里的地址就静默指向
// 一个不存在的仓库，「检查更新」只表现为「查不到版本」，排查起来毫无头绪。
// 这里的注入是编译期强制的 —— 拿不到就别想让固件过编译。
//
// 三个宏各管一头，缺一不可：
//   OTA_RELEASES_BASE_URL  产物下载与列表地址（<...>/-/releases[/download/<tag>/...]）
//   OTA_LATEST_VERSION_URL 「检查更新」的备用渠道（Release 徽章）
//   OTA_RELEASE_TAG        当前固件所属的 Release tag（= 完整版本标识），用于比较新旧
//                          （按语义比较，见 Ota.cpp 的 isRemoteNewer）
#ifndef OTA_RELEASES_BASE_URL
#error "缺少 OTA_RELEASES_BASE_URL：CI 需通过 -D OTA_RELEASES_BASE_URL=... 注入发布源地址"
#endif
#ifndef OTA_LATEST_VERSION_URL
#error "缺少 OTA_LATEST_VERSION_URL：CI 需通过 -D OTA_LATEST_VERSION_URL=... 注入最新版本徽章地址"
#endif
#ifndef OTA_RELEASE_TAG
#error "缺少 OTA_RELEASE_TAG：CI 需通过 -D OTA_RELEASE_TAG=... 注入 Release tag"
#endif

// 「检查更新」为什么准备了两个接口（细节见 Ota.cpp 的 probeLatestTag）：
//
// 渠道一 OTA_RELEASES_BASE_URL：Release 列表 JSON。信息最全，但它有个脆点 ——
// 服务器按 Accept 头**逐字**选体，只有精确等于 application/json 才回 JSON；
// 写成 "application/json, */*" 这种带通配的组合会被判成浏览器在要页面，
// 退回一整坨 HTML，解析随之失败。这种「差一个字就全盘失效、且失败得很安静」
// 的依赖，不该是设备唯一的取版本途径。此外仓库尚无 Release 时，该路由回的是
// {"errcode":404,...} 这种 **error 对象**而不是空数组，解析时要单独识别。
//
// 渠道二 OTA_LATEST_VERSION_URL：Release 徽章 SVG。给 README 嵌图用的公共只读
// 路由，没有内容协商这回事（带什么 Accept 都回 SVG），1KB 出头，右侧文案就是
// 最新 tag。作为渠道一的兜底。
//
// 两者不同源：Releases 在仓库 /-/releases 子路径下，徽章挂在仓库根上
// （/-/badge/release），拼不出来，所以各注入一个。
//
// ── 版本检查与其它网络功能共用一把 TLS 锁 ─────────────────────────────
//
// 设备侧内存最紧的地方不是堆总量，而是「同一时刻能存在几个 mbedTLS 上下文」：
// 每个 WiFiClientSecure 握手时要一次拿到约 16KB **连续** 堆，两三个并存就会
// 出现 MBEDTLS_ERR_SSL_ALLOC_FAILED（-32512）—— 现象是「版本检查时好时坏，
// 失败日志里列表、徽章两条渠道一起报 -1」。
//
// 触发条件是「版本检查」撞上别的 HTTPS 流量：VersionTask 拉列表（~40KB）、
// 推送队列正在发 HTTP 推送、甚至同时开着的网页反复打 /api/ota/status 把检查
// 反复重启。任何两条 HTTPS 路径并行，就有一半概率握手失败。
//
// 因此 Ota 内部所有出网的 TLS 阶段（检查、固件下载、LittleFS 下载）都先取一次
// TLS 轮次（utils/TlsTurn.{h,cpp} 里的 g_tlsMutex / TlsTurn，OTA、推送、SMTP 邮件共用），
// 拿到才建 HttpSession。
// 检查阶段一旦失败还会重试（见 OTA_PROBE_RETRY_PER_CHANNEL）；真正的下载阶段是
// 用户主动触发的长任务，不重试，只排队等锁 —— 排队既保证不并发，也不会对外网
// 的失败重试成风暴。

// 当前固件的平台标识，由 platformio.ini 的 -D APP_PLATFORM 注入（c3supermini / c3core / s3 / wroom）。
// 未注入时回退为 "generic"，保证固件仍能编译，只是 OTA 下载会 404。
#ifndef APP_PLATFORM
#define APP_PLATFORM "generic"
#endif
static const char* const OTA_PLATFORM = APP_PLATFORM;

// 拼接平台后缀的发布产物名，releaseTag 就是完整版本标识：
//   otaAssetName("1.0.0-20261004T121442-f2b513a", "ota")
//   -> "ota-1.0.0-20261004T121442-f2b513a-c3supermini.bin"
static inline String otaAssetName(const String& releaseTag, const char* prefix) {
  return String(prefix) + "-" + releaseTag + "-" + OTA_PLATFORM + ".bin";
}

// OTA 后台任务栈/优先级：
//   - 12 KiB 栈：HTTPS 下载 + mbedTLS 握手栈占用较高，实测低于此值会爆栈
//   - 优先级 1：低于 SIM Reader（3），高于 idle，避免抢占短信处理
static constexpr uint32_t    OTA_TASK_STACK_SIZE  = 12288;
static constexpr UBaseType_t OTA_TASK_PRIORITY    = 1;
// 单次 HTTP 请求的读写超时。它同时决定两件事：
//   1. 一次 TLS 请求最多占用多久（连接/握手失败时由 setConnectTimeout 兜底）
//   2. 两条渠道串行重试时的最坏耗时（2 渠道 × 2 轮 × 该值）
// 取 8s 的理由：CNB 的列表响应约 40KB，WiFi 正常时几百毫秒就收完；设备上实测
// SSL 握手失败（-32512 内存不足）时套接字会在 1~2s 内直接失败返回，不会真的等满。
// 调大只会让「公网握不上手」时白白占着 TLS 上下文不放，反而更容易撞内存不足。
static constexpr int         OTA_HTTP_TIMEOUT_MS  = 8000;
constexpr unsigned long      OTA_CHECK_DEBOUNCE_MS = 30000;  // 同一会话内至多每 30s 检查一次

// 单渠道的重试次数。失败多因瞬时内存不足（mbedTLS 握手要一次性拿到约 16KB 连续
// 堆），错开一小段时间后重建 TLS 客户端往往就能成 —— 设备上实测的「有时能更新、
// 有时报 -32512」正是这个模式。两条渠道各自重试，总时长仍有上界（见 OTA_HTTP_TIMEOUT_MS）。
static constexpr uint8_t     OTA_PROBE_RETRY_PER_CHANNEL = 2;

// Release 列表 JSON 的软上限。响应体（大头是每个 Release 的 markdown body）
// 随 Release 数量线性增长，当前两个 Release 已近 40KB。这里兜一手，防止畸形
// 或超长响应把 ESP32-C3 本就不宽裕的堆撑爆 —— 超过就截断，解析失败自然回落到
// 徽章渠道（那条只有 1KB 出头，永远可用）。
//
// 取 64KB 的理由：给当前体量留一倍余量，同时保证「body 字符串 + JsonDocument」
// 的峰值占用仍在 C3 可承受范围内。调小会让渠道一在 Release 变多后提前失效，
// 调大则峰值堆占用上升 —— 两头都要留意。
static constexpr size_t      OTA_LIST_BODY_MAX = 64 * 1024;

// 运行期探测本机是否有可用的 OTA 分区（app0/app1 双槽）。
// C3 的分区表是单 app 布局，没有 ota_1，此处返回 false —— 即使误开了
// ENABLE_FIRMWARE_UPGRADE 也能在开始前就给出明确提示，而不是写崩分区。
bool otaPartitionAvailable();

// Ota：固件 OTA + LittleFS 烧录入口。所有耗时操作都在独立 FreeRTOS 任务执行，
// 不阻塞 HTTP 请求。注意：本类内部使用**栈上**的 WiFiClientSecure，与 Push 共享禁用。
class Ota {
public:
  // 当前运行中固件版本（= 所属 Release 的 tag，由 CI 注入）
  static String           version();

  // 初始化（注册回调、清理上次状态等）
  static void             init();

  // 当前 OTA 状态快照（线程安全）
  static OtaStatusPayload status();

  // 异步：启动版本检查（去 CNB Release 查询最新版本）
  static void             startVersionCheck();

  // 异步：启动 OTA 升级到指定版本（Release tag，即完整版本标识，如
  // "1.0.0-20261004T121442-f2b513a"）。返回 false 表示已在进行或参数无效。
  static bool             startOnlineUpgrade(const String& releaseTag);

  // 处理本地上传的固件分片（HTTP /api/ota/upload）
  static bool             handleUploadChunk(uint8_t* data, size_t len, size_t index, bool final_);

  // 处理本地上传的 LittleFS 镜像分片（HTTP /api/ota/lfs）
  static bool             handleLfsUploadChunk(uint8_t* data, size_t len, size_t index,
                                               size_t totalSize, bool final_);
};
