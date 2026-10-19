#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
把「文泉驿微米黑」子集 TTF 渲染成 LovyanGFX 的 BDFfont 位图字体二进制。

输出格式 (小端) ：
  uint16  indexsize              字形数量
  uint16  index[indexsize]       升序排列的 Unicode 码点
  bytes   bitmap                 indexsize * (H * Wbytes) 个 1bpp 位图，
                                 按 index 顺序拼接，每行 Wbytes 字节、MSB 先行，
                                 与 LovyanGFX BDFfont::draw_char_bmp 的读取方式一致。

BDFfont 参数对应：
  width = W (CJK 全角)  halfwidth = HALF (ASCII)  height = H  baseline = BASELINE
  stride = H * ((W+7)//8) 恒为全角宽，ASCII 仅显示左侧 HALF 列。

用法：
  python3 script/gen_cnfont.py [子集TTF路径] [输出bin路径]
"""
import struct
import sys
import codecs

from PIL import Image, ImageDraw, ImageFont

# ---- 字形几何 ----
W = 16          # CJK 全角单元格宽（也是位图存储宽，ASCII 仅占左 HALF 列）
H = 16          # 单元格高
HALF = 9        # ASCII 显示宽（< 0x100 的码点）
BASELINE = 13   # 基线距单元格顶部的像素数
WBYTES = (W + 7) // 8   # = 2，每行字节数（MSB 先行）
GLYPH_BYTES = H * WBYTES  # = 32，每字形字节数（含 ASCII）

FONT_PATH = "script/font_gb2312.ttf"
OUT_PATH = "data/cnfont.bin"


def gb2312_codepoints():
    """遍历 GB2312 双字节编码，收集全部 Unicode 码点（含 ASCII 可见字符）。"""
    cps = set()
    for c in range(0x20, 0x7F):
        cps.add(c)
    for b1 in range(0xA1, 0x100):
        for b2 in range(0xA1, 0x100):
            try:
                for ch in bytes([b1, b2]).decode("gb2312"):
                    cps.add(ord(ch))
            except Exception:
                pass
    return sorted(cps)


def render_glyph(fnt, ch, is_ascii):
    """把单个字符渲染成 W×H 的 1bpp 位图（MSB 先行）。"""
    img = Image.new("1", (W, H), 0)
    bbox = fnt.getbbox(ch)
    if bbox is None:
        return img.tobytes()
    x0, y0, x1, y1 = bbox
    ink_w = x1 - x0
    ink_h = y1 - y0
    if ink_w <= 0 or ink_h <= 0:
        return img.tobytes()

    tw = HALF if is_ascii else W
    # 水平：在目标显示宽内居中，ASCII 锁在左 HALF 列（避免越界到右半区）
    ox = round((tw - ink_w) / 2) - x0
    if is_ascii:
        ox = max(0, min(ox, max(0, HALF - ink_w)))
    else:
        ox = max(0, min(ox, W - ink_w))
    # 垂直：以基线 BASELINE 对齐，超出则夹到单元格内
    oy = BASELINE
    if oy + y0 < 0:
        oy = -y0
    if oy + y1 > H:
        oy = H - y1

    draw = ImageDraw.Draw(img)
    draw.text((ox, oy), ch, fill=1, font=fnt)
    return img.tobytes()


def main():
    font_path = sys.argv[1] if len(sys.argv) > 1 else FONT_PATH
    out_path = sys.argv[2] if len(sys.argv) > 2 else OUT_PATH

    cps = gb2312_codepoints()
    fnt_cjk = ImageFont.truetype(font_path, 16)
    fnt_asc = ImageFont.truetype(font_path, 12)

    glyphs = bytearray()
    index = bytearray()
    for cp in cps:
        ch = chr(cp)
        is_ascii = cp < 0x100
        fnt = fnt_asc if is_ascii else fnt_cjk
        data = render_glyph(fnt, ch, is_ascii)
        if len(data) != GLYPH_BYTES:
            # 防御：强制对齐到 GLYPH_BYTES（右填充）
            data = data[:GLYPH_BYTES] + b"\x00" * (GLYPH_BYTES - len(data))
        glyphs += data
        index += struct.pack("<H", cp)

    with open(out_path, "wb") as f:
        f.write(struct.pack("<H", len(cps)))
        f.write(index)
        f.write(glyphs)

    print("字形数: %d" % len(cps))
    print("索引表: %d 字节  位图: %d 字节" % (len(index), len(glyphs)))
    print("产物: %s (%d 字节)" % (out_path, 4 + len(index) + len(glyphs)))


if __name__ == "__main__":
    main()
