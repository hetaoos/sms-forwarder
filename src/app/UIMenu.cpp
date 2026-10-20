#include "app/UIMenu.h"

#include <Preferences.h>

#include <Arduino.h>
#include <stdarg.h>
#include <string.h>

#include "app/Config.h"
#include "ui/Ui.h"
#include "ui/UiIcons.h"

// 构建期由 script/inject_app_version.py 注入；未注入时退回占位，保证本地编译通过
#ifndef APP_VERSION
#define APP_VERSION "unknown"
#endif
#ifndef APP_BUILD_DATE
#define APP_BUILD_DATE "-"
#endif
#ifndef APP_PLATFORM
#define APP_PLATFORM "esp32s3"
#endif

namespace {

// ---------- 菜单树（静态、编译期确定）----------
// 定义顺序刻意安排：子页面先于引用它的父页面定义，避免循环引用。
// 页面间不互相回指（无 parent 指针），返回上一级由 UIMenu 的导航栈维护。
//
// id 为 PageId::kNone 的是静态页（条目/正文编译期固定）；
// 其余为动态页，每次渲染时按最新状态快照重建文本（见 UIMenu::buildDynamic）。

// ---- 屏幕 ----
const MenuItem kBrightItems[] = {
    {UI_ICON_SUN, "亮度 +", nullptr, UiAction::kBrightnessUp},
    {UI_ICON_SUN, "亮度 -", nullptr, UiAction::kBrightnessDown},
};
const MenuPage kPageBright = {
    "亮度调节", kBrightItems,
    (uint8_t)(sizeof(kBrightItems) / sizeof(kBrightItems[0])), nullptr, 0,
    PageId::kNone, BarSrc::kBright,
};

const MenuItem kAutoOffItems[] = {
    {UI_ICON_CLOCK, "时间 +", nullptr, UiAction::kAutoOffUp},
    {UI_ICON_CLOCK, "时间 -", nullptr, UiAction::kAutoOffDown},
};
const MenuPage kPageAutoOff = {
    "自动熄屏", kAutoOffItems,
    (uint8_t)(sizeof(kAutoOffItems) / sizeof(kAutoOffItems[0])), nullptr, 0,
    PageId::kNone, BarSrc::kAutoOff,
};

const MenuItem kIdleItems[] = {
    {UI_ICON_CLOCK, "时间 +", nullptr, UiAction::kIdleUp},
    {UI_ICON_CLOCK, "时间 -", nullptr, UiAction::kIdleDown},
};
const MenuPage kPageIdle = {
    "自动返回", kIdleItems,
    (uint8_t)(sizeof(kIdleItems) / sizeof(kIdleItems[0])), nullptr, 0,
    PageId::kNone, BarSrc::kIdle,
};

const MenuItem kScreenItems[] = {
    {UI_ICON_SUN, "亮度调节", &kPageBright, UiAction::kNone},
    {UI_ICON_POWER, "自动熄屏", &kPageAutoOff, UiAction::kNone},
    {UI_ICON_HOME, "自动返回", &kPageIdle, UiAction::kNone},
    {UI_ICON_POWER, "屏幕开关", nullptr, UiAction::kScreenToggle},
    {UI_ICON_DISPLAY, "屏幕显示", nullptr, UiAction::kScreenView},
};
const MenuPage kPageScreen = {
    "屏幕", kScreenItems,
    (uint8_t)(sizeof(kScreenItems) / sizeof(kScreenItems[0])), nullptr, 0,
};

// ---- 模块（两个串口各一页）----
const MenuPage kPageModem1 = {"模块 1", nullptr, 0, nullptr, 0, PageId::kModem1};
const MenuPage kPageModem2 = {"模块 2", nullptr, 0, nullptr, 0, PageId::kModem2};
const MenuItem kModemItems[] = {
    {UI_ICON_SIM, "模块 1 (U1)", &kPageModem1, UiAction::kNone},
    {UI_ICON_SIM, "模块 2 (U2)", &kPageModem2, UiAction::kNone},
};
const MenuPage kPageModem = {
    "模块", kModemItems,
    (uint8_t)(sizeof(kModemItems) / sizeof(kModemItems[0])), nullptr, 0,
};

// ---- 动态信息页 ----
const MenuPage kPageSms = {"短信", nullptr, 0, nullptr, 0, PageId::kSms};
const MenuPage kPageSoc = {"SOC", nullptr, 0, nullptr, 0, PageId::kSoc};
const MenuPage kPageRgb = {"RGB 灯", nullptr, 0, nullptr, 0, PageId::kRgb,
                           BarSrc::kRgbBright};
const MenuPage kPageWifi = {"WIFI", nullptr, 0, nullptr, 0, PageId::kWifi};
const MenuPage kPageMail = {"邮件通知", nullptr, 0, nullptr, 0, PageId::kMail};
const MenuPage kPageMqtt = {"MQTT", nullptr, 0, nullptr, 0, PageId::kMqtt};
const MenuPage kPagePush = {"推送通道", nullptr, 0, nullptr, 0, PageId::kPush};
const MenuPage kPageAbout = {"关于", nullptr, 0, nullptr, 0, PageId::kAbout};

// ---- 根菜单 ----
// 条目多于菜单槽位（8 行）时自动滚动，页脚显示「当前/总数」
const MenuItem kRootItems[] = {
    {UI_ICON_HOME, "状态页", nullptr, UiAction::kBackToStatus},
    {UI_ICON_SMS, "短信", &kPageSms, UiAction::kNone},
    {UI_ICON_MICROCHIP, "SOC", &kPageSoc, UiAction::kNone},
    {UI_ICON_TOWER, "模块", &kPageModem, UiAction::kNone},
    {UI_ICON_DISPLAY, "屏幕", &kPageScreen, UiAction::kNone},
    {UI_ICON_LIGHTBULB, "RGB 灯", &kPageRgb, UiAction::kNone},
    {UI_ICON_WIFI, "WIFI", &kPageWifi, UiAction::kNone},
    {UI_ICON_MAIL, "邮件通知", &kPageMail, UiAction::kNone},
    {UI_ICON_MQTT, "MQTT", &kPageMqtt, UiAction::kNone},
    {UI_ICON_PUSH, "推送通道", &kPagePush, UiAction::kNone},
    {UI_ICON_INFO, "关于", &kPageAbout, UiAction::kNone},
};

const MenuPage kPageRoot = {
    "主菜单", kRootItems,
    (uint8_t)(sizeof(kRootItems) / sizeof(kRootItems[0])), nullptr, 0,
};

// 字节数紧凑格式：>=1MB 显示 "x.xxM"，>=1KB 显示 "xxx.xK"，否则 "xxxB"
String fmtBytes(uint32_t bytes) {
  char buf[16];
  if (bytes >= 1024u * 1024u) {
    snprintf(buf, sizeof(buf), "%.2fM", bytes / 1048576.0);
  } else if (bytes >= 1024u) {
    snprintf(buf, sizeof(buf), "%.1fK", bytes / 1024.0);
  } else {
    snprintf(buf, sizeof(buf), "%uB", (unsigned)bytes);
  }
  return String(buf);
}

// 秒数 -> 可读文本：0 = 关闭，<60 用秒，其余用「分（+秒）」
String secText(uint16_t v) {
  if (!v) return String("关闭");
  if (v < 60) return String(v) + "秒";
  const uint16_t s = v % 60;
  return String(v / 60) + "分" + (s ? String(s) + "秒" : String(""));
}

// 邮件通知类型位掩码 -> 「启动 SIM 短信 在线」
String mailTypesText(uint32_t m) {
  String s;
  if (m & cfg::kEmailNotifyStartup) s += "启动 ";
  if (m & cfg::kEmailNotifySim) s += "SIM ";
  if (m & cfg::kEmailNotifySms) s += "短信 ";
  if (m & cfg::kEmailNotifyOnline) s += "在线 ";
  s.trim();
  return s.length() ? s : String("无");
}

// 时间源名（ntp/sim/nvs/none）-> 中文
const char *timeSrcCn(const char *s) {
  if (!s || !*s) return "-";
  if (!strcmp(s, "ntp")) return "NTP 网络";
  if (!strcmp(s, "sim")) return "SIM 模组";
  if (!strcmp(s, "nvs")) return "上次记录";
  return "未同步";
}

}  // namespace

