#!/usr/bin/env python3
"""
ESP32-S3 发布产物打包脚本（单平台）。

CI（.cnb.yml）对每个 PlatformIO 环境调用一次，负责：
  1. 编译固件 + LittleFS 镜像（buildfs）
  2. 按分区表合并出可烧录的产物
  3. 归档到 dist/ 目录，文件名带平台后缀，供 Release 上传
  4. 生成该平台的 release notes 片段

平台差异（关键词、分区偏移、flash 大小）集中定义在 PLATFORMS 里。
本仓库目前只有一个硬件平台（esp32s3），新增硬件平台时只需在此处和
platformio.ini 各加一条。

data/ 的 gzip 压缩由本仓库已有的 script/uploadfs_gz.py（platformio.ini 里
pre: 加载的 extra_script）在 buildfs 阶段自动完成，构建进程退出时 atexit 会
把 data/ 还原，因此这里无需手动压缩/还原。

产物命名（<tag> 为 RELEASE_TAG，<platform> 见 PLATFORMS 的 key）：
  full-<tag>-<platform>.bin      首次安装（bootloader + 分区表 + app，会清空 NVS）
  update-<tag>-<platform>.bin    本地升级（app + LittleFS 资源分区）
  littlefs-<tag>-<platform>.bin  单独更新 Web UI 资源分区
  ota-<tag>-<platform>.bin       设备在线 OTA 用（仅 app 分区）
  firmware-<tag>-<platform>.elf  调试符号

<tag> 形如 1.0.0-20261004T121442-f2b513a，与 Release 的 tag 逐字一致 ——
设备端 OTA 拼的地址就是 <Release tag>/ota-<Release tag>-<platform>.bin。
"""

import argparse
import hashlib
import os
import re
import shutil
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DIST = os.path.join(ROOT, "dist")

# ── 平台定义 ────────────────────────────────────────────────────────
# app_offset  : app 分区起始偏移（ota_0），update 包从这里开始
# fs_offset   : LittleFS/资源分区偏移（data/ 打包进去的内容）
# flash_size  : 合并镜像写入 bootloader 头的 flash 大小
# chip        : esptool --chip 参数
# ota         : 是否有 OTA 双槽（决定是否产出 ota-*.bin，本平台为 True）
# desc        : Release notes 里显示的目标设备名
#
# 偏移必须与 partitions/esp32s3_16mb.csv 逐一对应：
#   nvs 0x009000 / otadata 0x019000 / app0 0x020000 / app1 0x420000
#   userdata 0x820000 / spiffs 0xD20000 / coredump 0xFE0000
PLATFORMS = {
    "esp32s3": {
        "pio_env": "esp32s3",
        "chip": "esp32s3",
        "app_offset": 0x20000,
        "fs_offset": 0xD20000,
        "flash_size": "16MB",
        "ota": True,
        "desc": "ESP32-S3（16MB flash，双 OTA 槽位）",
    },
}


def run(cmd, **kw):
    print("[build_release] $ %s" % " ".join(str(c) for c in cmd), flush=True)
    subprocess.run(cmd, check=True, cwd=ROOT, **kw)


def esptool_cmd():
    """优先用 PATH 里的 esptool，其次退回 esptool.py；都没有则报错。

    同时返回 (命令, 是否 esptool>=5)。5.0 起子命令与选项从下划线改为连字符
    （merge_bin → merge-bin、--flash_mode → --flash-mode），用一个布尔量区分，
    避免在配置里写死某一个大版本。
    """
    for name in ("esptool", "esptool.py"):
        if shutil.which(name):
            try:
                out = subprocess.run([name, "version"], capture_output=True,
                                     text=True).stdout
                m = re.search(r"(\d+)\.(\d+)", out)
                modern = bool(m) and int(m.group(1)) >= 5
            except Exception:
                modern = False
            return [name], modern
    raise SystemExit("[build_release] 未找到 esptool，请先 pip install 'esptool>=4.8'")


def force_rebuild():
    """CI 里每次都用干净构建：清掉该项目的 .pio/build 与 PlatformIO 包缓存。

    本地重复构建时用得到缓存，但 CI 每次都是全新容器，缓存只会带来
    「上次的产物混进来」的风险（尤其换了分区表却仍命中旧 build 目录）。
    tools 目录里的工具链不删，那是平台包不是本项目产物。
    """
    for path in (os.path.join(ROOT, ".pio", "build"),
                 os.path.join(os.environ.get("PLATFORMIO_CORE_DIR",
                                             os.path.expanduser("~/.platformio")),
                              "cache")):
        shutil.rmtree(path, ignore_errors=True)
    print("[build_release] 🧹 已清理 .pio/build 与 PlatformIO 包缓存")


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def _dir_has_files(d):
    if not os.path.isdir(d):
        return False
    for _root, _dirs, _files in os.walk(d):
        if _files:
            return True
    return False


