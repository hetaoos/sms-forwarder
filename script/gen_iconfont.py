#!/usr/bin/env python3
"""生成 LVGL 图标字体（src/ui/fonts/ui_font_icon_*.c）。

管线：
    FontAwesome 免费版 webfont (.woff2/.ttf)
        -> fontTools 解码并挑选所需图标
        -> lv_font_conv（node）转成 LVGL 内部格式的 C 文件

产出物已提交进仓库，日常编译不需要跑这个脚本；只在「增删图标 / 改图标字号」
时才需要重新生成。

依赖：
    python3 -m pip install fontTools brotli     # .woff2 输入才需要 brotli
    npm i lv_font_conv                          # 转换器本体
    npm i @fortawesome/fontawesome-free         # 图标源文件（webfonts/fa-solid-900.*）

用法：
    python3 script/gen_iconfont.py                          # 默认用 npm 包里的 woff2
    python3 script/gen_iconfont.py --ttf /path/fa-solid-900.ttf
    python3 script/gen_iconfont.py --size 20                # 改输出字号

许可：Font Awesome Free 字体为 SIL OFL 1.1，图标图形为 CC BY 4.0
      （出处已写入生成的 C 文件头，勿删）。
"""

import argparse
import json
import os
import subprocess
import sys
import tempfile

PROJECT_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# 项目里实际用到的图标（名字与 src/ui/UiIcons.h 的 UI_ICON_* 一一对应）
ICONS = [
    # ---- 状态页：各状态行左侧图标 ----
    "wifi",             # WiFi 连接状态
    "globe",            # IP / 网络
    "signal",           # RSSI 信号强度
    "clock",            # 运行时间
    "memory",           # 内部堆内存
    "microchip",        # PSRAM
    "lightbulb",        # RGB 指示灯
    "tower-broadcast",  # 通信模组 / 基站
    "sim-card",         # SIM 卡状态（状态页通信模组行）
    "comment-sms",      # 短信
    "envelope",         # 邮件通知
    "paper-plane",      # Webhook 推送
    "id-card",          # 本机 MAC
    "gauge-high",       # CPU 负载
    "satellite-dish",   # MQTT
    "terminal",         # AT 命令
    # ---- 菜单 / 通用 ----
    "house",            # 回到状态页
    "sun",              # 亮度
    "power-off",        # 屏幕开关
    "display",          # 屏幕显示页
    "arrows-rotate",    # 刷新
    "circle-info",      # 关于
    "arrow-left",       # 返回上一级
    "list-check",       # 菜单
]

DEFAULT_WOFF2 = os.path.join(
    PROJECT_DIR, "node_modules", "@fortawesome", "fontawesome-free",
    "webfonts", "fa-solid-900.woff2")
SYSTEM_WOFF2 = "/usr/local/lib/node_modules/@fortawesome/fontawesome-free/webfonts/fa-solid-900.woff2"
META_JSON = os.path.join(
    PROJECT_DIR, "node_modules", "@fortawesome", "fontawesome-free",
    "metadata", "icon-families.json")


def load_ttf(src, is_woff2):
    """读入字体文件，返回 TTFont；woff2 走 brotli 解压。"""
    from fontTools.ttLib import TTFont
    font = TTFont(src)
    if is_woff2:
        font.flavor = None      # 去掉 woff2 封装，还原为裸 sfnt
    return font


def resolve_codepoints(names, meta_path, ttf_path):
    """图标名 -> Unicode 码点，并校验字形确实存在于字体中。"""
    from fontTools.ttLib import TTFont
    meta = json.load(open(meta_path, encoding="utf-8"))
    font = TTFont(ttf_path)
    cmap = font.getBestCmap()

    codes, missing = [], []
    for name in names:
        entry = meta.get(name)
        if not entry:
            missing.append("%s (元数据中没有这个图标名)" % name)
            continue
        code = int(entry["unicode"], 16)
        if code not in cmap:
            missing.append("%s (U+%04X 不在字体 cmap 中)" % (name, code))
            continue
        codes.append(code)
    if missing:
        raise SystemExit("以下图标无法解析：\n  " + "\n  ".join(missing))
    return codes


