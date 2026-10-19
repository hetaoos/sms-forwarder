#pragma once

#include <cstddef>
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

  // 渲染一个菜单页（整页重绘，由 UI 状态机在导航变化时调用）
  // title：标题栏文字；items：选项文本数组（可为空）；itemCount：选项数；
  // sel：高亮项索引（-1 表示不高亮，如纯信息页）；
  // info：正文文本数组（与 items 互斥展示，可为空）；infoCount：正文行数；
  // foot：页脚提示/状态行（可为空）
  void renderMenu(const char *title, const char *const items[], uint8_t itemCount,
                  int8_t sel, const char *const info[], uint8_t infoCount,
                  const char *foot);

  // 「屏幕显示」页：实时镜像状态内容的可滚动视图（单击滚一行 / 长按翻一页）
  // info：状态快照；scroll：滚动偏移（行），返回应用后的偏移（已夹紧）
  int renderScreenView(const StatusInfo &info, int scroll);

  // 「屏幕显示」页可视行数（供上层计算翻页步长）
  int screenVisibleLines() const;

  // 抓取当前帧缓冲编码为 JPEG（jpge，质量 80），用于 Web 实时预览（方案 B）
  // 返回在 PSRAM 中分配的缓冲区（调用方用 free() 释放），失败时返回 nullptr，outLen 为 JPEG 字节数。
  // 无帧缓冲（hasFb=false）时不可用；屏幕熄屏不影响抓取（保留熄屏前最后一帧）
  uint8_t *takeScreenJpg(size_t &outLen);

  // 主循环定时调用：把当前帧缓冲预编码为 JPEG 写入预览缓存（方案 B 生产者，自节流约 1.4fps）
  // 编码较重，放在主循环而非 HTTP 任务，避免拖慢整个 Web；HTTP 处理器仅复制缓存返回
  void refreshScreenJpg();

  // 由 HTTP 处理器在收到 /api/screen 请求时调用，标记“有人正在看预览”。
  // 主循环仅在最近数秒内有请求时才编码，避免无人观看仍持续占用主循环与 PSRAM
  void requestScreenPreview();

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
