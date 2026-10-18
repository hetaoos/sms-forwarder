#include "hal/RgbLed.h"

#include <Arduino.h>

// 预设颜色表：索引 0=OFF, 1=RED, 2=GREEN, 3=BLUE, 4=WHITE
const RgbLed::Color RgbLed::kPalette[RgbLed::kCount] = {
    {"OFF", 0, 0, 0},       {"RED", 255, 0, 0},     {"GREEN", 0, 255, 0},
    {"BLUE", 0, 0, 255},    {"WHITE", 255, 255, 255},
};

// index：默认点亮的颜色索引
void RgbLed::begin(uint8_t index) { set(index); }

// index：目标颜色索引，超出范围按 kCount 回绕
void RgbLed::set(uint8_t index) {
  _index = index % kCount;
  const Color &c = kPalette[_index];
  rgbLedWrite(RGB_BUILTIN, c.r, c.g, c.b);
}

// 切到下一个预设颜色（循环）
void RgbLed::next() { set(_index + 1); }

// r/g/b：直接指定 RGB 通道值（0-255），不影响当前索引
void RgbLed::setRaw(uint8_t r, uint8_t g, uint8_t b) {
  rgbLedWrite(RGB_BUILTIN, r, g, b);
}
