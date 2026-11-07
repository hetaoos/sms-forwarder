#pragma once

#include <cstddef>
#include <stdint.h>

#include "app/Config.h"  // ENABLE_LCD（编译期开关，默认 1）

#if ENABLE_LCD

// ST7789 240x320 屏幕：内部持有 LovyanGFX 面板 + LVGL 显示端口（pimpl 隔离依赖）。
//
// 职责边界：本类只做「硬件 + 渲染引擎」，不画任何业务内容——所有页面由
// src/ui 下的 LVGL 部件搭建：
//   * LVGL 渲染到 PSRAM 全屏缓冲，flush 回调把它推给面板；
//   * 该缓冲同时是 Web 实时预览（/api/screen）的取帧源；
//   * 背光/亮屏由本类管，熄屏时不再往面板推数据。
class Display {
 public:
  Display();
  ~Display();
  Display(const Display &) = delete;             // 禁止拷贝（持有唯一 Impl 指针）
  Display &operator=(const Display &) = delete;  // 禁止拷贝赋值

  // 初始化面板、背光，并就绪 LVGL 端口（LVGL 由此接管全部绘制）
  bool begin();

  // percent：背光亮度百分比（0-100，超出自动钳制）
  void setBrightness(uint8_t percent);

  // 当前背光亮度百分比（0-100）
  uint8_t brightness() const;

  // on：true 点亮屏幕（内容保留在帧缓冲），false 熄屏睡眠
  void setScreenOn(bool on);

  // 当前屏幕是否点亮
  bool screenOn() const;

  // 底层实现是否已创建（初始化是否成功）
  bool available() const { return _impl != nullptr; }

  // LVGL 心跳：主循环每轮调用一次，内部推进计时器与脏区刷新
  void lvglTick();

  // 屏幕像素尺寸（面板旋转后的可视尺寸）
  int16_t width() const;
  int16_t height() const;

  // LVGL 渲染缓冲（PSRAM，每像素 16bit），可作为 Web 取帧源；无缓冲时返回 nullptr
  void *frame() const;
  size_t frameBytes() const;

  // 抓取当前帧缓冲编码为 JPEG（jpge，质量 80），用于 Web 实时预览
  // 返回 PSRAM 中分配的缓冲区（调用方 free() 释放），失败返回 nullptr
  uint8_t *takeScreenJpg(size_t &outLen);

  // 主循环定时调用：把当前帧缓冲预编码为 JPEG 写入预览缓存（自节流约 1.4fps）
  void refreshScreenJpg();

  // HTTP 处理器收到 /api/screen 时调用，标记“有人正在看预览”
  void requestScreenPreview();

  // LVGL flush 的实际搬运：把一块已渲染好的像素推给面板
  // px：LVGL 输出的 RGB565 标准序缓冲；坐标为闭区间 [x1,x2] x [y1,y2]
  // 仅由 LVGL 的 flush 回调（Display.cpp 内的 lvglFlushCb）调用，业务代码不要直接调
  void flushArea(int32_t x1, int32_t y1, int32_t x2, int32_t y2, uint8_t *px);

 private:
  struct Impl;   // 前向声明：实现细节隐藏在 .cpp
  Impl *_impl;   // pimpl 指针，避免头文件暴露 LovyanGFX / LVGL
};

#else  // !ENABLE_LCD —— 编译期关闭屏幕，整类退化为无副作用的空实现

// 纯 inline 的空实现替身：覆盖所有调用点，全部无状态、无副作用。
// 与 ENABLE_MQTT 同套路：Display.cpp 全实现不编译，这里顶住装配代码。
class Display {
 public:
  Display() = default;
  ~Display() = default;
  Display(const Display &) = delete;
  Display &operator=(const Display &) = delete;

  bool begin() { return false; }
  void lvglTick() {}
  bool screenOn() const { return false; }
  uint8_t brightness() const { return 0; }
  void refreshScreenJpg() {}
};

#endif  // ENABLE_LCD
