# 按条件编译引用外部库（方案 A）
#
# 仅 ENABLE_LCD 关联外部库（LovyanGFX + LVGL）；ENABLE_MQTT 用框架内置 esp-mqtt、
# ENABLE_LED 无外部库，均无需处理。
#
# 关闭 ENABLE_LCD 时：
#   1) 从 lib_deps 移除 LovyanGFX / LVGL —— 链接器本就不引用（已由 handler 的
#      #if 裁剪保证），这里进一步跳过「编译」这两个库源码，缩短构建时间；
#   2) 用 src_filter 排除 LCD 相关 .cpp/.c，既省编译时间，又避免库依赖查找（LDF）
#      按文本扫描到其中的 #include <lvgl.h> 而要求该库。
# 开启 ENABLE_LCD（默认）时本脚本不改动任何东西。

Import("env")
import os
import re


def _collect_defines():
    """汇总所有来源（ini build_flags + CLI 的 PLATFORMIO_BUILD_FLAGS + 已合并的
    CPPDEFINES / CCFLAGS）。取最后一次出现的定义作为有效值（CLI 覆盖 ini）。"""
    texts = []
    try:
        texts.append(" ".join(env.GetProjectOption("build_flags", "")))
    except Exception:
        pass
    texts.append(os.environ.get("PLATFORMIO_BUILD_FLAGS", ""))
    try:
        texts.append(" ".join(str(x) for x in env.get("CPPDEFINES", [])))
    except Exception:
        pass
    try:
        texts.append(str(env.get("CCFLAGS", "")))
    except Exception:
        pass
    return " ".join(texts)


def _flag_int(name, default=1):
    # 取所有匹配中的最后一个：CLI 的 -D 覆盖 ini 默认值
    matches = re.findall(r'-D\s*%s(?:\s*=\s*(\d+))?' % re.escape(name),
                         _collect_defines())
    if not matches:
        return default
    last = matches[-1]
    if last == "":
        return 1
    return 1 if int(last) != 0 else 0


if _flag_int("ENABLE_LCD", 1) == 0:
    # 1) 从 lib_deps 移除 LovyanGFX / LVGL
    try:
        deps = env.GetProjectOption("lib_deps", [])
    except Exception:
        deps = []
    if isinstance(deps, str):
        deps = [d for d in deps.splitlines() if d.strip()]
    try:
        cfg = env.GetProjectConfig()
        sec = "env:" + env.get("PIOENV", "")
        # 1a) 从 lib_deps 移除（避免全新环境拉取并编译）
        new_deps = [d for d in deps if not re.search(r'LovyanGFX|lvgl', d, re.IGNORECASE)]
        if len(new_deps) != len(deps):
            cfg.set(sec, "lib_deps", new_deps)
            print("feature_libs: ENABLE_LCD=0 -> 已从 lib_deps 移除 LVGL / LovyanGFX")
        # 1b) 加入 lib_ignore：即便库已缓存安装，也强制 LDF 跳过编译/链接
        ig = cfg.get(sec, "lib_ignore", [])
        if not isinstance(ig, list):
            ig = [ig]
        igset = list(ig)
        for name in ("LovyanGFX", "lvgl"):
            if name not in igset:
                igset.append(name)
        if igset != list(ig):
            cfg.set(sec, "lib_ignore", igset)
            print("feature_libs: ENABLE_LCD=0 -> lib_ignore 追加 LovyanGFX / lvgl")
    except Exception as e:
        print("feature_libs: 修改 lib_deps/lib_ignore 失败: %s" % e)

    # 2) 排除 LCD 相关源文件（含字体 .c，其内部无条件 #include "lvgl.h"）
    src_dir = env.get("PROJECT_SRC_DIR") or os.path.join(
        env.get("PROJECT_DIR", ""), "src"
    )
    lcd_files = set()

    # 已知纯 LCD 实现文件（其符号均由头文件空实现替身覆盖，关 LCD 时不编译安全）
    for k in ("hal/Display.cpp", "ui/Ui.cpp", "ui/UiFonts.cpp", "app/UIMenu.cpp"):
        if os.path.isfile(os.path.join(src_dir, k)):
            lcd_files.add(k)

    # 自动探测：直接 #include lvgl / LovyanGFX 的源文件（字体 .c 等）
    for root, _dirs, files in os.walk(src_dir):
        for fn in files:
            if not fn.endswith((".cpp", ".c", ".cc")):
                continue
            fp = os.path.join(root, fn)
            try:
                with open(fp, "r", errors="ignore") as fh:
                    content = fh.read()
            except Exception:
                continue
            if re.search(r'#include\s*[<"](lvgl|LovyanGFX)', content):
                lcd_files.add(os.path.relpath(fp, src_dir).replace(os.sep, "/"))

    if lcd_files:
        # 必须在默认包含 +<*> 基础上追加排除项；若只写排除项会导致整个 src/ 被排除
        cur = env.get("SRC_FILTER", "+<*>")
        if isinstance(cur, list):
            cur = " ".join(str(x) for x in cur)
        excl = " ".join("-<%s>" % f for f in sorted(lcd_files))
        env.Replace(SRC_FILTER="%s %s" % (cur, excl))
        print("feature_libs: ENABLE_LCD=0 -> 排除 LCD 源文件: %s" %
              ", ".join(sorted(lcd_files)))
