#pragma once

#include <stdint.h>

// 板载 RGB(WS2812, GPIO48)，通过 Arduino core 的 rgbLedWrite 驱动
class RgbLed {
 public:
  // 预设颜色：name 供 UI 显示，r/g/b 为 0-255 通道值
  struct Color {
    const char *name;
    uint8_t r, g, b;
  };

  // 初始化：index 为默认点亮的颜色索引（对 kCount 取模）
  void begin(uint8_t index = 1);

  // 切换到下一个预设颜色
  void next();

  // 直接按索引设置颜色（越界自动回绕）
  void set(uint8_t index);

  // 设置任意 RGB 值（不走预设）
  void setRaw(uint8_t r, uint8_t g, uint8_t b);

  // 当前颜色索引
  uint8_t index() const { return _index; }

  // 当前颜色（预设表项引用）
  const Color &color() const { return kPalette[_index]; }

  // 预设颜色总数
  static uint8_t count() { return kCount; }

 private:
  static const uint8_t kCount = 5;          // 预设颜色数量
  static const Color kPalette[kCount];      // 预设颜色表
  uint8_t _index = 1;                       // 当前颜色索引
};