// ================= 生命周期 =================

void UIMenu::begin() {
  _statusMode = true;
  _current = nullptr;
  _cursor = 0;
  _rootCursor = 0;
  _depth = 0;
  _view = false;
  _viewScroll = 0;
  _popup = false;
  _dirty = true;
  _dynCount = 0;
  loadConfig();   // 恢复背光亮度 / 自动熄屏 / 自动返回状态页（无记录时保持默认）
}

// 回到实时状态页：顺手记下根菜单当前选中的项，下次从状态页进菜单时接着用
void UIMenu::enterStatusMode() {
  if (_current == &kPageRoot) _rootCursor = _cursor;
  _statusMode = true;
  _dirty = true;
}

// ================= 按键 =================

void UIMenu::handle(KeyId key, KeyEvent ev) {
  if (ev == KeyEvent::kNone) return;
  _lastInputMs = millis();

  // 熄屏时任意键先唤醒，这一下不额外导航：
  // 唤醒后仍停在当前页面（状态页 / 菜单 / 文本页），
  // 自动熄屏与长按 BACK 熄屏共用这条路径，唤醒即可接着看
  if (!_disp.screenOn()) {
    _disp.setScreenOn(true);
    _dirty = true;
    return;
  }

  // 新短信弹窗期间：OK 打开全文（可滚动），其余任意键收起
  if (_popup) {
    if (ev == KeyEvent::kClick && key == KeyId::kOk) {
      String t;
      fillSmsViewOf(_popMsg, t);   // 正文长时到可滚动文本页里看全文
      openView("短信详情", t);
      dismissPopup();
    } else {
      dismissPopup();
    }
    return;
  }

  // 状态页下任意键 -> 进入主菜单（吞掉这一下），并回到上次选中的那一项
  if (_statusMode) {
    _statusMode = false;
    _current = &kPageRoot;
    _cursor = _rootCursor;
    _depth = 0;
    _dirty = true;
    return;
  }

  // 可滚动文本页：独立处理滚动 / 刷新 / 返回（Back 长按暂未定义，忽略）
  if (_view) {
    if (ev == KeyEvent::kLongPress && key == KeyId::kBack) return;  // 无定义操作
    if (ev == KeyEvent::kClick) {
      if (key == KeyId::kUp) _viewScroll -= 1;
      else if (key == KeyId::kDown) _viewScroll += 1;
      else if (key == KeyId::kBack) leaveView();
      // OK 单击：本页无子项，忽略
    } else if (ev == KeyEvent::kRepeat) {
      int page = ui::instance().viewVisibleLines() - 1;  // 长按 UP/DOWN：翻一页
      if (page < 1) page = 1;
      if (key == KeyId::kUp) _viewScroll -= page;
      else if (key == KeyId::kDown) _viewScroll += page;
      // OK/BACK 连发忽略
    } else if (ev == KeyEvent::kLongPress && key == KeyId::kOk) {
      if (_viewLive) fillStatusView(_viewText);  // 实时页：立即重抓一次
    }
    if (key == KeyId::kUp || key == KeyId::kDown || key == KeyId::kOk) _dirty = true;
    return;
  }

  // 长按 BACK：熄屏/唤醒
  if (ev == KeyEvent::kLongPress && key == KeyId::kBack) {
    _disp.setScreenOn(false);
    return;  // 熄屏后无需重绘
  }

  // 纯信息页（无选项）的 UP/DOWN 是滚正文，有选项时才是移动光标
  const bool infoPage = (currentItemCount() == 0);
  const int step = ui::instance().menuScrollStep();
  const int page = ui::instance().menuScrollPage();

  if (ev == KeyEvent::kClick) {
    if (key == KeyId::kUp) {
      if (infoPage) ui::instance().menuScrollBy(-step); else moveCursor(-1);
    } else if (key == KeyId::kDown) {
      if (infoPage) ui::instance().menuScrollBy(step); else moveCursor(1);
    }
    else if (key == KeyId::kOk) activate();
    else if (key == KeyId::kBack) back();
  } else if (ev == KeyEvent::kRepeat) {
    // 连发仅对 UP/DOWN/OK 有意义（滚动 / 移动光标 / 重复执行动作）
    if (key == KeyId::kUp) {
      if (infoPage) ui::instance().menuScrollBy(-page); else moveCursor(-1);
    } else if (key == KeyId::kDown) {
      if (infoPage) ui::instance().menuScrollBy(page); else moveCursor(1);
    }
    else if (key == KeyId::kOk) activate();
    // BACK 连发忽略
  }
  _dirty = true;
}

