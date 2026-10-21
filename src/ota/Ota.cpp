#include "Ota.h"
#include <esp_ota_ops.h>
#include <esp_partition.h>
#include <LittleFS.h>
#include <memory>   // std::unique_ptr（HttpSession 生命周期管理）
// Arduino 3.x 中 HTTPClient::getStreamPtr() 返回 NetworkClient*，
// WiFiClient 只是 NetworkClient 的兼容别名，需显式引入该声明。
#include <WiFiClient.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include "log/LogStore.h"     // devLog() —— 当前项目全局日志环（替代参考项目的 logger.h）
#include "utils/HttpSession.h"
#include "utils/TlsTurn.h"
#include <ArduinoJson.h>
#include <climits>   // INT_MIN（版本段不可比较的哨兵）

// 参考项目用单一 LOG 宏；本项目统一走 devLog().addf（默认按信息级记录）。
#define LOG(tag, fmt, ...) devLog().addf(LogLevel::kInfo, tag, fmt, ##__VA_ARGS__)

// ── 内部静态状态 ─────────────────────────────────────────────────
static volatile OtaState       g_state     = OtaState::IDLE;
static volatile uint8_t        g_progress  = 0;
static String                  g_message   = "";
static String                  g_currentVer = "";
static String                  g_latestVer  = "";
static volatile bool           g_inProgress = false;
static unsigned long           g_lastCheckMs = 0;  // 版本检查防抖时间戳

static esp_ota_handle_t        g_otaHandle  = 0;
static const esp_partition_t*   g_otaPart    = nullptr;
static TaskHandle_t            g_taskHandle = nullptr;

// LittleFS 手动上传专用状态
static const esp_partition_t*   g_lfsPart        = nullptr;
static size_t                  g_lfsWriteOffset = 0;
static size_t                  g_lfsTotalSize   = 0;

// ── TLS 轮次锁 ─────────────────────────────────────────────────────
// g_tlsMutex / TlsTurn 已抽到 utils/TlsTurn.{h,cpp}，由 OTA 检查/升级、
// 所有推送通道、SMTP 邮件共用，确保任意时刻最多一个 mbedTLS 上下文在建/在用，
// 避免两个 ~35KB 上下文并存撑爆堆（-32512）。本文件只使用 TlsTurn。

// 版本检查里 TLS 相关失败的短时退避。给 mbedTLS 释放内存、堆整理留出时间窗口。
//
// ⚠️ 调用点必须在 TLS 轮次之外（TlsTurn 已析构）—— 占着锁睡 300ms 等于把别的
// HTTPS 流量一起罚站。probeVia* 里的写法是「括号限定 TlsTurn 作用域」，
// 退避在括号之后执行。
static void tlsBackoff(uint8_t attemptIdx) {
    vTaskDelay(pdMS_TO_TICKS(attemptIdx == 0 ? 300 : 600));
}

// ── otaPartitionAvailable ─────────────────────────────────────────
// 探测当前分区表是否存在可切换的 OTA 槽位（esp_ota_get_next_update_partition
// 在没有 ota_1 时返回 nullptr）。C3 的单 app 分区表即属此类。
bool otaPartitionAvailable() {
    return esp_ota_get_next_update_partition(nullptr) != nullptr;
}

// ── 查找 LittleFS 所在的数据分区 ───────────────────────────────────
// Arduino 的 LittleFS 仍挂在 subtype=spiffs 的分区上，故按 spiffs 子类型查找。
// 找不到时返回 nullptr（调用方各自决定是跳过还是报错）。
static const esp_partition_t* findFsPartition() {
    return esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_SPIFFS, nullptr);
}

// ── Ota::version ────────────────────────────────────────
// 当前跑的固件怎么称呼自己：直接就是所属 Release 的 tag，也就是完整版本标识
// <VERSION>-<提交时间>-<短 SHA>（如 1.0.0-20261020T224521-f2b513a）。
// 这里不再自己拼一遍 —— 曾经用 APP_BUILD_DATE/TIME + SHA 现场拼，而 CI 一旦
// 注入了 APP_BUILD_TIMESTAMP 就会与 APP_BUILD_DATE 的格式漂移，拼出来的东西
// 与 Release tag 对不上，OTA 比较随之失灵。单一来源反而不会错。
String Ota::version() {
    return String(OTA_RELEASE_TAG);
}

// ── Ota::init ──────────────────────────────────────────────────────
void Ota::init() {
    g_currentVer = Ota::version();
    g_state      = OtaState::IDLE;
    g_progress   = 0;
    g_message    = "";
    g_latestVer  = "";
    g_inProgress = false;
    LOG("OTA", "初始化完成，版本: %s | 平台: %s | 编译: %s %s",
        g_currentVer.c_str(), OTA_PLATFORM, APP_BUILD_DATE, APP_BUILD_TIME);
    LOG("OTA", "发布源: %s（Release tag: %s）", OTA_RELEASES_BASE_URL, OTA_RELEASE_TAG);
}

// ── Ota::status ─────────────────────────────────────────────────
OtaStatusPayload Ota::status() {
    OtaStatusPayload p;
    p.state          = g_state;
    p.progress       = g_progress;
    p.message        = g_message;
    p.currentVersion = g_currentVer;
    p.latestVersion  = g_latestVer;
    return p;
}

// ── 远端最新版本的探测结果 ────────────────────────────────────────
enum class ProbeResult : uint8_t {
    FAILED,      // 网络/解析都失败，拿不到任何信息
    NO_RELEASE,  // 渠道通了，但远端确实一个 Release 都没有
    OK,          // 拿到了 tag（见 outTag）
};

// ── channel 1：Release 列表 JSON ──────────────────────────────────
// GET <OTA_RELEASES_BASE_URL>，Accept: application/json。
//
// 这个 Accept 头是本渠道的命门，别删也别改：服务器按它精确选体，
// 只有逐字等于 application/json 时才回 JSON；写成
// "application/json, */*" 这种带通配的组合会直接退回前端 HTML 页面
// （实测）。所以一方面要显式设它，另一方面也必须容忍它哪天失效 —— 见下面的
// 双通道设计：本渠道一旦解析失败，就交给徽章渠道。
//
// 列表按发布时间倒序（服务端保证），取首个即最新。这里不依赖 is_latest：
// CNB 会返回该字段但可能全为 false（没人在页面上手动标过 latest）。
//
// 单次尝试：调用方负责持有 TLS 轮次（见 utils/tls_turn）。返回值里 FAILED 既可能是
// 网络失败也可能是 TLS 内存不足，两者在重试层面等价 —— 都值得用新客户端再试一次。
// 从 Release 列表 body 中扫描「第一个 tag_name」的值。列表接口返回的是发布时间
// 倒序的数组（裸数组或 {releases:[...]}），首个 tag_name 即最新版本。
// 用轻量字符串扫描而非整体 JsonDocument 解析：后者会把每条 Release 的 changelog
// （整体已近 40KB）也加载进内存，解析峰值与 TLS 上下文（~35KB）叠加，在内存吃紧的
// 设备上触发 OOM（-32512）—— 正是「检查更新后系统卡死」的根因。
//
// 返回 true 表示已完整取到（outTag 非空）；false 表示尚未出现或值还没读完。
static bool extractFirstTagName(const String& body, String& outTag) {
    const char* key = "\"tag_name\"";
    int ki = body.indexOf(key);
    if (ki < 0) return false;                        // 还没出现
    int colon = body.indexOf(':', ki + (int)strlen(key));
    if (colon < 0) return false;
    int q1 = body.indexOf('"', colon);
    if (q1 < 0) return false;
    int q2 = body.indexOf('"', q1 + 1);
    if (q2 < 0) return false;                        // 值还没读完
    outTag = body.substring(q1 + 1, q2);
    return !outTag.isEmpty();
}

