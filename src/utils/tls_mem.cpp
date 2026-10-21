#include "utils/tls_mem.h"

#include <Arduino.h>         // psramFound()
#include <mbedtls/platform.h>
#include <esp_heap_caps.h>

namespace {
// 阈值：大于等于该大小的分配视为「大块缓冲」（典型是 SSL 收发内容缓冲 ~16KB），
// 优先落 PSRAM；较小的结构体 / 加解密 scratch（可能被硬件 DMA 访问）留在 DRAM 才安全。
constexpr size_t kTlsPsramThreshold = 4096;

void* tls_calloc(size_t n, size_t sz) {
  size_t total = n * sz;
  if (total >= kTlsPsramThreshold && psramFound()) {
    void* p = heap_caps_calloc(n, sz, MALLOC_CAP_SPIRAM);
    if (p) return p;
  }
  return heap_caps_calloc(n, sz, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
}

void tls_free(void* p) {
  if (p) heap_caps_free(p);  // heap_caps_free 自适应 PSRAM / DRAM
}
}  // namespace

void tlsMemInit() {
  // 仅在确有 PSRAM 时重定向；否则沿用 mbedTLS 默认（DRAM）分配，行为不变。
  if (!psramFound()) return;
  // 要求 arduino-esp32 的 mbedTLS 构建启用 MBEDTLS_PLATFORM_MEMORY（默认开启），
  // 否则本重定向不生效（分配仍走默认 calloc/free）。
  mbedtls_platform_set_calloc_free(tls_calloc, tls_free);
}