// ================= 渲染 =================

void UIMenu::render() {
  if (_statusMode || !_current) {
    _dirty = false;
    return;
  }

  // 当前页可能是动态页：先按最新状态快照重建条目 / 正文
  MenuPage pg = *_current;
  if (pg.id != PageId::kNone) buildDynamic(pg);

  // 条目全部交给 UI 层（区域自带滚动，高亮行会自动滚入视野）
  const uint8_t n = pg.itemCount;
  _dynCount = n;
  if ((uint8_t)_cursor >= n && n > 0) _cursor = n - 1;

  ui::MenuRow rows[kMenuSlots];
  for (uint8_t i = 0; i < n && i < kMenuSlots; i++) {
    rows[i].icon = pg.items[i].icon;
    rows[i].label = pg.items[i].label;
  }

  // 页脚：选项页给「第几项/共几项 + 操作提示」，信息页提示可滚动
  String foot;
  if (n > 0) {
    foot += String(_cursor + 1) + "/" + String(n) + " ";
    if (pg.id == PageId::kRgb) foot += "OK切换  " + String(_snap.rgbName);
    else foot += "OK进入 BACK返回";
  } else {
    foot += "U/D滚动 BACK返回";
  }

  // 底部进度条（调节类页面）
  int pct = -1;
  String barText;
  const bool hasBar = barOf(pg, pct, barText);

  // 菜单页只给出「该显示什么」，具体排版/绘制交给 LVGL 页面层
  ui::instance().menuUpdate(pg.title, rows, n, (int8_t)_cursor, pg.info, pg.infoCount,
                            foot.c_str(), hasBar ? pct : -1,
                            hasBar ? barText.c_str() : nullptr);
  _dirty = false;
}

