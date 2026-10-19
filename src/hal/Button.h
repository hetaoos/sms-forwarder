#pragma once

#include <cstddef>
#include <stdint.h>

// 按键事件类型
enum class KeyEvent : uint8_t {
  kNone = 0,        // 无事件
  kClick,           // 短按（按下并在长按阈值内释放）
  kLongPress,       // 长按（按住超过阈值，仅触发一次）
  kRepeat,          // 长按期间的连发（value 调节友好）
};

// 物理按键标识
enum class KeyId : uint8_t {
  kOk = 0,
  kUp,
  kDown,
  kBack,
  kCount,
};

// 单按键：INPUT_PULLUP，低电平有效；自带消抖 + 短按/长按/连发判定。
// 非阻塞，由外部按固定节拍（建议 5~10ms）调用 poll() 推进状态机。
class Button {
 public:
  // pin：GPIO 号；activeLow：true=低电平表示按下（配 INPUT_PULLUP）；
  // longMs：长按阈值；repeatMs：长按连发间隔（仅 kRepeat 用得到）。
  Button(uint8_t pin, bool activeLow = true,
         uint16_t longMs = 600, uint16_t repeatMs = 180);
  void begin();

  // 周期性调用，返回本次发生的事件（kNone 表示无）；nowMs 取 millis()
  KeyEvent poll(uint32_t nowMs);

 private:
  bool readPressed() const;

  uint8_t _pin;
  bool _activeLow;
  uint16_t _longMs;
  uint16_t _repeatMs;

  bool _rawState = false;     // 原始采样（含抖动）电平
  bool _wasPressed = false;   // 已确认的按下态
  uint32_t _debounceUntil = 0; // 抖动窗口结束时刻
  uint32_t _pressStart = 0;    // 本次按下的起始时刻
  bool _longFired = false;     // 长按是否已触发
  uint32_t _lastRepeat = 0;    // 上次连发时刻
};

// 4 键集合：聚合 OK/UP/DOWN/BACK，统一轮询并产出（键, 事件）
// 引脚/时序来自 cfg，放在 .cpp 构造函数里初始化（头文件不反向依赖 app 配置）
class Keypad {
 public:
  Keypad();
  void begin();

  // 轮询全部按键，返回是否有事件；有则通过 out 参数给出键与事件
  bool poll(uint32_t nowMs, KeyId &outKey, KeyEvent &outEvent);

 private:
  Button _ok, _up, _down, _back;
  Button *_btns[(size_t)KeyId::kCount];
};
