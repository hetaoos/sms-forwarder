#pragma once

#include <stdint.h>

#include "app/StatusInfo.h"

namespace lgfx {
inline namespace v1 {
class LovyanGFX;  // 前向声明：渲染目标基类，避免头文件暴露 LovyanGFX
}  // namespace v1
}  // namespace lgfx

// ST7789 240x320 屏幕：内部持有 LovyanGFX 实例（pimpl 隔离头文件依赖），
// 渲染目标为 PSRAM 中的全屏帧缓冲（16bit），变化时整屏推送面板
class Display {
 public:
  Display();
  ~Display();
  Display(const Display &) = delete;             // 禁止拷贝（持有唯一 Impl 指针）
  Display &operator=(const Display &) = delete;  // 禁止拷贝赋值

  // 初始化面板、背光与静态框架
  bool begin();

  // percent：背光亮度百分比（0-100，超出自动钳制）
  void setBrightness(uint8_t percent);

  // 当前背光亮度百分比（0-100）
  uint8_t brightness() const;

  // 内容有变化才刷新帧缓冲比较后的实际重绘
  // info：待渲染的状态快照
  void render(const StatusInfo &info);

  // on：true 点亮屏幕并补画缓存，false 熄屏睡眠
  void setScreenOn(bool on);

  // 当前屏幕是否点亮
  bool screenOn() const;

  // 底层实现是否已创建（初始化是否成功）
  bool available() const { return _impl != nullptr; }

 private:
  struct Impl;   // 前向声明：实现细节隐藏在 .cpp
  Impl *_impl;   // pimpl 指针，避免头文件暴露 LovyanGFX

  // 当前渲染目标：帧缓冲（PSRAM）可用时画进帧缓冲，否则直绘面板
  lgfx::LovyanGFX &target();
  // 帧缓冲模式：把整帧推送到面板；直绘模式：无需推送
  void pushFrame();
};
