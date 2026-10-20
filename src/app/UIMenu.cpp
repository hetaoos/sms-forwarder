#include "app/UIMenu.h"

#include <Preferences.h>

#include <Arduino.h>

#include "app/Config.h"
#include "ui/Ui.h"
#include "ui/UiIcons.h"

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
    {UI_ICON_SUN, "亮度 +", nullptr, UiAction::kBrightnessUp},
    {UI_ICON_SUN, "亮度 -", nullptr, UiAction::kBrightnessDown},
};

const char *const kAboutInfo[] = {
    "短信转发器",
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
    {UI_ICON_HOME, "状态页", nullptr, UiAction::kBackToStatus},
    {UI_ICON_SUN, "亮度调节", &kPageBright, UiAction::kNone},
    {UI_ICON_POWER, "屏幕开关", nullptr, UiAction::kScreenToggle},
    {UI_ICON_DISPLAY, "屏幕显示", nullptr, UiAction::kScreenView},
    {UI_ICON_INFO, "关于", &kPageAbout, UiAction::kNone},
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
  loadConfig();   // 恢复背光亮度 / 自动熄屏 / 自动返回状态页（无记录时保持默认）
}

void UIMenu::handle(KeyId key, KeyEvent ev) {
  if (ev == KeyEvent::kNone) return;
  _lastInputMs = millis();

  // 熄屏时任意键先唤醒，这一下不额外导航：
  // 唤醒后仍停在当前页面（状态页 / 菜单 / 屏幕显示页），
  // 自动熄屏与长按 BACK 熄屏共用这条路径，唤醒即可接着看
  if (!_disp.screenOn()) {
    _disp.setScreenOn(true);
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
    } else     if (ev == KeyEvent::kRepeat) {
      int page = ui::instance().screenViewVisibleLines() - 1;  // 长按 UP/DOWN：翻一页
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
  // 菜单页只给出「该显示什么」，具体排版/绘制交给 LVGL 页面层
  ui::MenuRow rows[16];
  uint8_t n = _current->itemCount;
  if (n > 16) n = 16;
  for (uint8_t i = 0; i < n; i++) {
    rows[i].icon = _current->items[i].icon;
    rows[i].label = _current->items[i].label;
  }

  String foot = "OK进入 BACK返回  亮度" + String(_disp.brightness()) + "%";
  ui::instance().menuUpdate(_current->title, rows, n, _cursor, _current->info,
                            _current->infoCount, foot.c_str());
  _dirty = false;
}

void UIMenu::renderStatus(const StatusInfo &info) { ui::instance().statusUpdate(info); }

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
  // 长按连调（kBtnRepeatMs 连发）时不逐次写 NVS，只标记待落盘，
  // 由 tick() 在停手 kDispSaveDebounceMs 后统一写一次
  _brightDirty = true;
  _brightDirtyMs = millis();
}

void UIMenu::renderScreenView() {
  if (!_screenView) return;
  if (!_disp.screenOn()) return;
  _provider(_screenSnap);  // 实时跟随：每次重绘抓最新快照
  _scroll = ui::instance().screenViewUpdate(_screenSnap, _scroll);
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
// ================= 屏幕设置（NVS 持久化）=================

// 时间值规整：0 保持「禁用」；非零钳到 [kUiSecMin, max]
uint16_t UIMenu::clampSec(int v, uint16_t max) {
  if (v <= 0) return 0;
  if (v > (int)max) return max;
  if (v < (int)cfg::kUiSecMin) return cfg::kUiSecMin;
  return (uint16_t)v;
}

void UIMenu::setAutoOffSec(uint16_t v) {
  _autoOffSec = clampSec((int)v, cfg::kUiAutoOffSecMax);
  saveConfig();
}

void UIMenu::setIdleSec(uint16_t v) {
  _idleSec = clampSec((int)v, cfg::kUiIdleReturnSecMax);
  saveConfig();
}

void UIMenu::setBrightness(uint8_t pct) {
  if (pct > 100) pct = 100;
  _disp.setBrightness(pct);
  _brightDirty = false;   // 外部设置是单次动作，直接落盘，不走防抖
  saveConfig();
}

// 空闲超时：先到点的先执行—— 1) 菜单/其它页退回实时状态页；2) 自动熄屏。
// 熄屏后内容保留在帧缓冲，任意键唤醒且仍停在当前页面（见 handle() 的唤醒分支）
void UIMenu::tick() {
  const uint32_t idle = millis() - _lastInputMs;
  if (_idleSec > 0 && idle >= (uint32_t)_idleSec * 1000UL) {
    if (_screenView) leaveScreenView();
    else if (!_statusMode) enterStatusMode();
  }
  if (_autoOffSec > 0 && idle >= (uint32_t)_autoOffSec * 1000UL &&
      _disp.screenOn()) {
    _disp.setScreenOn(false);
  }
  // 菜单连调亮度停手后补写一次 NVS
  if (_brightDirty && millis() - _brightDirtyMs >= cfg::kDispSaveDebounceMs) {
    _brightDirty = false;
    saveConfig();
  }
}

void UIMenu::exportTo(JsonObject &o) const {
  o["bright"] = _disp.brightness();
  o["autoOffSec"] = _autoOffSec;
  o["idleSec"] = _idleSec;
}

bool UIMenu::importFrom(const JsonObject &o) {
  // 缺失字段保持当前值（部分导入 / 向后兼容）
  _autoOffSec = clampSec(o["autoOffSec"] | (int)_autoOffSec, cfg::kUiAutoOffSecMax);
  _idleSec = clampSec(o["idleSec"] | (int)_idleSec, cfg::kUiIdleReturnSecMax);
  if (o["bright"].is<int>()) {
    int b = o["bright"].as<int>();
    if (b < 0) b = 0;
    if (b > 100) b = 100;
    _disp.setBrightness((uint8_t)b);
  }
  return saveConfig();
}

bool UIMenu::saveConfig() const {
  JsonDocument doc;
  JsonObject o = doc.to<JsonObject>();
  o["ver"] = 1;
  exportTo(o);

  String out;
  serializeJson(doc, out);

  Preferences prefs;
  if (!prefs.begin(cfg::kNvsNamespace, false)) return false;
  size_t written = prefs.putString(cfg::kNvsKeyDisp, out);
  prefs.end();
  return written == out.length();
}

void UIMenu::loadConfig() {
  String json;
  Preferences prefs;
  if (prefs.begin(cfg::kNvsNamespace, true)) {
    json = prefs.getString(cfg::kNvsKeyDisp, "");
    prefs.end();
  }
  // 无记录：沿用编译期默认值与面板默认亮度，不写盘（等用户首次设置）
  if (!json.length()) return;

  JsonDocument doc;
  if (deserializeJson(doc, json) != DeserializationError::Ok) return;
  // 缺失 / 越界字段保持当前值（向后兼容旧格式）
  _autoOffSec = clampSec(doc["autoOffSec"] | (int)_autoOffSec, cfg::kUiAutoOffSecMax);
  _idleSec = clampSec(doc["idleSec"] | (int)_idleSec, cfg::kUiIdleReturnSecMax);
  if (doc["bright"].is<int>()) {
    int b = doc["bright"].as<int>();
    if (b < 0) b = 0;
    if (b > 100) b = 100;
    _disp.setBrightness((uint8_t)b);
  }
}
