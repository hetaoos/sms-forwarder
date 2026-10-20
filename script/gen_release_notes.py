#!/usr/bin/env python3
"""
生成 Release 描述（release_notes.md）。

从 dist/ 目录扫描各平台产物计算 SHA256，并按平台分组，生成带烧录说明的
Markdown。本仓库目前只有一个硬件平台（esp32s3），产物靠文件名里的平台后缀区分。

入参 tag 就是 Release 的 tag，产物文件名里的 tag 用同一个值 —— 设备端 OTA 会
照这个规律拼下载地址：
  <Release tag>/ota-<Release tag>-esp32s3.bin
所以这里不存在「文件名 tag」与「Release tag」不一致的情况，别把它们拆开。
"""

import glob
import hashlib
import os
import sys
from datetime import datetime, timedelta, timezone

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DIST = os.path.join(ROOT, "dist")

# 与 platformio.ini / script/build_release.py 的 key 保持一致
PLATFORMS = {
    "esp32s3": {
        "desc": "ESP32-S3（16MB flash，双 OTA 槽位）",
        "chip": "esp32s3",
        "ota": True,
        "flash": "16MB",
        "full_off": "0x0",
        "update_off": "0x20000",
        "fs_off": "0xD20000",
        "app1_off": "0x420000",
        "userdata_off": "0x820000",
    },
}


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def find(tag, kind, platform):
    """在 dist/ 下找 <kind>-<tag>-<platform>.<ext>，返回路径或 None。"""
    for ext in ("bin", "elf"):
        p = os.path.join(DIST, "%s-%s-%s.%s" % (kind, tag, platform, ext))
        if os.path.exists(p):
            return p
    return None