static ProbeResult probeViaReleasesJsonOnce(String& outTag) {
    outTag = "";

    auto session = std::unique_ptr<HttpSession>(HttpSession::request(String(OTA_RELEASES_BASE_URL)));
    if (!session) {
        LOG("OTA", "版本检查[list]：无法建立 HTTP 连接");
        return ProbeResult::FAILED;
    }
    session->http()->setTimeout(OTA_HTTP_TIMEOUT_MS);
    // 必须逐字就是这个值，见函数头注释
    session->http()->addHeader("Accept", "application/json");
    session->http()->setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);
    session->http()->setRedirectLimit(5);

    int code = session->http()->GET();
    if (code != 200) {
        LOG("OTA", "版本检查[list]响应码: %d（期望 200）", code);
        return ProbeResult::FAILED;
    }

    // 只需要「最新 Release 的 tag_name」。曾经把整个响应体（含每条 Release 的
    // changelog，整体已近 40KB）整体反序列化，解析峰值内存与 TLS 上下文（~35KB）
    // 叠加，在内存吃紧的设备上触发 OOM（-32512）—— 即「检查更新后系统卡死」。
    //
    // 现在改为：边读边找首个 "tag_name" 的值，一旦完整拿到就立刻停止读取，
    // 于是 body 通常只累积到几 KB（而不是 40KB+），且完全不建 JsonDocument。
    // 兼容裸数组与 {releases:[...]} 两种包装：首个 "tag_name" 即最新版本。
    WiFiClient* stream = session->http()->getStreamPtr();
    String body;
    body.reserve(4096);
    {
        unsigned long lastDataMs = millis();
        while (session->http()->connected()) {
            int avail = stream->available();
            if (avail > 0) {
                lastDataMs = millis();
                char buf[512];
                int n = stream->readBytes(buf, min(avail, (int)sizeof(buf)));
                if (n <= 0) break;
                body.concat(buf, n);
                // 拿到首个 tag_name 即可停读，避免继续把 changelog 拉进内存
                if (extractFirstTagName(body, outTag)) break;
                if (body.length() > OTA_LIST_BODY_MAX) break;  // 见 Ota.h
            } else {
                // 静默期超过超时就当收完了（或链路已死），交给下面的判定
                if (millis() - lastDataMs > (unsigned long)OTA_HTTP_TIMEOUT_MS) break;
                vTaskDelay(pdMS_TO_TICKS(10));
            }
        }
    }

    if (!outTag.isEmpty()) {
        LOG("OTA", "版本检查[list] 最新 tag: %s", outTag.c_str());
        return ProbeResult::OK;
    }

    // 没拿到 tag_name：可能是错误对象（带 errcode，视为「暂无 Release」），
    // 也可能是结构不认识 / 响应被截断（网络中断、超过 OTA_LIST_BODY_MAX）。
    // 这些都算本渠道未拿到有效版本，交给徽章渠道兜底。
    if (body.indexOf("\"errcode\"") >= 0) {
        LOG("OTA", "版本检查[list] 远端返回 errcode，视为暂无 Release");
        return ProbeResult::NO_RELEASE;
    }
    LOG("OTA", "版本检查[list] 未找到 tag_name（%u 字节）", (unsigned)body.length());
    return ProbeResult::FAILED;
}

// 列表渠道：先取 TLS 轮次（与本进程内其它 HTTPS 流量互斥），再带重试地跑单次尝试。
// 重试是为了吃掉「瞬时 TLS 内存不足」，见 OTA_PROBE_RETRY_PER_CHANNEL 的说明。
static ProbeResult probeViaReleasesJson(String& outTag) {
    for (uint8_t attempt = 0; attempt < OTA_PROBE_RETRY_PER_CHANNEL; ++attempt) {
        ProbeResult r = ProbeResult::FAILED;
        {
            // 轮次的作用域就是这一次尝试：尝试一结束就释放，退避期间不占着锁，
            // 否则「重试」会把别的 HTTPS 流量也一并挡住。
            TlsTurn turn("OTA-版本检查[list]", OTA_HTTP_TIMEOUT_MS);
            if (!turn.held()) {
                // 其它 TLS 会话（推送/下载）正占着，本轮直接放弃 —— 让检查任务
                // 尽快收尾，而不是把整个检查拖成几十秒。
                const char* holder = tlsHolderNow();
                LOG("OTA", "版本检查[list]：TLS 通道被 %s 占用，本轮跳过",
                    holder ? holder : "其它任务");
                return ProbeResult::FAILED;
            }
            if (attempt > 0) {
                LOG("OTA", "版本检查[list] 第 %u 次重试", (unsigned)(attempt + 1));
            }
            r = probeViaReleasesJsonOnce(outTag);
        }
        if (r != ProbeResult::FAILED) return r;
        if (attempt + 1 < OTA_PROBE_RETRY_PER_CHANNEL) tlsBackoff(attempt);
    }
    // 走到这里说明每次都 FAILED（其余语义在上面已直接返回）
    return ProbeResult::FAILED;
}

// ── 从徽章 SVG 里取右侧文案 ───────────────────────────────────────
// 徽章 SVG 里每个 <text> 都出现两次（阴影 + 本体，同一文案），所以文案表是
// [label, label, message, message]。取第三个即 message —— 也就是最新 Release
// 的 tag（无 Release 时为 "none"）。
//
// 不用 XML 解析器：这里只面对 CNB 自己吐的、格式稳定的十几个字节，为一个字段
// 引一整套 DOM 解析得不偿失。但也正因为它脆，段数对不上时一律当作「拿不到」，
// 而不是按位置猜一个可能是 label 的值回去。
static String parseBadgeMessage(const String& svg) {
    String texts[8];
    int count = 0;
    int pos = 0;
    while (count < 8) {
        int lt = svg.indexOf("<text", pos);
        if (lt < 0) break;
        int gt = svg.indexOf('>', lt);
        if (gt < 0) break;
        int endTag = svg.indexOf("</text>", gt);
        if (endTag < 0) break;
        texts[count++] = svg.substring(gt + 1, endTag);
        pos = endTag + 7;
    }
    // 正常徽章是 4 段（label 两段 + message 两段）。段数对不上就说明格式变了。
    if (count != 4) return "";
    String msg = texts[2];
    msg.trim();
    return msg;
}

