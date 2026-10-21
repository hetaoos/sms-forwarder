#pragma once

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <stdint.h>

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

// ── 占用登记（诊断用） ──────────────────────────────────────────────
// 当前持有 TLS 轮次者的标识：构造 TlsTurn 时由调用点传入的字符串字面量
// （如 "OTA-下载固件" / "推送-HTTP" / "邮件-SMTP"）；无人持有时为 nullptr。
// 拿不到锁时会把它连同已占用时长一起打出来，直接回答「是谁正占着通道」。
//
// 生命周期：持锁成功后写入，释放前清除（先清登记再放锁，避免放锁后下一任
// 已写入新占用者、却被这里抹掉）。跨任务读取只进诊断日志、不参与控制逻辑，
// 极端竞态下可能短暂看到上一任的残留或空值，均属正常。
extern volatile const char* g_tlsHolder;

// 当前持有者拿到轮次的时刻（millis()），仅用于诊断日志里的「已占用时长」。
extern volatile uint32_t g_tlsHolderMs;

// 读取当前 TLS 占用者标识（无人占用 / 刚释放时返回 nullptr）。仅用于诊断日志；
// 内部对 volatile 指针做一次性读取，调用方应存入局部变量再使用。
const char* tlsHolderNow();

class TlsTurn {
public:
    TlsTurn() : held_(false) {}                 // 空句柄（未持有，可安全析构）
    // owner：本次轮次的申请者标识。须是字符串字面量等静态存储期指针，会登记到
    //        g_tlsHolder，供他人拿不到锁时诊断「谁在占用」。
    // waitMs：等待上限。超时未拿到锁时记一条 kWarn：申请者 + 当前占用者 +
    //         已占用时长，直接定位「谁卡住了通道」。
    TlsTurn(const char* owner, uint32_t waitMs);
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

    // 显式归还轮次锁（幂等）。用于「任务即将被 vTaskDelete 终止、栈上析构
    // 不会执行」的场景：调用点须在终止前手动释放，否则 g_tlsMutex 永久泄漏，
    // 后续所有 HTTPS 流量（版本检查/推送/邮件）都拿不到轮次，直到重启。
    void release();

private:
    bool held_;
};
