#include "ui/Ui.h"

#include <Arduino.h>

#include "app/Config.h"
#include "app/Psram.h"   // PSRAM 占用统计（开机自查 PSRAM 去向）
#include "log/LogStore.h"
#include "ui/UiFonts.h"
#include "ui/UiIcons.h"

// 三个页面的布局是固定分辨率（240x320）下的绝对排版：
//   标题栏 34px -> 状态行 7x28 -> AT 区 54px -> 页脚 20px
// 页面内部的横向排列全部交给 flex，省去手工居中；文字竖直方向由 flex 的
// ALIGN_CENTER 统一处理，字库加载后行高变化也不需要调整坐标。
namespace ui {

namespace {

LogStore &_log = devLog();

// ---------- 配色 ----------
constexpr uint32_t C_BG = 0x090D13;      // 页面底色
constexpr uint32_t C_HEADER = 0x132A44;  // 标题栏底色
constexpr uint32_t C_SEL = 0x1C3A5E;     // 菜单选中行底色
constexpr uint32_t C_LINE = 0x1F2A38;    // 分隔线
constexpr uint32_t C_TEXT = 0xE8EEF6;    // 主文字
constexpr uint32_t C_LABEL = 0x8A9AB0;   // 次要文字 / 未选中行
constexpr uint32_t C_HINT = 0x5C6878;    // 页脚提示
constexpr uint32_t C_ACCENT = 0x4DA3FF;  // 强调色（图标 / AP 模式）
constexpr uint32_t C_OK = 0x3AD97A;      // 正常
constexpr uint32_t C_ERR = 0xE5544B;     // 异常

// ---------- 版式 ----------
// 屏宽取自 LVGL（已经过旋转换算），避免把 240 写死在多处
lv_coord_t kScreenW = 240;

constexpr lv_coord_t kPad = 8;                 // 左右边距
constexpr lv_coord_t kHeaderH = 34;            // 标题栏高度
constexpr lv_coord_t kRowTop = kHeaderH + 4;   // 状态区首行 Y：38
// 行高取 23：图标字体（20px）行高就是 23，再矮会把图标裁掉；
// 12 行同时可见 = 276px（38~314），屏底留 6px
constexpr lv_coord_t kRowH = 23;
constexpr lv_coord_t kMenuTop = kHeaderH + 6;  // 菜单首行 Y：40
constexpr lv_coord_t kMenuRowH = 30;           // 菜单行高
constexpr lv_coord_t kInfoRowH = 24;           // 正文行距（固定坐标，故正文不折行）
constexpr lv_coord_t kBarTopMax = 284;         // 底部进度条的 Y 上限（不让位给页脚）
constexpr lv_coord_t kViewTop = kHeaderH + 4;  // 屏幕显示区 Y：38
constexpr lv_coord_t kViewH = 254;             // 屏幕显示区高度（38~292）
constexpr lv_coord_t kFootTop = 294;           // 菜单/屏幕显示页的页脚行
constexpr lv_coord_t kLabelW = 48;             // 状态行标签列宽
constexpr lv_coord_t kGap = 4;                 // 行内元素间距

// 状态行定义（左侧图标 + 标签，值在右侧右对齐）。
// 顺序即屏幕自上而下的顺序；kRowSsid/kRowPass 见下方「按模式互斥」说明。
enum : uint8_t {
  kRowWifi = 0,
  kRowPass,    // AP 热点密码：仅 AP 模式显示
  kRowIp,
  kRowRssi,    // STA 信号：AP 模式下无意义，隐藏
  kRowMac,
  kRowUptime,
  kRowMem,
  kRowLoad,
  kRowModem1,
  kRowModem2,
  kRowMqtt,
  kRowMail,
  kRowPush,
  kStatusRowCnt
};

struct RowDef {
  const char *icon;
  const char *label;
};
constexpr RowDef kStatusRowDef[kStatusRowCnt] = {
    {UI_ICON_WIFI, "WIFI"},        // 已连：SSID；AP：热点名
    {UI_ICON_INFO, "密码"},        // AP 时才显示（开放热点）
    {UI_ICON_GLOBE, "IP"},
    {UI_ICON_SIGNAL, "RSSI"},
    {UI_ICON_MAC, "MAC"},
    {UI_ICON_CLOCK, "运行"},        // 运行时长（已格式化）
    {UI_ICON_MEMORY, "内存"},       // 内部堆剩余 / PSRAM 剩余
    {UI_ICON_CPU, "负载"},          // CPU 使用率 / 芯片温度
    {UI_ICON_SIM, "模组1"},
    {UI_ICON_SIM, "模组2"},
    {UI_ICON_MQTT, "MQTT"},
    {UI_ICON_MAIL, "邮件"},
    {UI_ICON_PUSH, "推送"},
};

// 按 kind 取字体（ui::UiFonts 负责实现细节）
//   0 图标 / 1 正文 / 2 标题 / 3 ASCII / 4 开机页 Logo
const lv_font_t *fontFor(uint8_t kind) {
  switch (kind) {
    case 0: return fontIcon();
    case 2: return fontTitle();
    case 3: return fontAscii();
    case 4: return fontLogo();
    default: return fontText();
  }
}

// 铺一块横向排布的行容器（不含背景色，由调用方按需补）
lv_obj_t *makeRow(lv_obj_t *parent, lv_coord_t h, lv_coord_t pad) {
  lv_obj_t *o = lv_obj_create(parent);
  lv_obj_remove_style_all(o);
  lv_obj_set_scrollable(o, false);
  lv_obj_set_width(o, LV_PCT(100));
  lv_obj_set_height(o, h);
  lv_obj_set_flex_flow(o, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(o, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START);
  lv_obj_set_style_pad_left(o, pad, 0);
  lv_obj_set_style_pad_right(o, pad, 0);
  lv_obj_set_style_pad_gap(o, 6, 0);
  return o;
}

// 标题栏：图标 + 标题 + 右侧时钟/附加文本（返回标题栏容器）
lv_obj_t *makeHeader(lv_obj_t *parent) {
  lv_obj_t *h = makeRow(parent, kHeaderH, kPad);
  lv_obj_set_pos(h, 0, 0);
  lv_obj_set_style_bg_color(h, lv_color_hex(C_HEADER), 0);
  lv_obj_set_style_bg_opa(h, LV_OPA_COVER, 0);
  return h;
}

}  // namespace

Ui &instance() {
  static Ui ui;
  return ui;
}

// 菜单槽位 / 正文槽位：业务侧（UIMenu）按这两个上限裁剪内容，
// 屏幕尺寸若调整，只改这里的常量与 Ui.h 的 kMaxMenuRows / kMaxInfoRows 即可
uint8_t Ui::menuSlots() { return kMaxMenuRows; }
uint8_t Ui::infoSlots() { return kMaxInfoRows; }

// ---------------------------------------------------------------------------
// 标签管理
// ---------------------------------------------------------------------------

lv_obj_t *Ui::newLabel(lv_obj_t *parent, uint8_t kind, uint32_t color, const char *text,
                       lv_text_align_t align) {
  if (_labelCount >= kMaxLabels) {
    _log.addf(LogLevel::kWarn, "ui", "标签数超出登记上限 %u，忽略新增", (unsigned)kMaxLabels);
    return nullptr;
  }
  lv_obj_t *l = lv_label_create(parent);
  lv_obj_remove_style_all(l);
  // 注意：remove_style_all 连类默认的宽高样式也一起清掉了，而样式缺失时高度解析不到，
  // flex 就会按 0 高度去算交叉轴居中 -> 文字整体下偏（label 只在自己盒子的顶部起画，
  // 不会自动垂直居中）。这里必须显式设回 LV_SIZE_CONTENT，让标签高度等于真实文本
  // 高度，居中才成立；多行的（AT 命令/响应、信息页）也会随行数自动长高。
  lv_obj_set_width(l, LV_SIZE_CONTENT);
  lv_obj_set_height(l, LV_SIZE_CONTENT);
  lv_obj_set_scrollable(l, false);
  lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
  lv_obj_set_style_text_align(l, align, 0);
  lv_obj_set_style_text_font(l, fontFor(kind), 0);
  lv_label_set_text(l, text && *text ? text : "");
  _labels[_labelCount].obj = l;
  _labels[_labelCount].kind = kind;
  _labelCount++;
  return l;
}

void Ui::applyFonts() {
  for (uint8_t i = 0; i < _labelCount; i++) {
    LabelRef &r = _labels[i];
    if (!r.obj) continue;
    lv_obj_set_style_text_font(r.obj, fontFor(r.kind), 0);
    lv_obj_invalidate(r.obj);   // 强制重画，让新字体立即生效
  }
  // 此刻字形缓存已随首屏文本逐个建立起来（同样落在 PSRAM），这里把账目打明白：
  // 渲染缓冲 + TTF 正文 + 字形缓存的总占用一目了然，便于判断是否需要压缩 cache
  _log.addf(LogLevel::kInfo, "ui",
            "中文字库已接管 %u 个文本部件；PSRAM 已用 %u KB / 共 %u KB（剩余最大块 %u KB）",
            (unsigned)_labelCount, (unsigned)(psramUsedBytes() / 1024),
            (unsigned)(psramTotalBytes() / 1024), (unsigned)(psramLargestFreeBlock() / 1024));
}

// ---------------------------------------------------------------------------
// 页面装配
// ---------------------------------------------------------------------------

void Ui::begin() {
  if (_built) return;
  kScreenW = (lv_coord_t)lv_display_get_horizontal_resolution(NULL);
  if (kScreenW <= 0) kScreenW = cfg::kTftWidth;
  buildStatus();
  buildMenu();
  buildView();
  buildPopup();
  buildBoot();
  // 开机先落在开机页：此时中文字库与各业务模块都还没就绪，状态页还不能取数
  lv_screen_load(_scrBoot);
  _current = 3;
  fontsBegin();      // 中文字库走延迟加载（等 LittleFS 挂载）
  _built = true;
}

// ---- 开机页 ----
// 纯图标 + ASCII：不等中文字库，也不读任何业务模块，因此可以在初始化期间安全显示。
// 布局：居中 Logo（48px 图标）+ 产品名 + 进度条 + 一行提示。
void Ui::buildBoot() {
  _scrBoot = lv_obj_create(NULL);
  lv_obj_remove_style_all(_scrBoot);
  lv_obj_set_scrollable(_scrBoot, false);
  lv_obj_set_style_bg_color(_scrBoot, lv_color_hex(C_BG), 0);
  lv_obj_set_style_bg_opa(_scrBoot, LV_OPA_COVER, 0);

  // 整块内容（Logo + 产品名 + 进度条 + 提示）在 320 高的屏上大致垂直居中
  lv_obj_t *logo = newLabel(_scrBoot, 4, C_ACCENT, UI_ICON_SMS, LV_TEXT_ALIGN_CENTER);
  lv_obj_set_pos(logo, 0, 102);
  lv_obj_set_width(logo, kScreenW);
  lv_obj_set_height(logo, 49);   // Logo 字体行高

  lv_obj_t *ttl = newLabel(_scrBoot, 3, C_TEXT, "SMS Forwarder", LV_TEXT_ALIGN_CENTER);
  lv_obj_set_pos(ttl, 0, 164);
  lv_obj_set_width(ttl, kScreenW);

  // 进度条：底槽 + 指示块，宽度由 bootUpdate 按百分比推进
  constexpr lv_coord_t barW = 140, barH = 6;
  lv_obj_t *bar = lv_bar_create(_scrBoot);
  lv_obj_remove_style_all(bar);
  lv_obj_set_pos(bar, (kScreenW - barW) / 2, 192);
  lv_obj_set_size(bar, barW, barH);
  lv_obj_set_style_bg_color(bar, lv_color_hex(C_LINE), 0);
  lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
  lv_obj_set_style_radius(bar, 3, 0);
  lv_obj_set_style_bg_color(bar, lv_color_hex(C_ACCENT), LV_PART_INDICATOR);
  lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_INDICATOR);
  lv_obj_set_style_radius(bar, 3, LV_PART_INDICATOR);
  lv_bar_set_range(bar, 0, 100);
  lv_bar_set_value(bar, 0, LV_ANIM_OFF);
  _bootBar = bar;

