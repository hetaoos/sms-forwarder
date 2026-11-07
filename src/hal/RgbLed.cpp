#include "hal/RgbLed.h"

#include "app/Config.h"  // ENABLE_LED（编译期开关，默认 1）

#if ENABLE_LED

#include <Arduino.h>

// 预设颜色表：索引 0=OFF, 1=RED, 2=GREEN, 3=BLUE, 4=WHITE
const RgbLed::Color RgbLed::kPalette[RgbLed::kCount] = {
    {"OFF", 0, 0, 0},       {"RED", 255, 0, 0},     {"GREEN", 0, 255, 0},
    {"BLUE", 0, 0, 255},    {"WHITE", 255, 255, 255},
};

// 自定义颜色的显示名（setRaw 后 color().name 指向它）
static const char kCustomName[] = "CUSTOM";

// index：默认颜色索引（0 = OFF 即默认关闭）
void RgbLed::begin(uint8_t index) { set(index); }

// index：目标颜色索引，超出范围按 kCount 回绕
void RgbLed::set(uint8_t index) {
  _index = index % kCount;
  _cur = kPalette[_index];
  apply();
}

// 切到下一个预设颜色（循环）
void RgbLed::next() { set(_index + 1); }

// r/g/b：直接指定 RGB 通道值（不影响预设索引）
void RgbLed::setRaw(uint8_t r, uint8_t g, uint8_t b) {
  _cur.name = kCustomName;
  _cur.r = r;
  _cur.g = g;
  _cur.b = b;
  apply();
}

// percent：亮度百分比（0-100），对当前颜色实时生效
void RgbLed::setBrightness(uint8_t percent) {
  if (percent > 100) percent = 100;
  if (_bright == percent) return;
  _bright = percent;
  apply();
}

// 当前亮度百分比（0-100）
uint8_t RgbLed::brightness() const { return _bright; }

// 按当前颜色与亮度写硬件：out = ch * bright / 100（0 亮度即熄灭）
void RgbLed::apply() {
  uint8_t r = (uint8_t)((uint16_t)_cur.r * _bright / 100);
  uint8_t g = (uint8_t)((uint16_t)_cur.g * _bright / 100);
  uint8_t b = (uint8_t)((uint16_t)_cur.b * _bright / 100);
  rgbLedWrite(RGB_BUILTIN, r, g, b);
}

#endif  // ENABLE_LED
