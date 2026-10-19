#pragma once

#include "hal/Button.h"
#include <queue>
#include <utility>

// 远程按键注入队列：Web 端通过 /api/key 把按键事件入队，
// 主循环 App::tick() 取出后交给 UI 状态机，与物理按键走同一处理路径。
// HTTP（async_tcp）任务与主循环分属不同任务，用互斥量保护共享队列。
class KeyInject {
 public:
  static KeyInject &instance();

  // 压入一个按键事件（队列满则丢弃，避免无限堆积占用内存）
  void push(KeyId key, KeyEvent ev);

  // 取出一个按键事件；无事件返回 false
  bool pop(KeyId &outKey, KeyEvent &outEv);

 private:
  KeyInject();
  std::queue<std::pair<KeyId, KeyEvent>> _q;
  // 复用 ESP32 FreeRTOS 互斥量（与 Display.cpp 风格一致）
  void *_mux = nullptr;
};