// ── channel 2：Release 徽章 SVG ───────────────────────────────────
// GET <OTA_LATEST_VERSION_URL>。徽章是给 README 嵌图用的公共只读路由，
// 对 Accept 不挑剔（实测带什么头都回 SVG），因此不受渠道 1 那个命门影响。
//
// 代价是它只给一个字符串，且格式是 SVG 文本。解析见 parseBadgeMessage()。
//
// 两个渠道是「主 + 备」而非「或」：列表渠道能拿到更完整的列表（将来想做
// changelog 展示时有用），徽章渠道则更抗折腾（1KB 响应、无内容协商陷阱）。
// 谁先成功用谁，都失败才算检查失败。
//
// 单次尝试，调用方负责持有 TLS 轮次（与列表渠道同一把锁）。
static ProbeResult probeViaBadgeOnce(String& outTag) {
    outTag = "";

    auto session = std::unique_ptr<HttpSession>(HttpSession::request(String(OTA_LATEST_VERSION_URL)));
    if (!session) {
        LOG("OTA", "版本检查[badge]：无法建立 HTTP 连接");
        return ProbeResult::FAILED;
    }
    session->http()->setTimeout(OTA_HTTP_TIMEOUT_MS);
    // 徽章对 Accept 不敏感，这里只是表明意图
    session->http()->addHeader("Accept", "image/svg+xml");
    session->http()->setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);
    session->http()->setRedirectLimit(5);

    int code = session->http()->GET();
    if (code != 200) {
        LOG("OTA", "版本检查[badge]响应码: %d（期望 200）", code);
        return ProbeResult::FAILED;
    }

    // 徽章只有 1KB 出头，整体读进内存最省事；上限兜一手，防止异常响应撑爆堆
    WiFiClient* stream = session->http()->getStreamPtr();
    String svg;
    svg.reserve(2048);
    unsigned long lastDataMs = millis();
    while (session->http()->connected()) {
        int avail = stream->available();
        if (avail > 0) {
            lastDataMs = millis();
            char buf[256];
            int n = stream->readBytes(buf, min(avail, (int)sizeof(buf)));
            if (n <= 0) break;
            svg.concat(buf, n);
            if (svg.length() > 8192) break;  // 远超正常体积，不再等
        } else {
            if (millis() - lastDataMs > (unsigned long)OTA_HTTP_TIMEOUT_MS) break;
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }

    String msg = parseBadgeMessage(svg);
    if (msg.isEmpty()) {
        LOG("OTA", "版本检查[badge] 解析失败（%u 字节）", (unsigned)svg.length());
        return ProbeResult::FAILED;
    }
    if (msg.equalsIgnoreCase("none")) {
        LOG("OTA", "版本检查[badge] 远端暂无 Release");
        return ProbeResult::NO_RELEASE;
    }
    outTag = msg;
    LOG("OTA", "版本检查[badge] 最新 tag: %s", outTag.c_str());
    return ProbeResult::OK;
}

// 徽章渠道：同样是「取轮次 + 带重试」。它只有 1KB 出头，是本设计的兜底，
// 所以更要保证它别因为一次瞬时内存不足就整轮失效。
static ProbeResult probeViaBadge(String& outTag) {
    for (uint8_t attempt = 0; attempt < OTA_PROBE_RETRY_PER_CHANNEL; ++attempt) {
        ProbeResult r = ProbeResult::FAILED;
        {
            TlsTurn turn("OTA-版本检查[badge]", OTA_HTTP_TIMEOUT_MS);
            if (!turn.held()) {
                const char* holder = tlsHolderNow();
                LOG("OTA", "版本检查[badge]：TLS 通道被 %s 占用，本轮跳过",
                    holder ? holder : "其它任务");
                return ProbeResult::FAILED;
            }
            if (attempt > 0) {
                LOG("OTA", "版本检查[badge] 第 %u 次重试", (unsigned)(attempt + 1));
            }
            r = probeViaBadgeOnce(outTag);
        }
        if (r != ProbeResult::FAILED) return r;
        if (attempt + 1 < OTA_PROBE_RETRY_PER_CHANNEL) tlsBackoff(attempt);
    }
    return ProbeResult::FAILED;
}

// ── probeLatestTag — 依次试两条渠道 ───────────────────────────────
// 任一渠道给出 OK / NO_RELEASE 就采纳（这两者都代表「渠道通了」）；
// 只有两条都 FAILED 才算整体失败。
//
// ⚠️ 这两条渠道**不能并行**。并行意味着两个 TLS 上下文同时握手，正好是本函数
// 最想避免的情形（设备上「版本检查时好时坏」的成因）。串行 + 各自重试，
// 代价是失败时最坏耗时长一些，换来的是确定能拿到结果。
static ProbeResult probeLatestTag(String& outTag) {
    ProbeResult r = probeViaReleasesJson(outTag);
    if (r != ProbeResult::FAILED) return r;

    LOG("OTA", "列表渠道不可用，改用徽章渠道");
    return probeViaBadge(outTag);
}

// 当前固件所属的 Release tag —— 由 CI 注入的 OTA_RELEASE_TAG。它本身就是完整
// 版本标识，与远端给出的 tag 同量纲，可以直接比较。
static String currentReleaseTag() {
    return String(OTA_RELEASE_TAG);
}

// ── 版本标识的解析与比较 ──────────────────────────────────────────
//
// tag 形如 <VERSION>-<YYYYMMDDTHHMMSS>-<短SHA>。用字符串 != 判「有没有新版本」
// 是错的，会实打实地把设备往旧版本上带：
//
//   设备当前 tag: 1.0.0-20261004T125918-931584f
//   远端最新 tag: 1.0.0-20261004T125645-931584f
//
// 两者 VERSION 相同、短 SHA 相同，只是时间戳不同 —— 而且设备这个还更晚。
// 旧代码直接比字符串不相等，于是报「发现新版本」，用户点升级就把固件**降级**
// 到同一份代码的更早一次构建。这不是显示问题，是行为错误。
//
// 根因是版本标识里的时间戳（HEAD 提交时间）只用于区分构建，却被当作排序
// 依据：它只随代码提交递增，而设备上的固件可能来自本地分支（取本地 HEAD
// 的提交时间，与已发布 Release 没有可比性）。所以比较必须按语义来，而不
// 是按字面。
struct VersionParts {
    String   version;    // 1.0.0
    String   timestamp;  // 20261004T125918（可能为空）
    String   sha;        // 931584f（可能为空）
    bool     valid = false;
};

