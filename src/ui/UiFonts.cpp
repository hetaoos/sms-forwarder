#include "ui/UiFonts.h"

#include <Arduino.h>
#include <LittleFS.h>
#include <esp_heap_caps.h>

#include "app/Config.h"
#include "app/Psram.h"
#include "log/LogStore.h"

namespace ui {

namespace {

LogStore &_log = devLog();

const lv_font_t *g_ascii = &lv_font_montserrat_14;  // 兜底：只有 ASCII

// TTF 文件正文（PSRAM，TinyTTF 直接引用这块内存，全程不释放）
uint8_t *g_ttfData = nullptr;
size_t g_ttfSize = 0;

lv_font_t *g_text = nullptr;   // kUiFontTextPx
lv_font_t *g_title = nullptr;  // kUiFontTitlePx

bool g_begin = false;
bool g_ready = false;
bool g_readyFlag = false;   // 就绪的那一帧置位，由 fontsTakeReadyFlag 消费
bool g_failed = false;      // TTF 读不出来（文件缺失/内存不足）：不再重试
uint32_t g_nextTryMs = 0;

// 载入 TTF 到 PSRAM 并创建两个字号；全程给日志，便于排查「字库没起来」
bool loadFromFs() {
  File f = LittleFS.open(cfg::kUiFontPath, "r");
  if (!f) return false;
  const size_t size = (size_t)f.size();
  if (size == 0) { f.close(); return false; }

  // TTF 常驻 PSRAM：stb_truetype 直接从这块内存解析 cmap/loca/glyf，
  // 不额外复制一份；内部 DRAM 也要留给网络栈与任务栈，所以严格 PSRAM 优先
  bool inPsram = false;
  uint8_t *data = (uint8_t *)psramAlloc(size, inPsram);
  if (!data) {
    _log.addf(LogLevel::kError, "ui", "中文字库 %u KB 载入 PSRAM 失败", (unsigned)(size / 1024));
    f.close();
    g_failed = true;
    return false;
  }
  const size_t got = f.read(data, size);
  f.close();
  if (got != size) {
    heap_caps_free(data);
    _log.addf(LogLevel::kError, "ui", "中文字库读取不完整 %u/%u", (unsigned)got, (unsigned)size);
    g_failed = true;
    return false;
  }

  g_text = lv_tiny_ttf_create_data_ex(data, size, cfg::kUiFontTextPx,
                                      LV_FONT_KERNING_NORMAL, cfg::kUiFontGlyphCacheCnt);
  g_title = lv_tiny_ttf_create_data_ex(data, size, cfg::kUiFontTitlePx,
                                       LV_FONT_KERNING_NORMAL, cfg::kUiFontGlyphCacheCnt);
  if (!g_text || !g_title) {
    if (g_text) { lv_tiny_ttf_destroy(g_text); g_text = nullptr; }
    if (g_title) { lv_tiny_ttf_destroy(g_title); g_title = nullptr; }
    heap_caps_free(data);
    _log.addf(LogLevel::kError, "ui", "TinyTTF 字体创建失败");
    g_failed = true;
    return false;
  }

  g_ttfData = data;
  g_ttfSize = size;
  _log.addf(LogLevel::kInfo, "ui", "中文字库就绪 %s（%u KB / %s，%d/%d px，PSRAM 已用 %u KB）",
            cfg::kUiFontPath, (unsigned)(size / 1024), inPsram ? "PSRAM" : "内部 DRAM",
            (int)cfg::kUiFontTextPx, (int)cfg::kUiFontTitlePx,
            (unsigned)(psramUsedBytes() / 1024));
  return true;
}

}  // namespace

void fontsBegin() {
  g_begin = true;
  g_nextTryMs = 0;   // 下一次 tick 立即尝试一次
}

void fontsTick() {
  if (!g_begin || g_ready || g_failed) return;
  const uint32_t now = millis();
  if ((int32_t)(now - g_nextTryMs) < 0) return;
  g_nextTryMs = now + 500;   // 资源分区挂载前调用是安全的（未挂载时 exists/open 直接失败）

  if (loadFromFs()) {
    g_ready = true;
    g_readyFlag = true;
  }
}

bool fontsReady() { return g_ready; }

const lv_font_t *fontIcon() { return &ui_font_icon_20; }
const lv_font_t *fontLogo() { return &ui_font_logo; }

const lv_font_t *fontText() { return g_ready && g_text ? g_text : g_ascii; }

const lv_font_t *fontTitle() { return g_ready && g_title ? g_title : g_ascii; }

const lv_font_t *fontAscii() { return g_ascii; }

bool fontsTakeReadyFlag() {
  const bool v = g_readyFlag;
  g_readyFlag = false;
  return v;
}

}  // namespace ui