void UIMenu::renderStatus(const StatusInfo &info) { ui::instance().statusUpdate(info); }

// ================= 导航 =================

uint8_t UIMenu::currentItemCount() const {
  if (!_current) return 0;
  return (_current->id != PageId::kNone) ? _dynCount : _current->itemCount;
}

void UIMenu::moveCursor(int8_t dir) {
  const int8_t n = (int8_t)currentItemCount();
  if (n <= 0) return;
  _cursor += dir;
  if (_cursor < 0) _cursor = n - 1;
  if (_cursor >= n) _cursor = 0;
}

void UIMenu::activate() {
  if (!_current) return;
  const bool dyn = (_current->id != PageId::kNone);
  const MenuItem *items = dyn ? _dynItems : _current->items;
  const uint8_t n = currentItemCount();
  if (n == 0 || _cursor < 0 || (uint8_t)_cursor >= n) return;
  const MenuItem &it = items[_cursor];
  if (it.sub) {  // 进入子页面：压栈当前页（连同光标位置），再下钻
    if (_depth < kMaxDepth) {
      _stack[_depth].page = _current;
      _stack[_depth].cursor = _cursor;
      _depth++;
    }
    _current = it.sub;
    _cursor = 0;
    return;
  }
  switch (it.action) {
    case UiAction::kBackToStatus:
      enterStatusMode();
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
    case UiAction::kScreenView: {
      String t;
      fillStatusView(t);
      openView("屏幕显示", t);
      _viewLive = true;
      break;
    }
    case UiAction::kRgbNext:
      if (_rgbFn) _rgbFn(0);
      break;
    case UiAction::kRgbUp:
      if (_rgbFn) _rgbFn(1);
      break;
    case UiAction::kRgbDown:
      if (_rgbFn) _rgbFn(2);
      break;
    case UiAction::kAutoOffUp:
      setAutoOffSec(stepSec(_autoOffSec, 1, cfg::kUiAutoOffSecMax));
      break;
    case UiAction::kAutoOffDown:
      setAutoOffSec(stepSec(_autoOffSec, -1, cfg::kUiAutoOffSecMax));
      break;
    case UiAction::kIdleUp:
      setIdleSec(stepSec(_idleSec, 1, cfg::kUiIdleReturnSecMax));
      break;
    case UiAction::kIdleDown:
      setIdleSec(stepSec(_idleSec, -1, cfg::kUiIdleReturnSecMax));
      break;
    case UiAction::kSmsOpen: {
      if (_smsCount <= 0) break;
      if (_smsSel < 0 || _smsSel >= _smsCount) _smsSel = 0;
      String t;
      fillSmsView(t);
      openView("短信详情", t);
      break;
    }
    default:
      break;
  }
}

void UIMenu::back() {
  if (_depth > 0) {  // 弹出上一级页面，并恢复到离开时选中的那一项
    _depth--;
    _current = _stack[_depth].page;
    _cursor = _stack[_depth].cursor;
  } else {
    enterStatusMode();  // 根页面再返回 -> 状态页
  }
}

// ================= 可滚动文本页 =================

void UIMenu::openView(const char *title, const String &text) {
  if (_depth < kMaxDepth) {
    _stack[_depth].page = _current ? _current : &kPageRoot;
    _stack[_depth].cursor = _cursor;
    _depth++;
  }
  _view = true;
  _viewLive = false;
  _viewTitle = title ? title : "";
  _viewText = text;
  _viewScroll = 0;
  _statusMode = false;
  _dirty = true;
}