// 解析 <VERSION>[-<时间>][-<短SHA>]。只做切分，不校验 VERSION 是否为数字 ——
// 那是 compareVersionString 的职责，分开才能把「结构对不对」与「能不能比」
// 两种失败区分开。三种形态都要认（历史 Release 里都出现过）：
//   1.0.0                        仅 VERSION（SHA 缺失）
//   1.0.0-931584f                VERSION + SHA（无时间戳）
//   1.0.0-20261004T125918-931584f VERSION + 时间 + SHA
// 段数更多（VERSION 里自带 '-'）一律判为不可解析，交由兜底规则处理 —— 宁可
// 保守，不可误判。
static VersionParts parseVersionTag(const String& tag) {
    VersionParts p;
    int first = tag.indexOf('-');
    if (first < 0) {
        // 整个 tag 就是 VERSION，SHA 未知。是否合法由调用方的 compareVersionString 定夺。
        p.version = tag;
        p.valid   = true;
        return p;
    }
    if (first == 0) return p;                 // 形如 "-..."，非法

    p.version = tag.substring(0, first);

    String rest       = tag.substring(first + 1);
    int    secondDash = rest.indexOf('-');
    if (secondDash < 0) {
        p.sha = rest;                         // VERSION + SHA
    } else {
        p.timestamp = rest.substring(0, secondDash);
        p.sha       = rest.substring(secondDash + 1);
    }
    p.valid = true;
    return p;
}

// 比较两个 VERSION 段（点分十进制）。返回 -1 / 0 / 1。
// 任何一段不是纯数字就返回「无法比较」（INT_MIN 哨兵）。
static int compareVersionString(const String& a, const String& b) {
    int ia = 0, ib = 0;
    const int la = a.length(), lb = b.length();

    for (;;) {
        long va = 0, vb = 0;
        int na = 0, nb = 0;

        while (ia < la && a[ia] >= '0' && a[ia] <= '9') { va = va * 10 + (a[ia] - '0'); ia++; na++; }
        while (ib < lb && b[ib] >= '0' && b[ib] <= '9') { vb = vb * 10 + (b[ib] - '0'); ib++; nb++; }

        if (na == 0 || nb == 0) return INT_MIN;   // 某段不是数字，别硬比
        if (va != vb) return va < vb ? -1 : 1;

        const bool aEnd = (ia >= la), bEnd = (ib >= lb);
        if (aEnd && bEnd) return 0;
        if (aEnd != bEnd) return aEnd ? -1 : 1;   // 短的一方更小：1.0 < 1.0.1
        if (a[ia] != '.' || b[ib] != '.') return INT_MIN;
        ia++; ib++;
    }
}

// 远端 tag 相对当前 tag 是否「确实更新」。判据刻意保守：拿不准就不升，
// 因为「误报有新版本」的代价是把设备刷成别的固件，而「漏报」只是晚一次升级。
//   - 两边都解析不出来            → 退回字符串不等（维持旧行为，不额外阻止升级）
//   - VERSION 不同                 → 按点分数字比；比不动就按字符串不等认作新版
//   - VERSION 相同、两边 SHA 都有  → SHA 相同 = 同一份代码，不是新版本；
//                                    SHA 不同 = 代码确实变了，认作新版
//   - VERSION 相同、任一 SHA 缺失  → 无从判断代码是否相同，**不认作新版**
//
// 最后一条是有意保守的：远端 tag 若是不带 SHA 的老式短标识（如 "1.0.0"），
// 无法证明它比设备上的 "1.0.0-...-xxxxxxx" 更新，就不该拿它去覆盖设备。
//
// 这套规则让「同一 commit 重新构建」不再触发升级 —— 覆盖了本次上报的场景，
// 也顺手堵住了「本地编译的固件被流水线旧 Release 顶掉」这一类降级。
static bool isRemoteNewer(const String& remoteTag, const String& currentTag) {
    if (remoteTag == currentTag) return false;

    VersionParts r = parseVersionTag(remoteTag);
    VersionParts c = parseVersionTag(currentTag);
    if (!r.valid || !c.valid) {
        LOG("OTA", "版本标识无法解析（remote=%s current=%s），按字符串不等判定",
            remoteTag.c_str(), currentTag.c_str());
        return true;
    }

    int vcmp = compareVersionString(r.version, c.version);
    if (vcmp == INT_MIN) {
        LOG("OTA", "VERSION 段不可比较（%s vs %s），按字符串不等判定",
            r.version.c_str(), c.version.c_str());
        return true;
    }
    if (vcmp != 0) return vcmp > 0;

    // VERSION 相同：代码是否变过，只看短 SHA
    if (r.sha.isEmpty() || c.sha.isEmpty()) {
        LOG("OTA", "VERSION 相同但缺少提交标识（remote=%s current=%s），保守判为无新版",
            remoteTag.c_str(), currentTag.c_str());
        return false;
    }
    if (r.sha == c.sha) {
        LOG("OTA", "同一版本同一提交（%s），仅时间戳不同，不视为新版本", r.sha.c_str());
        return false;
    }
    return true;
}

// ── checkVersionTask — 仅查询最新版本，不下载固件 ─────────────────
// 判定「有没有新版本」的规则：
//   1. 两条渠道都失败           → 明确报「检查失败」，别让前端干转圈
//   2. 渠道通了但无 Release      → 报「暂无可升级的发布」
//   3. 远端不比当前新            → 已是最新（判据见 isRemoteNewer）
//   4. 远端确实更新              → 有新版本，latestVersion 供前端展示与升级
//
// 注意 3/4 不是「字符串相等／不等」。同名 VERSION 的重新构建曾会被判成新版本并
// 把设备降级，判据必须按语义来，细节见 isRemoteNewer 的注释。
static void checkVersionTask(void* /*param*/) {
    g_state     = OtaState::CHECKING;
    g_progress  = 0;
    g_message   = "正在查询最新版本...";
    g_latestVer = "";

    const String cur = currentReleaseTag();

    String remoteTag;
    ProbeResult r = probeLatestTag(remoteTag);

    if (r == ProbeResult::FAILED) {
        LOG("OTA", "版本检查失败：两条渠道都拿不到结果");
        g_inProgress = false;
        g_state      = OtaState::IDLE;
        g_message    = "检查更新失败：无法获取最新版本";
        vTaskDelete(nullptr);
        return;
    }
    if (r == ProbeResult::NO_RELEASE) {
        LOG("OTA", "远端暂无可升级的 Release");
        g_message = "远端暂无可升级的发布";
    } else if (!isRemoteNewer(remoteTag, cur)) {
        // g_message 必须显式置空（不能只靠"不赋值"）：上面已初始化为
        // "正在查询最新版本..."，若不清空，前端在 idle 时会把按钮文案一直
        // 停在"正在查询最新版本..."。空字符串让前端按契约显示「已是最新版本」
        // 并把升级按钮置灰。
        g_message = "";
        LOG("OTA", "已是最新版本（当前: %s，远端: %s）", cur.c_str(), remoteTag.c_str());
    } else {
        LOG("OTA", "发现新版本: %s（当前 Release tag: %s）", remoteTag.c_str(), cur.c_str());
        g_latestVer = remoteTag;
    }

    g_inProgress = false;
    g_state      = OtaState::IDLE;
    vTaskDelete(nullptr);
}

