#include "TlsTurn.h"

SemaphoreHandle_t g_tlsMutex = xSemaphoreCreateMutex();

TlsTurn::TlsTurn(uint32_t waitMs) {
    // 极端情况：创建失败则退化为不互斥，避免把整条链路卡死
    held_ = (g_tlsMutex != nullptr) &&
            (xSemaphoreTake(g_tlsMutex, pdMS_TO_TICKS(waitMs)) == pdTRUE);
}

void TlsTurn::release() {
    if (held_ && g_tlsMutex != nullptr) xSemaphoreGive(g_tlsMutex);
    held_ = false;
}

TlsTurn::~TlsTurn() { release(); }
