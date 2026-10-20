#pragma once

#include <lvgl.h>

#include <Arduino.h>
#include <stdint.h>

#include "app/StatusInfo.h"

// LVGL 界面层：三个页面（实时状态 / 菜单 / 屏幕显示）都在这里用 LVGL 部件搭建，
// 业务侧（app/UIMenu）只负责「现在该显示什么数据」，不碰具体绘制。
//
// 页面切换用 lv_screen_load 换活动屏幕；三个屏幕在 begin() 时一次性建好，
// 之后只更新文本内容 —— LVGL 自带脏区计算，只把有变化的部分推到面板。
namespace ui {

// 菜单行：icon 可为空（纯信息行的场合）
struct MenuRow {
  const char *icon;   // 图标 UTF-8（见 UiIcons.h 的 UI_ICON_*）
  const char *label;  // 行文案
};

class Ui;
Ui &instance();

class Ui {
 public:
  static Ui &instance();

  // 建立三个页面的部件树。须在 Display::begin()（LVGL 端口就绪）之后调用
  void begin();

  // 部件树是否已建好
  bool ready() const { return _scrStatus != nullptr; }

  // 每轮调用：推进中文字库加载，就绪后自动刷新一次全部页面
  void tick();

  // ---- 实时状态页 ----
  // 切到状态页并按快照更新内容（内容无变化时不会触发重绘）
  void statusUpdate(const StatusInfo &s);

  // ---- 菜单页 ----
  // rows/n：选项；sel：高亮下标（-1 表示纯信息页，展示 info）；
  // info/infoCount：正文；foot：页脚（可为 nullptr）
  void menuUpdate(const char *title, const MenuRow *rows, uint8_t n, int8_t sel,
                  const char *const *info, uint8_t infoCount, const char *foot);

  // ---- 屏幕显示页（可滚动的状态快照）----
  // scroll：期望滚动偏移（行），返回夹紧后的实际偏移
  int screenViewUpdate(const StatusInfo &s, int scroll);

  // 屏幕显示页一屏能放几行（供“长按翻页”算步长）
  int screenViewVisibleLines() const;

 private:
  Ui() = default;   // 单例：一律通过 ui::instance() 取用
  friend Ui &instance();

  void buildStatus();
  void buildMenu();
  void buildScreenView();

  // 建立一个受管理的标签（登记后可在字库就绪时统一换字体）
  // kind：0 图标 / 1 正文 / 2 标题 / 3 ASCII；align：LV_TEXT_ALIGN_*
  lv_obj_t *newLabel(lv_obj_t *parent, uint8_t kind, uint32_t color, const char *text,
                     lv_text_align_t align);

  // 中文字库就绪后把所有标签的字体刷一遍，避免挂 UI 时用的是 ASCII 兜底字体
  void applyFonts();

  lv_obj_t *_scrStatus = nullptr;   // 实时状态页
  lv_obj_t *_scrMenu = nullptr;     // 菜单页
  lv_obj_t *_scrView = nullptr;     // 屏幕显示页

  // 状态页：标题栏时钟 + 7 个状态行的数值标签
  lv_obj_t *_stClock = nullptr;
  lv_obj_t *_stValue[7] = {};
  String _stCache[7];
  lv_obj_t *_atCmd = nullptr;    // 最近一条 AT 命令
  lv_obj_t *_atResp = nullptr;   // 最近一条 AT 响应
  String _atCache;               // AT 区内容指纹（变化才重设文本）

  // 菜单页：标题 + 固定数量的行槽位（用显隐切换，避免反复建/删对象）
  static constexpr uint8_t kMaxMenuRows = 8;
  static constexpr uint8_t kMaxInfoRows = 8;
  lv_obj_t *_mnTitle = nullptr;
  lv_obj_t *_mnRow[kMaxMenuRows] = {};
  lv_obj_t *_mnIcon[kMaxMenuRows] = {};
  lv_obj_t *_mnLabel[kMaxMenuRows] = {};
  lv_obj_t *_mnInfo[kMaxInfoRows] = {};
  lv_obj_t *_mnFoot = nullptr;
  String _mnCache;               // 上次渲染的内容指纹（相同则跳过整页更新）

  // 屏幕显示页：定高容器 + 单个可偏移的长文本
  lv_obj_t *_svBody = nullptr;
  lv_obj_t *_svText = nullptr;
  lv_obj_t *_svFoot = nullptr;

  // 标签登记表（固定容量：UI 文本数量有上界，借此不引入动态容器）
  static constexpr uint8_t kMaxLabels = 96;
  struct LabelRef {
    lv_obj_t *obj;
    uint8_t kind;
  };
  LabelRef _labels[kMaxLabels];
  uint8_t _labelCount = 0;
  uint8_t _current = 0;   // 当前活动页：0 状态 / 1 菜单 / 2 屏幕显示
  bool _built = false;    // begin() 是否已装配完成（部件只建一次）
};

Ui &instance();

}  // namespace ui