// ── otaTaskAbort — 失败后延时清理并终止任务（仅限 FreeRTOS 任务内调用）──
// ⚠️ 本函数以 vTaskDelete 收尾，**不做 C++ 栈展开**：调用点作用域内的
// TlsTurn / HttpSession 等 RAII 对象析构不会执行 —— TLS 上下文（~35KB）与
// g_tlsMutex 轮次锁会一起泄漏，且锁泄漏会让后续所有 HTTPS 流量（版本检查、
// 推送、邮件）永远拿不到轮次，直到重启才恢复。
// 在持有 TLS 对象的块内调用前，必须先显式 reset()/release() 再进入本函数，
// 参见 onlineUpgradeTask 下载阶段的 dlAbort 助手。
[[noreturn]] static void otaTaskAbort(const String& userMsg) {
    g_state      = OtaState::FAILED;
    g_message    = userMsg;
    vTaskDelay(pdMS_TO_TICKS(5000));
    g_inProgress = false;
    g_state      = OtaState::IDLE;
    vTaskDelete(nullptr);
    for (;;);
}
// ── onlineUpgradeTask — 后台 FreeRTOS 任务（param = heap-allocated String* tag）──
// param 是**目标 Release 的 tag**，也就是完整版本标识
// <VERSION>-<构建时间>-<短 SHA>（如 "1.0.0-20261004T121442-f2b513a"）。
// 它同时也是产物文件名里的 <tag>，下载地址按
// <OTA_RELEASES_BASE_URL>/download/<tag>/ota-<tag>-<platform>.bin 拼出来。
static void onlineUpgradeTask(void* param) {
    // 取出调用方通过 param 传入的目标 tag，并立即释放堆内存
    String releaseTag = *reinterpret_cast<String*>(param);
    delete reinterpret_cast<String*>(param);

    g_latestVer = releaseTag;

    // 注意：每个 HTTP/TLS 阶段使用独立的 HttpSession（unique_ptr），确保
    // WiFiClientSecure 的 mbedtls 上下文（~35KB heap）在下一阶段开始前
    // 由 destructor 释放，避免多个 TLS 上下文并存导致 MBEDTLS_ERR_SSL_ALLOC_FAILED。
    // 除此之外，出网前还要取 TLS 轮次锁（TlsTurn）—— 本任务与「版本检查」可能
    // 同时醒来，只靠「阶段内不并存」挡不住跨通信模组的并发握手。

    // --- 阶段1（已跳过版本检查，Release tag 由调用方提供）---
    LOG("OTA", "开始在线升级，目标 Release: %s", releaseTag.c_str());

    // --- 阶段2: 下载并写入固件 ---
    String firmwareUrl = String(OTA_RELEASES_BASE_URL)
                         + "/download/" + releaseTag + "/"
                         + otaAssetName(releaseTag, "ota");
    LOG("OTA", "开始下载固件: %s", firmwareUrl.c_str());
    g_state    = OtaState::DOWNLOADING;
    g_progress = 0;
    g_message  = "正在下载固件...";

    g_otaPart = esp_ota_get_next_update_partition(nullptr);
    if (!g_otaPart) {
        LOG("OTA", "esp_ota_get_next_update_partition 返回 null（当前平台分区表无 OTA 双槽）");
        otaTaskAbort("当前硬件平台的分区表不支持 OTA 升级，请改用 USB 烧录");
    }

    esp_err_t err = esp_ota_begin(g_otaPart, OTA_WITH_SEQUENTIAL_WRITES, &g_otaHandle);
    if (err != ESP_OK) {
        LOG("OTA", "esp_ota_begin 失败: %s", esp_err_to_name(err));
        otaTaskAbort("OTA 初始化失败: " + String(esp_err_to_name(err)));
    }

    bool dlFailed    = false;
    int  totalWritten = 0;
    {
        // 下载是用户主动触发的长任务，不重试，但必须排队等轮次 —— 与版本检查
        // 撞在一起时宁可多等一会儿，也别两个 TLS 上下文同时握手。
        //
        // ⚠️ 块内的失败一律走 dlAbort，不要直接调 otaTaskAbort：后者以
        // vTaskDelete 收尾且不做 C++ 栈展开，dlSession（mbedTLS 上下文 ~35KB）
        // 与 dlTurn（g_tlsMutex 轮次锁）都会泄漏 —— 锁泄漏会让后续所有 HTTPS
        // 流量永远拿不到轮次，直到重启。下载循环内的失败是例外：只置
        // dlFailed 后 break，到本 block 末尾正常出作用域，析构照常执行。
        TlsTurn dlTurn("OTA-下载固件", OTA_HTTP_TIMEOUT_MS * 2);
        if (!dlTurn.held()) {
            // 未拿到锁：此时无 TLS 会话可漏，dlTurn 也未持有锁，可直接中止
            const char* holder = tlsHolderNow();
            String busy = String("网络通道繁忙（") + (holder ? holder : "其它任务")
                        + " 占用中），请稍后重试";
            esp_ota_abort(g_otaHandle);
            otaTaskAbort(busy);
        }
        auto dlSession = std::unique_ptr<HttpSession>(HttpSession::request(firmwareUrl));
        // 块内中止助手：先归还 OTA 句柄、TLS 会话与轮次锁，再终止任务
        auto dlAbort = [&](const String& msg) {
            esp_ota_abort(g_otaHandle);
            dlSession.reset();   // ~HttpSession：http.end() + 释放 mbedTLS 上下文
            dlTurn.release();    // 归还 g_tlsMutex，避免后续 HTTPS 全部拿不到锁
            otaTaskAbort(msg);
        };
        if (!dlSession) {
            dlAbort("无法建立固件下载连接");
        }
        dlSession->http()->setTimeout(OTA_HTTP_TIMEOUT_MS);
        dlSession->http()->setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);
        dlSession->http()->setRedirectLimit(10);
        int dlCode = dlSession->http()->GET();
        if (dlCode != 200) {
            LOG("OTA", "固件下载 HTTP 响应码: %d", dlCode);
            dlAbort("连接固件服务器失败，响应码: " + String(dlCode));
        }

        int contentLen = dlSession->http()->getSize();
        LOG("OTA", "固件大小: %d 字节", contentLen);
        if (contentLen <= 0) {
            dlAbort("固件服务器未返回有效大小");
        }
        if ((size_t)contentLen > g_otaPart->size) {
            LOG("OTA", "固件大小超过 OTA 分区: %d > %u", contentLen, (unsigned)g_otaPart->size);
            dlAbort("固件大小超过 OTA 分区");
        }

        WiFiClient* dlStream = dlSession->http()->getStreamPtr();
        uint8_t dlBuf[512];
        int remaining        = contentLen;
        unsigned long lastDataMs = millis();

        while (dlSession->http()->connected() && (remaining > 0 || remaining == -1)) {
            int available = dlStream->available();
            if (available > 0) {
                lastDataMs = millis();
                int toRead  = min(available, (int)sizeof(dlBuf));
                int readLen = dlStream->readBytes(dlBuf, toRead);
                if (readLen > 0) {
                    if ((size_t)totalWritten + (size_t)readLen > g_otaPart->size) {
                        LOG("OTA", "固件写入超过 OTA 分区边界");
                        dlFailed = true;
                        break;
                    }
                    esp_err_t wErr = esp_ota_write(g_otaHandle, dlBuf, readLen);
                    if (wErr != ESP_OK) {
                        LOG("OTA", "esp_ota_write 失败: %s", esp_err_to_name(wErr));
                        dlFailed = true;
                        break;
                    }
                    totalWritten += readLen;
                    if (remaining > 0) remaining -= readLen;
                    if (contentLen > 0) {
                        g_progress = (uint8_t)((totalWritten * 100) / contentLen);
                    }
                }
            } else {
                if (millis() - lastDataMs > (unsigned long)OTA_HTTP_TIMEOUT_MS) {
                    LOG("OTA", "固件下载超时：%d ms 内无数据，已写入 %d/%d 字节", OTA_HTTP_TIMEOUT_MS, totalWritten, contentLen);
                    dlFailed = true;
                    break;
                }
                vTaskDelay(pdMS_TO_TICKS(10));
            }
        }

        if (!dlFailed && contentLen > 0 && totalWritten < contentLen) {
            LOG("OTA", "固件下载不完整：%d/%d 字节", totalWritten, contentLen);
            dlFailed = true;
        }
    }  // dlSession destructor 此处释放 HTTPClient 及 mbedtls TLS 上下文

    if (dlFailed) {
        esp_ota_abort(g_otaHandle);
        LOG("OTA", "固件下载失败，已中止 OTA");
        otaTaskAbort("固件下载失败，请检查网络");
    }

    LOG("OTA", "下载完成，共写入 %d 字节，提交 OTA...", totalWritten);

    err = esp_ota_end(g_otaHandle);
    if (err != ESP_OK) {
        LOG("OTA", "esp_ota_end 失败: %s", esp_err_to_name(err));
        otaTaskAbort("固件校验失败: " + String(esp_err_to_name(err)));
    }

    // --- 阶段3: 下载并写入 LittleFS 分区 ---
    String fsUrl = String(OTA_RELEASES_BASE_URL)
                   + "/download/" + releaseTag + "/"
                   + otaAssetName(releaseTag, "littlefs");
    LOG("OTA", "开始下载 LittleFS: %s", fsUrl.c_str());
    g_state    = OtaState::FLASHING_FS;
    g_progress = 0;
    g_message  = "正在下载 Web UI...";

    const esp_partition_t* fsPart = findFsPartition();
    if (!fsPart) {
        LOG("OTA", "找不到 LittleFS 分区，跳过 Web UI 升级");
        // 不中止升级，仅写入固件
    } else {
        bool fsFailed      = false;
        int  fsTotalWritten = 0;
        // 同上：LittleFS 是可选阶段。拿不到轮次（别的 HTTPS 任务正占着）就跳过，
        // 只升固件 —— 让整条升级链不必为一个可选步骤卡住。
        TlsTurn fsTurn("OTA-下载WebUI", OTA_HTTP_TIMEOUT_MS * 2);
        if (!fsTurn.held()) {
            const char* holder = tlsHolderNow();
            LOG("OTA", "LittleFS：网络通道繁忙（%s 占用中），跳过 Web UI 升级",
                holder ? holder : "其它任务");
            fsFailed = true;
        } else {
            auto fsSession = std::unique_ptr<HttpSession>(HttpSession::request(fsUrl));
            if (!fsSession) {
                LOG("OTA", "LittleFS：无法建立 HTTP 连接，跳过 Web UI 升级");
                fsFailed = true;
            } else {
                fsSession->http()->setTimeout(OTA_HTTP_TIMEOUT_MS);
                fsSession->http()->setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);
                fsSession->http()->setRedirectLimit(10);
                int fsCode = fsSession->http()->GET();
                if (fsCode != 200) {
                    LOG("OTA", "LittleFS 下载 HTTP 响应码: %d，跳过 Web UI 升级", fsCode);
                    // 不中止，仅升级固件
                } else {
                    int fsContentLen = fsSession->http()->getSize();
                    LOG("OTA", "LittleFS 大小: %d 字节", fsContentLen);
                    if (fsContentLen <= 0) {
                        LOG("OTA", "LittleFS 响应未返回有效大小，跳过 Web UI 升级");
                        fsFailed = true;
                    } else if ((size_t)fsContentLen > fsPart->size) {
                        LOG("OTA", "LittleFS 镜像超过分区大小: %d > %u", fsContentLen, (unsigned)fsPart->size);
                        fsFailed = true;
                    } else {
                        g_message = "正在写入 Web UI...";

                        LittleFS.end();
                        esp_err_t eraseErr = esp_partition_erase_range(fsPart, 0, fsPart->size);
                        if (eraseErr != ESP_OK) {
                            LOG("OTA", "LittleFS 分区擦除失败: %s", esp_err_to_name(eraseErr));
                            // 同固件下载阶段：块内直接 abort 会因 vTaskDelete 不栈展开
                            // 而泄漏 fsSession/fsTurn，先显式释放再终止任务
                            fsSession.reset();
                            fsTurn.release();
                            otaTaskAbort("Web UI 分区擦除失败: " + String(esp_err_to_name(eraseErr)));
                        }

                        WiFiClient* fsStream = fsSession->http()->getStreamPtr();
                        uint8_t fsBuf[512];
                        int fsRemaining        = fsContentLen;
                        unsigned long fsLastDataMs = millis();

                        while (fsSession->http()->connected() && (fsRemaining > 0 || fsRemaining == -1)) {
                            int fsAvailable = fsStream->available();
                            if (fsAvailable > 0) {
                                fsLastDataMs = millis();
                                int fsToRead  = min(fsAvailable, (int)sizeof(fsBuf));
                                int fsReadLen = fsStream->readBytes(fsBuf, fsToRead);
                                if (fsReadLen > 0) {
                                    if ((size_t)fsTotalWritten + (size_t)fsReadLen > fsPart->size) {
                                        LOG("OTA", "LittleFS 写入超过分区边界");
                                        fsFailed = true;
                                        break;
                                    }
                                    esp_err_t wErr = esp_partition_write(fsPart, fsTotalWritten, fsBuf, fsReadLen);
                                    if (wErr != ESP_OK) {
                                        LOG("OTA", "LittleFS esp_partition_write 失败: %s", esp_err_to_name(wErr));
                                        fsFailed = true;
                                        break;
                                    }
                                    fsTotalWritten += fsReadLen;
                                    if (fsRemaining > 0) fsRemaining -= fsReadLen;
                                    if (fsContentLen > 0) {
                                        g_progress = (uint8_t)((fsTotalWritten * 100) / fsContentLen);
                                    }
                                }
                            } else {
                                if (millis() - fsLastDataMs > (unsigned long)OTA_HTTP_TIMEOUT_MS) {
                                    LOG("OTA", "LittleFS 下载超时，已写入 %d/%d 字节", fsTotalWritten, fsContentLen);
                                    fsFailed = true;
                                    break;
                                }
                                vTaskDelay(pdMS_TO_TICKS(10));
                            }
                        }

                        if (fsContentLen > 0 && fsTotalWritten < fsContentLen) {
                            fsFailed = true;
                        }
                    }
                }
            }
        }  // fsSession destructor 此处释放 HTTPClient 及 mbedtls TLS 上下文

        if (fsFailed) {
            LOG("OTA", "LittleFS 写入不完整或失败，继续完成固件升级");
        } else {
            LOG("OTA", "LittleFS 写入完成，共 %d 字节", fsTotalWritten);
        }
    }

    // --- 阶段4: 设置启动分区并重启 ---
    err = esp_ota_set_boot_partition(g_otaPart);
    if (err != ESP_OK) {
        LOG("OTA", "esp_ota_set_boot_partition 失败: %s", esp_err_to_name(err));
        otaTaskAbort("设置启动分区失败: " + String(esp_err_to_name(err)));
    }

    g_progress = 100;
    g_state    = OtaState::SUCCESS;
    g_message  = "升级成功！设备将在 2 秒后自动重启";
    LOG("OTA", "OTA 升级完成，2s 后重启");
    vTaskDelay(pdMS_TO_TICKS(2000));
    esp_restart();

    vTaskDelete(nullptr);
}

