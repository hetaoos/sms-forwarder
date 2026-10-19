"""
data/ 资源自动 gzip 打包脚本（PlatformIO extra_scripts，pre: 方式加载）。

工作机制（目录交换）：
  1. 本次命令行包含 buildfs / uploadfs / uploadfsota 目标时，脚本加载阶段
     立即把 data/ 整体换名为 .data_src/，并在新的 data/ 中生成各文件的
     gzip 版本 <文件名>.gz（原文件只在 .data_src/ 中保留）；
  2. LittleFS 镜像随后由只含 .gz 的 data/ 构建，并上传到 spiffs 资源分区；
  3. 进程退出时（atexit，构建成功/失败都会执行）删除临时 data/ 并把
     .data_src/ 换回 data/，仓库里始终保持可读的源文件。

设备侧无需改动：ESPAsyncWebServer 的 serveStatic 与 WebPanel.sendFile()
都会优先探测同名 .gz 并附带 Content-Encoding: gzip，浏览器自动解压，
因此上传 .gz 既省 flash 又不影响开发时直接编辑源文件。

健壮性：若进程被强行杀死（SIGKILL 等）导致 .data_src/ 残留，下一次运行
任何 FS 目标时会先自动恢复现场，不会丢失源文件。

额外便利：pio run -t upload（烧录固件）完成后，自动再上传一次 FS 镜像
（子进程执行，内部仍走上面的 gzip 流程），与 esp32-sms-forwarding 一致。

用法：
    pio run -e esp32s3 -t buildfs    # 只构建镜像（自动 gzip，可本地验证）
    pio run -e esp32s3 -t uploadfs   # 构建并上传资源（自动 gzip）
    pio run -e esp32s3 -t upload     # 烧录固件，并自动上传资源
"""

Import("env")  # noqa: F821 — SCons 注入

import atexit
import gzip
import os
import shutil
import subprocess
import sys

# 暂存原始文件的目录名（位于项目根，仅 FS 构建期间存在）
SRC_DIR_NAME = ".data_src"

# 会触发 FS 镜像构建/上传的 PlatformIO 目标
FS_TARGETS = {"uploadfs", "uploadfsota", "buildfs"}

# SCons 注入的本次命令行目标（如 ['uploadfs']）
_COMMAND_TARGETS = set(COMMAND_LINE_TARGETS)  # noqa: F821 — SCons 全局


def _empty_dir(path):
    """删除 path 下的全部内容，但保留 path 目录本身。

    用于 data/ 在容器中是挂载点（mount point）的场景：挂载点目录项本身不能被
    rename / rmdir（会报 EBUSY），只能修改其内容，因此清空时务必保留目录本身。
    对可能嵌套的挂载点子目录同样做递归兜底。
    """
    if not os.path.isdir(path):
        return
    for entry in os.scandir(path):
        p = entry.path
        try:
            if entry.is_dir(follow_symlinks=False):
                shutil.rmtree(p, ignore_errors=True)
                if os.path.exists(p):
                    _empty_dir(p)
                    try:
                        os.rmdir(p)
                    except OSError:
                        pass
            else:
                os.remove(p)
        except OSError:
            pass


def _restore(data_dir, src_dir):
    """删除临时 data/，把 .data_src/ 换回 data/。

    本地优先用 rename（快）；容器内 data/ 多为挂载点，rename/rmtree 整个目录会报
    EBUSY，此时退化为「清空 data/ 内容 + 把 .data_src/ 内容拷回」的分支，保留
    data/ 目录本身。
    """
    if not os.path.isdir(src_dir):
        return
    try:
        shutil.rmtree(data_dir, ignore_errors=True)
        os.rename(src_dir, data_dir)
    except OSError as e:
        print("[gzip-data] restore rename 失败 (%s)，改用挂载点兼容分支" % e)
        _empty_dir(data_dir)
        shutil.copytree(src_dir, data_dir, dirs_exist_ok=True)
    shutil.rmtree(src_dir, ignore_errors=True)


