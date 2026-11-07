#pragma once

#include <stdint.h>

#include "app/Config.h"  // ENABLE_LED（编译期开关，默认 1）

#if ENABLE_LED

// 板载 RGB(WS2812, GPIO48)，通过 Arduino core 的 rgbLedWrite 驱动
class RgbLed {
 public:
  // 预设颜色：name 供 UI 显示，r/g/b 为 0-255 通道值
  struct Color {
    const char *name;
    uint8_t r, g, b;
  };

  // 初始化：index 为默认颜色索引（对 kCount 取模，0 = OFF 即默认关闭）
  void begin(uint8_t index = 0);

  // 切换到下一个预设颜色
  void next();

  // 直接按索引设置颜色（越界自动回绕）
  void set(uint8_t index);

  // 设置任意 RGB 值（不走预设；color() 的 name 为 "CUSTOM"）
  void setRaw(uint8_t r, uint8_t g, uint8_t b);

  // percent：亮度百分比（0-100，超出自动钳制），对当前颜色实时生效
  void setBrightness(uint8_t percent);

  // 当前亮度百分比（0-100）
  uint8_t brightness() const;

  // 当前颜色索引（setRaw 不影响）
  uint8_t index() const { return _index; }

  // 当前颜色（setRaw 后 name 为 "CUSTOM"，r/g/b 为设置值）
  const Color &color() const { return _cur; }

  // 预设颜色总数
  static uint8_t count() { return kCount; }

 private:
  // 按当前颜色与亮度写硬件（折算：out = ch * bright / 100）
  void apply();

  static const uint8_t kCount = 5;      // 预设颜色数量
  static const Color kPalette[kCount];  // 预设颜色表
  uint8_t _index = 0;                   // 当前颜色索引（0 = OFF）
  Color _cur = kPalette[0];             // 当前颜色（预设或自定义）
  uint8_t _bright = 100;                // 亮度百分比（100 = 原色）
};

#else  // !ENABLE_LED —— 编译期关闭 RGB 灯，整类退化为无副作用的空实现

// 纯 inline 的空实现替身：覆盖所有调用点，全部无状态、无副作用。
// 与 ENABLE_MQTT 同套路：RgbLed.cpp 全实现不编译，这里顶住装配代码。
class RgbLed {
 public:
  struct Color { const char *name; uint8_t r, g, b; };

  void begin(uint8_t = 0) {}
  void next() {}
  void setBrightness(uint8_t) {}
  uint8_t brightness() const { return 0; }
  uint8_t index() const { return 0; }
  const Color &color() const { static const Color c{"OFF", 0, 0, 0}; return c; }
};

#endif  // ENABLE_LED
