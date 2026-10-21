#include "app/UIMenu.h"

#include <Preferences.h>

#include <Arduino.h>
#include <stdarg.h>
#include <string.h>

#include "app/Config.h"
#include "ota/Ota.h"    // 工具箱：检查新版本并更新（Ota 状态机）
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

const MenuPage kPageScreenInfo = {"屏幕信息", nullptr, 0, nullptr, 0, PageId::kScreenInfo};

const MenuItem kScreenItems[] = {
    {UI_ICON_SUN, "亮度调节", &kPageBright, UiAction::kNone},
    {UI_ICON_POWER, "自动熄屏", &kPageAutoOff, UiAction::kNone},
    {UI_ICON_HOME, "自动返回", &kPageIdle, UiAction::kNone},
    {UI_ICON_POWER, "屏幕开关", nullptr, UiAction::kScreenToggle},
    {UI_ICON_INFO, "屏幕信息", &kPageScreenInfo, UiAction::kNone},
};
const MenuPage kPageScreen = {
    "屏幕", kScreenItems,
    (uint8_t)(sizeof(kScreenItems) / sizeof(kScreenItems[0])), nullptr, 0,
};

// ---- 通信模组（两个串口各一页）----
const MenuPage kPageModem1 = {"通信模组 1", nullptr, 0, nullptr, 0, PageId::kModem1};
const MenuPage kPageModem2 = {"通信模组 2", nullptr, 0, nullptr, 0, PageId::kModem2};
const MenuItem kModemItems[] = {
    {UI_ICON_SIM, "通信模组 1 (U1)", &kPageModem1, UiAction::kNone},
    {UI_ICON_SIM, "通信模组 2 (U2)", &kPageModem2, UiAction::kNone},
};
const MenuPage kPageModem = {
    "通信模组", kModemItems,
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

// ---- 工具箱：检查新版本并更新 / 重启 / 恢复出厂（动态页，内容随 OTA 状态重建）----
const MenuPage kPageToolbox = {"工具箱", nullptr, 0, nullptr, 0, PageId::kToolbox};

// ---- 根菜单 ----
// 条目多于菜单槽位（8 行）时自动滚动，页脚显示「当前/总数」
const MenuItem kRootItems[] = {
    {UI_ICON_HOME, "仪表盘", nullptr, UiAction::kBackToStatus},
    {UI_ICON_SMS, "短信", &kPageSms, UiAction::kNone},
    {UI_ICON_MICROCHIP, "SOC", &kPageSoc, UiAction::kNone},
    {UI_ICON_TOWER, "通信模组", &kPageModem, UiAction::kNone},
    {UI_ICON_DISPLAY, "屏幕", &kPageScreen, UiAction::kNone},
    {UI_ICON_LIGHTBULB, "RGB 灯", &kPageRgb, UiAction::kNone},
    {UI_ICON_WIFI, "WIFI", &kPageWifi, UiAction::kNone},
    {UI_ICON_MAIL, "邮件通知", &kPageMail, UiAction::kNone},
    {UI_ICON_MQTT, "MQTT", &kPageMqtt, UiAction::kNone},
    {UI_ICON_PUSH, "推送通道", &kPagePush, UiAction::kNone},
    {UI_ICON_MENU, "工具箱", &kPageToolbox, UiAction::kNone},
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
  if (!strcmp(s, "sim")) return "SIM 通信模组";
  if (!strcmp(s, "nvs")) return "上次记录";
  return "未同步";
}

// OTA 状态机 -> 中文（工具箱页状态行用）
const char *otaStateCn(OtaState s) {
  switch (s) {
    case OtaState::IDLE: return "空闲";
    case OtaState::CHECKING: return "正在检查更新";
    case OtaState::DOWNLOADING: return "正在下载固件";
    case OtaState::WRITING: return "正在写入固件";
    case OtaState::SUCCESS: return "升级成功";
    case OtaState::FAILED: return "升级失败";
    case OtaState::FLASHING_FS: return "正在下载 Web UI";
  }
  return "-";
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
    else if (key == KeyId::kOk) activate(ev);
    else if (key == KeyId::kBack) back();
  } else if (ev == KeyEvent::kRepeat) {
    // 连发仅对 UP/DOWN/OK 有意义（滚动 / 移动光标 / 重复执行动作）
    if (key == KeyId::kUp) {
      if (infoPage) ui::instance().menuScrollBy(-page); else moveCursor(-1);
    } else if (key == KeyId::kDown) {
      if (infoPage) ui::instance().menuScrollBy(page); else moveCursor(1);
    }
    else if (key == KeyId::kOk) activate(ev);
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
    rows[i].scroll = pg.items[i].scroll;
  }

  // 页脚：选项页给「第几项/共几项 + 操作提示」，信息页提示可滚动
  String foot;
  if (n > 0) {
    foot += String(_cursor + 1) + "/" + String(n) + " ";
    if (pg.id == PageId::kRgb) foot += "OK切换  " + String(_snap.rgbName);
    else if (pg.id == PageId::kToolbox) foot += "OK执行 BACK返回";
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

void UIMenu::activate(KeyEvent ev) {
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
    // ---- 工具箱 ----
    case UiAction::kOtaCheck: {
      // 检查中 / 升级中忽略连按；其余情况发起一次新检查
      if (_toolPhase == ToolPhase::kCheck || _toolPhase == ToolPhase::kUpgrade) break;
      const OtaStatusPayload st = Ota::status();
      if (st.state != OtaState::IDLE) {
        _toolMsg = "其它升级任务进行中";
        break;
      }
      if (!_otaCheckFn) break;
      _otaCheckFn();
      _toolPhase = ToolPhase::kCheck;
      _toolMs = millis();
      _toolStarted = false;
      _toolMsg = "正在检查更新...";
      break;
    }
    case UiAction::kOtaUpgrade: {
      // 用户确认升级：仅在「已发现新版本」时有效（入口由 fillToolbox 动态给出）
      if (_toolPhase != ToolPhase::kFound || !_toolNewVer.length() || !_otaUpgradeFn) break;
      _otaUpgradeFn(_toolNewVer);
      _toolPhase = ToolPhase::kUpgrade;
      _toolMs = millis();
      _toolStarted = false;
      _toolMsg = "开始升级...";
      break;
    }
    case UiAction::kRestart: {
      if (_restartPending || !_restartFn) break;   // 倒计时中不重复触发
      _restartPending = true;
      _restartMs = millis();
      break;
    }
    case UiAction::kReset: {
      // 只认短按：长按连发（kRepeat）不计入，必须实打实按满 kToolResetConfirm 次
      if (ev != KeyEvent::kClick || _resetPending || !_resetFn) break;
      _resetCount++;
      _resetMs = millis();
      if (_resetCount >= cfg::kToolResetConfirm) {
        _resetPending = true;
        _resetActMs = millis();
      }
      break;
    }
    case UiAction::kAccountReset: {
      // 只认短按：长按连发不计入，必须实打实按满 kToolResetConfirm 次
      if (ev != KeyEvent::kClick || _accResetPending || !_accResetFn) break;
      _accResetCount++;
      _accResetMs = millis();
      if (_accResetCount >= cfg::kToolResetConfirm) {
        _accResetPending = true;
        _accResetActMs = millis();
      }
      break;
    }
    default:
      break;
  }
}

void UIMenu::back() {
  // 重启 / 恢复出厂的倒计时宽限内按 BACK：取消本次操作（倒计时未到就不会执行）
  if (_restartPending) {
    _restartPending = false;
    _toolMsg = "已取消重启";
    return;
  }
  if (_resetPending) {
    _resetPending = false;
    _resetCount = 0;
    _toolMsg = "已取消重置";
    return;
  }
  if (_accResetPending) {
    _accResetPending = false;
    _accResetCount = 0;
    _toolMsg = "已取消重置Web密码";
    return;
  }
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
  _viewScroll = ui::instance().textViewUpdate(_viewTitle.c_str(), _viewText.c_str(),
                                              _viewScroll);
  _dirty = false;
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
  out += "\n通信模组: ";
  if (m.modem > 0) {
    out += "通信模组";
    out += String((int)m.modem);
    if (m.alias[0]) {
      out += " (";
      out += m.alias;
      out += ")";
    }
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
    case PageId::kScreenInfo: fillScreenInfo(pg); break;
    case PageId::kToolbox: fillToolbox(pg); break;
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
                     const MenuPage *sub, UiAction action, bool scroll) {
  if (pg.itemCount >= kMenuSlots) return;
  const uint8_t i = pg.itemCount;
  snprintf(_itemBuf[i], sizeof(_itemBuf[0]), "%s", label ? label : "");
  _dynItems[i].icon = icon;
  _dynItems[i].label = _itemBuf[i];
  _dynItems[i].sub = sub;
  _dynItems[i].action = action;
  _dynItems[i].scroll = scroll;
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
    // 行文案可能过长：启用横向跑马灯滚动（不换行、不截断省略号）
    addItem(pg, UI_ICON_SMS, buf, nullptr, UiAction::kSmsOpen, true);
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

// ---- 通信模组：单个串口的完整遥测 ----
void UIMenu::fillModem(MenuPage &pg, uint8_t idx) {
  const ModemSnapshot &m = _snap.modems[idx];
  const char *defName = (idx == 0) ? "通信模组1" : "通信模组2";
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
  addInfo(pg, "IMEI: %s", m.imei);
  // 引脚：idx 0 -> 串口 U1，idx 1 -> 串口 U2
  const int8_t tx = (idx == 0) ? cfg::kUart1Tx : cfg::kUart2Tx;
  const int8_t rx = (idx == 0) ? cfg::kUart1Rx : cfg::kUart2Rx;
  addInfo(pg, "引脚: TX%d RX%d", (int)tx, (int)rx);
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
  // AP 配网热点信息：直接在 WIFI 页展示
  addInfo(pg, "热点: %s", _snap.apSsid);
  addInfo(pg, "热点密码: %s", _snap.apPass);
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
  addInfo(pg, "内存: %s / %s", fmtBytes(_snap.freeHeap).c_str(), _snap.heapSize);
  addInfo(pg, "PSRAM: %s / %s", fmtBytes(_snap.freePsram).c_str(), _snap.psramTotal);
  addInfo(pg, "运行: %s", _snap.uptime);
  addInfo(pg, "时间: %s", _snap.dateTime);
  addInfo(pg, "时间源: %s", timeSrcCn(_snap.timeSrc));
  addInfo(pg, "WiFi: %s", _snap.wifiMode);
  addInfo(pg, "短信上限: %u 条", (unsigned)cfg::kSmsStoreMax);
}

// ---- 屏幕信息：型号 / 分辨率 / 接线（取自 cfg 编译期常量）----
void UIMenu::fillScreenInfo(MenuPage &pg) {
  addInfo(pg, "型号: ST7789");
  addInfo(pg, "分辨率: %dx%d", (int)cfg::kTftWidth, (int)cfg::kTftHeight);
  addInfo(pg, "旋转: %d", (int)cfg::kTftRotation);
  addInfo(pg, "SPI: %dMHz", (int)(cfg::kTftSpiFreq / 1000000));
  addInfo(pg, "背光: %d%%", (int)cfg::kTftBacklight);
  addInfo(pg, "CS:%d RST:%d DC:%d", (int)cfg::kTftCs, (int)cfg::kTftRst,
          (int)cfg::kTftDc);
  addInfo(pg, "SDA:%d SCL:%d", (int)cfg::kTftMosi, (int)cfg::kTftSclk);
  addInfo(pg, "BLK:%d", (int)cfg::kTftBlk);
}

// ---- 工具箱：检查新版本并更新 / 重启 / 恢复出厂 ----

// 推进 OTA 状态机：由「工具箱」页每次渲染时调用（动态页 500ms 节流）。
//  - kCheck：等检查收尾（Ota 状态回 IDLE）。发现新版本 -> 只记录版本并给出
//    「升级到新版本」入口，是否更新由用户按 OK 确认（kOtaUpgrade），不自动升级；
//    没有新版本 -> 按 Ota 的 message 给结论（为空 = 已是最新）。
//  - kFound：等待用户确认，不自动前进。
//  - kUpgrade：等终态。SUCCESS 后设备 2s 自动重启；FAILED 后保留升级入口可重试。
// 发起后 Ota 内部可能因 30s 防抖 / 通道占用而没真正启动（状态一直是 IDLE），
// 超过 kToolOtaStartGraceMs 宽限即回报「操作未开始」，避免页面永远停在检查中。
void UIMenu::pollToolbox() {
  const ToolPhase phase = _toolPhase;
  if (phase == ToolPhase::kIdle || phase == ToolPhase::kFound) return;
  const OtaStatusPayload st = Ota::status();
  const uint32_t now = millis();

  if (!_toolStarted) {
    if (st.state != OtaState::IDLE) {
      _toolStarted = true;   // 确认已启动，进入等待收尾
    } else if (now - _toolMs >= cfg::kToolOtaStartGraceMs) {
      // 被 30s 防抖 / 通道占用挡下：检查并未真正开始，回到之前的状态
      _toolPhase = _toolNewVer.length() ? ToolPhase::kFound : ToolPhase::kIdle;
      _toolMsg = "操作未开始，请稍后再试";
    }
    return;
  }

  if (phase == ToolPhase::kCheck) {
    if (st.state != OtaState::IDLE) return;   // 检查仍在进行（CHECKING）
    if (!st.latestVersion.isEmpty()) {
      // 发现新版本：不自动升级，展示入口等用户确认（见 kOtaUpgrade）
      _toolPhase = ToolPhase::kFound;
      _toolNewVer = st.latestVersion;
      _toolMsg = "发现新版本，按 OK 升级";
    } else {
      _toolPhase = ToolPhase::kIdle;
      _toolNewVer = "";   // 确实没有新版本，收回升级入口
      // message 为空串即「已是最新版本」（Ota 检查收尾的约定，见 checkVersionTask）
      _toolMsg = st.message.length() ? st.message : String("已是最新版本");
    }
    return;
  }

  // phase == kUpgrade：等到终态；下载 / 烧录途中继续由进度条展示
  if (st.state == OtaState::SUCCESS) {
    _toolPhase = ToolPhase::kIdle;
    _toolMsg = "升级成功，即将重启";
  } else if (st.state == OtaState::FAILED) {
    _toolMsg = st.message.length() ? st.message : String("升级失败");
    // 保留升级入口，用户可重试（版本仍是刚查到的那个）
    _toolPhase = _toolNewVer.length() ? ToolPhase::kFound : ToolPhase::kIdle;
  }
}

// 工具箱页内容：当前/最新版本 + OTA 状态 + 动作项。
// 「升级到新版本」项只在发现新版本后出现——更新必须用户按 OK 确认，不自动下载。
// 底部进度条只在下载与烧录阶段显示（barOf 读 Ota::status().progress）。
void UIMenu::fillToolbox(MenuPage &pg) {
  pollToolbox();   // 先推进状态机，再按最新状态渲染

  const OtaStatusPayload st = Ota::status();
  addInfo(pg, "当前版本: %s", st.currentVersion.c_str());
  if (_toolNewVer.length()) {
    addInfo(pg, "最新版本: %s", _toolNewVer.c_str());
  }
  addInfo(pg, "状态: %s", otaStateCn(st.state));
  if (_toolMsg.length()) addInfo(pg, "%s", _toolMsg.c_str());

  addItem(pg, UI_ICON_GLOBE, "检查新版本", nullptr, UiAction::kOtaCheck);
  // 确认入口：仅「已发现新版本」时出现，按 OK 才下载升级
  if (_toolPhase == ToolPhase::kFound && _toolNewVer.length()) {
    addItem(pg, UI_ICON_REFRESH, "升级到新版本", nullptr, UiAction::kOtaUpgrade);
  }
  addItem(pg, UI_ICON_POWER, "重启", nullptr, UiAction::kRestart);
  // 重置行按确认进度显示「重置 (n/5)」；按满次数后保持 5/5 直到执行或取消
  char buf[32];
  if (_resetCount > 0) snprintf(buf, sizeof(buf), "重置 (%u/5)", (unsigned)_resetCount);
  else snprintf(buf, sizeof(buf), "重置");
  addItem(pg, UI_ICON_INFO, buf, nullptr, UiAction::kReset);

  // Web 账号：查看当前用户名（只读）+ 重置登录密码为随机密码（连按确认）
  const String accUser = _accUserFn ? _accUserFn() : String("-");
  addInfo(pg, "Web账号: %s", accUser.c_str());
  char abuf[32];
  if (_accResetCount > 0)
    snprintf(abuf, sizeof(abuf), "重置Web密码 (%u/%u)", (unsigned)_accResetCount,
             (unsigned)cfg::kToolResetConfirm);
  else
    snprintf(abuf, sizeof(abuf), "重置Web密码");
  addItem(pg, UI_ICON_MAC, abuf, nullptr, UiAction::kAccountReset);

  // 倒计时提示（确认后的宽限，BACK 可取消；执行动作由 tick() 到点发起）
  if (_restartPending) addInfo(pg, "设备即将重启，BACK 取消");
  if (_resetPending) addInfo(pg, "正在恢复出厂设置...");

  pg.bar = (st.state == OtaState::DOWNLOADING || st.state == OtaState::WRITING ||
            st.state == OtaState::FLASHING_FS)
               ? BarSrc::kOta
               : BarSrc::kNone;
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
    case BarSrc::kOta: {
      // 工具箱：OTA 下载 / 烧录进度（只读快照，任务在 Ota 后台跑）
      const OtaStatusPayload st = Ota::status();
      pct = (int)st.progress;
      text = String(pct) + "%";
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

  // 动态页（SOC / 通信模组 / WIFI / 邮件 / MQTT / 推送 / RGB）自节流刷新：
  // 菜单页平时只在按键后重绘，这些页的数据会自己变，需要按周期补一次
  if (!_statusMode && !_view && !_popup && _current && _current->id != PageId::kNone) {
    if (now - _lastDynMs >= cfg::kUiRefreshMs) {
      _lastDynMs = now;
      _dirty = true;
    }
  }

  // ---- 工具箱 ----
  // 重启 / 恢复出厂：确认后延迟 kToolActDelayMs 执行，给屏幕留出提示时间，
  // 期间按 BACK 可取消（见 back()）。回调内部还会再延迟 2s 才真正重启
  // （与 Web「系统维护」一致），屏幕提示不会被打断。
  if (_restartPending && now - _restartMs >= cfg::kToolActDelayMs) {
    _restartPending = false;
    if (_restartFn) _restartFn();
  }
  if (_resetPending && now - _resetActMs >= cfg::kToolActDelayMs) {
    _resetPending = false;
    if (_resetFn) _resetFn();
  }
  // 重置连按计数：停手超时未续按、或已离开工具箱页 -> 清零，重新计
  if (_resetCount > 0 && !_resetPending) {
    if (now - _resetMs >= cfg::kToolResetTimeoutMs || _current != &kPageToolbox) {
      _resetCount = 0;
      _dirty = true;   // 标签从「重置 (n/5)」收回「重置」
    }
  }

  // Web 密码重置：确认后延迟 kToolActDelayMs 执行，给屏幕留出提示时间，
  // 期间按 BACK 可取消（见 back()）。执行时生成随机密码、写 NVS、作废会话，
  // 并以可滚动文本页展示新密码（旧密码与所有会话已失效）。
  if (_accResetPending && now - _accResetActMs >= cfg::kToolActDelayMs) {
    _accResetPending = false;
    if (_accResetFn) {
      _accNewPass = _accResetFn();
      String u = _accUserFn ? _accUserFn() : String("-");
      if (_accNewPass.length()) {
        String t = "用户名: " + u + "\n新密码: " + _accNewPass +
                   "\n\n请立即用新密码登录网页，旧密码与所有会话已失效。";
        openView("Web 登录信息", t);
      } else {
        openView("Web 登录信息", "重置失败，请重试或重启设备");
      }
    }
  }
  // 重置 Web 密码连按计数：停手超时未续按、或已离开工具箱页 -> 清零，重新计
  if (_accResetCount > 0 && !_accResetPending) {
    if (now - _accResetMs >= cfg::kToolResetTimeoutMs || _current != &kPageToolbox) {
      _accResetCount = 0;
      _dirty = true;   // 标签从「重置Web密码 (n/5)」收回「重置Web密码」
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
