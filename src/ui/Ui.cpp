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
constexpr uint32_t C_PANEL = 0x121A26;   // 卡片/区块底色
constexpr uint32_t C_SEL = 0x1C3A5E;     // 菜单选中行底色
constexpr uint32_t C_LINE = 0x1F2A38;    // 分隔线
constexpr uint32_t C_TEXT = 0xE8EEF6;    // 主文字
constexpr uint32_t C_LABEL = 0x8A9AB0;   // 次要文字 / 未选中行
constexpr uint32_t C_HINT = 0x5C6878;    // 页脚提示
constexpr uint32_t C_ACCENT = 0x4DA3FF;  // 强调色（图标 / AP 模式）
constexpr uint32_t C_OK = 0x3AD97A;      // 正常
constexpr uint32_t C_ERR = 0xE5544B;     // 异常
constexpr uint32_t C_CMD = 0x5AC8FF;     // AT 命令
constexpr uint32_t C_RESP = 0x9BE36F;    // AT 响应

// ---------- 版式 ----------
// 屏宽取自 LVGL（已经过旋转换算），避免把 240 写死在多处
lv_coord_t kScreenW = 240;

constexpr lv_coord_t kPad = 8;                 // 左右边距
constexpr lv_coord_t kHeaderH = 34;            // 标题栏高度
constexpr lv_coord_t kRowTop = kHeaderH + 4;   // 状态区首行 Y：38
constexpr lv_coord_t kRowH = 28;               // 状态行高
constexpr lv_coord_t kRowCount = 7;            // 状态行数
constexpr lv_coord_t kAtTop = kRowTop + kRowCount * kRowH + 4;  // AT 区 Y：238
constexpr lv_coord_t kAtH = 54;                // AT 区高度
constexpr lv_coord_t kFootTop = kAtTop + kAtH + 2;              // 页脚 Y：294
constexpr lv_coord_t kFootH = 20;              // 页脚高度
constexpr lv_coord_t kMenuTop = kHeaderH + 6;  // 菜单首行 Y：40
constexpr lv_coord_t kMenuRowH = 30;           // 菜单行高
constexpr lv_coord_t kViewTop = kHeaderH + 4;  // 屏幕显示区 Y：38
constexpr lv_coord_t kViewH = 254;             // 屏幕显示区高度（38~292）

