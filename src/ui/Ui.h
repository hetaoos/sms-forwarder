#pragma once

#include <lvgl.h>

#include <Arduino.h>
#include <stdint.h>

#include "app/StatusInfo.h"

// LVGL 界面层：状态页 / 菜单页 / 可滚动文本页 / 新短信弹窗 / 开机页都在这里
// 用 LVGL 部件搭建，业务侧（app/UIMenu）只负责「现在该显示什么数据」，不碰绘制。
//
// 页面切换用 lv_screen_load 换活动屏幕；五个屏幕在 begin() 时一次性建好，
// 之后只更新文本内容 —— LVGL 自带脏区计算，只把有变化的部分推到面板。
namespace ui {

// 菜单行：icon 可为空（纯信息行的场合）
struct MenuRow {
  const char *icon;   // 图标 UTF-8（见 UiIcons.h 的 UI_ICON_*）
  const char *label;  // 行文案
  bool scroll = false;  // 行文案过长时横向滚动（跑马灯）而非省略号截断
};

class Ui;
Ui &instance();

class Ui {
 public:
  static Ui &instance();

  // 建立各页面的部件树。须在 Display::begin()（LVGL 端口就绪）之后调用
  void begin();

  // 部件树是否已建好
  bool ready() const { return _scrStatus != nullptr; }

  // 每轮调用：推进中文字库加载，就绪后自动刷新一次全部页面
  void tick();

  // 菜单一屏能放几行 / 几行正文（供业务侧裁剪内容与算翻页步长）
  static uint8_t menuSlots();
  static uint8_t infoSlots();

  // ---- 开机页 ----
  // 切到开机页并按 0~100 推进进度条。开机页不依赖中文字库（纯图标 + ASCII），
  // 因此可以在字库加载、通信模组初始化期间安全显示。
  void bootUpdate(uint8_t percent);

  // ---- 实时状态页 ----
  // 切到状态页并按快照更新内容（内容无变化时不会触发重绘）
  void statusUpdate(const StatusInfo &s);

  // ---- 菜单页 ----
  // rows/n：选项（可多于可见行数，区域自带滚动，高亮行会自动滚入视野）；
  // sel：高亮下标（-1 表示纯信息页，展示 info）；
  // info/infoCount：正文（过长自动换行 + 随区域滚动）；foot：页脚（可为 nullptr）；
  // barPct：底部进度条取值（0-100，负数 = 不显示进度条）；
  // barText：进度条右侧的数值文本（如 "80%" / "60秒"，可为 nullptr）
  void menuUpdate(const char *title, const MenuRow *rows, uint8_t n, int8_t sel,
                  const char *const *info, uint8_t infoCount, const char *foot,
                  int barPct = -1, const char *barText = nullptr);

  // 菜单正文区滚动（纯信息页用：dy > 0 向下看更多内容）；返回一行的像素高
  void menuScrollBy(int dy);
  // 一行 / 一页的滚动步长（像素），供业务侧换算上下键与长按翻页
  int menuScrollStep() const;
  int menuScrollPage() const;

  // ---- 可滚动文本页（屏幕显示 / 短信详情）----
  // title：标题栏文案；text：正文（可含换行）；scroll：期望滚动偏移（行）
  // 返回夹紧后的实际偏移
  int textViewUpdate(const char *title, const char *text, int scroll);

  // 文本页一屏能放几行（供「长按翻页」算步长）
  int viewVisibleLines() const;

  // ---- 新短信弹窗 ----
  // phone：发件号码；body：正文；modem：接收通信模组串口号（<0 未知）；
  // time：可读时间串；incoming：true 收到 / false 发出
  void smsPopupUpdate(const char *phone, const char *body, int8_t modem,
                      const char *time, bool incoming);

 private:
  Ui() = default;   // 单例：一律通过 ui::instance() 取用
  friend Ui &instance();

  void buildStatus();
  void buildMenu();
  void buildView();
  void buildPopup();
  void buildBoot();

  // 建立一个受管理的标签（登记后可在字库就绪时统一换字体）
  // kind：0 图标 / 1 正文 / 2 标题 / 3 ASCII；align：LV_TEXT_ALIGN_*
  lv_obj_t *newLabel(lv_obj_t *parent, uint8_t kind, uint32_t color, const char *text,
                     lv_text_align_t align);

  // 建立标题栏内的标签：登记进 _hdr*，由 layoutHeader() 统一按基线对齐
  lv_obj_t *newHeaderLabel(lv_obj_t *parent, uint8_t kind, uint32_t color,
                           const char *text, lv_text_align_t align);