def ensure_data():
    """CNB 默认用部分/浅克隆，data/ 的 blob 可能没物化到工作树（导致 data/ 为空、
    LittleFS 镜像缺 Web UI）。这里在构建前显式从 git 还原 data/，触发缺失 blob 的
    拉取与写入。失败不致命：uploadfs_gz.py 在 buildfs 阶段还有「从 git 对象库提取」
    的兜底，二者同源（都依赖 git 对象可达）。
    """
    data_dir = os.path.join(ROOT, "data")
    if _dir_has_files(data_dir):
        return
    print("[build_release] 工作树 data/ 为空，尝试 git 物化")
    for cmd in (["git", "checkout", "--", "data"],
                ["git", "restore", "--source=HEAD", "--worktree", "--", "data"]):
        try:
            subprocess.run(cmd, cwd=ROOT, check=False,
                           capture_output=True, text=True)
        except Exception as e:
            print("[build_release] %s 异常: %s" % (" ".join(cmd), e))
        if _dir_has_files(data_dir):
            break
    if _dir_has_files(data_dir):
        print("[build_release] ✅ data/ 已物化")
    else:
        print("[build_release] ⚠️ git 物化 data/ 未成功，"
              "将依赖 uploadfs_gz 的 git 提取兜底")


def build_platform(platform, tag, skip_build=False):
    cfg = PLATFORMS[platform]
    env = cfg["pio_env"]
    build_dir = os.path.join(ROOT, ".pio", "build", env)
    os.makedirs(DIST, exist_ok=True)

    if not skip_build:
        ensure_data()
        force_rebuild()
        run(["pio", "run", "-e", env])

        # LittleFS 资源镜像：本仓库的 pre:script/uploadfs_gz.py 会在 buildfs 阶段
        # 自动把 data/ 里各文件 gzip 后打包进镜像，并在进程退出时还原 data/，
        # 因此这里直接调用 buildfs 即可，无需手动压缩/还原。
        run(["pio", "run", "-e", env, "-t", "buildfs"])

    et, modern = esptool_cmd()
    merge_bin = "merge-bin" if modern else "merge_bin"
    flash_mode = "--flash-mode" if modern else "--flash_mode"
    flash_freq = "--flash-freq" if modern else "--flash_freq"
    flash_size = "--flash-size" if modern else "--flash_size"
    # flash_mode/flash_freq 保持 keep，避免覆盖 bootloader 内部已编译的 flash
    # 参数字节，防止使 ESP-IDF 附加的 SHA256 摘要失效（否则 ROM 校验失败导致
    # ets_loader.c 78 崩溃并死循环重启）。
    keep = [flash_mode, "keep", flash_freq, "keep"]

    full = os.path.join(DIST, "full-%s-%s.bin" % (tag, platform))
    update = os.path.join(DIST, "update-%s-%s.bin" % (tag, platform))

    # full: bootloader + 分区表 + app（首次安装，会清空 NVS）
    run(et + ["--chip", cfg["chip"], merge_bin] + keep +
        [flash_size, cfg["flash_size"], "-o", full,
         "0x0", os.path.join(build_dir, "bootloader.bin"),
         "0x8000", os.path.join(build_dir, "partitions.bin"),
         hex(cfg["app_offset"]), os.path.join(build_dir, "firmware.bin")])

    # update: app + LittleFS 资源分区（从 app 偏移起，保留 NVS 与 WiFi 配置；
    # 注意会覆盖 userdata 与另一 OTA 槽，属于「整片重新烧录」用途）。
    run(et + ["--chip", cfg["chip"], merge_bin] + keep +
        [flash_size, cfg["flash_size"],
         "--target-offset", hex(cfg["app_offset"]), "-o", update,
         hex(cfg["app_offset"]), os.path.join(build_dir, "firmware.bin"),
         hex(cfg["fs_offset"]), os.path.join(build_dir, "littlefs.bin")])

    shutil.copy(os.path.join(build_dir, "littlefs.bin"),
                os.path.join(DIST, "littlefs-%s-%s.bin" % (tag, platform)))
    shutil.copy(os.path.join(build_dir, "firmware.bin"),
                os.path.join(DIST, "ota-%s-%s.bin" % (tag, platform)))
    shutil.copy(os.path.join(build_dir, "firmware.elf"),
                os.path.join(DIST, "firmware-%s-%s.elf" % (tag, platform)))

    # OTA 只对双 app 分区表的平台有意义，产物同样保留（占用很小）以便手动烧录
    print("[build_release] ✅ %s (%s) 产物已生成" % (platform, cfg["desc"]))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--platform", required=True, choices=sorted(PLATFORMS))
    ap.add_argument("--tag", required=True,
                    help="完整版本标识（= Release tag），如 1.0.0-20261004T121442-f2b513a。"
                         "它会原样进入产物文件名，必须与 Release 的 tag 一致")
    ap.add_argument("--skip-build", action="store_true", help="跳过编译，直接用已有 .pio 产物打包")
    args = ap.parse_args()

    build_platform(args.platform, args.tag, skip_build=args.skip_build)
    print("[build_release] 🎉 完成：%s" % args.platform)


if __name__ == "__main__":
    main()