// ── Ota::startVersionCheck ──────────────────────────────────────────
void Ota::startVersionCheck() {
    if (g_inProgress) return;
    unsigned long now = millis();
    if (g_lastCheckMs != 0 && now - g_lastCheckMs < OTA_CHECK_DEBOUNCE_MS) return;
    g_lastCheckMs = now;
    g_inProgress  = true;
    g_latestVer   = "";
    g_state       = OtaState::CHECKING;  // 同步设置，确保首次 getStatus() 即可见
    xTaskCreate(checkVersionTask, "ota_check", OTA_TASK_STACK_SIZE, nullptr, OTA_TASK_PRIORITY, nullptr);
}

// ── Ota::startOnlineUpgrade ─────────────────────────────────────────
// releaseTag 是目标 Release 的 tag（完整版本标识，如 "1.0.0-20261004T121442-f2b513a"），
// 由 /api/ota/start 把 /api/ota/version 查到的 latestVersion 原样传回来。
// 旧版前端传的是产物文件名里的平台标记，会导致下载 404 —— 因此这里只做白名单
// 校验，不猜也不转换。
bool Ota::startOnlineUpgrade(const String& releaseTag) {
    if (releaseTag.isEmpty()) return false;
    // Release tag 只可能是 [0-9A-Za-z._-]，出现斜杠等字符说明前端传错了东西，
    // 拼进 URL 只会换回一个 404 HTML，不如直接拒绝
    for (size_t i = 0; i < releaseTag.length(); ++i) {
        char c = releaseTag[i];
        if (!isalnum((unsigned char)c) && c != '.' && c != '_' && c != '-') {
            LOG("OTA", "拒绝非法的 Release tag: %s", releaseTag.c_str());
            return false;
        }
    }
    if (g_inProgress) return false;

    // 在堆上分配 tag，传入任务后由任务负责 delete
    String* tagParam = new String(releaseTag);

    g_inProgress = true;
    g_latestVer  = releaseTag;
    g_progress   = 0;
    g_message    = "";
    g_state      = OtaState::DOWNLOADING;

    BaseType_t ret = xTaskCreate(
        onlineUpgradeTask,
        "ota_online",
        OTA_TASK_STACK_SIZE,
        tagParam,
        OTA_TASK_PRIORITY,
        &g_taskHandle
    );

    if (ret != pdPASS) {
        delete tagParam;
        g_inProgress = false;
        g_state      = OtaState::FAILED;
        g_message    = "无法创建 OTA 任务（内存不足）";
        LOG("OTA", "xTaskCreate 失败，OTA 任务未启动");
        return false;
    }

    return true;
}

