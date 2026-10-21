#pragma once

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <stdint.h>

// ── 全局 TLS 通道（计数信号量）────────────────────────────────
// 设备内存最紧的不是堆总量，而是「同时存在的 mbedTLS 上下文数量」：每个
// WiFiClientSecure 握手要一次拿到约 16KB 连续堆，多个上下文并存会间歇性
// 爆出 MBEDTLS_ERR_SSL_ALLOC_FAILED（-32512）。
//
// 早期用一把互斥锁把并发压到 1（任意时刻最多 1 个上下文）；现改为计数信号量，
// 允许最多 kMaxTlsChannels 个上下文并发：OTA 检查/升级、HTTP 推送、SMTP 邮件、
// 以及启用 TLS 的 MQTT 长连接共享这 N 个「通道」。大块 TLS 缓冲已重定向到
// PSRAM（见 utils/tls_mem.{h,cpp}），因此 N 个上下文并存也不会把内部 DRAM 撑爆。
//
// 静态创建（不占堆、无需判空）；极端情况下创建失败则退化为不互斥，避免把
// 整条链路卡死。

constexpr int kMaxTlsChannels = 4;

// 占用登记表：每个通道一个槽，用于诊断「谁正占着哪个通道、占了多久」。
// 访问须持 g_tlsRegMux（跨任务只读诊断也走它）。
struct TlsSlot {
  const char* owner = nullptr;  // 申请者标识（静态存储期字符串字面量）
  uint32_t    ms    = 0;        // 拿到通道的时刻（millis()）
  bool        used  = false;
};

extern SemaphoreHandle_t g_tlsSem;                 // 计数信号量：上限 kMaxTlsChannels
extern SemaphoreHandle_t g_tlsRegMux;              // 保护 g_tlsSlots 登记表
extern TlsSlot          g_tlsSlots[kMaxTlsChannels];

// 读取当前任意一个 TLS 占用者标识（无人占用时返回 nullptr）。仅用于诊断日志；
// 内部对登记表做一次性读取，调用方应存入局部变量再使用。
const char* tlsHolderNow();

class TlsTurn {
public:
    TlsTurn() : held_(false), slot_(-1) {}   // 空句柄（未持有，可安全析构）
    // owner：本次通道的申请者标识（须是字符串字面量等静态存储期指针）。
    // waitMs：等待上限。超时未拿到通道时记一条 kWarn，列出当前全部占用者 +
    //         各已占用时长，直接定位「谁卡住了通道」。
    // preferBack：true 时从末尾槽分配（常驻/长连接，如 MQTT-TLS），false 时从
    //         头部槽分配（临时连接：OTA/推送/邮件）。两者分离可避免临时连接在
    //         「找空槽」时反复跳过常驻连接占用的槽。
    TlsTurn(const char* owner, uint32_t waitMs, bool preferBack = false);
    ~TlsTurn();

    // 允许移动（用于把锁随会话一起在栈上传递），禁止拷贝。
    TlsTurn(TlsTurn&& o) noexcept : held_(o.held_), slot_(o.slot_) {
        o.held_ = false;
        o.slot_ = -1;
    }
    TlsTurn& operator=(TlsTurn&& o) noexcept {
        if (this != &o) {
            if (held_) release();
            held_ = o.held_;
            slot_ = o.slot_;
            o.held_ = false;
            o.slot_ = -1;
        }
        return *this;
    }
    TlsTurn(const TlsTurn&)            = delete;
    TlsTurn& operator=(const TlsTurn&) = delete;

    bool held() const { return held_; }

    // 显式归还通道（幂等）。用于「任务即将被 vTaskDelete 终止、栈上析构
    // 不会执行」的场景：调用点须在终止前手动释放，否则信号量永久泄漏，
    // 后续所有 HTTPS 流量都拿不到通道，直到重启。
    void release();

private:
    bool held_;
    int  slot_;   // 在 g_tlsSlots 中的索引；-1 表示未登记
};