  lv_obj_t *hint = newLabel(_scrBoot, 3, C_HINT, "Starting...", LV_TEXT_ALIGN_CENTER);
  lv_obj_set_pos(hint, 0, 208);
  lv_obj_set_width(hint, kScreenW);
}

// 切到开机页并推进进度条（App 在开机窗口内每 ~100ms 调一次）
void Ui::bootUpdate(uint8_t percent) {
  if (!_scrBoot) return;
  if (_current != 3) {
    lv_screen_load(_scrBoot);
    _current = 3;
  }
  if (_bootBar) lv_bar_set_value(_bootBar, percent, LV_ANIM_OFF);
}

// ---- 实时状态页 ----
void Ui::buildStatus() {
  // 行定义与成员数组容量必须对齐（改行定义时忘记同步会越界写成员数组）
  static_assert(kStatusRowCnt == kStatusRows, "状态行定义与槽位数不一致");

  _scrStatus = lv_obj_create(NULL);
  lv_obj_remove_style_all(_scrStatus);
  lv_obj_set_scrollable(_scrStatus, false);
  lv_obj_set_style_bg_color(_scrStatus, lv_color_hex(C_BG), 0);
  lv_obj_set_style_bg_opa(_scrStatus, LV_OPA_COVER, 0);

  // 标题栏：图标 + 产品名 + 右侧时钟
  lv_obj_t *hdr = makeHeader(_scrStatus);
  newLabel(hdr, 0, C_ACCENT, UI_ICON_SMS, LV_TEXT_ALIGN_LEFT);
  lv_obj_t *ttl = newLabel(hdr, 2, C_TEXT, "短信转发器", LV_TEXT_ALIGN_LEFT);
  lv_obj_set_flex_grow(ttl, 1);
  _stClock = newLabel(hdr, 1, C_TEXT, "--:--:--", LV_TEXT_ALIGN_RIGHT);

  // 状态行：图标 / 标签在左侧，值右对齐。
  // 用 flex 列容器承载：隐藏某行（AP 模式下隐藏 RSSI）后续行会自动上移，
  // 不需要重新计算坐标。
  lv_obj_t *body = lv_obj_create(_scrStatus);
  lv_obj_remove_style_all(body);
  lv_obj_set_scrollable(body, false);
  lv_obj_set_pos(body, 0, kRowTop);
  lv_obj_set_size(body, LV_PCT(100), (kStatusRowCnt - 1) * kRowH);
  lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);