// ── Ota::handleUploadChunk ──────────────────────────────────────────
bool Ota::handleUploadChunk(uint8_t* data, size_t len, size_t index, bool final) {
    esp_err_t err;

    if (index == 0) {
        // 第一块：初始化
        if (g_inProgress) return false;
        g_inProgress = true;
        g_state      = OtaState::WRITING;
        g_progress   = 0;
        g_message    = "正在写入固件...";

        g_otaPart = esp_ota_get_next_update_partition(nullptr);
        if (!g_otaPart) {
            g_state   = OtaState::FAILED;
            g_message = "当前硬件平台的分区表不支持 OTA 升级";
            LOG("OTA", "手动上传：找不到 OTA 分区（本平台无 OTA 双槽）");
            vTaskDelay(pdMS_TO_TICKS(5000));
            g_inProgress = false;
            g_state      = OtaState::IDLE;
            return false;
        }

        err = esp_ota_begin(g_otaPart, OTA_WITH_SEQUENTIAL_WRITES, &g_otaHandle);
        if (err != ESP_OK) {
            g_state   = OtaState::FAILED;
            g_message = "OTA 初始化失败: " + String(esp_err_to_name(err));
            LOG("OTA", "手动上传 esp_ota_begin 失败: %s", esp_err_to_name(err));
            vTaskDelay(pdMS_TO_TICKS(5000));
            g_inProgress = false;
            g_state      = OtaState::IDLE;
            return false;
        }
        LOG("OTA", "手动上传开始，OTA 分区: %s", g_otaPart->label);
    }

    if (g_otaPart && index + len > g_otaPart->size) {
        esp_ota_abort(g_otaHandle);
        g_state   = OtaState::FAILED;
        g_message = "固件大小超过 OTA 分区";
        LOG("OTA", "手动上传超过 OTA 分区边界: %u > %u", (unsigned)(index + len), (unsigned)g_otaPart->size);
        vTaskDelay(pdMS_TO_TICKS(5000));
        g_inProgress = false;
        g_state      = OtaState::IDLE;
        return false;
    }

    // 写入当前块
    err = esp_ota_write(g_otaHandle, data, len);
    if (err != ESP_OK) {
        esp_ota_abort(g_otaHandle);
        g_state   = OtaState::FAILED;
        g_message = "固件写入失败: " + String(esp_err_to_name(err));
        LOG("OTA", "手动上传 esp_ota_write 失败: %s", esp_err_to_name(err));
        vTaskDelay(pdMS_TO_TICKS(5000));
        g_inProgress = false;
        g_state      = OtaState::IDLE;
        return false;
    }

    // 简单进度（按字节数无法精确，只在写入时递增）
    if (g_progress < 95) g_progress += 1;

    if (final) {
        err = esp_ota_end(g_otaHandle);
        if (err != ESP_OK) {
            g_state   = OtaState::FAILED;
            g_message = "固件校验失败: " + String(esp_err_to_name(err));
            LOG("OTA", "手动上传 esp_ota_end 失败: %s", esp_err_to_name(err));
            vTaskDelay(pdMS_TO_TICKS(5000));
            g_inProgress = false;
            g_state      = OtaState::IDLE;
            return false;
        }

        err = esp_ota_set_boot_partition(g_otaPart);
        if (err != ESP_OK) {
            g_state   = OtaState::FAILED;
            g_message = "设置启动分区失败: " + String(esp_err_to_name(err));
            LOG("OTA", "手动上传 esp_ota_set_boot_partition 失败: %s", esp_err_to_name(err));
            vTaskDelay(pdMS_TO_TICKS(5000));
            g_inProgress = false;
            g_state      = OtaState::IDLE;
            return false;
        }

        g_progress = 100;
        g_state    = OtaState::SUCCESS;
        g_message  = "上传成功！设备将在 2 秒后自动重启";
        LOG("OTA", "手动上传完成，2s 后重启");

        xTaskCreate([](void*) {
            vTaskDelay(pdMS_TO_TICKS(2000));
            esp_restart();
            vTaskDelete(nullptr);
        }, "ota_rst", 2048, nullptr, 5, nullptr);
    }

    return true;
}