  // 标题栏按基线对齐：LVGL 只居中「行盒」，而各字号字体在行盒内的上下留白
  // 不同，直接居中会让标题与时钟一高一低。这里以标题字体的行盒为基准竖直居中，
  // 算出一条公共基线，其余标签按各自字体基线摆到这条线上。
  // 字体切换（中文字库就绪）后必须重算，故在 applyFonts() 里也调一次。
  void layoutHeader();

  // 中文字库就绪后把所有标签的字体刷一遍，避免挂 UI 时用的是 ASCII 兜底字体
  void applyFonts();

  lv_obj_t *_scrStatus = nullptr;   // 实时状态页
  lv_obj_t *_scrMenu = nullptr;     // 菜单页
  lv_obj_t *_scrView = nullptr;     // 可滚动文本页
  lv_obj_t *_scrPopup = nullptr;    // 新短信弹窗
  lv_obj_t *_scrBoot = nullptr;     // 开机页（Logo + 进度条）
  lv_obj_t *_bootBar = nullptr;

  // 状态页：标题栏时钟 + 13 个状态行（其中 2 个按模式互斥显隐，实际同时可见 12 行）
  static constexpr uint8_t kStatusRows = 13;
  lv_obj_t *_stClock = nullptr;
  lv_obj_t *_stRow[kStatusRows] = {};      // 行容器（AP 模式下切换显隐）
  lv_obj_t *_stValue[kStatusRows] = {};    // 各行右对齐的值
  String _stCache[kStatusRows];            // 上次的值（变化才重设文本）
  bool _stHidden[kStatusRows] = {};        // 上一次的显隐状态（变化才改）

  // 菜单页：标题 + 可滚动正文区（列容器）。
  // 选项与正文都放进这个可竖向滚动的列容器：
  //   - 条目多于一屏可以滚（高亮行自动滚入视野，业务侧无需再算视口）；
  //   - 正文过长自动换行并整体滚动，不再靠写死坐标截断。
  // 槽位仍是固定数量（避免反复建/删部件），隐藏的部件不参与 flex 布局。
  static constexpr uint8_t kMaxMenuRows = 12;
  static constexpr uint8_t kMaxInfoRows = 20;
  lv_obj_t *_mnTitle = nullptr;
  lv_obj_t *_mnBody = nullptr;
  lv_obj_t *_mnRow[kMaxMenuRows] = {};
  lv_obj_t *_mnIcon[kMaxMenuRows] = {};
  lv_obj_t *_mnLabel[kMaxMenuRows] = {};
  lv_obj_t *_mnInfo[kMaxInfoRows] = {};
  lv_obj_t *_mnFoot = nullptr;
  String _mnTitlePrev;          // 上次渲染的标题（变了就说明换了页，滚动回顶）
  lv_obj_t *_mnBar = nullptr;      // 底部进度条（调节类页面显示）
  lv_obj_t *_mnBarVal = nullptr;   // 进度条右侧的数值文本
  String _mnCache;               // 上次渲染的内容指纹（相同则跳过整页更新）

  // 可滚动文本页：定高容器 + 单个可偏移的长文本
  lv_obj_t *_svTitle = nullptr;
  lv_obj_t *_svBody = nullptr;
  lv_obj_t *_svText = nullptr;
  lv_obj_t *_svFoot = nullptr;
  String _svCache;               // 上次渲染的「标题 + 正文」（相同则跳过重排）

  // 新短信弹窗：号码 / 元信息 / 正文 / 页脚
  lv_obj_t *_popPhone = nullptr;
  lv_obj_t *_popMeta = nullptr;
  lv_obj_t *_popBody = nullptr;
  String _popCache;              // 上次渲染的弹窗指纹（相同则跳过）

  // 标签登记表（固定容量：UI 文本数量有上界，借此不引入动态容器）
  static constexpr uint8_t kMaxLabels = 128;
  struct LabelRef {
    lv_obj_t *obj;
    uint8_t kind;
  };
  LabelRef _labels[kMaxLabels];
  uint8_t _labelCount = 0;
  // 标题栏标签登记表（layoutHeader 需要按字号逐个算基线偏移）
  static constexpr uint8_t kMaxHeaderLabels = 16;
  LabelRef _hdrLabels[kMaxHeaderLabels];
  uint8_t _hdrCount = 0;
  // 当前活动页：0 状态 / 1 菜单 / 2 文本 / 3 开机 / 4 弹窗
  uint8_t _current = 0;
  bool _built = false;    // begin() 是否已装配完成（部件只建一次）
};

Ui &instance();

}  // namespace ui