// 状态行定义（左侧图标 + 标签，值在右侧）
struct RowDef {
  const char *icon;
  const char *label;
};
constexpr RowDef kStatusRows[kRowCount] = {
    {UI_ICON_WIFI, "WIFI"},       {UI_ICON_GLOBE, "IP"},
    {UI_ICON_SIGNAL, "RSSI"},     {UI_ICON_CLOCK, "UPTIME"},
    {UI_ICON_MEMORY, "HEAP"},     {UI_ICON_MICROCHIP, "PSRAM"},
    {UI_ICON_LIGHTBULB, "RGB"},
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

// 按 kind 取字体（ui::UiFonts 负责实现细节）
const lv_font_t *fontFor(uint8_t kind) {
  switch (kind) {
    case 0: return fontIcon();
    case 2: return fontTitle();
    case 3: return fontAscii();
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
  buildScreenView();
  lv_screen_load(_scrStatus);
  _current = 0;
  fontsBegin();      // 中文字库走延迟加载（等 LittleFS 挂载）
  _built = true;
}

// ---- 实时状态页 ----
void Ui::buildStatus() {
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

  // 状态行：图标 / 标签放在左侧，值右对齐
  lv_obj_t *body = lv_obj_create(_scrStatus);
  lv_obj_remove_style_all(body);
  lv_obj_set_scrollable(body, false);
  lv_obj_set_pos(body, 0, kRowTop);
  lv_obj_set_size(body, LV_PCT(100), kRowCount * kRowH);
  lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);

  for (uint8_t i = 0; i < kRowCount; i++) {
    lv_obj_t *row = makeRow(body, kRowH, kPad);
    lv_obj_set_style_border_side(row, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_width(row, 1, 0);
    lv_obj_set_style_border_color(row, lv_color_hex(C_LINE), 0);
    newLabel(row, 0, C_LABEL, kStatusRows[i].icon, LV_TEXT_ALIGN_LEFT);
    lv_obj_t *nm = newLabel(row, 1, C_LABEL, kStatusRows[i].label, LV_TEXT_ALIGN_LEFT);
    lv_obj_set_width(nm, 74);
    // 值由 statusUpdate 填充，这里先留空，保证首帧一定被判定为“有变化”
    _stValue[i] = newLabel(row, 1, C_TEXT, "", LV_TEXT_ALIGN_RIGHT);
    lv_obj_set_flex_grow(_stValue[i], 1);
  }

  // 最近一条 AT 命令 / 响应
  lv_obj_t *at = lv_obj_create(_scrStatus);
  lv_obj_remove_style_all(at);
  lv_obj_set_scrollable(at, false);
  lv_obj_set_pos(at, kPad, kAtTop);
  lv_obj_set_size(at, kScreenW - kPad * 2, kAtH);
  lv_obj_set_style_bg_color(at, lv_color_hex(C_PANEL), 0);
  lv_obj_set_style_bg_opa(at, LV_OPA_COVER, 0);
  lv_obj_set_style_radius(at, 6, 0);
  lv_obj_set_style_pad_all(at, 6, 0);
  lv_obj_set_style_pad_gap(at, 2, 0);
  lv_obj_set_flex_flow(at, LV_FLEX_FLOW_COLUMN);

  lv_obj_t *atTitle = makeRow(at, 16, 0);
  newLabel(atTitle, 0, C_LABEL, UI_ICON_TERMINAL, LV_TEXT_ALIGN_LEFT);
  newLabel(atTitle, 1, C_LABEL, "LAST AT", LV_TEXT_ALIGN_LEFT);

  _atCmd = newLabel(at, 1, C_CMD, "", LV_TEXT_ALIGN_LEFT);
  lv_obj_set_width(_atCmd, LV_PCT(100));
  lv_label_set_long_mode(_atCmd, LV_LABEL_LONG_WRAP);
  _atResp = newLabel(at, 1, C_RESP, "", LV_TEXT_ALIGN_LEFT);
  lv_obj_set_width(_atResp, LV_PCT(100));
  lv_label_set_long_mode(_atResp, LV_LABEL_LONG_WRAP);

  // 页脚：接口与串口引脚一览（纯 ASCII，用兜底字体即可）
  lv_obj_t *foot = newLabel(_scrStatus, 3, C_HINT, "HTTP :80  U1 17/18  U2 41/40",
                            LV_TEXT_ALIGN_LEFT);
  lv_obj_set_pos(foot, kPad, kFootTop + 2);
  lv_obj_set_width(foot, kScreenW - kPad * 2);
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

  // 正文槽位：与选项互斥（信息页用它）
  for (uint8_t i = 0; i < kMaxInfoRows; i++) {
    lv_obj_t *lb = newLabel(_scrMenu, 1, C_TEXT, "", LV_TEXT_ALIGN_LEFT);
    lv_obj_set_pos(lb, kPad, kMenuTop + i * 26);
    lv_obj_set_width(lb, kScreenW - kPad * 2);
    lv_label_set_long_mode(lb, LV_LABEL_LONG_WRAP);
    lv_obj_set_hidden(lb, true);
    _mnInfo[i] = lb;
  }

  // 页脚文案含中文（“OK进入 / 亮度”），不能挂只有英文字形的 ASCII 兜底字体，
  // 否则汉字会画成占位方块；中文字库未就绪时 fontText() 自动退回 ASCII 字体
  _mnFoot = newLabel(_scrMenu, 1, C_HINT, "", LV_TEXT_ALIGN_LEFT);
  lv_obj_set_pos(_mnFoot, kPad, kFootTop + 2);
  lv_obj_set_width(_mnFoot, kScreenW - kPad * 2);
}

// ---- 屏幕显示页（可滚动的状态快照）----
void Ui::buildScreenView() {
  _scrView = lv_obj_create(NULL);
  lv_obj_remove_style_all(_scrView);
  lv_obj_set_scrollable(_scrView, false);
  lv_obj_set_style_bg_color(_scrView, lv_color_hex(C_BG), 0);
  lv_obj_set_style_bg_opa(_scrView, LV_OPA_COVER, 0);

  lv_obj_t *hdr = makeHeader(_scrView);
  newLabel(hdr, 0, C_ACCENT, UI_ICON_DISPLAY, LV_TEXT_ALIGN_LEFT);
  lv_obj_t *ttl = newLabel(hdr, 2, C_TEXT, "屏幕显示", LV_TEXT_ALIGN_LEFT);
  lv_obj_set_flex_grow(ttl, 1);

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

  _svFoot = newLabel(_scrView, 3, C_HINT, "", LV_TEXT_ALIGN_LEFT);
  lv_obj_set_pos(_svFoot, kPad, kFootTop + 2);
  lv_obj_set_width(_svFoot, kScreenW - kPad * 2);
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

  String v[kRowCount];
  uint32_t c[kRowCount];
  v[0] = s.wifiUp ? "UP" : (s.apMode ? "AP" : "DOWN");
  c[0] = s.wifiUp ? C_OK : (s.apMode ? C_ACCENT : C_ERR);
  v[1] = s.ip ? s.ip : "-";
  c[1] = s.wifiUp ? C_TEXT : (s.apMode ? C_ACCENT : C_ERR);
  v[2] = s.wifiUp ? String(s.rssi) + " dBm" : "-";
  c[2] = C_TEXT;
  v[3] = String(s.uptimeSec) + " s";
  c[3] = C_TEXT;
  v[4] = fmtBytes(s.freeHeap);
  c[4] = C_TEXT;
  v[5] = s.psramSize ? fmtBytes(s.freePsram) + "/" + fmtBytes(s.psramSize) : "-";
  c[5] = C_TEXT;
  v[6] = s.ledName ? s.ledName : "-";
  c[6] = C_TEXT;

  for (uint8_t i = 0; i < kRowCount; i++) {
    if (!_stValue[i] || _stCache[i] == v[i]) continue;
    _stCache[i] = v[i];
    lv_label_set_text(_stValue[i], v[i].c_str());
    lv_obj_set_style_text_color(_stValue[i], lv_color_hex(c[i]), 0);
  }

  // AT 区：把三条内容合成一个指纹，只有真变了才去动 label
  String key = String((unsigned)s.lastPort) + "|" + (s.lastCmd ? s.lastCmd : "") + "|" +
               (s.lastResp ? s.lastResp : "");
  if (key != _atCache) {
    _atCache = key;
    String cmd = s.lastPort ? "UART" + String(s.lastPort) + "  " : String();
    cmd += s.lastCmd && *s.lastCmd ? s.lastCmd : "(none)";
    if (_atCmd) lv_label_set_text(_atCmd, cmd.c_str());
    if (_atResp) lv_label_set_text(_atResp, s.lastResp ? s.lastResp : "");
  }
}

void Ui::menuUpdate(const char *title, const MenuRow *rows, uint8_t n, int8_t sel,
                    const char *const *info, uint8_t infoCount, const char *foot) {
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

  // 内容指纹：标题/选中/页脚/选项/正文任一变化才重设整页，避免按键连发时的重排
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

  lv_label_set_text(_mnFoot, foot ? foot : "");
}

int Ui::screenViewUpdate(const StatusInfo &s, int scroll) {
  if (!_scrView || !_svText) return scroll;
  if (_current != 2) {
    lv_screen_load(_scrView);
    _current = 2;
  }

  // 把状态快照摊平成纯文本行（每刷新一次重来一次，行数本身很少）
  String text = "WIFI: ";
  text += s.wifiUp ? "UP" : (s.apMode ? "AP" : "DOWN");
  text += "\nIP: ";
  text += s.ip ? s.ip : "-";
  text += "\nRSSI: ";
  text += s.wifiUp ? String(s.rssi) + " dBm" : "-";
  text += "\nUPTIME: ";
  text += String(s.uptimeSec) + " s";
  text += "\nHEAP: ";
  text += fmtBytes(s.freeHeap);
  text += "\nPSRAM: ";
  text += s.psramSize ? fmtBytes(s.freePsram) + "/" + fmtBytes(s.psramSize) : "-";
  text += "\nRGB: ";
  text += s.ledName ? s.ledName : "-";
  text += "\n";
  text += "\nLAST AT";
  if (s.lastPort) {
    text += "\nUART";
    text += String(s.lastPort);
  }
  if (s.lastCmd && *s.lastCmd) text += String("\n") + s.lastCmd;
  if (s.lastResp && *s.lastResp) text += String("\n") + s.lastResp;

  int lines = 1;
  for (size_t i = 0; i < text.length(); i++) {
    if (text[i] == '\n') lines++;
  }

  const int lineH = (int)lv_font_get_line_height(fontText());
  if (lineH <= 0) return scroll;
  int visible = kViewH / lineH;
  if (visible < 1) visible = 1;

  int maxScroll = lines - visible;
  if (maxScroll < 0) maxScroll = 0;
  if (scroll < 0) scroll = 0;
  if (scroll > maxScroll) scroll = maxScroll;

  lv_label_set_text(_svText, text.c_str());
  lv_obj_set_y(_svText, -scroll * lineH);
  lv_label_set_text(_svFoot, ("U/D:scroll O:ref B:back  " + String(scroll + 1) + "/" +
                              String(lines))
                                 .c_str());
  return scroll;
}

int Ui::screenViewVisibleLines() const {
  const int lineH = (int)lv_font_get_line_height(fontText());
  if (lineH <= 0) return 1;
  int v = kViewH / lineH;
  return v > 0 ? v : 1;
}

}  // namespace ui
