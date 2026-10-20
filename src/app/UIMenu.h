#pragma once

#include <ArduinoJson.h>

#include <stdint.h>
#include <functional>

#include "app/Config.h"
#include "app/StatusInfo.h"
#include "hal/Button.h"
#include "hal/Display.h"

// 菜单动作标识（叶子节点执行的具体行为）
enum class UiAction : uint8_t {
  kNone = 0,
  kBackToStatus,   // 切回实时状态页
  kBrightnessUp,   // 背光 +10%
  kBrightnessDown, // 背光 -10%
  kScreenToggle,   // 屏幕开关
  kRgbNext,        // RGB 灯切换到下一个预设颜色
  kRgbUp,          // RGB 亮度 +
  kRgbDown,        // RGB 亮度 -
  kAutoOffUp,      // 自动熄屏时间 +
  kAutoOffDown,    // 自动熄屏时间 -
  kIdleUp,         // 自动返回状态页时间 +
  kIdleDown,       // 自动返回状态页时间 -
  kSmsOpen,        // 打开短信详情（可滚动文本页，取当前光标项）
  kOtaCheck,       // 工具箱：检查新版本（发现新版本后不自动升级，留待用户确认）
  kOtaUpgrade,     // 工具箱：升级到检查到的新版本（用户确认入口，仅发现新版本后出现）
  kRestart,        // 工具箱：重启设备
  kReset,          // 工具箱：恢复出厂设置（需连按 OK kToolResetConfirm 次确认）
};

// 动态页标识：静态页为 kNone（内容编译期确定）；
// 非 kNone 的页面在每次 render 时按最新状态快照重建文本（见 UIMenu::buildDynamic）
enum class PageId : uint8_t {
  kNone = 0,
  kSms,      // 短信列表（条目由短信预览生成）
  kSoc,      // SOC 信息
  kModem1,   // 通信模组 1（串口 U1）
  kModem2,   // 通信模组 2（串口 U2）
  kWifi,     // WiFi 状态
  kMail,     // 邮件通知状态
  kMqtt,     // MQTT 状态
  kPush,     // 推送通道状态
  kRgb,      // RGB 灯
  kAbout,    // 关于
  kScreenInfo,  // 屏幕信息（型号 / 接线）
  kToolbox,  // 工具箱（检查新版本并更新 / 重启 / 恢复出厂）
};

// 进度条数据源：菜单页底部可显示一条进度条（亮度 / 超时 / RGB 亮度调节用）。
// kNone 表示本页不显示进度条。
enum class BarSrc : uint8_t {
  kNone = 0,
  kBright,     // 屏幕背光亮度（0-100）
  kAutoOff,    // 自动熄屏时间（0 - kUiAutoOffSecMax）
  kIdle,       // 自动返回状态页时间（0 - kUiIdleReturnSecMax）
  kRgbBright,  // RGB 灯亮度（0-100）
  kOta,        // OTA 升级进度（0-100，工具箱页下载/烧录阶段显示）
};

// 前向声明：菜单项可指向子页面
struct MenuPage;

// 单个菜单项：有 sub 则进入子页面（忽略 action），否则执行 action
struct MenuItem {
  const char *icon;              // 选项图标（UiIcons.h 的 UI_ICON_*，可为空）
  const char *label;             // 选项文字
  const MenuPage *sub;           // 子页面（非空表示可下钻）
  UiAction action;               // 叶子动作
  bool scroll = false;           // 行文案过长时横向滚动（跑马灯）而非省略号截断
};

// 一个菜单页：标题 + 选项列表，或一组正文（info 与 items 通常互斥）。
// 页面间无回指指针：返回上一级由 UIMenu 的导航栈维护，避免编译期循环引用。
struct MenuPage {
  const char *title;             // 标题栏
  const MenuItem *items;         // 选项数组（可为空）
  uint8_t itemCount;             // 选项数
  const char *const *info;       // 正文数组（与 items 互斥，可为空）
  uint8_t infoCount;             // 正文行数
  PageId id = PageId::kNone;     // 动态页标识（kNone = 静态页）
  BarSrc bar = BarSrc::kNone;    // 底部进度条数据源（kNone = 不显示）
};