void UIMenu::leaveView() {
  _view = false;
  _viewLive = false;
  _viewScroll = 0;
  if (_depth > 0) {   // 回到打开文本页时所在的菜单页（含当时选中的项）
    _depth--;
    _current = _stack[_depth].page;
    _cursor = _stack[_depth].cursor;
  } else {
    _current = &kPageRoot;
    _cursor = _rootCursor;
  }
  _dirty = true;
}

void UIMenu::renderView() {
  if (!_view) return;
  if (!_disp.screenOn()) return;
  if (_viewLive) fillStatusView(_viewText);  // 实时跟随：每次重绘抓最新快照
  _viewScroll = ui::instance().textViewUpdate(_viewTitle.c_str(), _viewText.c_str(),
                                              _viewScroll);
  _dirty = false;
}

// 把状态快照摊平成可滚动的纯文本（「屏幕显示」页）
void UIMenu::fillStatusView(String &out) {
  _provider(_snap);
  out = "";
  out += "WIFI: ";
  out += _snap.wifiUp ? "已连接" : (_snap.apMode ? "AP 配网" : "未连接");
  out += "\n网络: ";
  out += _snap.ssid;
  out += "\nIP: ";
  out += _snap.ip;
  out += "\n信号: ";
  if (_snap.wifiUp) {
    out += String(_snap.rssi);
    out += " dBm";
  } else {
    out += "-";
  }
  out += "\nMAC: ";
  out += _snap.mac;
  out += "\n运行: ";
  out += _snap.uptime;
  out += "\n内存: ";
  out += _snap.mem;
  out += "\n负载: ";
  out += _snap.load;
  out += "\n模组1: ";
  out += _snap.modem[0];
  out += "\n模组2: ";
  out += _snap.modem[1];
  out += "\nMQTT: ";
  out += _snap.mqtt;
  out += "\n邮件: ";
  out += _snap.mail;
  out += "\n推送: ";
  out += _snap.push;
  out += "\nRGB: ";
  out += _snap.ledName;
  out += "\n";
  out += "\n最近 AT";
  if (_snap.lastPort) {
    out += "\n串口";
    out += String(_snap.lastPort);
  }
  if (_snap.lastCmd && *_snap.lastCmd) {
    out += "\n";
    out += _snap.lastCmd;
  }
  if (_snap.lastResp && *_snap.lastResp) {
    out += "\n";
    out += _snap.lastResp;
  }
}

// 短信详情文本（「短信」页选中项）
void UIMenu::fillSmsView(String &out) const {
  fillSmsViewOf(_smsPrev[_smsSel], out);
}

void UIMenu::fillSmsViewOf(const SmsPreview &m, String &out) {
  out = "号码: ";
  out += m.phone[0] ? m.phone : "-";
  out += "\n时间: ";
  out += m.time[0] ? m.time : "-";
  out += "\n模组: ";
  if (m.modem > 0) {
    out += "模组";
    out += String((int)m.modem);
  } else {
    out += "未知";
  }
  out += "\n方向: ";
  out += m.incoming ? "收到" : "发出";
  out += "\n";
  out += "\n";
  out += m.text[0] ? m.text : "(无正文)";
}

// ================= 新短信弹窗 =================

void UIMenu::showSms(const SmsPreview &m) {
  _popMsg = m;
  _popup = true;
  _popMs = millis();
  _lastInputMs = millis();   // 弹窗期间不触发自动熄屏 / 自动返回
  if (!_disp.screenOn()) _disp.setScreenOn(true);   // 熄屏时也要弹出来
  _dirty = true;
}

void UIMenu::renderPopup() {
  ui::instance().smsPopupUpdate(_popMsg.phone, _popMsg.text, _popMsg.modem,
                                _popMsg.time, _popMsg.incoming);
  _dirty = false;
}

void UIMenu::dismissPopup() {
  _popup = false;
  _dirty = true;   // 回到原页面需要重绘一次
}

// ================= 动态页内容 =================

void UIMenu::buildDynamic(MenuPage &pg) {
  _provider(_snap);   // 动态页每次渲染前抓一次最新快照

  for (uint8_t i = 0; i < kInfoSlots; i++) {
    _infoBuf[i][0] = 0;
    _infoPtr[i] = _infoBuf[i];
  }
  for (uint8_t i = 0; i < kMenuSlots; i++) {
    _itemBuf[i][0] = 0;
    _dynItems[i].icon = nullptr;
    _dynItems[i].label = _itemBuf[i];
    _dynItems[i].sub = nullptr;
    _dynItems[i].action = UiAction::kNone;
  }
  pg.info = _infoPtr;
  pg.infoCount = 0;
  pg.items = _dynItems;
  pg.itemCount = 0;

  switch (pg.id) {
    case PageId::kSms: fillSms(pg); break;
    case PageId::kSoc: fillSoc(pg); break;
    case PageId::kModem1: fillModem(pg, 0); break;
    case PageId::kModem2: fillModem(pg, 1); break;
    case PageId::kWifi: fillWifi(pg); break;
    case PageId::kMail: fillMail(pg); break;
    case PageId::kMqtt: fillMqtt(pg); break;
    case PageId::kPush: fillPush(pg); break;
    case PageId::kRgb: fillRgb(pg); break;
    case PageId::kAbout: fillAbout(pg); break;
    default: break;
  }
}

