#include "hal/Display.h"

#include <Arduino.h>
#include <LovyanGFX.hpp>
#include <esp_heap_caps.h>  // PSRAM 分配（LVGL 渲染缓冲 / 预览 JPEG）
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>  // 互斥量：保护屏幕预览 JPEG 缓存（主循环写 / HTTP 读）
#include <lvgl.h>
#include <string.h>

#include "jpge.h"  // 单头+单cpp JPEG 编码器（lib/jpge），用于 /api/screen 实时预览

#include "app/Config.h"
#include "app/Psram.h"   // PSRAM 优先分配与容量查询（LVGL 自定义堆 + 大块缓冲）
#include "log/LogStore.h"

namespace {
// PSRAM 不足、本次分配只能落进内部 DRAM 时的告警（按 10s 节流，
// 避免内存吃紧瞬间把日志环刷满）
void logPsramFallback(size_t size) {
  static uint32_t lastMs = 0;
  const uint32_t now = millis();
  if (lastMs && now - lastMs < 10000) return;
  lastMs = now;
  devLog().addf(LogLevel::kWarn, "psram",
                "PSRAM 不足，%u 字节退入内部 DRAM（PSRAM 剩余 %u KB / 最大块 %u KB）",
                (unsigned)size, (unsigned)(psramFreeBytes() / 1024),
                (unsigned)(psramLargestFreeBlock() / 1024));
}
}  // namespace