// LCD 短信预览条目（定长、无堆分配）：菜单「短信」页与新短信弹窗共用。
// text 截断到 160 字节——小屏幕上 5~6 行足够，过长只会拖慢渲染。
struct SmsPreview {
  uint32_t id = 0;        // 存储 ID
  bool incoming = true;   // true = 收到，false = 发出
  int8_t modem = -1;      // 通信模组串口号 1..2，-1 = 未知
  uint32_t ts = 0;        // 时间戳（纪元秒或开机秒数）
  char phone[24] = "";    // 对方号码
  char time[18] = "";     // 可读时间 "10-20 12:00" / "12:00:00"
  char text[160] = "";    // 正文（UTF-8，超长截断）
  char alias[18] = "";    // 来源通信模组别名（备注，空则无）
};

// LCD 菜单导航状态机：由 4 键驱动（OK / UP / DOWN / BACK）
//  - 状态页下任意键 -> 进入主菜单
//  - UP / DOWN：移动光标（长按连发）
//  - OK：进入子页 / 执行动作（长按连发，便于亮度调节）
//  - BACK：返回上一级；根页面再按 -> 回状态页
//  - 长按 BACK：熄屏 / 唤醒
//
// 三种「非菜单」显示态，优先级自上而下：
//   1. 新短信弹窗（showSms）：任意键或超时收起，收起后回到原页面
//   2. 可滚动文本页（屏幕显示 / 短信详情）：U/D 滚动，长按 OK 刷新，BACK 返回
//   3. 实时状态页：由 App 按周期刷新
//
// 持有 Display 引用以直接调节背光与开关屏；状态页内容仍由 App 负责渲染。
//
// 同时是「屏幕设置」的归属通信模组（与邮件 / MQTT / 推送 / 黑名单同套路）：
// 背光亮度、无操作自动熄屏、无操作自动退回状态页三项自身持有并落 NVS，
// 对外提供读写接口与导入导出，供 Web「屏幕显示」页直接操作。
class UIMenu {
 public:
  // 菜单槽位上限：与 UI 层 ui::Ui 的 kMaxMenuRows 保持一致（超出部分由 UI 滚动查看）
  static constexpr uint8_t kMenuSlots = 12;
  // 正文行上限：与 UI 层 ui::Ui 的 kMaxInfoRows 保持一致
  static constexpr uint8_t kInfoSlots = 20;

  explicit UIMenu(Display &disp) : _disp(disp) {}

  void begin();

  // 处理一次按键事件（来自 Keypad::poll 的结果）
  void handle(KeyId key, KeyEvent ev);

  // 重绘当前菜单页（仅导航变化 / 动态页刷新周期到达后调用）
  void render();

  // 实时状态页：由 App 按刷新周期调用，把快照交给 LVGL 页面更新
  void renderStatus(const StatusInfo &info);

  // 状态快照提供者：由 App 注入，动态页每次渲染前抓最新状态
  void setStatusProvider(std::function<void(StatusInfo &)> f) { _provider = std::move(f); }

  // 短信列表提供者：由 App 注入。填充最多 max 条预览（时间倒序，最新在前），
  // total 出参为存储总条数；返回实际填充条数。
  // 只在「短信」菜单页可见时调用——扫文件有成本，不做周期采集。
  using SmsListFn = std::function<int(SmsPreview *out, int max, int &total)>;
  void setSmsListProvider(SmsListFn f) { _smsFn = std::move(f); }

  // RGB 灯控制回调（App 注入，避免 UI 层直接依赖 RgbLed）：
  // cmd = 0 切换下一个预设颜色 / 1 亮度 +10 / 2 亮度 -10
  using RgbFn = std::function<void(uint8_t cmd)>;
  void setRgbControl(RgbFn f) { _rgbFn = std::move(f); }