// 追加一行正文（超出槽位忽略）
void UIMenu::addInfo(MenuPage &pg, const char *fmt, ...) {
  if (pg.infoCount >= kInfoSlots) return;
  char *dst = _infoBuf[pg.infoCount];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(dst, sizeof(_infoBuf[0]), fmt, ap);
  va_end(ap);
  pg.infoCount++;
}

// 追加一个选项（超出槽位忽略）
void UIMenu::addItem(MenuPage &pg, const char *icon, const char *label,
                     const MenuPage *sub, UiAction action) {
  if (pg.itemCount >= kMenuSlots) return;
  const uint8_t i = pg.itemCount;
  snprintf(_itemBuf[i], sizeof(_itemBuf[0]), "%s", label ? label : "");
  _dynItems[i].icon = icon;
  _dynItems[i].label = _itemBuf[i];
  _dynItems[i].sub = sub;
  _dynItems[i].action = action;
  pg.itemCount++;
}

// ---- 短信：列出最近若干条，选中后打开可滚动的详情 ----
void UIMenu::fillSms(MenuPage &pg) {
  if (_smsFn) _smsCount = _smsFn(_smsPrev, cfg::kUiSmsPreviewMax, _smsTotal);
  else { _smsCount = 0; _smsTotal = 0; }
  if (_smsSel >= _smsCount) _smsSel = 0;
  if (_smsSel < 0) _smsSel = 0;

  if (_smsCount <= 0) {
    addItem(pg, UI_ICON_SMS, "暂无短信", nullptr, UiAction::kNone);
    return;
  }
  for (int i = 0; i < _smsCount; i++) {
    const SmsPreview &m = _smsPrev[i];
    // 行文案："收 13800138000 10-20 12:00"（方向 + 号码 + 时间）
    char buf[32];
    snprintf(buf, sizeof(buf), "%s %s %s", m.incoming ? "收" : "发",
             m.phone[0] ? m.phone : "-", m.time[0] ? m.time : "-");
    addItem(pg, UI_ICON_SMS, buf, nullptr, UiAction::kSmsOpen);
  }
  // 选中项由 _cursor 决定：这里把光标映射回预览下标，供 kSmsOpen 取用
  _smsSel = (_cursor >= 0 && _cursor < _smsCount) ? _cursor : 0;
}

// ---- SOC：芯片 / 资源 / 负载 / 时间 ----
void UIMenu::fillSoc(MenuPage &pg) {
  addInfo(pg, "芯片: %s", _snap.chip);
  addInfo(pg, "内核: %s 核   主频: %s", _snap.cores, _snap.cpuFreq);
  addInfo(pg, "Flash: %s", _snap.flash);
  addInfo(pg, "内存: %s / %s", fmtBytes(_snap.freeHeap).c_str(), _snap.heapSize);
  addInfo(pg, "PSRAM: %s / %s", fmtBytes(_snap.freePsram).c_str(), _snap.psramTotal);
  addInfo(pg, "CPU: %s   温度: %s", _snap.cpu, _snap.temp);
  addInfo(pg, "运行: %s", _snap.uptime);
  addInfo(pg, "时间: %s", _snap.dateTime);
  addInfo(pg, "时间源: %s", timeSrcCn(_snap.timeSrc));
}

// ---- 模块：单个串口的完整遥测 ----
void UIMenu::fillModem(MenuPage &pg, uint8_t idx) {
  const ModemSnapshot &m = _snap.modems[idx];
  const char *defName = (idx == 0) ? "模组1" : "模组2";
  addInfo(pg, "状态: %s", m.state);
  addInfo(pg, "名称: %s", m.alias[0] ? m.alias : defName);
  addInfo(pg, "型号: %s", m.model);
  addInfo(pg, "固件: %s", m.revision);
  addInfo(pg, "SIM: %s", m.sim ? "就绪" : "未就绪");
  if (m.csq >= 0 && m.csq <= 31) {
    addInfo(pg, "信号: %d (%d dBm)", (int)m.csq, -113 + 2 * (int)m.csq);
  } else {
    addInfo(pg, "信号: 未知");
  }
  addInfo(pg, "运营商: %s", m.oper);
  addInfo(pg, "注册: %s", m.reg);
  addInfo(pg, "号码: %s", m.num);
}