// ---------------------------------------------------------------------------
// LVGL 自定义堆分配：把 LVGL 的运行时分配统一导向 PSRAM。
// 约定见 src/lv_conf.h（LV_USE_STDLIB_MALLOC = LV_STDLIB_CUSTOM）：
// lv_mem.c 里的通用层负责 _malloc_zeroed / _reallocf 等包装，最终落到下面
// 三个 *_core 实现上。PSRAM 不足时回退内部 DRAM，保证 UI 仍能起来。
// ---------------------------------------------------------------------------
extern "C" {

// 自定义后端没有状态要初始化（分配本身走 heap_caps_*，随拿随用），
// 但 LVGL 的 lv_init/lv_deinit 会调用它们，缺了链接不过
void lv_mem_init(void) {}
void lv_mem_deinit(void) {}
void lv_mem_monitor_core(lv_mem_monitor_t * mon_p) {
  if (mon_p) memset(mon_p, 0, sizeof(*mon_p));   // 堆统计交给 ESP-IDF（这里用不上）
}

// PSRAM 耗尽时才退到内部 DRAM，并把「退化」这件事记进日志（按 10s 节流，
// 避免内存吃紧时把日志环刷爆）。UI 相关的运行时分配都在 PSRAM，
// 内部 DRAM 留给网络栈、任务栈与 FreeRTOS 堆。
void * lv_malloc_core(size_t size) {
  if (size == 0) size = 1;   // 空分配按 1 字节处理，避免部分实现返回 NULL
  void *p = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!p) {
    logPsramFallback(size);
    p = heap_caps_malloc(size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  }
  return p;
}

void lv_free_core(void * p) {
  if (p) heap_caps_free(p);
}

void * lv_realloc_core(void * p, size_t new_size) {
  if (new_size == 0) {
    heap_caps_free(p);
    return nullptr;
  }
  void *np = heap_caps_realloc(p, new_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!np) {
    logPsramFallback(new_size);
    np = heap_caps_realloc(p, new_size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  }
  return np;
}

}  // extern "C"

namespace {

// 本通信模组日志出口：全局日志环（串口由 LogStore 统一镜像输出）
LogStore &_log = devLog();

// LVGL 的 flush 回调是 C 风格函数指针，拿不到 this；整机只有一块屏，
// 用文件域指针回指到唯一的 Display 实例（begin() 时赋值）
Display *g_display = nullptr;

// ---------- LovyanGFX 自定义引脚面板 ----------
// 按 cfg 中的引脚/频率配置 SPI 总线、ST7789 面板与 PWM 背光
class St7789Panel : public lgfx::LGFX_Device {
 public:
  St7789Panel() {
    {  // ---- SPI 总线配置 ----
      auto cfgBus = _bus.config();
      cfgBus.spi_host = SPI2_HOST;        // 使用 SPI2 外设
      cfgBus.spi_mode = 0;                // SPI 模式 0
      cfgBus.freq_write = cfg::kTftSpiFreq;  // 写入时钟
      cfgBus.freq_read = 8000000;         // 读取时钟（保守值）
      cfgBus.pin_sclk = cfg::kTftSclk;    // 时钟引脚
      cfgBus.pin_mosi = cfg::kTftMosi;    // 数据引脚
      cfgBus.pin_miso = -1;               // 屏幕无 MISO
      cfgBus.pin_dc = cfg::kTftDc;        // 数据/命令引脚
      _bus.config(cfgBus);
      _panel.setBus(&_bus);
    }
    {  // ---- 面板配置 ----
      auto cfgPanel = _panel.config();
      cfgPanel.pin_cs = cfg::kTftCs;          // 片选
      cfgPanel.pin_rst = cfg::kTftRst;        // 复位
      cfgPanel.pin_busy = -1;                 // 无 BUSY
      cfgPanel.memory_width = cfg::kTftWidth; // 显存宽
      cfgPanel.memory_height = cfg::kTftHeight;// 显存高
      cfgPanel.panel_width = cfg::kTftWidth;  // 可见区宽
      cfgPanel.panel_height = cfg::kTftHeight;// 可见区高
      cfgPanel.offset_x = 0;                  // X 偏移
      cfgPanel.offset_y = 0;                  // Y 偏移
      cfgPanel.invert = true;                 // 该面板需要反色
      cfgPanel.rgb_order = false;             // RGB 顺序
      cfgPanel.dlen_16bit = false;            // 8 位传输宽度
      cfgPanel.bus_shared = false;            // 独占总线
      _panel.config(cfgPanel);
    }
    {  // ---- 背光配置 ----
      auto cfgLight = _light.config();
      cfgLight.pin_bl = cfg::kTftBlk;   // 背光引脚
      cfgLight.invert = false;          // 高电平点亮
      cfgLight.freq = 44100;            // PWM 频率
      cfgLight.pwm_channel = 7;         // PWM 通道
      _light.config(cfgLight);
      _panel.setLight(&_light);
    }
    setPanel(&_panel);
  }

 private:
  lgfx::Panel_ST7789 _panel;  // ST7789 面板驱动
  lgfx::Bus_SPI _bus;         // SPI 总线
  lgfx::Light_PWM _light;     // PWM 背光
};

// ---------- LVGL 端口回调 ----------

// LVGL 时间源：直接用 Arduino 的毫秒计数，省掉单独的心跳任务
uint32_t lvglTickGetMs() { return millis(); }

// LVGL 每画脏一个区域就回调一次；这里把它整块交给 LovyanGFX 推给面板。
// 注意像素字节序：
//   * LVGL 输出的是标准 RGB565（主机小端，低字节在前）；
//   * ST7789 走 8 位 SPI 传输 16 位色时按「高字节先传」的顺序接收，
//     而 LovyanGFX 的 pushImage(uint16_t*) 默认把入参当作「已是面板序」直接搬运。
// 因此在推送前就地做一次 16 位字节交换，之后无需逐像素转换。
void lvglFlushCb(lv_display_t *disp, const lv_area_t *area, uint8_t *px) {
  if (g_display && px) g_display->flushArea(area->x1, area->y1, area->x2, area->y2, px);
  lv_display_flush_ready(disp);
}

}  // namespace

// ---------- Display ----------
// pimpl 实现：面板实例 + LVGL 显示对象 + PSRAM 渲染缓冲 + 预览 JPEG 缓存
struct Display::Impl {
  St7789Panel lcd;                  // LovyanGFX 设备（面板/背光）
  lv_display_t *disp = nullptr;     // LVGL 显示对象
  uint8_t *fbBuf = nullptr;         // 全屏渲染缓冲（PSRAM，RGB565）
  size_t fbBytes = 0;               // 渲染缓冲字节数
  int16_t w = 0;                    // 面板宽（旋转后）
  int16_t h = 0;                    // 面板高（旋转后）
  bool screenOn = true;             // 屏幕点亮状态
  uint8_t bright = cfg::kTftBacklight;  // 当前背光亮度百分比（重启复位为默认）
  uint32_t nextTickMs = 0;          // 下一次可以跑 lv_timer_handler 的时刻

  // 屏幕实时预览缓存：主循环预编码 JPEG，HTTP 处理器仅复制返回，
  // 避免把重编码（~50-200ms）压在 async_tcp 任务上导致整个 Web 卡顿
  uint8_t *jpgBuf = nullptr;        // PSRAM 中的 JPEG 缓存（容量 w*h*3）
  size_t jpgLen = 0;                // 当前缓存 JPEG 字节数
  uint32_t jpgLastMs = 0;           // 上次编码时间戳（节流用）
  uint32_t jpgWantedMs = 0;         // 最近一次被请求预览的时间戳（按需生成门控）
  SemaphoreHandle_t jpgMux = nullptr;  // 保护缓存读写（主循环写 / HTTP 读）
};

Display::Display() : _impl(nullptr) {}

Display::~Display() {
  if (_impl) {
    if (_impl->jpgBuf) { free(_impl->jpgBuf); _impl->jpgBuf = nullptr; }
    if (_impl->jpgMux) { vSemaphoreDelete(_impl->jpgMux); _impl->jpgMux = nullptr; }
    if (_impl->fbBuf) { heap_caps_free(_impl->fbBuf); _impl->fbBuf = nullptr; }
  }
  delete _impl;
  _impl = nullptr;
  if (g_display == this) g_display = nullptr;
}

// 初始化面板，并把 LVGL 接起来（LVGL 从此接管全部绘制）
bool Display::begin() {
  if (!_impl) _impl = new Impl();
  _impl->lcd.init();
  _impl->lcd.setRotation(cfg::kTftRotation);
  setBrightness(cfg::kTftBacklight);
  _impl->w = _impl->lcd.width();
  _impl->h = _impl->lcd.height();

  // 全屏渲染缓冲：240x320x16bit ≈ 150KB。严格优先 PSRAM（与项目其它大块内存
  // 一致），PSRAM 不可用才退内部 DRAM；拿不到则屏幕保持黑屏，其余功能不受影响
  _impl->fbBytes = (size_t)_impl->w * _impl->h * 2;
  bool fbInPsram = false;
  _impl->fbBuf = (uint8_t *)psramAlloc(_impl->fbBytes, fbInPsram);
  if (!_impl->fbBuf) {
    _log.addf(LogLevel::kError, "disp", "LVGL 渲染缓冲分配失败，屏幕不可用");
    return false;
  }
  if (!fbInPsram) _log.addf(LogLevel::kWarn, "disp", "渲染缓冲未能落在 PSRAM，退用内部 DRAM");
  memset(_impl->fbBuf, 0, _impl->fbBytes);

  lv_init();                             // LVGL 内核
  lv_tick_set_cb(lvglTickGetMs);         // 时间源：millis()
  g_display = this;

  lv_display_t *disp = lv_display_create(_impl->w, _impl->h);
  if (!disp) {
    _log.addf(LogLevel::kError, "disp", "LVGL 显示对象创建失败");
    return false;
  }
  lv_display_set_flush_cb(disp, lvglFlushCb);
  // FULL 模式：缓冲有一屏那么大，LVGL 每次可以完整渲染脏区，也让我们能直接拿
  // 这块缓冲当 /api/screen 的取帧源
  lv_display_set_buffers(disp, _impl->fbBuf, nullptr, (uint32_t)_impl->fbBytes,
                         LV_DISPLAY_RENDER_MODE_FULL);
  _impl->disp = disp;

  // 实时预览缓存：与渲染缓冲同放 PSRAM；容量取原始 RGB 大小（JPEG 必小于原始）
  bool jpgInPsram = false;
  _impl->jpgBuf = (uint8_t *)psramAlloc((size_t)_impl->w * _impl->h * 3, jpgInPsram);
  _impl->jpgMux = xSemaphoreCreateMutex();
  if (!_impl->jpgBuf || !_impl->jpgMux) {
    _log.addf(LogLevel::kWarn, "disp", "屏幕预览缓存分配失败，/api/screen 将不可用");
    if (_impl->jpgBuf) { heap_caps_free(_impl->jpgBuf); _impl->jpgBuf = nullptr; }
    if (_impl->jpgMux) { vSemaphoreDelete(_impl->jpgMux); _impl->jpgMux = nullptr; }
  }

  _log.addf(LogLevel::kInfo, "disp",
            "ST7789 + LVGL 就绪，%dx%d，缓冲 %u KB / 预览缓存 %u KB（%s）；PSRAM 已用 %u KB / 共 %u KB",
            (int)_impl->w, (int)_impl->h, (unsigned)(_impl->fbBytes / 1024),
            (unsigned)((size_t)_impl->w * _impl->h * 3 / 1024),
            fbInPsram && jpgInPsram ? "PSRAM" : "内部 DRAM",
            (unsigned)(psramUsedBytes() / 1024), (unsigned)(psramTotalBytes() / 1024));
  return true;
}

int16_t Display::width() const { return _impl ? _impl->w : 0; }
int16_t Display::height() const { return _impl ? _impl->h : 0; }
void *Display::frame() const { return _impl ? _impl->fbBuf : nullptr; }
size_t Display::frameBytes() const { return _impl ? _impl->fbBytes : 0; }

// LVGL 心跳：按上一次 handler 给出的到期时间推进，避免每轮空跑定时器链表
void Display::lvglTick() {
  if (!_impl || !_impl->disp) return;
  const uint32_t now = millis();
  if ((int32_t)(now - _impl->nextTickMs) < 0) return;   // 未到期（含溢出处理）
  const uint32_t wait = lv_timer_handler();
  _impl->nextTickMs = now + (wait > 0 ? wait : cfg::kLvglMinTimerGapMs);
}

// 把 LVGL 渲染好的一块像素推给面板（见 lvglFlushCb 里的字节序说明）
void Display::flushArea(int32_t x1, int32_t y1, int32_t x2, int32_t y2, uint8_t *px) {
  if (!_impl || !_impl->screenOn || x2 < x1 || y2 < y1) return;  // 熄屏时不推数据
  const int32_t w = x2 - x1 + 1;
  const int32_t h = y2 - y1 + 1;

  uint16_t *p = (uint16_t *)px;
  const uint32_t n = (uint32_t)w * h;
  for (uint32_t i = 0; i < n; i++) {
    const uint16_t v = p[i];
    p[i] = (uint16_t)((v >> 8) | (v << 8));   // 标准 RGB565 -> 面板序（字节交换）
  }
  _impl->lcd.pushImage(x1, y1, w, h, (const uint16_t *)p);
}

// percent：亮度百分比，内部折算为 0-255 的 PWM 占空比
void Display::setBrightness(uint8_t percent) {
  if (!_impl) return;
  if (percent > 100) percent = 100;
  _impl->bright = percent;
  _impl->lcd.setBrightness((uint8_t)((uint32_t)percent * 255 / 100));
}

// 当前背光亮度百分比（0-100）
uint8_t Display::brightness() const { return _impl ? _impl->bright : 0; }

// on：true 唤醒；false 熄屏睡眠（渲染继续，只是不再往面板推）
void Display::setScreenOn(bool on) {
  if (!_impl || _impl->screenOn == on) return;
  _impl->screenOn = on;

  if (on) {
    _impl->lcd.wakeup();
    setBrightness(_impl->bright);   // 唤醒后恢复当前亮度（睡眠会关掉 PWM 输出）
    // 睡眠期间硬件丢弃了原有显存：整屏标记待重绘，让下一次 handler 重新推一帧
    if (_impl->disp) lv_obj_invalidate(lv_screen_active());
    _log.addf(LogLevel::kInfo, "disp", "屏幕已点亮");
  } else {
    _impl->lcd.sleep();             // 关背光 + 面板睡眠
    _log.addf(LogLevel::kInfo, "disp", "屏幕已熄灭");
  }
}

bool Display::screenOn() const { return _impl && _impl->screenOn; }

// 主循环定时预编码：把当前渲染缓冲编码为 JPEG 写入预览缓存（预览的「生产者」）
// 自节流（约 1.4fps），仅做像素转换 + 编码，结果锁存入 jpgBuf；HTTP 只负责复制返回
// 仅当最近数秒内有 /api/screen 请求（有人正在看预览）时才生成
void Display::refreshScreenJpg() {
  if (!_impl || !_impl->fbBuf || !_impl->jpgBuf || !_impl->jpgMux) return;
  const uint32_t now = millis();
  if (now - _impl->jpgWantedMs > 3000) return;  // 超过 3s 无请求 -> 停止生成
  if (!_impl->screenOn) return;   // 熄屏时不更新缓存（保留最后一帧）
  if (now - _impl->jpgLastMs < 700) return;   // 节流，约 1.4fps
  _impl->jpgLastMs = now;

  const int w = _impl->w, h = _impl->h;
  // 中转：渲染缓冲 RGB565 -> 连续 RGB888（jpge 需要 RGB 交错、逐行自上而下）
  const size_t rgbBytes = (size_t)w * h * 3;
  uint8_t *rgb = (uint8_t *)heap_caps_malloc(rgbBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!rgb) return;
  // 渲染缓冲此刻是「面板序」（见 flushArea），须先交换高低字节还原成标准 RGB565
  // 再展开，否则预览会出现 R/B 错位，与屏幕实际显示颜色不一致
  const uint8_t *src = (const uint8_t *)_impl->fbBuf;
  uint8_t *d = rgb;
  for (int y = 0; y < h; y++) {
    const uint8_t *row = src + (size_t)y * w * 2;
    for (int x = 0; x < w; x++) {
      uint16_t px = (uint16_t)row[x * 2 + 1] | ((uint16_t)row[x * 2] << 8);
      uint8_t r5 = (px >> 11) & 0x1F, g6 = (px >> 5) & 0x3F, b5 = px & 0x1F;
      *d++ = (uint8_t)((r5 << 3) | (r5 >> 2));   // R
      *d++ = (uint8_t)((g6 << 2) | (g6 >> 4));   // G
      *d++ = (uint8_t)((b5 << 3) | (b5 >> 2));   // B
    }
  }

  // 注意：jpge::params 是普通聚合结构体、无默认构造函数，必须值初始化（prm{}）把
  // 全部成员清零；否则 m_subsampling 等会是垃圾值，init() 的 switch 不命中任何 case、
  // m_mcu_x/m_mcu_y 保持未初始化，随后按垃圾尺寸分配并越界写入 -> panic 重启
  jpge::params prm{};
  prm.m_quality = 80;             // 质量 1-100
  prm.m_subsampling = jpge::H2V2; // 彩色 4:2:0；默认 Y_ONLY 会变灰度
  int cap = (int)rgbBytes;
  uint8_t *jbuf = (uint8_t *)heap_caps_malloc(cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  size_t outLen = 0;
  if (jbuf) {
    int jsize = cap;
    if (jpge::compress_image_to_jpeg_file_in_memory(jbuf, jsize, w, h, 3, rgb, prm) && jsize > 0) {
      outLen = (size_t)jsize;
    } else {
      // 容量不足时 jsize 会被置为所需大小，扩容重试一次
      heap_caps_free(jbuf);
      jbuf = (uint8_t *)heap_caps_malloc(jsize > 0 ? (size_t)jsize : (size_t)cap * 2,
                                         MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
      if (jbuf) {
        int js2 = jsize;
        if (jpge::compress_image_to_jpeg_file_in_memory(jbuf, js2, w, h, 3, rgb, prm) && js2 > 0)
          outLen = (size_t)js2;
      }
    }
  }
  heap_caps_free(rgb);  // 中转缓冲不再需要
  if (!jbuf || outLen == 0) { if (jbuf) heap_caps_free(jbuf); return; }

  // 锁存到缓存：加锁保证 HTTP 处理器读取到一致的一帧
  if (xSemaphoreTake(_impl->jpgMux, pdMS_TO_TICKS(50)) == pdTRUE) {
    memcpy(_impl->jpgBuf, jbuf, outLen);
    _impl->jpgLen = outLen;
    xSemaphoreGive(_impl->jpgMux);
  }
  heap_caps_free(jbuf);
}

// 由 HTTP 处理器在收到 /api/screen 时调用：标记“有人正在看预览”，
// 主循环据此在最近数秒内持续生成 JPEG 缓存；无人请求则停止生成
void Display::requestScreenPreview() {
  if (_impl) _impl->jpgWantedMs = millis();
}

// 屏幕实时预览（预览的「消费者」）：仅从缓存复制当前 JPEG 帧，几乎零耗时，
// 不再做任何编码，避免阻塞 async_tcp 任务导致整个 Web 卡顿。返回 PSRAM 缓冲区（调用方 free）
uint8_t *Display::takeScreenJpg(size_t &outLen) {
  outLen = 0;
  if (!_impl || !_impl->jpgBuf || !_impl->jpgMux) return nullptr;
  if (xSemaphoreTake(_impl->jpgMux, pdMS_TO_TICKS(50)) != pdTRUE) return nullptr;
  size_t len = _impl->jpgLen;
  uint8_t *out = nullptr;
  if (len > 0) {
    out = (uint8_t *)heap_caps_malloc(len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (out) memcpy(out, _impl->jpgBuf, len);
  }
  xSemaphoreGive(_impl->jpgMux);
  if (!out) return nullptr;
  outLen = len;
  return out;
}