  // ============ 工具箱回调（App 注入，避免 UI 层直接依赖 Ota / NVS / 文件系统）============
  // check：发起一次版本检查；upgrade(tag)：升级到指定 Release tag（仅用户确认后
  // 才调用，不自动升级）；restart / reset：重启设备 / 恢复出厂。
  // 四个回调都只负责「发起」：耗时动作在 Ota 后台任务或延迟重启任务里执行，
  // 不阻塞主循环与屏幕渲染。
  using OtaCheckFn = std::function<void()>;
  using OtaUpgradeFn = std::function<void(const String &tag)>;
  using SysFn = std::function<void()>;
  void setOtaCheckFn(OtaCheckFn f) { _otaCheckFn = std::move(f); }
  void setOtaUpgradeFn(OtaUpgradeFn f) { _otaUpgradeFn = std::move(f); }
  void setRestartFn(SysFn f) { _restartFn = std::move(f); }
  void setResetFn(SysFn f) { _resetFn = std::move(f); }

  // ============ 新短信弹窗 ============
  // 由 App 在主循环（loop 任务）检测到新短信时调用：立刻弹到屏上并点亮屏幕。
  // 只存一份定长副本，不持有外部引用，无跨任务生命周期问题。
  void showSms(const SmsPreview &m);
  bool inPopup() const { return _popup; }
  // 重绘弹窗（内容固定，只在首次与弹窗内容变化时真正重绘）
  void renderPopup();
  // 收起弹窗（按键 / 超时）；不改动当前页面状态，收起后自然回到原页面
  void dismissPopup();

  // 是否处于实时状态页（App 据此决定渲染路径）
  bool inStatusMode() const { return _statusMode; }
  // 回到实时状态页；顺手记下根菜单当前选中的项，下次进菜单时接着用
  void enterStatusMode();

  // 是否处于可滚动文本页（屏幕显示 / 短信详情）
  bool inView() const { return _view; }
  // 重绘文本页：有实时数据源的（屏幕显示）顺带抓一次最新快照
  void renderView();
  // 退出文本页，返回压栈前所在的菜单页
  void leaveView();

  // 菜单内容是否需要重绘（导航变化 / 动态页刷新后置位，App 重绘后清零）
  bool dirty() const { return _dirty; }

  // 最近一次有效输入的时间戳（ms），供 App 做空闲自动休眠
  uint32_t lastInputMs() const { return _lastInputMs; }

  // ============ 屏幕设置（NVS 持久化，Web「屏幕显示」页读写）============

  // 无操作自动熄屏时间（秒，0 = 不自动熄屏）
  uint16_t autoOffSec() const { return _autoOffSec; }
  // 无操作自动退回实时状态页时间（秒，0 = 不自动返回）
  uint16_t idleSec() const { return _idleSec; }

  // 设置两项超时（越界钳制，0 = 关闭该功能）；立即生效并落盘
  void setAutoOffSec(uint16_t v);
  void setIdleSec(uint16_t v);

  // 外部（Web）设置背光亮度：立即生效并落盘
  void setBrightness(uint8_t pct);

  // 空闲超时处理（App::tick 每轮调用）：自动退回状态页 + 自动熄屏 + 弹窗超时
  void tick();

  // 配置导出：{"bright":n,"autoOffSec":n,"idleSec":n}
  void exportTo(JsonObject &o) const;
  // 配置导入（缺失字段保持当前值）；写盘失败返回 false
  bool importFrom(const JsonObject &o);

 private:
  // 当前页的有效条目数：动态页取上次重建后的条数，静态页取编译期数组长度
  uint8_t currentItemCount() const;
  void moveCursor(int8_t dir);
  // ev 传入按键事件：重置确认只认短按（kClick），长按连发不计入，防止按住不放凑满次数
  void activate(KeyEvent ev);
  void back();
  void setBright(int v);

