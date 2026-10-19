#include "app/UIMenu.h"

#include <Arduino.h>

#include "app/Config.h"

// 构建期由 script/inject_app_version.py 注入 APP_VERSION（字符串字面量）；
// 未注入时退回占位，保证本地编译通过
#ifndef APP_VERSION
#define APP_VERSION "unknown"
#endif

namespace {

// ---------- 菜单树（静态、编译期确定）----------
// 定义顺序刻意安排：子页面先于引用它的父页面定义，避免循环引用。
// 页面间不互相回指（无 parent 指针），返回上一级由 UIMenu 的导航栈维护。
const MenuItem kBrightItems[] = {
    {"亮度 +", nullptr, UiAction::kBrightnessUp},
    {"亮度 -", nullptr, UiAction::kBrightnessDown},
};

const char *const kAboutInfo[] = {
    "SMS Forwarder",
    "Fw: " APP_VERSION,
    "ESP32-S3 / 8MB PSRAM",
    "Web :80  U1 17/18  U2 41/40",
};

const MenuPage kPageBright = {
    "亮度调节", kBrightItems,
    (uint8_t)(sizeof(kBrightItems) / sizeof(kBrightItems[0])), nullptr, 0,
};
const MenuPage kPageAbout = {
    "关于", nullptr, 0, kAboutInfo,
    (uint8_t)(sizeof(kAboutInfo) / sizeof(kAboutInfo[0])),
};

const MenuItem kRootItems[] = {
    {"状态页", nullptr, UiAction::kBackToStatus},
    {"亮度调节", &kPageBright, UiAction::kNone},
    {"屏幕开关", nullptr, UiAction::kScreenToggle},
    {"屏幕显示", nullptr, UiAction::kScreenView},
    {"关于", &kPageAbout, UiAction::kNone},
};

const MenuPage kPageRoot = {
    "主菜单", kRootItems,
    (uint8_t)(sizeof(kRootItems) / sizeof(kRootItems[0])), nullptr, 0,
};

}  // namespace

void UIMenu::begin() {
  _statusMode = true;
  _current = nullptr;
  _cursor = 0;
  _depth = 0;
  _screenView = false;
  _scroll = 0;
  _dirty = true;
}

void UIMenu::handle(KeyId key, KeyEvent ev) {
  if (ev == KeyEvent::kNone) return;
  _lastInputMs = millis();

  // 熄屏时任意键先唤醒，这一下不额外导航
  if (!_disp.screenOn()) {
    _disp.setScreenOn(true);
    if (_statusMode) {
      _statusMode = false;
      _current = &kPageRoot;
      _cursor = 0;
    }
    _dirty = true;
    return;
  }

  // 状态页下任意键 -> 进入主菜单（吞掉这一下）
  if (_statusMode) {
    _statusMode = false;
    _current = &kPageRoot;
    _cursor = 0;
    _depth = 0;
    _dirty = true;
    return;
  }

  // 「屏幕显示」可滚动页：独立处理滚动 / 刷新 / 返回（Back 长按暂未定义，忽略）
  if (_screenView) {
    if (ev == KeyEvent::kLongPress && key == KeyId::kBack) return;  // 无定义操作
    if (ev == KeyEvent::kClick) {
      if (key == KeyId::kUp) _scroll -= 1;
      else if (key == KeyId::kDown) _scroll += 1;
      else if (key == KeyId::kBack) leaveScreenView();
      // OK 单击：本页无子项，忽略
    } else if (ev == KeyEvent::kRepeat) {
      int page = _disp.screenVisibleLines() - 1;  // 长按 UP/DOWN：翻一页
      if (page < 1) page = 1;
      if (key == KeyId::kUp) _scroll -= page;
      else if (key == KeyId::kDown) _scroll += page;
      // OK/BACK 连发忽略
    } else if (ev == KeyEvent::kLongPress && key == KeyId::kOk) {
      _provider(_screenSnap);  // 长按 OK：刷新（实时跟随下即立即重抓）
    }
    if (key == KeyId::kUp || key == KeyId::kDown || key == KeyId::kOk) _dirty = true;
    return;
  }

  // 长按 BACK：熄屏/唤醒
  if (ev == KeyEvent::kLongPress && key == KeyId::kBack) {
    _disp.setScreenOn(false);
    return;  // 熄屏后无需重绘
  }

  if (ev == KeyEvent::kClick) {
    if (key == KeyId::kUp) moveCursor(-1);
    else if (key == KeyId::kDown) moveCursor(1);
    else if (key == KeyId::kOk) activate();
    else if (key == KeyId::kBack) back();
  } else if (ev == KeyEvent::kRepeat) {
    // 连发仅对 UP/DOWN/OK 有意义（移动光标 / 重复执行动作）
    if (key == KeyId::kUp) moveCursor(-1);
    else if (key == KeyId::kDown) moveCursor(1);
    else if (key == KeyId::kOk) activate();
    // BACK 连发忽略
  }
  _dirty = true;
}