def _swap_to_gz(data_dir, src_dir):
    """data/ -> .data_src/，新 data/ 中只放 <文件>.gz；返回 (原始字节, 压缩后字节)。

    本地优先用 rename（原子、快）；容器内 data/ 常为挂载点，rename 会报 EBUSY，
    此时退化为「拷贝原件到 .data_src/ + 清空 data/ 内容再写入 .gz」的分支，保留
    data/ 目录本身。两种分支都把原始文件留在 .data_src/，由 _restore 还原。
    """
    # 防御：若上次异常退出残留了 .data_src/，先清掉，避免下方 rename/copytree 冲突
    if os.path.exists(src_dir):
        shutil.rmtree(src_dir, ignore_errors=True)
    try:
        os.rename(data_dir, src_dir)  # 原件整体移走（原子操作，本地快）
        renamed = True
    except OSError as e:
        print("[gzip-data] swap rename 失败 (%s)，改用挂载点兼容分支" % e)
        shutil.copytree(data_dir, src_dir)
        renamed = False

    if not renamed:
        # data/ 是挂载点/特殊文件系统：不能 rename/rmtree 整个目录本身，
        # 只能清空其内容后原地复用，避免 EBUSY。
        _empty_dir(data_dir)
        os.makedirs(data_dir, exist_ok=True)

    total_raw = total_gz = 0
    for root, _dirs, files in os.walk(src_dir):
        for fname in sorted(files):
            src_path = os.path.join(root, fname)
            rel = os.path.relpath(src_path, src_dir)
            with open(src_path, "rb") as f:
                content = f.read()
            # 字体等二进制资源（.bin）保持原样上传，不 gzip：避免运行时解压依赖，
            # 也能被 LittleFS 直接按名读取（如 /cnfont.bin）。
            if fname.endswith(".bin"):
                dst_path = os.path.join(data_dir, rel)
                os.makedirs(os.path.dirname(dst_path), exist_ok=True)
                with open(dst_path, "wb") as df:
                    df.write(content)
                total_raw += len(content)
                continue
            # 其余文本资源（html/js/css）gzip 后上传，省 flash
            dst_path = os.path.join(data_dir, rel + ".gz")
            os.makedirs(os.path.dirname(dst_path), exist_ok=True)
            # mtime=0：去除时间戳，使 .gz 内容确定性可复现，避免每次构建的
            # LittleFS 镜像哈希都变化（利于缓存与产物校验）
            with gzip.GzipFile(dst_path, "wb", mtime=0) as gf:
                gf.write(content)
            total_raw += len(content)
            total_gz += os.path.getsize(dst_path)
    return total_raw, total_gz


def _prepare_gzip_data(env):
    """加载阶段：必要时做 gzip 交换并注册退出恢复；返回是否发生了交换。"""
    project_dir = env.subst("$PROJECT_DIR")
    data_dir = os.path.join(project_dir, "data")
    src_dir = os.path.join(project_dir, SRC_DIR_NAME)

    # 上次异常退出的现场：优先恢复。即便 data/ 已不存在也要还原，否则源文件会
    # 永久困在 .data_src/ 中（例如本地 rename 分支崩溃在「data/ 已被移走、尚未
    # 重建」的窗口时，data/ 缺失但 .data_src/ 持有唯一副本）。
    if os.path.isdir(src_dir):
        _restore(data_dir, src_dir)
        print("[gzip-data] recovered leftover %s from previous run" % SRC_DIR_NAME)

    if not os.path.isdir(data_dir):
        print("[gzip-data] data/ not found, skip")
        return False

    total_raw, total_gz = _swap_to_gz(data_dir, src_dir)
    print("[gzip-data] packed %d -> %d bytes (gzip); image will contain .gz only"
          % (total_raw, total_gz))

    atexit.register(_restore, data_dir, src_dir)
    atexit.register(print, "[gzip-data] original files restored to data/")
    return True


# ---- 机制 1：请求 FS 目标时，加载阶段即完成 gzip 交换，退出时恢复 ----
# 交换发生在任何构建决策之前，因此镜像一定由 .gz 版本生成；
# atexit 保证构建成功/失败/异常退出后 data/ 都能恢复。
if FS_TARGETS & _COMMAND_TARGETS:
    _prepare_gzip_data(env)


# ---- 机制 2：烧录固件后自动上传 FS 镜像（子进程，内部走机制 1）----
def _upload_fs_after_upload(source, target, env):
    if FS_TARGETS & _COMMAND_TARGETS:
        return  # 已显式请求 FS 目标，避免重复上传
    print("\n[gzip-data] uploading filesystem image...")
    ret = subprocess.call(
        [sys.executable, "-m", "platformio", "run", "--target", "uploadfs",
         "--environment", env["PIOENV"]],
        cwd=env.subst("$PROJECT_DIR"),
    )
    if ret == 0:
        print("[gzip-data] filesystem upload OK")
    else:
        print("[gzip-data] filesystem upload failed (code %d)" % ret)


env.AddPostAction("upload", _upload_fs_after_upload)
