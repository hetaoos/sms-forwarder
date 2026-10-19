#include "hal/FontCN.h"

#include <Arduino.h>
#include <LittleFS.h>
#include <LovyanGFX.hpp>
#include <esp_heap_caps.h>  // ps_malloc（字体缓冲放 PSRAM）

#include "log/LogStore.h"

namespace {
LogStore &_log = devLog();
}  // namespace

// 必须与 script/gen_cnfont.py 的输出几何保持一致
static constexpr uint8_t kCnWidth = 16;    // CJK 全角单元格宽（也是位图存储宽）
static constexpr uint8_t kCnHalf = 9;      // ASCII 显示宽（< 0x100 的码点）
static constexpr uint8_t kCnHeight = 16;   // 单元格高
static constexpr uint8_t kCnBase = 13;     // 基线距单元格顶部的像素
static constexpr uint8_t kCnYAdv = 16;     // 行行进

FontCN &FontCN::instance() {
  static FontCN i;
  return i;
}

bool FontCN::init() {
  if (_ready) return true;

  // 资源分区（全局 LittleFS，即 Web UI 所在 spiffs）；WebPanel 之后还会再挂载，幂等
  if (!LittleFS.begin(false)) {
    _log.addf(LogLevel::kWarn, "font", "资源分区未挂载，中文点阵字体加载失败");
    return false;
  }
  File f = LittleFS.open("/cnfont.bin", "r");
  if (!f) {
    _log.addf(LogLevel::kWarn, "font", "资源分区缺少 /cnfont.bin（请先 pio run -t uploadfs）");
    return false;
  }

  size_t sz = f.size();
  if (sz < 4) {
    f.close();
    _log.addf(LogLevel::kWarn, "font", "/cnfont.bin 尺寸异常");
    return false;
  }

  // 优先放入 PSRAM（8MB 充足）；失败回退内部堆
  uint8_t *buf = (uint8_t *)ps_malloc(sz);
  if (!buf) buf = (uint8_t *)malloc(sz);
  if (!buf) {
    f.close();
    _log.addf(LogLevel::kError, "font", "字体缓冲分配失败（%u 字节）", (unsigned)sz);
    return false;
  }

  size_t got = 0;
  while (got < sz) {
    int n = f.read(buf + got, sz - got);
    if (n <= 0) break;
    got += (size_t)n;
  }
  f.close();
  if (got != sz) {
    free(buf);
    _log.addf(LogLevel::kError, "font", "/cnfont.bin 读取不完整 %u/%u", (unsigned)got, (unsigned)sz);
    return false;
  }

  // 文件布局：uint16 indexsize + uint16[indexsize] 升序码点 + 位图
  uint16_t indexsize = (uint16_t)buf[0] | ((uint16_t)buf[1] << 8);
  const uint16_t *index = (const uint16_t *)(buf + 2);
  const uint8_t *bitmap = buf + 2 + (size_t)indexsize * 2;

  // 静态 BDFfont：成员为 const，只能在构造时赋值（用运行时指针/参数）
  static lgfx::BDFfont bdf(bitmap, index, indexsize, kCnWidth, kCnHalf, kCnHeight, kCnBase, kCnYAdv);
  _buf = buf;  // 保存以保证 PSRAM 缓冲常驻（不释放）
  _font = &bdf;
  _ready = true;

  _log.addf(LogLevel::kInfo, "font", "中文点阵字体就绪：%u 字形，%u 字节（PSRAM）",
            (unsigned)indexsize, (unsigned)sz);
  return true;
}

void FontCN::applyTo(lgfx::LovyanGFX &g) {
  if (_ready && _font) {
    g.setFont(_font);
  } else {
    g.setFont(&lgfx::fonts::Font2);  // 未就绪：回退英文，保证可读
  }
}