  // ---- 工具箱 ----
  // 推进 OTA 检查/升级状态机（由「工具箱」动态页每次渲染时调用，500ms 节流）：
  // 检查收尾发现新版本只记录并展示「升级到新版本」入口，不自动升级——
  // 是否更新由用户按 OK 确认（见 activate 的 kOtaUpgrade）。
  void pollToolbox();
  // 填充「工具箱」页：版本信息 + OTA 状态 + 三个动作项（下载/烧录阶段挂进度条）
  void fillToolbox(MenuPage &pg);

  // ---- 可滚动文本页 ----
  // 打开一个文本页：title 为标题；fill 为空表示静态文本（不随周期刷新）
  void openView(const char *title, const String &text);
  // 短信详情文本：短信页取选中项，弹窗取弹窗里那份
  void fillSmsView(String &out) const;
  static void fillSmsViewOf(const SmsPreview &m, String &out);

  // ---- 动态页 ----
  // 按当前状态快照填充 pg 的选项 / 正文（pg 是 *_current 的副本，可安全改写）
  void buildDynamic(MenuPage &pg);
  // 追加一行正文（超出槽位忽略）
  void addInfo(MenuPage &pg, const char *fmt, ...);
  // 追加一个选项（超出槽位忽略）
  void addItem(MenuPage &pg, const char *icon, const char *label, const MenuPage *sub,
               UiAction action, bool scroll = false);
  // 逐条填充各动态页
  void fillSms(MenuPage &pg);
  void fillSoc(MenuPage &pg);
  void fillModem(MenuPage &pg, uint8_t idx);
  void fillWifi(MenuPage &pg);
  void fillMail(MenuPage &pg);
  void fillMqtt(MenuPage &pg);
  void fillPush(MenuPage &pg);
  void fillRgb(MenuPage &pg);
  void fillAbout(MenuPage &pg);
  void fillScreenInfo(MenuPage &pg);

  // ---- 进度条 ----
  // 按页面的 bar 数据源算出进度条百分比与右侧数值文本；无进度条返回 false
  bool barOf(const MenuPage &pg, int &pct, String &text) const;

  // 载入 NVS 配置（无记录时保持编译期默认值）
  void loadConfig();
  // 写入 NVS（三项一次落盘）
  bool saveConfig() const;
  // 时间值规整：0 保持「禁用」；非零钳到 [kUiSecMin, max]
  static uint16_t clampSec(int v, uint16_t max);
  // 按步长调整一项超时（用于菜单 +/- 连发）：返回新值
  static uint16_t stepSec(uint16_t cur, int delta, uint16_t max);

  Display &_disp;                 // 显示设备（调节背光/开关屏）
  bool _statusMode = true;        // 默认停留在实时状态页
  const MenuPage *_current = nullptr;  // 当前所在页面
  int8_t _cursor = 0;             // 当前高亮项

  // 导航栈：除页面外还记住离开时的光标位置——返回时回到上次选中的那一项，
  // 而不是一律回到第一项（层层下钻再返回时体验差别很大）
  struct Frame {
    const MenuPage *page;
    int8_t cursor;
  };
  static constexpr uint8_t kMaxDepth = 8;  // 导航栈深度上限
  Frame _stack[kMaxDepth];
  uint8_t _depth = 0;             // 当前栈深（0 = 根页面）
  uint8_t _dynCount = 0;          // 当前动态页的条目数（静态页无意义）
  int8_t _rootCursor = 0;         // 根菜单上次选中的项（从状态页再进来时恢复）

  // ---- 可滚动文本页 ----
  bool _view = false;             // 是否处于可滚动文本页
  String _viewTitle;              // 文本页标题
  String _viewText;               // 文本页正文（含换行）
  int _viewScroll = 0;            // 滚动偏移（行）
  bool _viewLive = false;         // 是否有实时数据源（true = 每次刷新重抓）
  StatusInfo _snap;               // 最近一次抓取的状态快照（动态页与文本页共用）

