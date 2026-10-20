/**
 * @file lv_conf.h
 * LVGL 9.6 配置文件（本项目的 delta 配置）
 *
 * 这里只列出与模板默认值不同的项，其余宏由 lvgl 内部的 lv_conf_internal.h
 * 兜底默认取值，既省篇幅也避免后续升级时配置项失配。
 *
 * 通过 platformio.ini 的 -DLV_CONF_PATH=lv_conf.h + -Isrc 命中本文件。
 */
#ifndef LV_CONF_H
#define LV_CONF_H

/* 自定义 stdlib：让 LVGL 的堆分配统一走 PSRAM（见 src/hal/Display.cpp 的
 * lv_malloc_core / lv_realloc_core / lv_free_core 实现）。
 * 这里必须在 LVGL 内部使用之前把签名声明出去。
 */
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

void * lv_malloc_core(size_t size);
void * lv_realloc_core(void * p, size_t new_size);
void lv_free_core(void * p);

#ifdef __cplusplus
} /*extern "C"*/
#endif

/*============================================================================
 * MEMORY AND STANDARD LIBRARY
 *============================================================================*/

/** LVGL 的分配走上面声明的 *_core 实现（PSRAM 优先），字符串/格式化用 libc */
#define LV_USE_STDLIB_MALLOC LV_STDLIB_CUSTOM
#define LV_USE_STDLIB_STRING LV_STDLIB_CLIB
#define LV_USE_STDLIB_SPRINTF LV_STDLIB_CLIB

/*============================================================================
 * RENDERING CONFIGURATION
 *============================================================================*/

/** ST7789 为 16 位色，SPI 传输；LVGL 输出标准 RGB565，字节序在 flush 里处理 */
#define LV_COLOR_FORMAT_DEFAULT LV_COLOR_FORMAT_RGB565

/** 刷新周期 33ms（约 30fps）；主循环每轮都会调用 lv_timer_handler，实际由
 *  lv_timer_handler 的返回值决定下一帧时间点 */
#define LV_DEF_REFR_PERIOD 33

/** 每行 stride 不做额外对齐：全缓冲 240*2=480 字节本就是 4 的倍数 */
#define LV_DRAW_BUF_STRIDE_ALIGN 1

/** 圆角/阴影：状态卡片需要圆角 */
#define LV_DRAW_SW_COMPLEX 1
#define LV_GRADIENT_MAX_STOPS 2
#define LV_USE_DRAW_SW_COMPLEX_GRADIENTS 0
#define LV_DRAW_SW_SHADOW_CACHE_SIZE 0
#define LV_DRAW_SW_CIRCLE_CACHE_SIZE 4

/** 用不到的渲染后端一律关掉 */
#define LV_USE_VECTOR_GRAPHIC 0
#define LV_USE_THORVG 0
#define LV_USE_SNAPSHOT 0
#define LV_USE_MATRIX 0
#define LV_USE_DRAW_VG_LITE 0
#define LV_USE_PPA 0
#define LV_USE_DRAW_SDL 0

/*============================================================================
 * TEXT & FONT SETTINGS
 *============================================================================*/

#define LV_TXT_ENC LV_TXT_ENC_UTF8

/** Montserrat 14 仅用于中文字库（TinyTTF）加载完成前的开机首帧兜底 */
#define LV_FONT_MONTSERRAT_14 1

/** 运行时 TTF：中文按 GB2312 全集从 PSRAM 动态渲染（见 src/ui/UiFonts.cpp） */
#define LV_USE_TINY_TTF 1
#define LV_TINY_TTF_FILE_SUPPORT 0
#define LV_TINY_TTF_CACHE_GLYPH_CNT 192
#define LV_TINY_TTF_CACHE_KERNING_CNT 64

/** 缺字形时画占位方块，便于发现漏字而非静默空白 */
#define LV_USE_FONT_PLACEHOLDER 1

/*============================================================================
 * WIDGETS
 *============================================================================*/
/* 本项目只用标签 + 进度条做 UI，其余控件全部关掉以省 flash。
 * 页面结构由 lv_obj 容器 + flex 布局搭建，不依赖 menu/list 这类复合控件。 */

#define LV_WIDGETS_HAS_DEFAULT_VALUE 0
#define LV_USE_ANIMIMG 0
#define LV_USE_ARC 0
#define LV_USE_ARCLABEL 0
#define LV_USE_BAR 1
#define LV_USE_BUTTON 0
#define LV_USE_BUTTONMATRIX 0
#define LV_USE_CALENDAR 0
#define LV_USE_CANVAS 0
#define LV_USE_CHART 0
#define LV_USE_CHECKBOX 0
#define LV_USE_DROPDOWN 0
#define LV_USE_IMAGE 0
#define LV_USE_IMAGEBUTTON 0
#define LV_USE_KEYBOARD 0
#define LV_USE_LABEL 1
#define LV_USE_LED 0
#define LV_USE_LINE 0
#define LV_USE_LIST 0
#define LV_USE_MENU 0
#define LV_USE_MSGBOX 0
#define LV_USE_ROLLER 0
#define LV_USE_SCALE 0
#define LV_USE_SLIDER 0
#define LV_USE_SPAN 0
#define LV_USE_SPINBOX 0
#define LV_USE_SPINNER 0
#define LV_USE_SWITCH 0
#define LV_USE_TABLE 0
#define LV_USE_TABVIEW 0
#define LV_USE_TEXTAREA 0
#define LV_USE_TILEVIEW 0
#define LV_USE_WIN 0
#define LV_USE_3DTEXTURE 0

/*============================================================================
 * LAYOUTS / THEMES
 *============================================================================*/

#define LV_USE_FLEX 1
#define LV_USE_GRID 1

/** 不用内置主题：页面样式由 src/ui 自己给，省掉主题表与配套字体 */
#define LV_USE_THEME_DEFAULT 0
#define LV_USE_THEME_SIMPLE 0
#define LV_USE_THEME_MONO 0

/*============================================================================
 * IMAGE / DECODERS
 *============================================================================*/
#define LV_CACHE_DEF_SIZE 0
#define LV_IMAGE_HEADER_CACHE_DEF_CNT 0
#define LV_USE_LODEPNG 0
#define LV_USE_LIBPNG 0
#define LV_USE_BMP 0
#define LV_USE_TJPGD 0
#define LV_USE_LIBJPEG_TURBO 0
#define LV_USE_LIBWEBP 0
#define LV_USE_SVG 0
#define LV_USE_RLE 0
#define LV_USE_LZ4 0

/*============================================================================
 * OBSERVER / MISC
 *============================================================================*/
#define LV_USE_OBSERVER 0

/*============================================================================
 * BUILD
 *============================================================================*/
/** 别编译 examples / demos */
#define LV_BUILD_EXAMPLES 0
#define LV_BUILD_DEMOS 0

#endif /*LV_CONF_H*/
