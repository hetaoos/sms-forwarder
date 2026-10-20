#pragma once

// 图标字体：由 script/gen_iconfont.py 从 Font Awesome Free 生成 LVGL 位图字体，
// 只用实际需要的 21 个图标，编译进 flash（约 32KB）。
//
// 每个宏是单个字形对应的 UTF-8 字面量：直接拼进 lv_label 的文本即可，
// 但必须与“只用图标字体”的 label 搭配——中文字库里没有这些私用区码点。
// 参见 ui::Icon() 辅助函数（src/ui/UiTheme.h）。
//
// 许可：字体 SIL OFL 1.1，图标图形 CC BY 4.0（Fonticons, Inc.）

// ---- 状态行 ----
#define UI_ICON_WIFI       "\xEF\x87\xAB"   // U+F1EB  wifi
#define UI_ICON_GLOBE      "\xEF\x82\xAC"   // U+F0AC  globe
#define UI_ICON_SIGNAL     "\xEF\x80\x92"   // U+F012  signal
#define UI_ICON_CLOCK      "\xEF\x80\x97"   // U+F017  clock
#define UI_ICON_MEMORY     "\xEF\x94\xB8"   // U+F538  memory
#define UI_ICON_MICROCHIP  "\xEF\x8B\x9B"   // U+F2DB  microchip
#define UI_ICON_LIGHTBULB  "\xEF\x83\xAB"   // U+F0EB  lightbulb
#define UI_ICON_TOWER      "\xEF\x94\x99"   // U+F519  tower-broadcast
#define UI_ICON_SIM        "\xEF\x9F\x84"   // U+F7C4  sim-card
#define UI_ICON_SMS        "\xEF\x9F\x8D"   // U+F7CD  comment-sms
#define UI_ICON_MAIL       "\xEF\x83\xA0"   // U+F0E0  envelope
#define UI_ICON_PUSH       "\xEF\x87\x98"   // U+F1D8  paper-plane
#define UI_ICON_TERMINAL   "\xEF\x84\xA0"   // U+F120  terminal
// ---- 菜单 / 通用 ----
#define UI_ICON_HOME       "\xEF\x80\x95"   // U+F015  house
#define UI_ICON_SUN        "\xEF\x86\x85"   // U+F185  sun
#define UI_ICON_POWER      "\xEF\x80\x91"   // U+F011  power-off
#define UI_ICON_DISPLAY    "\xEE\x85\xA3"   // U+E163  display
#define UI_ICON_REFRESH    "\xEF\x80\xA1"   // U+F021  arrows-rotate
#define UI_ICON_INFO       "\xEF\x81\x9A"   // U+F05A  circle-info
#define UI_ICON_BACK       "\xEF\x81\xA0"   // U+F060  arrow-left
#define UI_ICON_MENU       "\xEF\x82\xAE"   // U+F0AE  list-check