void UIMenu::render() {
  if (_statusMode || !_current) {
    _dirty = false;
    return;
  }
  const char *its[16];
  uint8_t n = _current->itemCount;
  for (uint8_t i = 0; i < n && i < 16; i++) its[i] = _current->items[i].label;

  String foot = "OK进入 BACK返回  亮度" + String(_disp.brightness()) + "%";
  _disp.renderMenu(_current->title, its, n, _cursor, _current->info,
                   _current->infoCount, foot.c_str());
  _dirty = false;
}

void UIMenu::moveCursor(int8_t dir) {
  int8_t n = _current ? _current->itemCount : 0;
  if (n <= 0) return;
  _cursor += dir;
  if (_cursor < 0) _cursor = n - 1;
  if (_cursor >= n) _cursor = 0;
}

void UIMenu::activate() {
  if (!_current || _current->itemCount == 0) return;
  const MenuItem &it = _current->items[_cursor];
  if (it.sub) {  // 进入子页面：压栈当前页，再下钻
    if (_depth < kMaxDepth) _stack[_depth++] = _current;
    _current = it.sub;
    _cursor = 0;
    return;
  }
  switch (it.action) {
    case UiAction::kBackToStatus:
      _statusMode = true;
      break;
    case UiAction::kBrightnessUp:
      setBright((int)_disp.brightness() + 10);
      break;
    case UiAction::kBrightnessDown:
      setBright((int)_disp.brightness() - 10);
      break;
    case UiAction::kScreenToggle:
      _disp.setScreenOn(!_disp.screenOn());
      break;
    case UiAction::kScreenView:
      _screenView = true;
      _statusMode = false;
      _scroll = 0;
      _provider(_screenSnap);  // 进入即抓一次最新快照
      _dirty = true;
      break;
    default:
      break;
  }
}

void UIMenu::back() {
  if (_depth > 0) {  // 弹出上一级页面
    _depth--;
    _current = _stack[_depth];
    _cursor = 0;
  } else {
    _statusMode = true;  // 根页面再返回 -> 状态页
  }
}

void UIMenu::setBright(int v) {
  if (v < 0) v = 0;
  if (v > 100) v = 100;
  _disp.setBrightness((uint8_t)v);
}

void UIMenu::renderScreenView() {
  if (!_screenView) return;
  if (!_disp.screenOn()) return;
  _provider(_screenSnap);  // 实时跟随：每次重绘抓最新快照
  _scroll = _disp.renderScreenView(_screenSnap, _scroll);
  _dirty = false;
}

void UIMenu::leaveScreenView() {
  _screenView = false;
  _scroll = 0;
  _statusMode = false;  // 返回根菜单
  _current = &kPageRoot;
  _cursor = 0;
  _dirty = true;
}
