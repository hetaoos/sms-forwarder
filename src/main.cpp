#include <Arduino.h>

#include "app/App.h"

// 全局应用单例：Arduino 入口只负责驱动它
static App gApp;

// Arduino 启动入口：完成各模组初始化
void setup() {
  gApp.begin();
}

// Arduino 主循环入口：反复调用应用的调度函数
void loop() {
  gApp.tick();
}
