#include "TlsTurn.h"

#include <Arduino.h>  // millis() / String
#include "log/LogStore.h"

namespace { LogStore& _log = devLog(); }

// 静态创建计数信号量与登记表锁；创建失败则退化为不互斥（g_tlsSem==nullptr）。
SemaphoreHandle_t g_tlsSem =
    xSemaphoreCreateCounting((UBaseType_t)kMaxTlsChannels, (UBaseType_t)kMaxTlsChannels);
SemaphoreHandle_t g_tlsRegMux = xSemaphoreCreateMutex();
TlsSlot g_tlsSlots[kMaxTlsChannels] = {};

const char* tlsHolderNow() {
  if (g_tlsRegMux == nullptr) return nullptr;
  const char* r = nullptr;
  if (xSemaphoreTake(g_tlsRegMux, pdMS_TO_TICKS(20)) == pdTRUE) {
    for (int i = 0; i < kMaxTlsChannels; ++i) {
      if (g_tlsSlots[i].used && g_tlsSlots[i].owner) { r = g_tlsSlots[i].owner; break; }
    }
    xSemaphoreGive(g_tlsRegMux);
  }
  return r;
}

// 拿不到通道：扫描登记表，列出当前 kMaxTlsChannels 个通道的占用者与时长。
static void logBusy(const char* who) {
  if (g_tlsRegMux == nullptr) {
    _log.addf(LogLevel::kWarn, "tls", "申请不到 TLS 通道（%s）：登记表不可用", who);
    return;
  }
  if (xSemaphoreTake(g_tlsRegMux, pdMS_TO_TICKS(50)) != pdTRUE) {
    _log.addf(LogLevel::kWarn, "tls", "申请不到 TLS 通道（%s）：登记表忙，无法诊断", who);
    return;
  }
  String list;
  int n = 0;
  for (int i = 0; i < kMaxTlsChannels; ++i) {
    if (g_tlsSlots[i].used && g_tlsSlots[i].owner) {
      if (n) list += "; ";
      list += String(i) + ":[";
      list += g_tlsSlots[i].owner;
      list += String("]约") + String((unsigned)(millis() - g_tlsSlots[i].ms)) + "ms";
      ++n;
    }
  }
  xSemaphoreGive(g_tlsRegMux);
  if (n == 0) {
    _log.addf(LogLevel::kWarn, "tls",
              "申请不到 TLS 通道（%s）：通道已满但无登记占用者（可能刚释放）", who);
  } else {
    _log.addf(LogLevel::kWarn, "tls",
              "申请不到 TLS 通道（%s）：已满 %d/%d -> %s", who, n, kMaxTlsChannels,
              list.c_str());
  }
}

// 在登记表里按方向找一个空闲槽并登记（owner/ms 一并写入）；无空闲返回 -1。
// 调用方须已持 g_tlsRegMux。
static int reserveSlot(const char* who, bool preferBack) {
  for (int i = 0; i < kMaxTlsChannels; ++i) {
    int idx = preferBack ? (kMaxTlsChannels - 1 - i) : i;
    if (!g_tlsSlots[idx].used) {
      g_tlsSlots[idx].used = true;
      g_tlsSlots[idx].owner = who;
      g_tlsSlots[idx].ms = millis();
      return idx;
    }
  }
  return -1;
}

TlsTurn::TlsTurn(const char* owner, uint32_t waitMs, bool preferBack)
    : held_(false), slot_(-1) {
  const char* who = (owner && *owner) ? owner : "(anonymous)";
  // 极端情况：信号量没建成则退化为不互斥，避免把整条链路卡死
  if (g_tlsSem == nullptr) return;

  if (xSemaphoreTake(g_tlsSem, pdMS_TO_TICKS(waitMs)) != pdTRUE) {
    logBusy(who);
    return;
  }
  // 已拿到计数许可：登记到槽位表（持登记表锁，按方向找空闲槽）
  if (g_tlsRegMux != nullptr &&
      xSemaphoreTake(g_tlsRegMux, portMAX_DELAY) == pdTRUE) {
    int idx = reserveSlot(who, preferBack);
    if (idx >= 0) {
      slot_ = idx;
      held_ = true;
    } else {
      // 许可已拿到却无空闲槽（理论不会发生，计数与槽数一致）：归还许可
      xSemaphoreGive(g_tlsSem);
    }
    xSemaphoreGive(g_tlsRegMux);
  } else {
    // 拿不到登记表锁：退化为不登记（仍持许可，功能正常，仅诊断缺失）
    held_ = true;
  }
}

void TlsTurn::release() {
  if (held_) {
    if (slot_ >= 0 && g_tlsRegMux != nullptr &&
        xSemaphoreTake(g_tlsRegMux, portMAX_DELAY) == pdTRUE) {
      g_tlsSlots[slot_].used = false;
      g_tlsSlots[slot_].owner = nullptr;
      g_tlsSlots[slot_].ms = 0;
      xSemaphoreGive(g_tlsRegMux);
    }
    if (g_tlsSem != nullptr) xSemaphoreGive(g_tlsSem);
  }
  held_ = false;
  slot_ = -1;
}

TlsTurn::~TlsTurn() { release(); }
