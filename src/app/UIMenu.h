#pragma once

#include <stdint.h>
#include <functional>

#include "hal/Button.h"
#include "hal/Display.h"

// 菜单动作标识（叶子节点执行的具体行为）
enum class UiAction : uint8_t {
  kNone = 0,
  kBackToStatus,   // 切回实时状态页
  kBrightnessUp,   // 背光 +10%
  kBrightnessDown, // 背光 -10%
  kScreenToggle,   // 屏幕开关
  kScreenView,     // 进入「屏幕显示」可滚动页
};

// 前向声明：菜单项可指向子页面
struct MenuPage;

// 单个菜单项：有 sub 则进入子页面（忽略 action），否则执行 action
struct MenuItem {
  const char *label;             // 选项文字
  const MenuPage *sub;           // 子页面（非空表示可下钻）
  UiAction action;               // 叶子动作
};

// 一个菜单页：标题 + 选项列表，或一组正文（info 与 items 互斥）
// 页面间无回指指针：返回上一级由 UIMenu 的导航栈维护，避免编译期循环引用
struct MenuPage {
  const char *title;             // 标题栏
  const MenuItem *items;         // 选项数组（可为空）
  uint8_t itemCount;             // 选项数
  const char *const *info;       // 正文数组（与 items 互斥，可为空）
  uint8_t infoCount;             // 正文行数
};

// LCD 菜单导航状态机：由 4 键驱动（OK / UP / DOWN / BACK）
//  - 状态页下任意键 -> 进入主菜单
//  - UP / DOWN：移动光标（长按连发）
//  - OK：进入子页 / 执行动作（长按连发，便于亮度调节）
//  - BACK：返回上一级；根页面再按 -> 回状态页
//  - 长按 BACK：熄屏 / 唤醒
// 持有 Display 引用以直接调节背光与开关屏；状态页内容仍由 App 负责渲染。
class UIMenu {
 public:
  explicit UIMenu(Display &disp) : _disp(disp) {}

  void begin();

  // 处理一次按键事件（来自 Keypad::poll 的结果）
  void handle(KeyId key, KeyEvent ev);

  // 重绘当前菜单页（仅导航变化后调用）
  void render();

  // 状态快照提供者：由 App 注入，每次重绘「屏幕显示」页时抓取最新状态
  void setStatusProvider(std::function<void(StatusInfo &)> f) { _provider = std::move(f); }

  // 是否处于实时状态页（App 据此决定渲染路径）
  bool inStatusMode() const { return _statusMode; }
  void enterStatusMode() { _statusMode = true; }

  // 是否处于「屏幕显示」可滚动页
  bool inScreenView() const { return _screenView; }
  // 重绘「屏幕显示」页（实时跟随：每次调用抓最新快照后整页重绘）
  void renderScreenView();
  // 退出「屏幕显示」页，返回根菜单
  void leaveScreenView();

  // 菜单内容是否需要重绘（导航变化后置位，App 重绘后清零）
  bool dirty() const { return _dirty; }

  // 最近一次有效输入的时间戳（ms），供 App 做空闲自动休眠
  uint32_t lastInputMs() const { return _lastInputMs; }

 private:
  void moveCursor(int8_t dir);
  void activate();
  void back();
  void setBright(int v);

  Display &_disp;                 // 显示设备（调节背光/开关屏）
  bool _statusMode = true;        // 默认停留在实时状态页
  const MenuPage *_current = nullptr;  // 当前所在页面
  int8_t _cursor = 0;             // 当前高亮项
  static constexpr uint8_t kMaxDepth = 8;  // 导航栈深度上限
  const MenuPage *_stack[kMaxDepth];       // 上级页面栈（返回用）
  uint8_t _depth = 0;             // 当前栈深（0 = 根页面）
  bool _screenView = false;       // 是否处于「屏幕显示」可滚动页
  int _scroll = 0;                // 「屏幕显示」页滚动偏移（行）
  StatusInfo _screenSnap;         // 「屏幕显示」页最近一次抓取的快照
  std::function<void(StatusInfo &)> _provider = [](StatusInfo &) {};  // 状态快照提供者
  uint32_t _lastInputMs = 0;      // 上次输入时刻
  bool _dirty = true;             // 菜单内容待重绘标记
};
