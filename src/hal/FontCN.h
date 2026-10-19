#pragma once

#include <LovyanGFX.hpp>

// 中文点阵字体加载器：开机从资源分区 /cnfont.bin（LovyanGFX BDFfont 格式，
// 由 script/gen_cnfont.py 生成）读入 PSRAM，构造 lgfx::BDFfont 实例。
// 与 LovyanGFX 原生字体用法一致：g.setFont(...) 后即可 drawString 中文。
// 注意：BDFfont 仅 1bpp、固定单元格，UTF-8 由 LovyanGFX 负责解码。
class FontCN {
 public:
  static FontCN &instance();

  // 从资源分区载入并实例化；重复调用安全（仅首次生效）
  bool init();

  // 是否加载成功（失败则 Display 回退 ASCII 字体，不影响英文显示）
  bool ready() const { return _ready; }

  // 将中文 BDF 字体应用到绘制目标（替换原 Font0/Font2）；未就绪时回退 Font2
  void applyTo(lgfx::LovyanGFX &g);

 private:
  FontCN() = default;

  bool _ready = false;
  uint8_t *_buf = nullptr;        // PSRAM 中字体数据（常驻，不释放）
  lgfx::IFont *_font = nullptr;   // 指向静态 BDFfont 实例
};
