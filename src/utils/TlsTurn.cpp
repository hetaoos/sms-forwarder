#include "TlsTurn.h"

#include <Arduino.h>  // millis()
#include "log/LogStore.h"

namespace { LogStore& _log = devLog(); }

SemaphoreHandle_t g_tlsMutex = xSemaphoreCreateMutex();
volatile const char* g_tlsHolder   = nullptr;
volatile uint32_t    g_tlsHolderMs = 0;

const char* tlsHolderNow() {
    return const_cast<const char*>(g_tlsHolder);
}

TlsTurn::TlsTurn(const char* owner, uint32_t waitMs) {
    const char* who = (owner && *owner) ? owner : "(anonymous)";
    // 极端情况：创建失败则退化为不互斥，避免把整条链路卡死
    held_ = (g_tlsMutex != nullptr) &&
            (xSemaphoreTake(g_tlsMutex, pdMS_TO_TICKS(waitMs)) == pdTRUE);
    if (held_) {
        // 登记申请者与占用时刻：后续拿不到锁的任务靠它诊断「谁在占用」
        g_tlsHolder   = who;
        g_tlsHolderMs = millis();
        return;
    }
    if (g_tlsMutex == nullptr) return;  // 锁都没建成（退化不互斥），无从诊断

    // 没拿到锁：打印是谁在申请、等了多久、以及当前谁正占用着（占用多久）。
    // 持锁失败路径不持锁，此处记日志不存在与 g_tlsMutex 的死锁风险。
    const char* holder = tlsHolderNow();
    if (holder) {
        _log.addf(LogLevel::kWarn, "tls",
                  "申请不到 TLS 轮次：申请者[%s] 等待 %ums 超时；"
                  "当前占用者[%s]，已占用约 %ums",
                  who, (unsigned)waitMs, holder,
                  (unsigned)(millis() - g_tlsHolderMs));
    } else {
        _log.addf(LogLevel::kWarn, "tls",
                  "申请不到 TLS 轮次：申请者[%s] 等待 %ums 超时；"
                  "当前无登记占用者（可能刚释放）",
                  who, (unsigned)waitMs);
    }
}

void TlsTurn::release() {
    if (held_) {
        // 先清登记再放锁：避免放锁后下一任已写入新占用者、却被这里抹掉
        g_tlsHolder   = nullptr;
        g_tlsHolderMs = 0;
        if (g_tlsMutex != nullptr) xSemaphoreGive(g_tlsMutex);
    }
    held_ = false;
}

TlsTurn::~TlsTurn() { release(); }