def main():
    tag = sys.argv[1]
    # Commit 与版本时间：优先取环境变量（由 gen_version.sh / CI 注入），
    # 缺失时按本地 git（HEAD 提交时间）与当前 CST 时间兜底，保证脚本可独立运行。
    import subprocess

    full_sha = os.environ.get("FULL_SHA", "")
    if not full_sha:
        try:
            full_sha = subprocess.check_output(
                ["git", "rev-parse", "HEAD"], stderr=subprocess.DEVNULL
            ).decode().strip()
        except Exception:
            full_sha = "unknown"
    # 时间戳与 gen_version.sh 同源：HEAD 提交时间（CST）；git 不可用才退回当前时间
    timestamp = os.environ.get("TIMESTAMP", "")
    if not timestamp:
        try:
            timestamp = subprocess.check_output(
                ["git", "log", "-1", "--format=%cd",
                 "--date=format-local:%Y-%m-%dT%H:%M:%S+08:00"],
                env=dict(os.environ, TZ="Asia/Shanghai"),
                stderr=subprocess.DEVNULL,
            ).decode().strip()
        except Exception:
            timestamp = ""
        if not timestamp:
            timestamp = datetime.now(timezone(timedelta(hours=8))).strftime(
                "%Y-%m-%dT%H:%M:%S+08:00")
    out = sys.argv[2] if len(sys.argv) > 2 else os.path.join(ROOT, "release_notes.md")

    lines = []
    lines.append("## 固件版本: %s\n" % tag)
    lines.append("| 属性 | 值 |")
    lines.append("|------|-----|")
    lines.append("| Commit | %s |" % full_sha)
    lines.append("| 提交时间 | %s |" % timestamp)
    lines.append("| 支持平台 | %s |" % "、".join(PLATFORMS))
    lines.append("")

    # 每个平台一组文件说明
    lines.append("## 文件说明\n")
    lines.append("> 文件名末尾的 `-esp32s3` 为平台标识。\n")
    for key, cfg in PLATFORMS.items():
        lines.append("### %s（`%s`）\n" % (cfg["desc"], key))
        lines.append("| 文件名 | 用途 | 写入偏移 |")
        lines.append("|--------|------|----------|")
        lines.append("| full-%s-%s.bin | 首次安装（含 bootloader + 分区表，**会清空所有配置**） | `%s` |" % (tag, key, cfg["full_off"]))
        lines.append("| update-%s-%s.bin | 固件升级（从 `%s` 起，含 Web UI 资源分区；会覆盖另一 OTA 槽与 userdata，属整片重烧） | `%s` |" % (tag, key, cfg["update_off"], cfg["update_off"]))
        lines.append("| littlefs-%s-%s.bin | 单独更新 Web UI 资源分区（spiffs） | `%s` |" % (tag, key, cfg["fs_off"]))
        ota_note = ("设备 OTA 在线升级专用（仅应用分区，写入 ota_0/ota_1 任一槽位，"
                    "保留 NVS 与 userdata）")
        lines.append("| ota-%s-%s.bin | %s | OTA 分区（`%s` 或 `%s`） |" % (tag, key, ota_note, cfg["update_off"], cfg["app1_off"]))
        lines.append("| firmware-%s-%s.elf | 调试符号文件，配合 coredump.bin 离线分析用 | — |" % (tag, key))
        lines.append("")
        lines.append("> userdata（用户数据，%s）刷固件后保留；如需清空请整片烧录 `full-*.bin`。" % cfg["userdata_off"])
        lines.append("")

    lines.append("## 烧录说明\n")
    lines.append("### 方式一：图形化烧录（推荐新手，零安装）\n")
    lines.append("使用 **ESPConnect** 浏览器工具，无需安装任何软件：")
    lines.append("")
    lines.append("> 工具地址：https://thelastoutpostworkshop.github.io/ESPConnect/")
    lines.append("> 浏览器要求：Chrome 或 Edge（Firefox / Safari 不支持 Web Serial API）")
    lines.append("")
    lines.append("1. 波特率选择 `921600`，点击「连接」，选择 \"USB JTAG/serial debug unit\" 设备")
    lines.append("2. 「闪存工具」→「烧录固件」，**首次安装**烧录地址填 `0x0` 并上传 `full.bin`")
    lines.append("3. **固件升级**（保留配置）烧录地址填 `0x20000` 并上传 `ota.bin`（仅应用分区）")
    lines.append("")
    lines.append("### 方式二：命令行烧录（esptool）\n")
    lines.append("```bash")
    lines.append('pip install "esptool>=4.8"')
    lines.append("```")
    lines.append("")
    for key, cfg in PLATFORMS.items():
        lines.append("**%s**：" % cfg["desc"])
        lines.append("")
        lines.append("```bash")
        lines.append("# 首次安装（清空 NVS）")
        lines.append("esptool --chip %s --baud 921600 write_flash %s full-%s-%s.bin" % (cfg["chip"], cfg["full_off"], tag, key))
        lines.append("# 仅升级应用（保留 NVS / userdata，可用 OTA 或手动烧录）")
        lines.append("esptool --chip %s --baud 921600 write_flash %s ota-%s-%s.bin" % (cfg["chip"], cfg["update_off"], tag, key))
        lines.append("# 仅更新 Web UI 资源分区")
        lines.append("esptool --chip %s --baud 921600 write_flash %s littlefs-%s-%s.bin" % (cfg["chip"], cfg["fs_off"], tag, key))
        lines.append("```")
        lines.append("")

    # ---- 先生成 SHA256 表：产物由各平台 job 落到共享的 dist/，这里只做扫描 ----
    hashes = ["## SHA256 校验", "", "| 文件名 | SHA256 |", "|--------|--------|"]
    for key in PLATFORMS:
        for kind in ("full", "update", "littlefs", "ota"):
            p = find(tag, kind, key)
            if p:
                hashes.append("| %s | %s |" % (os.path.basename(p), sha256(p)))
        p = find(tag, "firmware", key)
        if p:
            hashes.append("| %s | %s |" % (os.path.basename(p), sha256(p)))
    if len(hashes) == 4:
        hashes.append("| — | dist/ 为空，未找到任何产物 |")
    hashes.append("")
    lines.extend(hashes)

    with open(out, "w") as f:
        f.write("\n".join(lines))
    print("[gen_release_notes] ✅ 已生成 %s" % out)


if __name__ == "__main__":
    main()
