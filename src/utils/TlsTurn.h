#pragma once

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

// ── 全局 TLS 轮次锁 ─────────────────────────────────────────────────
// 设备内存最紧的不是堆总量，而是**同时存在的 TLS 上下文数量**：每个
// WiFiClientSecure 握手要一次拿到约 16KB 连续堆，两个上下文并存就会间歇性
// 爆出 MBEDTLS_ERR_SSL_ALLOC_FAILED（-32512）—— 表现正是「版本检查时好时坏 /
// 检查更新时发邮件导致系统卡死」。
//
// 因此 OTA 检查/升级、所有 HTTP 推送通道、SMTP 邮件必须共用同一把锁：任意时刻
// 最多只允许一个 mbedTLS 上下文在建/在用。等待者用优先级继承互斥量，不会把
// 持有者饿死。
//
// 静态创建（不占堆、无需判空）；若极端情况下创建失败则退化为不互斥，避免把
// 整条链路卡死。
extern SemaphoreHandle_t g_tlsMutex;

class TlsTurn {
public:
    TlsTurn() : held_(false) {}                 // 空句柄（未持有，可安全析构）
    explicit TlsTurn(uint32_t waitMs);
    ~TlsTurn();

    // 允许移动（用于把锁随会话一起在栈上传递），禁止拷贝。
    TlsTurn(TlsTurn&& o) noexcept : held_(o.held_) { o.held_ = false; }
    TlsTurn& operator=(TlsTurn&& o) noexcept {
        if (this != &o) {
            if (held_) release();
            held_ = o.held_;
            o.held_ = false;
        }
        return *this;
    }
    TlsTurn(const TlsTurn&)            = delete;
    TlsTurn& operator=(const TlsTurn&) = delete;

    bool held() const { return held_; }

private:
    void release();
    bool held_;
};