  // ---- 新短信弹窗 ----
  bool _popup = false;            // 弹窗是否正在显示
  SmsPreview _popMsg;             // 弹窗内容（定长副本）
  uint32_t _popMs = 0;            // 弹窗起始时刻（ms）

  // ---- 短信列表（「短信」菜单页）----
  SmsPreview _smsPrev[cfg::kUiSmsPreviewMax];  // 预览条目（最新在前）
  int _smsCount = 0;                           // 已填充条数
  int _smsTotal = 0;                           // 存储总条数
  int8_t _smsSel = 0;                          // 短信页当前选中的预览下标

  std::function<void(StatusInfo &)> _provider = [](StatusInfo &) {};  // 状态快照提供者
  SmsListFn _smsFn;               // 短信列表提供者（未注入时短信页显示「暂无」）
  RgbFn _rgbFn;                   // RGB 灯控制回调（未注入时 RGB 页只显示，不响应）
  uint32_t _lastInputMs = 0;      // 上次输入时刻
  uint32_t _lastDynMs = 0;        // 动态页上次刷新时刻（自节流 500ms）
  bool _dirty = true;             // 菜单内容待重绘标记

  // 动态页的文本缓冲（正文行 / 选项文字），避免渲染路径产生堆分配
  char _infoBuf[kInfoSlots][44];
  const char *_infoPtr[kInfoSlots];
  char _itemBuf[kMenuSlots][32];
  MenuItem _dynItems[kMenuSlots];

  // ---- 屏幕设置（NVS）----
  uint16_t _autoOffSec = cfg::kUiAutoOffDefaultSec;    // 0 = 禁用
  uint16_t _idleSec = cfg::kUiIdleReturnDefaultSec;    // 0 = 禁用
  bool _brightDirty = false;      // 亮度已改、待落盘（菜单连调时防抖）
  uint32_t _brightDirtyMs = 0;    // 亮度最后一次被改动的时刻（ms）

  // ---- 工具箱（检查更新 / 重启 / 恢复出厂）----
  // OTA 操作阶段：kIdle 空闲 / kCheck 已发起检查 / kFound 发现新版本（等用户确认）/
  // kUpgrade 已发起升级。与 Ota 内部状态机并行：本类只驱动「检查->等确认->升级」
  // 的链式前进，下载/烧录进度直接读 Ota::status() 展示，不自己维护。
  enum class ToolPhase : uint8_t { kIdle = 0, kCheck, kFound, kUpgrade };
  ToolPhase _toolPhase = ToolPhase::kIdle;
  String _toolMsg;                // 状态行附加说明（操作结论：已是最新/失败原因等）
  String _toolNewVer;             // 检查发现的新版本（待用户按 OK 确认后升级）
  uint32_t _toolMs = 0;           // 发起 OTA 操作的时刻（判断是否真正启动）
  bool _toolStarted = false;      // 是否已见到非 IDLE 状态（确认操作真正开始）
  uint8_t _resetCount = 0;        // 重置连按计数（满 kToolResetConfirm 次才执行）
  uint32_t _resetMs = 0;          // 上次按重置的时刻（超时未续按则清零）
  bool _resetPending = false;     // 已按满次数、倒计时等待执行（BACK 可取消）
  uint32_t _resetActMs = 0;       // 按满次数的时刻（倒计时起点）
  bool _restartPending = false;   // 重启倒计时中（BACK 可取消）
  uint32_t _restartMs = 0;        // 确认重启的时刻（倒计时起点）
  OtaCheckFn _otaCheckFn;         // 未注入时「检查新版本并更新」无响应
  OtaUpgradeFn _otaUpgradeFn;     // 未注入时检查到新版本也无法升级
  SysFn _restartFn;               // 未注入时重启项无响应
  SysFn _resetFn;                 // 未注入时重置项无响应
};