def main():
    ap = argparse.ArgumentParser(description="生成 LVGL 图标字体")
    ap.add_argument("--ttf", help="FontAwesome solid TTF/OTF 路径")
    ap.add_argument("--woff2", help="FontAwesome solid WOFF2 路径（自动解封装）")
    ap.add_argument("--size", type=int, default=24, help="输出字号（像素）")
    ap.add_argument("--bpp", type=int, default=4, choices=[1, 2, 3, 4, 8],
                    help="抗锯齿位深")
    ap.add_argument("--meta", help="FontAwesome icon-families.json 路径")
    # 生成开机 Logo 用的大字字体时指定这两个参数：
    #   python3 script/gen_iconfont.py --size 48 --icons comment-sms --out ui_font_logo.c
    ap.add_argument("--icons", help="逗号分隔的图标名，默认用脚本内的 ICONS 列表")
    ap.add_argument("--out", help="输出文件名，默认 ui_font_icon_<size>.c")
    args = ap.parse_args()

    ttf_path = args.ttf
    if not ttf_path:
        woff2 = args.woff2 or (DEFAULT_WOFF2 if os.path.exists(DEFAULT_WOFF2)
                               else SYSTEM_WOFF2)
        if not os.path.exists(woff2):
            sys.exit("缺少图标字体源文件：先 npm i @fortawesome/fontawesome-free，"
                     "或用 --ttf / --woff2 指定路径")
        print("[iconfont] 解码 %s" % os.path.basename(woff2))
        tmp_ttf = os.path.join(tempfile.gettempdir(), "fa-solid-900.ttf")
        load_ttf(woff2, True).save(tmp_ttf)
        ttf_path = tmp_ttf

    meta_path = args.meta or META_JSON
    if not os.path.exists(meta_path):
        sys.exit("缺少图标元数据 %s：先 npm i @fortawesome/fontawesome-free" % meta_path)

    names = [s.strip() for s in args.icons.split(",") if s.strip()] if args.icons else ICONS
    codes = resolve_codepoints(names, meta_path, ttf_path)
    print("[iconfont] 选中 %d 个图标" % len(codes))

    out_name = args.out or ("ui_font_icon_%d.c" % args.size)
    out_path = os.path.join(PROJECT_DIR, "src", "ui", "fonts", out_name)
    os.makedirs(os.path.dirname(out_path), exist_ok=True)

    font_name = os.path.splitext(out_name)[0]
    # --symbols 收的是「字符」而非码点串，这里直接喂 UTF-8 字符，避免转义歧义
    symbols = "".join(chr(c) for c in codes)

    cmd = [
        "npx", "--yes", "lv_font_conv",
        "--format", "lvgl",
        "--font", ttf_path,
        "--symbols", symbols,
        "--size", str(args.size),
        "--bpp", str(args.bpp),
        "--no-compress",
        "--lv-include", "lvgl.h",
        "--lv-font-name", font_name,
        "-o", out_path,
    ]
    print("[iconfont] 生成 %s" % out_path)
    if subprocess.call(cmd) != 0:
        sys.exit("lv_font_conv 失败：网络首次使用会下载它，失败多半是 npm 不可用")

    header = (
        "/* 由 script/gen_iconfont.py 从 Font Awesome Free 生成，请勿手工编辑。\n"
        " * Font Awesome Free 字体：SIL OFL 1.1；图标图形：CC BY 4.0\n"
        " * 出处：https://fontawesome.com  (Fonticons, Inc.)\n"
        " */\n")
    with open(out_path, encoding="utf-8") as f:
        body = f.read()
    with open(out_path, "w", encoding="utf-8") as f:
        f.write(header + body)
    print("[iconfont] OK：%d 字节" % os.path.getsize(out_path))


if __name__ == "__main__":
    main()