// ── Ota::handleLfsUploadChunk ───────────────────────────────────────
bool Ota::handleLfsUploadChunk(uint8_t* data, size_t len, size_t index, size_t totalSize, bool final) {
    if (index == 0) {
        // 第一块：初始化
        if (g_inProgress) return false;
        g_inProgress     = true;
        g_state          = OtaState::FLASHING_FS;
        g_progress       = 0;
        g_message        = "正在写入 Web UI...";
        g_lfsWriteOffset = 0;
        g_lfsTotalSize   = totalSize;

        g_lfsPart = findFsPartition();
        if (!g_lfsPart) {
            g_state   = OtaState::FAILED;
            g_message = "找不到 LittleFS 分区";
            LOG("OTA", "LittleFS 手动上传：找不到分区");
            vTaskDelay(pdMS_TO_TICKS(5000));
            g_inProgress = false;
            g_state      = OtaState::IDLE;
            return false;
        }

        // 卸载 LittleFS，擦除整个分区
        LittleFS.end();
        esp_err_t eraseErr = esp_partition_erase_range(g_lfsPart, 0, g_lfsPart->size);
        if (eraseErr != ESP_OK) {
            g_state   = OtaState::FAILED;
            g_message = "LittleFS 分区擦除失败: " + String(esp_err_to_name(eraseErr));
            LOG("OTA", "LittleFS 手动上传 esp_partition_erase_range 失败: %s", esp_err_to_name(eraseErr));
            vTaskDelay(pdMS_TO_TICKS(5000));
            g_inProgress = false;
            g_state      = OtaState::IDLE;
            return false;
        }
        LOG("OTA", "LittleFS 手动上传开始，分区: %s (0x%x, %u 字节)",
            g_lfsPart->label, (unsigned)g_lfsPart->address, (unsigned)g_lfsPart->size);
    }

    // 写入当前块
    if (len > 0) {
        if (g_lfsPart && g_lfsWriteOffset + len > g_lfsPart->size) {
            g_state   = OtaState::FAILED;
            g_message = "LittleFS 镜像超过分区大小";
            LOG("OTA", "LittleFS 手动上传超过分区边界: %u > %u",
                (unsigned)(g_lfsWriteOffset + len), (unsigned)g_lfsPart->size);
            vTaskDelay(pdMS_TO_TICKS(5000));
            g_inProgress = false;
            g_state      = OtaState::IDLE;
            return false;
        }
        esp_err_t wErr = esp_partition_write(g_lfsPart, g_lfsWriteOffset, data, len);
        if (wErr != ESP_OK) {
            g_state   = OtaState::FAILED;
            g_message = "LittleFS 写入失败: " + String(esp_err_to_name(wErr));
            LOG("OTA", "LittleFS 手动上传 esp_partition_write 失败 @ offset %u: %s",
                (unsigned)g_lfsWriteOffset, esp_err_to_name(wErr));
            vTaskDelay(pdMS_TO_TICKS(5000));
            g_inProgress = false;
            g_state      = OtaState::IDLE;
            return false;
        }
        g_lfsWriteOffset += len;
        if (g_lfsTotalSize > 0) {
            g_progress = (uint8_t)((g_lfsWriteOffset * 100) / g_lfsTotalSize);
        } else if (g_progress < 95) {
            g_progress += 1;
        }
    }

    if (final) {
        g_progress = 100;
        g_state    = OtaState::SUCCESS;
        g_message  = "Web UI 上传成功！设备将在 2 秒后自动重启";
        LOG("OTA", "LittleFS 手动上传完成，共 %u 字节，2s 后重启", (unsigned)g_lfsWriteOffset);
        xTaskCreate([](void*) {
            vTaskDelay(pdMS_TO_TICKS(2000));
            esp_restart();
            vTaskDelete(nullptr);
        }, "lfs_rst", 2048, nullptr, 5, nullptr);
    }

    return true;
}