// ---- WIFI：工作模式 / 连接 / 已保存配置 ----
void UIMenu::fillWifi(MenuPage &pg) {
  addInfo(pg, "模式: %s", _snap.wifiMode);
  addInfo(pg, "网络: %s", _snap.ssid);
  addInfo(pg, "IP: %s", _snap.ip);
  if (_snap.wifiUp) addInfo(pg, "信号: %d dBm", (int)_snap.rssi);
  else addInfo(pg, "信号: -");
  addInfo(pg, "MAC: %s", _snap.mac);
  addInfo(pg, "已保存: %u 组", (unsigned)_snap.wifiSaved);
  addInfo(pg, "网页: http://%s", _snap.ip);
  addInfo(pg, "时间: %s", _snap.dateTime);
  addInfo(pg, "时间源: %s", timeSrcCn(_snap.timeSrc));
}

// ---- 邮件通知：开关 / 配置 / 服务器 / 收件 / 队列 ----
void UIMenu::fillMail(MenuPage &pg) {
  addInfo(pg, "状态: %s", _snap.mailOn ? (_snap.mailCfg ? "已启用" : "已启用(未配置)")
                                      : "已停用");
  addInfo(pg, "配置: %s", _snap.mailCfg ? "完整" : "不完整");
  addInfo(pg, "服务器: %s", _snap.mailServer);
  addInfo(pg, "发件: %s", _snap.mailUser);
  addInfo(pg, "收件: %s", _snap.mailTo);
  addInfo(pg, "待发: %u 封", (unsigned)_snap.mailQueue);
  addInfo(pg, "排程: %s", _snap.mailSched);
  addInfo(pg, "通知: %s", mailTypesText(_snap.mailTypes).c_str());
}

// ---- MQTT：开关 / 配置 / 连接 / 主题 / 错误 ----
void UIMenu::fillMqtt(MenuPage &pg) {
  const char *conn = _snap.mqttConn ? "已连接"
                     : (_snap.mqttErr[0] && strcmp(_snap.mqttErr, "-") ? "连接失败"
                                                                       : "未连接");
  addInfo(pg, "开关: %s", _snap.mqttOn ? "已启用" : "已停用");
  addInfo(pg, "配置: %s", _snap.mqttCfg ? "完整" : "不完整");
  addInfo(pg, "连接: %s", _snap.mqttOn ? conn : "-");
  addInfo(pg, "服务器: %s", _snap.mqttServer);
  addInfo(pg, "客户端: %s", _snap.mqttClient);
  addInfo(pg, "主题: %s", _snap.mqttTopic);
  addInfo(pg, "订阅命令: %s", _snap.mqttSub ? "开" : "关");
  addInfo(pg, "错误: %s", _snap.mqttErr);
}

// ---- 推送通道：开关 / 策略 / 各通道 ----
void UIMenu::fillPush(MenuPage &pg) {
  addInfo(pg, "状态: %s", _snap.pushOn ? "已启用" : "已停用");
  addInfo(pg, "策略: %s", _snap.pushStrategy);
  addInfo(pg, "有效通道: %u 个", (unsigned)_snap.pushCh);
  for (uint8_t i = 0; i < _snap.pushListCount; i++) {
    const StatusInfo::ChanBrief &c = _snap.pushList[i];
    addInfo(pg, "%u. %s %s", (unsigned)(i + 1), c.label,
            !c.on ? "停用" : (c.valid ? "有效" : "缺字段"));
  }
}

// ---- RGB 灯：切换颜色 + 亮度调节（带进度条）----
void UIMenu::fillRgb(MenuPage &pg) {
  addItem(pg, UI_ICON_LIGHTBULB, "切换颜色", nullptr, UiAction::kRgbNext);
  addItem(pg, UI_ICON_SUN, "亮度 +", nullptr, UiAction::kRgbUp);
  addItem(pg, UI_ICON_SUN, "亮度 -", nullptr, UiAction::kRgbDown);
}