  for (uint8_t i = 0; i < kStatusRowCnt; i++) {
    lv_obj_t *row = makeRow(body, kRowH, kPad);
    lv_obj_set_style_pad_gap(row, kGap, 0);
    lv_obj_set_style_border_side(row, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_width(row, 1, 0);
    lv_obj_set_style_border_color(row, lv_color_hex(C_LINE), 0);
    _stRow[i] = row;
    newLabel(row, 0, C_LABEL, kStatusRowDef[i].icon, LV_TEXT_ALIGN_LEFT);
    lv_obj_t *nm = newLabel(row, 1, C_LABEL, kStatusRowDef[i].label, LV_TEXT_ALIGN_LEFT);
    lv_obj_set_width(nm, kLabelW);
    // 值由 statusUpdate 填充，这里先留空，保证首帧一定被判定为“有变化”；
    // 超长值（SSID / MAC / 运营商）以省略号截断，不撑破行
    _stValue[i] = newLabel(row, 1, C_TEXT, "", LV_TEXT_ALIGN_RIGHT);
    lv_obj_set_flex_grow(_stValue[i], 1);
    lv_label_set_long_mode(_stValue[i], LV_LABEL_LONG_MODE_DOTS);
  }
}

// ---- 菜单页 ----
void Ui::buildMenu() {
  _scrMenu = lv_obj_create(NULL);
  lv_obj_remove_style_all(_scrMenu);
  lv_obj_set_scrollable(_scrMenu, false);
  lv_obj_set_style_bg_color(_scrMenu, lv_color_hex(C_BG), 0);
  lv_obj_set_style_bg_opa(_scrMenu, LV_OPA_COVER, 0);

  lv_obj_t *hdr = makeHeader(_scrMenu);
  newLabel(hdr, 0, C_ACCENT, UI_ICON_MENU, LV_TEXT_ALIGN_LEFT);
  _mnTitle = newLabel(hdr, 2, C_TEXT, "", LV_TEXT_ALIGN_LEFT);
  lv_obj_set_flex_grow(_mnTitle, 1);

  // 选项槽位：固定数量，靠显隐 + 改文本复用，避免反复创建/销毁部件
  for (uint8_t i = 0; i < kMaxMenuRows; i++) {
    lv_obj_t *row = makeRow(_scrMenu, kMenuRowH, kPad);
    // 整行铺满宽度（高亮底色也跟着通栏），文字由 makeRow 的内边距缩进
    lv_obj_set_pos(row, 0, kMenuTop + i * kMenuRowH);
    lv_obj_set_style_radius(row, 6, 0);
    _mnRow[i] = row;
    _mnIcon[i] = newLabel(row, 0, C_LABEL, "", LV_TEXT_ALIGN_LEFT);
    lv_obj_t *lb = newLabel(row, 1, C_LABEL, "", LV_TEXT_ALIGN_LEFT);
    lv_obj_set_flex_grow(lb, 1);
    _mnLabel[i] = lb;
    lv_obj_set_hidden(row, true);
  }

  // 正文槽位：与选项互斥（信息页用它）。
  // 行距固定 24px，因此正文一律用 DOTS 截断而不是 WRAP——一旦某行折行就会
  // 压到下一行上（槽位坐标是写死的），宁可截断也不叠字。
  for (uint8_t i = 0; i < kMaxInfoRows; i++) {
    lv_obj_t *lb = newLabel(_scrMenu, 1, C_TEXT, "", LV_TEXT_ALIGN_LEFT);
    lv_obj_set_pos(lb, kPad, kMenuTop + i * kInfoRowH);
    lv_obj_set_width(lb, kScreenW - kPad * 2);
    lv_label_set_long_mode(lb, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_hidden(lb, true);
    _mnInfo[i] = lb;
  }

  // 底部进度条：调节类页面（亮度 / 超时）用它直观显示当前值。
  // 位置紧跟在选项区下方，右侧留一列显示数值文本；不显示时整组隐藏。
  constexpr lv_coord_t barW = 150, barH = 8;
  lv_obj_t *bar = lv_bar_create(_scrMenu);
  lv_obj_remove_style_all(bar);
  lv_obj_set_pos(bar, kPad, kBarTopMax);
  lv_obj_set_size(bar, barW, barH);
  lv_obj_set_style_bg_color(bar, lv_color_hex(C_LINE), 0);
  lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
  lv_obj_set_style_radius(bar, 4, 0);
  lv_obj_set_style_bg_color(bar, lv_color_hex(C_ACCENT), LV_PART_INDICATOR);
  lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_INDICATOR);
  lv_obj_set_style_radius(bar, 4, LV_PART_INDICATOR);
  lv_bar_set_range(bar, 0, 100);
  lv_bar_set_value(bar, 0, LV_ANIM_OFF);
  lv_obj_set_hidden(bar, true);
  _mnBar = bar;

  _mnBarVal = newLabel(_scrMenu, 1, C_TEXT, "", LV_TEXT_ALIGN_RIGHT);
  lv_obj_set_pos(_mnBarVal, kPad + barW + kGap, kBarTopMax - 4);
  lv_obj_set_width(_mnBarVal, kScreenW - kPad * 2 - barW - kGap);
  lv_label_set_long_mode(_mnBarVal, LV_LABEL_LONG_MODE_DOTS);
  lv_obj_set_hidden(_mnBarVal, true);

  // 页脚文案含中文（“OK进入 / 亮度”），不能挂只有英文字形的 ASCII 兜底字体，
  // 否则汉字会画成占位方块；中文字库未就绪时 fontText() 自动退回 ASCII 字体
  _mnFoot = newLabel(_scrMenu, 1, C_HINT, "", LV_TEXT_ALIGN_LEFT);
  lv_obj_set_pos(_mnFoot, kPad, kFootTop + 2);
  lv_obj_set_width(_mnFoot, kScreenW - kPad * 2);
  lv_label_set_long_mode(_mnFoot, LV_LABEL_LONG_MODE_DOTS);
}

// ---- 可滚动文本页（屏幕显示 / 短信详情）----
// 与旧「屏幕显示」页结构一致，只是标题栏文案改为可写，好让短信详情复用同一屏。
void Ui::buildView() {
  _scrView = lv_obj_create(NULL);
  lv_obj_remove_style_all(_scrView);
  lv_obj_set_scrollable(_scrView, false);
  lv_obj_set_style_bg_color(_scrView, lv_color_hex(C_BG), 0);
  lv_obj_set_style_bg_opa(_scrView, LV_OPA_COVER, 0);

  lv_obj_t *hdr = makeHeader(_scrView);
  newLabel(hdr, 0, C_ACCENT, UI_ICON_DISPLAY, LV_TEXT_ALIGN_LEFT);
  _svTitle = newLabel(hdr, 2, C_TEXT, "", LV_TEXT_ALIGN_LEFT);
  lv_obj_set_flex_grow(_svTitle, 1);

  // 定高容器 + 一个可整体上下偏移的长文本，实现「按行滚动」
  _svBody = lv_obj_create(_scrView);
  lv_obj_remove_style_all(_svBody);
  lv_obj_set_scrollable(_svBody, false);
  lv_obj_set_pos(_svBody, kPad, kViewTop);
  lv_obj_set_size(_svBody, kScreenW - kPad * 2, kViewH);
  lv_obj_set_style_pad_all(_svBody, 0, 0);

  // 多行文本放在容器里，超出部分的渲染由 LVGL 自动裁剪到容器范围
  _svText = newLabel(_svBody, 1, C_TEXT, "", LV_TEXT_ALIGN_LEFT);
  lv_obj_set_width(_svText, LV_PCT(100));

  _svFoot = newLabel(_scrView, 1, C_HINT, "", LV_TEXT_ALIGN_LEFT);
  lv_obj_set_pos(_svFoot, kPad, kFootTop + 2);
  lv_obj_set_width(_svFoot, kScreenW - kPad * 2);
  lv_label_set_long_mode(_svFoot, LV_LABEL_LONG_MODE_DOTS);
}

// ---- 新短信弹窗 ----
// 收到短信时自动浮到屏上：号码用标题字号突出，下面是时间与来源模组，
// 正文区定高自动换行，页脚提示「按任意键返回」。
void Ui::buildPopup() {
  _scrPopup = lv_obj_create(NULL);
  lv_obj_remove_style_all(_scrPopup);
  lv_obj_set_scrollable(_scrPopup, false);
  lv_obj_set_style_bg_color(_scrPopup, lv_color_hex(C_BG), 0);
  lv_obj_set_style_bg_opa(_scrPopup, LV_OPA_COVER, 0);

  lv_obj_t *hdr = makeHeader(_scrPopup);
  newLabel(hdr, 0, C_ACCENT, UI_ICON_SMS, LV_TEXT_ALIGN_LEFT);
  lv_obj_t *ttl = newLabel(hdr, 2, C_TEXT, "新短信", LV_TEXT_ALIGN_LEFT);
  lv_obj_set_flex_grow(ttl, 1);

  // 号码（标题字号，整行放不下就截断）
  _popPhone = newLabel(_scrPopup, 2, C_TEXT, "", LV_TEXT_ALIGN_LEFT);
  lv_obj_set_pos(_popPhone, kPad, 42);
  lv_obj_set_width(_popPhone, kScreenW - kPad * 2);
  lv_label_set_long_mode(_popPhone, LV_LABEL_LONG_MODE_DOTS);

  // 元信息：方向 + 时间 + 来源模组
  _popMeta = newLabel(_scrPopup, 1, C_LABEL, "", LV_TEXT_ALIGN_LEFT);
  lv_obj_set_pos(_popMeta, kPad, 70);
  lv_obj_set_width(_popMeta, kScreenW - kPad * 2);
  lv_label_set_long_mode(_popMeta, LV_LABEL_LONG_MODE_DOTS);

  // 正文：定高容器 + 自动换行，超出部分由容器裁剪
  lv_obj_t *body = lv_obj_create(_scrPopup);
  lv_obj_remove_style_all(body);
  lv_obj_set_scrollable(body, false);
  lv_obj_set_pos(body, kPad, 94);
  lv_obj_set_size(body, kScreenW - kPad * 2, 190);
  lv_obj_set_style_pad_all(body, 0, 0);
  _popBody = newLabel(body, 1, C_TEXT, "", LV_TEXT_ALIGN_LEFT);
  lv_obj_set_width(_popBody, LV_PCT(100));
  lv_label_set_long_mode(_popBody, LV_LABEL_LONG_MODE_WRAP);

  lv_obj_t *foot = newLabel(_scrPopup, 1, C_HINT, "按任意键返回", LV_TEXT_ALIGN_CENTER);
  lv_obj_set_pos(foot, 0, kFootTop + 2);
  lv_obj_set_width(foot, kScreenW);
}

// ---------------------------------------------------------------------------
// 页面更新
// ---------------------------------------------------------------------------

void Ui::tick() {
  fontsTick();
  if (fontsTakeReadyFlag()) applyFonts();   // 字库从 ASCII 兜底切到中文后全量刷新
}

void Ui::statusUpdate(const StatusInfo &s) {
  if (!_scrStatus) return;
  if (_current != 0) {
    lv_screen_load(_scrStatus);
    _current = 0;
  }
  if (!_built) return;   // 尚未装配（Display 初始化失败时不会调用到这里）

  // 标题栏时钟（整秒才变，这里靠文本比对跳过无效重绘）
  String t = s.time && *s.time ? s.time : "--:--:--";
  if (t != lv_label_get_text(_stClock)) lv_label_set_text(_stClock, t.c_str());

  String v[kStatusRows];
  uint32_t c[kStatusRows];
  for (uint8_t i = 0; i < kStatusRows; i++) c[i] = C_TEXT;

  // WIFI：已连显示 SSID，AP 模式显示热点名，未连显示连接状态
  if (s.wifiUp) {
    v[kRowWifi] = String("已连 ") + (s.ssid ? s.ssid : "-");
    c[kRowWifi] = C_OK;
  } else if (s.apMode) {
    v[kRowWifi] = String("AP ") + (s.ssid ? s.ssid : "-");
    c[kRowWifi] = C_ACCENT;
  } else {
    v[kRowWifi] = "未连接";
    c[kRowWifi] = C_ERR;
  }
  v[kRowPass] = s.apPass ? s.apPass : "-";
  v[kRowIp] = s.ip ? s.ip : "-";
  v[kRowRssi] = s.wifiUp ? String(s.rssi) + " dBm" : "-";
  v[kRowMac] = s.mac;
  v[kRowUptime] = s.uptime;
  v[kRowMem] = s.mem;
  v[kRowLoad] = s.load;
  v[kRowModem1] = s.modem[0];
  v[kRowModem2] = s.modem[1];
  v[kRowMqtt] = s.mqtt;
  v[kRowMail] = s.mail;
  v[kRowPush] = s.push;
  // 服务三行按状态着色：已就绪绿、已关闭灰、中间态用普通色
  for (uint8_t i = kRowMqtt; i <= kRowPush; i++) {
    if (v[i] == "已连接" || v[i] == "已配置" || v[i].endsWith("个通道")) c[i] = C_OK;
    else if (v[i] == "已关闭") c[i] = C_HINT;
  }
  // 模组行：就绪绿、故障红，其余中间态用普通色
  for (uint8_t i = kRowModem1; i <= kRowModem2; i++) {
    if (v[i].startsWith("就绪")) c[i] = C_OK;
    else if (v[i] == "故障") c[i] = C_ERR;
  }

  // 按模式互斥：AP 模式显示「密码」隐藏「RSSI」，反之亦然（两者只显示一个，
  // 保证始终 12 行、不会挤出屏幕）
  const bool hide[2] = {!s.apMode, s.apMode};   // 密码行、RSSI 行是否隐藏
  const uint8_t toggle[2] = {kRowPass, kRowRssi};
  for (uint8_t k = 0; k < 2; k++) {
    const uint8_t i = toggle[k];
    if (_stRow[i] && _stHidden[i] != hide[k]) {
      _stHidden[i] = hide[k];
      lv_obj_set_hidden(_stRow[i], hide[k]);
    }
  }

  for (uint8_t i = 0; i < kStatusRows; i++) {
    if (!_stValue[i] || _stCache[i] == v[i]) continue;
    _stCache[i] = v[i];
    lv_label_set_text(_stValue[i], v[i].c_str());
    lv_obj_set_style_text_color(_stValue[i], lv_color_hex(c[i]), 0);
  }
}

void Ui::menuUpdate(const char *title, const MenuRow *rows, uint8_t n, int8_t sel,
                    const char *const *info, uint8_t infoCount, const char *foot,
                    int barPct, const char *barText) {
  if (!_scrMenu) return;
  if (_current != 1) {
    lv_screen_load(_scrMenu);
    _current = 1;
  }
  if (n > kMaxMenuRows) {
    _log.addf(LogLevel::kWarn, "ui", "菜单项 %u 超出槽位上限，多余项被丢弃", (unsigned)n);
    n = kMaxMenuRows;
  }
  if (infoCount > kMaxInfoRows) infoCount = kMaxInfoRows;
  if (barPct > 100) barPct = 100;

  // 内容指纹：标题/选中/页脚/选项/正文/进度条任一变化才重设整页，
  // 避免按键连发时的重排（连发每秒 5 次，内容多数时候没变）
  String fp = (title ? title : "");
  fp += '|';
  fp += (int)sel;
  fp += '|';
  fp += (foot ? foot : "");
  fp += '|';
  for (uint8_t i = 0; i < n; i++) {
    fp += rows[i].icon ? rows[i].icon : "";
    fp += rows[i].label ? rows[i].label : "";
    fp += ';';
  }
  fp += '|';
  for (uint8_t i = 0; i < infoCount; i++) {
    fp += info[i] ? info[i] : "";
    fp += ';';
  }
  fp += '|';
  fp += barPct;
  fp += '|';
  fp += (barText ? barText : "");
  if (fp == _mnCache) return;
  _mnCache = fp;

  lv_label_set_text(_mnTitle, title ? title : "");

  for (uint8_t i = 0; i < kMaxMenuRows; i++) {
    if (i >= n) {
      lv_obj_set_hidden(_mnRow[i], true);
      continue;
    }
    lv_obj_set_hidden(_mnRow[i], false);
    const bool hi = ((int8_t)i == sel);
    lv_obj_set_style_bg_color(_mnRow[i], lv_color_hex(C_SEL), 0);
    lv_obj_set_style_bg_opa(_mnRow[i], hi ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
    if (_mnIcon[i]) {
      lv_label_set_text(_mnIcon[i], rows[i].icon ? rows[i].icon : "");
      lv_obj_set_style_text_color(_mnIcon[i], lv_color_hex(hi ? C_ACCENT : C_LABEL), 0);
    }
    if (_mnLabel[i]) {
      lv_label_set_text(_mnLabel[i], rows[i].label ? rows[i].label : "");
      lv_obj_set_style_text_color(_mnLabel[i], lv_color_hex(hi ? C_TEXT : C_LABEL), 0);
    }
  }

  for (uint8_t i = 0; i < kMaxInfoRows; i++) {
    if (i >= infoCount) {
      lv_obj_set_hidden(_mnInfo[i], true);
      continue;
    }
    lv_obj_set_hidden(_mnInfo[i], false);
    lv_label_set_text(_mnInfo[i], info[i] ? info[i] : "");
  }

  // 进度条：紧跟在选项区下方（选项少时贴近内容，选项多时退到页脚上方让位）
  const bool showBar = (barPct >= 0);
  if (_mnBar) {
    if (showBar) {
      lv_coord_t y = kMenuTop + n * kMenuRowH + 6;
      if (y > kBarTopMax) y = kBarTopMax;
      lv_obj_set_y(_mnBar, y);
      lv_bar_set_value(_mnBar, barPct, LV_ANIM_OFF);
    }
    lv_obj_set_hidden(_mnBar, !showBar);
  }
  if (_mnBarVal) {
    if (showBar) {
      lv_obj_set_y(_mnBarVal, (_mnBar ? lv_obj_get_y(_mnBar) : kBarTopMax) - 4);
      lv_label_set_text(_mnBarVal, barText ? barText : "");
    }
    lv_obj_set_hidden(_mnBarVal, !showBar);
  }

  lv_label_set_text(_mnFoot, foot ? foot : "");
}

int Ui::textViewUpdate(const char *title, const char *text, int scroll) {
  if (!_scrView || !_svText) return scroll;
  if (_current != 2) {
    lv_screen_load(_scrView);
    _current = 2;
  }

  // 标题 + 正文的指纹：完全相同（只是滚动没变）就跳过重排，
  // 文本页每 500ms 会被刷一次，短信详情这类静态内容不该反复重排版。
  String fp = (title ? title : "");
  fp += '\n';
  fp += (text ? text : "");

  int lines = 1;
  for (size_t i = 0; i < fp.length(); i++) {
    if (fp[i] == '\n') lines++;
  }
  const int lineH = (int)lv_font_get_line_height(fontText());
  if (lineH <= 0) return scroll;
  int visible = kViewH / lineH;
  if (visible < 1) visible = 1;

  int maxScroll = lines - visible;
  if (maxScroll < 0) maxScroll = 0;
  if (scroll < 0) scroll = 0;
  if (scroll > maxScroll) scroll = maxScroll;

  if (fp != _svCache) {
    _svCache = fp;
    if (_svTitle) lv_label_set_text(_svTitle, title ? title : "");
    lv_label_set_text(_svText, text ? text : "");
    lv_obj_set_y(_svText, -scroll * lineH);
    lv_label_set_text(_svFoot,
                      ("U/D滚动 BACK返回  " + String(scroll + 1) + "/" + String(lines))
                          .c_str());
  } else {
    // 内容没变但滚动位置可能变了：只挪文本与页脚计数
    lv_obj_set_y(_svText, -scroll * lineH);
    lv_label_set_text(_svFoot,
                      ("U/D滚动 BACK返回  " + String(scroll + 1) + "/" + String(lines))
                          .c_str());
  }
  return scroll;
}

int Ui::viewVisibleLines() const {
  const int lineH = (int)lv_font_get_line_height(fontText());
  if (lineH <= 0) return 1;
  int v = kViewH / lineH;
  return v > 0 ? v : 1;
}

void Ui::smsPopupUpdate(const char *phone, const char *body, int8_t modem,
                        const char *time, bool incoming) {
  if (!_scrPopup) return;
  if (_current != 4) {
    lv_screen_load(_scrPopup);
    _current = 4;
  }

  String fp = (phone ? phone : "");
  fp += '|';
  fp += (body ? body : "");
  fp += '|';
  fp += (int)modem;
  fp += '|';
  fp += (time ? time : "");
  fp += '|';
  fp += incoming ? 'i' : 'o';
  if (fp == _popCache) return;   // 弹窗内容固定，重复调用（每轮 tick）不再重排
  _popCache = fp;

  if (_popPhone) lv_label_set_text(_popPhone, phone && *phone ? phone : "未知号码");
  if (_popBody) lv_label_set_text(_popBody, body && *body ? body : "(无正文)");
  if (_popMeta) {
    String meta = incoming ? "收到" : "发出";
    if (time && *time) {
      meta += "  ";
      meta += time;
    }
    if (modem > 0) {
      meta += "  模组";
      meta += String((int)modem);
    }
    lv_label_set_text(_popMeta, meta.c_str());
  }
}

}  // namespace ui
