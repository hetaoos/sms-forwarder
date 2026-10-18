#pragma once

// PSRAM 优先的「字节缓冲区 / 容器分配器」工具集。
//
// 背景：Arduino 的 String 走普通 malloc，在 arduino-esp32 上只落在内部 DRAM
// （327KB，还要与 FreeRTOS 堆、各任务栈、LWIP 缓冲共用）；而 ESP32-S3 片外还
// 挂着 8MB PSRAM。本项目里有两类典型的大块瞬时内存：
//   - 邮件正文构造（含 CSS + 多模组表格，单封可达数 KB~十几 KB）；
//   - 短信列表分页（一页最多 100 条，序列化后可达数十 KB）。
// 把它们放到 PSRAM 可显著降低 DRAM 峰值与碎片，避免与网络/栈抢内存。
//
// 设计约定：
//   - 优先 MALLOC_CAP_SPIRAM，失败（未焊 PSRAM / 空间不足）自动回退 DRAM，
//     调用方无需区分硬件，只需检查 ok()；
//   - PsBuffer 始终以 '\0' 结尾，可直接当 C 字符串交给第三方库
//     （如 ReadyMail 的 body(const char*, size_t)），全程零 DRAM 拷贝。

#include <Arduino.h>
#include <esp_heap_caps.h>

#include <cstddef>
#include <cstring>
#include <new>

#include <ArduinoJson/Memory/Allocator.hpp>

// PSRAM 可增长的字节缓冲区（禁止拷贝；内容始终 NUL 结尾）。
class PsBuffer {
 public:
  // cap 为初始内容容量（字节，不含结尾 NUL）；分配失败时 ok() 为 false
  explicit PsBuffer(size_t cap = 512) { reserve(cap); }
  ~PsBuffer() { release(); }

  PsBuffer(const PsBuffer &) = delete;
  PsBuffer &operator=(const PsBuffer &) = delete;

  // 保证至少能容纳 n 字节内容（已用内容保留）
  bool reserve(size_t n) {
    if (_cap >= n) return true;
    return reallocTo(growCap(n));
  }

  // ---- 追加 ----
  bool append(const char *s, size_t n) {
    if (n == 0) return true;
    if (!reserve(_len + n)) return false;
    memcpy(_buf + _len, s, n);
    _len += n;
    _buf[_len] = '\0';
    return true;
  }
  bool append(const char *s) { return s ? append(s, strlen(s)) : true; }
  bool append(const String &s) { return append(s.c_str(), (size_t)s.length()); }
  // 追加另一个缓冲（禁止自追加：扩容会让源指针失效）
  bool append(const PsBuffer &o) {
    if (&o == this) return false;
    return append(o.c_str(), o.length());
  }
  bool append(char c) {
    if (!reserve(_len + 1)) return false;
    _buf[_len++] = c;
    _buf[_len] = '\0';
    return true;
  }

  PsBuffer &operator+=(const char *s) { append(s); return *this; }
  PsBuffer &operator+=(const String &s) { append(s); return *this; }

  // ---- 查询 ----
  // 分配是否一切正常（false = 发生过分配失败，内容可能不完整，不应使用）
  bool ok() const { return _buf != nullptr && !_oom; }
  size_t length() const { return _len; }
  size_t capacity() const { return _cap; }
  bool empty() const { return _len == 0; }
  const char *c_str() const { return _buf ? _buf : ""; }
  const char *data() const { return c_str(); }
  // 当前缓冲区是否在 PSRAM（日志/排查用；回退 DRAM 时为 false）
  bool inPsram() const { return _psram; }

  // 清空内容但保留已分配容量（复用同一块 PSRAM）
  void clear() {
    _len = 0;
    if (_buf) _buf[0] = '\0';
    _oom = false;
  }

 private:
  // PSRAM 优先分配；失败回退普通 DRAM
  static void *psAlloc(size_t n, bool &inPsram) {
    void *p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM);
    if (p) { inPsram = true; return p; }
    inPsram = false;
    return heap_caps_malloc(n, MALLOC_CAP_8BIT);
  }

  // 容量增长策略：按 2 倍扩张并留一点余量，避免大正文反复搬迁
  static size_t growCap(size_t need) {
    size_t next = _cap0;
    while (next < need) {
      if (next > (size_t)-1 / 2) { next = need; break; }
      next *= 2;
    }
    return next;
  }

  bool reallocTo(size_t newCap) {
    bool inPsram = false;
    char *nb = (char *)psAlloc(newCap + 1, inPsram);
    if (!nb) { _oom = true; return false; }
    if (_len > 0 && _buf) memcpy(nb, _buf, _len);
    nb[_len] = '\0';
    release();
    _buf = nb;
    _cap = newCap;
    _psram = inPsram;
    return true;
  }

  void release() {
    if (_buf) heap_caps_free(_buf);
    _buf = nullptr;
    _cap = 0;
  }

  static constexpr size_t _cap0 = 512;  // 容量增长起点

  char *_buf = nullptr;    // 缓冲区（PSRAM 优先）
  size_t _len = 0;         // 已用字节数（不含结尾 NUL）
  size_t _cap = 0;         // 内容容量（分配量为 _cap + 1）
  bool _psram = false;     // 当前缓冲区是否在 PSRAM
  bool _oom = false;       // 是否发生过分配失败
};

// std:: 容器用的 PSRAM 分配器（如 std::vector<SmsMessage, PsramAllocator<...>>）。
// 注意：只管容器自身的存储，元素内部的 String 仍走 malloc（DRAM）。
template <class T>
class PsramAllocator {
 public:
  using value_type = T;
  using size_type = std::size_t;

  PsramAllocator() noexcept = default;
  template <class U>
  PsramAllocator(const PsramAllocator<U> &) noexcept {}

  T *allocate(std::size_t n) {
    void *p = heap_caps_malloc(n * sizeof(T), MALLOC_CAP_SPIRAM);
    if (!p) p = heap_caps_malloc(n * sizeof(T), MALLOC_CAP_8BIT);
    if (!p) throw std::bad_alloc();
    return static_cast<T *>(p);
  }
  void deallocate(T *p, std::size_t) noexcept { heap_caps_free(p); }

  template <class U>
  bool operator==(const PsramAllocator<U> &) const noexcept { return true; }
  template <class U>
  bool operator!=(const PsramAllocator<U> &) const noexcept { return false; }
};

// ArduinoJson 7 的自定义分配器：让 JsonDocument 的内存池落在 PSRAM。
// 用法：JsonDocument doc(&psramJsonAllocator());
class PsramJsonAllocator : public ArduinoJson::Allocator {
 public:
  void *allocate(size_t size) override {
    void *p = heap_caps_malloc(size, MALLOC_CAP_SPIRAM);
    return p ? p : heap_caps_malloc(size, MALLOC_CAP_8BIT);
  }
  void deallocate(void *ptr) override { heap_caps_free(ptr); }
  void *reallocate(void *ptr, size_t new_size) override {
    if (new_size == 0) { deallocate(ptr); return nullptr; }
    void *p = heap_caps_realloc(ptr, new_size, MALLOC_CAP_SPIRAM);
    return p ? p : heap_caps_realloc(ptr, new_size, MALLOC_CAP_8BIT);
  }
};

// 全局单例（无状态，可跨编译单元共用）
inline ArduinoJson::Allocator *psramJsonAllocator() {
  static PsramJsonAllocator alloc;
  return &alloc;
}