// ---- 关于：软硬件版本 / 开发人员 / 说明 ----
void UIMenu::fillAbout(MenuPage &pg) {
  addInfo(pg, "短信转发器 SMS Forwarder");
  addInfo(pg, "固件: %s", APP_VERSION);
  addInfo(pg, "构建: %s", APP_BUILD_DATE);
  addInfo(pg, "平台: %s", APP_PLATFORM);
  addInfo(pg, "硬件: %s", _snap.chip);
  addInfo(pg, "开发: Null");
  addInfo(pg, "网页: http://%s", _snap.ip);
  addInfo(pg, "串口: U1 %d/%d  U2 %d/%d", (int)cfg::kUart1Tx, (int)cfg::kUart1Rx,
          (int)cfg::kUart2Tx, (int)cfg::kUart2Rx);
  addInfo(pg, "说明: 双串口 4G 短信转发");
}

// ================= 进度条 =================

bool UIMenu::barOf(const MenuPage &pg, int &pct, String &text) const {
  switch (pg.bar) {
    case BarSrc::kBright: {
      const int v = (int)_disp.brightness();
      pct = v;
      text = String(v) + "%";
      return true;
    }
    case BarSrc::kRgbBright: {
      pct = (int)_snap.rgbBright;
      text = String((int)_snap.rgbBright) + "%";
      return true;
    }
    case BarSrc::kAutoOff: {
      const int v = (int)_autoOffSec;
      pct = v ? v * 100 / (int)cfg::kUiAutoOffSecMax : 0;
      if (v && pct < 1) pct = 1;   // 非零但不足 1% 时也给一格，避免看着像关闭
      text = secText(_autoOffSec);
      return true;
    }
    case BarSrc::kIdle: {
      const int v = (int)_idleSec;
      pct = v ? v * 100 / (int)cfg::kUiIdleReturnSecMax : 0;
      if (v && pct < 1) pct = 1;
      text = secText(_idleSec);
      return true;
    }
    default:
      return false;
  }
}

// ================= 屏幕设置（NVS 持久化）=================

void UIMenu::setBright(int v) {
  if (v < 0) v = 0;
  if (v > 100) v = 100;
  _disp.setBrightness((uint8_t)v);
  // 长按连调（kBtnRepeatMs 连发）时不逐次写 NVS，只标记待落盘，
  // 由 tick() 在停手 kDispSaveDebounceMs 后统一写一次
  _brightDirty = true;
  _brightDirtyMs = millis();
}

// 时间值规整：0 保持「禁用」；非零钳到 [kUiSecMin, max]
uint16_t UIMenu::clampSec(int v, uint16_t max) {
  if (v <= 0) return 0;
  if (v > (int)max) return max;
  if (v < (int)cfg::kUiSecMin) return cfg::kUiSecMin;
  return (uint16_t)v;
}

// 按步长调整一项超时（菜单 +/- 连发用）：从「关闭」起跳到最小值，反向可回到关闭
uint16_t UIMenu::stepSec(uint16_t cur, int delta, uint16_t max) {
  const int step = (max > 600) ? 10 : 5;
  int v = (int)cur;
  if (cur == 0 && delta > 0) v = cfg::kUiSecMin;
  else v += delta * step;
  return clampSec(v, max);
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

// 空闲超时：新短信弹窗优先（到点自动收起），其余先到点的先执行——
// 1) 菜单/其它页退回实时状态页；2) 自动熄屏。
// 熄屏后内容保留在帧缓冲，任意键唤醒且仍停在当前页面（见 handle() 的唤醒分支）
void UIMenu::tick() {
  const uint32_t now = millis();

  // 新短信弹窗：到点自动收起（按任意键可提前收起）
  if (_popup && now - _popMs >= cfg::kUiSmsPopupMs) dismissPopup();

  // 动态页（SOC / 模块 / WIFI / 邮件 / MQTT / 推送 / RGB）自节流刷新：
  // 菜单页平时只在按键后重绘，这些页的数据会自己变，需要按周期补一次
  if (!_statusMode && !_view && !_popup && _current && _current->id != PageId::kNone) {
    if (now - _lastDynMs >= cfg::kUiRefreshMs) {
      _lastDynMs = now;
      _dirty = true;
    }
  }

  const uint32_t idle = now - _lastInputMs;
  if (_idleSec > 0 && idle >= (uint32_t)_idleSec * 1000UL) {
    if (_view) leaveView();
    else if (!_statusMode) enterStatusMode();
  }
  if (_autoOffSec > 0 && idle >= (uint32_t)_autoOffSec * 1000UL &&
      _disp.screenOn()) {
    _disp.setScreenOn(false);
  }
  // 菜单连调亮度停手后补写一次 NVS
  if (_brightDirty && now - _brightDirtyMs >= cfg::kDispSaveDebounceMs) {
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
