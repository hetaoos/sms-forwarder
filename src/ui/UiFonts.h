#pragma once

#include <lvgl.h>

#include <stdint.h>

// 字体管理：把「中文向量字形」与「图标位图字形」两个来源统一成 LVGL 可直接用的
// lv_font_t*，页面代码只从这里取字体，不关心它到底是 TinyTTF 还是位图字体。
//
//   fontText / fontTitle：运行时从 LittleFS 的 TTF 用 LVGL TinyTTF 渲染，
//                         TTF 常驻 PSRAM；加载完成前返回 Montserrat（仅 ASCII）
//   fontIcon：           icon 生成器（script/gen_iconfont.py）产出的位图字体，
//                        编译进 flash，始终可用
//
// 图标字体定义在 .c 里（C 链接），这里必须在全局作用域 extern "C" 声明，
// 放到 namespace ui 内会被 C++ 名字改编，链接时找不到符号。
extern "C" const lv_font_t ui_font_icon_20;
// 开机页 Logo：同一个图标字体的 48px 版本（script/gen_iconfont.py --size 48
// --icons comment-sms --out ui_font_logo.c），同样是编译进 flash 的位图字体
extern "C" const lv_font_t ui_font_logo;

namespace ui {

// 建立 TTF 加载状态机（真正读取在 fontsTick 里做）。可在 LVGL 就绪后立刻调用
void fontsBegin();

// 主循环调用：文件系统挂载成功后会自动完成加载（每 500ms 重试一次）
void fontsTick();

// 中文字库是否已就绪（未就绪时 UI 用 ASCII 字体显示，汉字会缺形）
bool fontsReady();

// 图标字体（始终可用）
const lv_font_t *fontIcon();

// 开机页 Logo 字体（48px 图标，始终可用）
const lv_font_t *fontLogo();

// 正文 / 菜单 / 状态值（中文字库未就绪时返回 ASCII 兜底字体）
const lv_font_t *fontText();

// 标题栏（中文字库未就绪时返回 ASCII 兜底字体）
const lv_font_t *fontTitle();

// 纯 ASCII 小字（页脚等），始终可用
const lv_font_t *fontAscii();

// 字库加载完成的那一帧返回 true（用于通知 UI 全量刷新一次）
bool fontsTakeReadyFlag();

}  // namespace ui
