"""
data/ 资源自动 gzip 打包脚本（PlatformIO extra_scripts，pre: 方式加载）。

工作机制（复制到临时目录压缩，不再原地 swap）：
  1. 本次命令行包含 buildfs / uploadfs / uploadfsota 目标时，脚本加载阶段
     把 data/ 整体复制到可写临时目录，并在其中生成各文件的 gzip 版本
     <文件名>.gz（.ttf/.bin 等二进制资源保持原样）；
  2. 然后把 buildfs 的源目录（$PROJECT_DATA_DIR）重定向到该临时目录，
     LittleFS 镜像由只含 .gz 的临时目录构建，并上传到 spiffs 资源分区；
  3. 源 data/ 全程保持明文、可直接编辑，临时目录在进程退出时被清理。

这种「复制到可写目录再压缩」的方式，使脚本在 CI/容器（/workspace 常只读）
与本地都能一致工作——无需原地 rename/copytree data/，因此不会因只读文件系统
（EROFS/EBUSY）失败而把 data/ 弄空、导致镜像缺少 Web UI。

设备侧无需改动：ESPAsyncWebServer 的 serveStatic 与 WebPanel.sendFile()
都会优先探测同名 .gz 并附带 Content-Encoding: gzip，浏览器自动解压，
因此上传 .gz 既省 flash 又不影响开发时直接编辑源文件。

额外便利：pio run -t upload（烧录固件）完成后，自动再上传一次 FS 镜像
（子进程执行，内部仍走上面的临时目录压缩流程），与 esp32-sms-forwarding 一致。

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
import tempfile

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


def _dir_has_files(d):
    """目录存在且至少含一个文件（递归）。"""
    if not os.path.isdir(d):
        return False
    for _root, _dirs, _files in os.walk(d):
        if _files:
            return True
    return False


def _git_extract_data(project_dir, out_dir):
    """从 git 仓库把 data/ 原样提取到 out_dir（out_dir 下直接是 data/ 内容）。

    绕开可能为空/只读的工作树 data/ 挂载。优先 git archive（快），失败再逐文件
    git show（兼容浅克隆/部分克隆缺 blob 的情况）。返回提取到的文件数（0 表示失败）。
    """
    parent = os.path.dirname(out_dir)  # out_dir 形如 <tmp>/data
    os.makedirs(out_dir, exist_ok=True)
    # 1) git archive（普通完整 clone 即可拿到 blob）
    p = subprocess.run(
        "git archive HEAD data/ | tar -x -C %s" % parent,
        shell=True, cwd=project_dir, capture_output=True, text=True,
    )
    if p.returncode == 0 and _dir_has_files(out_dir):
        return sum(len(_[2]) for _ in os.walk(out_dir))
    # 2) 逐文件 git show（兼容浅克隆：archive 可能因缺 blob 失败）
    try:
        listing = subprocess.run(
            ["git", "ls-tree", "-r", "--name-only", "HEAD", "data/"],
            cwd=project_dir, capture_output=True, text=True, check=True,
        )
        rels = [l for l in listing.stdout.splitlines() if l]
        for rel in rels:
            blob = subprocess.run(
                ["git", "show", "HEAD:%s" % rel],
                cwd=project_dir, capture_output=True,
            )
            if blob.returncode != 0 or not blob.stdout:
                continue
            dst = os.path.join(parent, rel)  # rel 形如 data/xxx
            os.makedirs(os.path.dirname(dst), exist_ok=True)
            with open(dst, "wb") as f:
                f.write(blob.stdout)
        return sum(len(_[2]) for _ in os.walk(out_dir))
    except Exception as e:
        print("[gzip-data] git 逐文件提取异常: %s" % e)
        return 0


def _resolve_data_source(project_dir, data_dir):
    """确定可用的 data 源目录。

    CI/容器里 /workspace/data 常是只读挂载点且检出异常（目录在但为空），
    导致直接读取得到空目录、littlefs 镜像缺 Web UI。此函数优先用工作树 data/，
    若为空则依次回退：
      1) 上次 swap 残留的 .data_src；
      2) 从 git 仓库提取 data/ 到可写临时目录（绕开只读/空挂载的 data/）。
    返回 (source_dir, 需清理的临时根目录或 None)。
    """
    if _dir_has_files(data_dir):
        return data_dir, None
    print("[gzip-data] 工作树 data/ 为空/缺失，尝试自愈数据源")
    print("[gzip-data] data/ realpath=%s islink=%s"
          % (os.path.realpath(data_dir), os.path.islink(data_dir)))
    src_dir = os.path.join(project_dir, SRC_DIR_NAME)
    if _dir_has_files(src_dir):
        print("[gzip-data] 使用 .data_src 残骸作为数据源")
        return src_dir, None
    tmp = tempfile.mkdtemp(prefix="sms_data_git_")
    out = os.path.join(tmp, "data")
    n = _git_extract_data(project_dir, out)
    if n > 0:
        print("[gzip-data] 已从 git 提取 data/（%d 文件）到临时目录" % n)
        return out, tmp
    print("[gzip-data] ⚠️ git 提取 data/ 也失败，镜像将不含 Web UI")
    return data_dir, None


def _build_gz_tree(src_dir, out_dir):
    """把 src_dir 的内容写入 out_dir：文本资源 gzip 为 <名>.gz，.ttf/.bin 原样。

    不移动/改写源目录（src_dir 可只读），而是把结果输出到独立的可写 out_dir。
    用于 buildfs 前把 data/ 复制到可写临时目录并压缩的场景（CI/容器 /workspace
    常为只读，无法原地 swap）。
    """
    total_raw = total_gz = 0
    for root, dirs, files in os.walk(src_dir):
        # 防御：不打包上次异常 swap 残留的 .data_src 目录
        if SRC_DIR_NAME in dirs:
            dirs.remove(SRC_DIR_NAME)
        for fname in sorted(files):
            src_path = os.path.join(root, fname)
            rel = os.path.relpath(src_path, src_dir)
            with open(src_path, "rb") as f:
                content = f.read()
            # 字体等二进制资源（.ttf）保持原样，不 gzip：运行时需直接按名读取。
            if fname.endswith(".bin") or fname.endswith(".ttf"):
                dst_path = os.path.join(out_dir, rel)
                os.makedirs(os.path.dirname(dst_path), exist_ok=True)
                with open(dst_path, "wb") as df:
                    df.write(content)
                total_raw += len(content)
                continue
            # 其余文本资源（html/js/css）gzip 后上传，省 flash
            dst_path = os.path.join(out_dir, rel + ".gz")
            os.makedirs(os.path.dirname(dst_path), exist_ok=True)
            # mtime=0：去除时间戳，使 .gz 内容确定性可复现，避免每次构建的
            # LittleFS 镜像哈希都变化（利于缓存与产物校验）
            with gzip.GzipFile(dst_path, "wb", mtime=0) as gf:
                gf.write(content)
            total_raw += len(content)
            total_gz += os.path.getsize(dst_path)
    return total_raw, total_gz


def _prepare_gzip_data(env):
    """加载阶段：把 data/ 复制到可写临时目录并生成 .gz，再让 buildfs 从临时目录读取。

    不原地 swap：无论 data/ 是否只读（CI/容器常见），都能正确产出压缩镜像。
    若工作树 data/ 在 CI 里是空的只读挂载（检出异常），会自动从 git 仓库提取
    真实 data/ 到可写临时目录，保证镜像一定含 Web UI。源 data/ 始终不被改动。
    """
    project_dir = env.subst("$PROJECT_DIR")
    data_dir = os.path.join(project_dir, "data")

    src, tmp_to_clean = _resolve_data_source(project_dir, data_dir)
    if not _dir_has_files(src):
        print("[gzip-data] ❌ 找不到 data/ 文件（工作树与 git 均为空），"
              "构建出的 littlefs 镜像将不含 Web UI！")
        return False

    # 始终复制到可写临时目录再压缩，再让 buildfs 从该目录读取。
    # 这样 CI/本地行为完全一致，且彻底绕开对 data/ 是否可写的依赖。
    tmp_root = tempfile.mkdtemp(prefix="sms_data_gz_")
    gz_dir = os.path.join(tmp_root, "data")
    os.makedirs(gz_dir)
    total_raw, total_gz = _build_gz_tree(src, gz_dir)
    print("[gzip-data] 已复制 data/ 到可写临时目录并 gzip 压缩: "
          "%d -> %d bytes" % (total_raw, total_gz))
    # esp32 的 buildfs 源目录由 $PROJECT_DATA_DIR（兼容旧版 $DATA_DIR）决定
    env.Replace(PROJECT_DATA_DIR=gz_dir)
    env.Replace(DATA_DIR=gz_dir)
    atexit.register(shutil.rmtree, tmp_root, ignore_errors=True)
    if tmp_to_clean:
        atexit.register(shutil.rmtree, tmp_to_clean, ignore_errors=True)
    return True


# ---- 机制 1：请求 FS 目标时，加载阶段即把 data/ 复制到临时目录并 gzip 压缩，
# 再把 buildfs 源目录重定向到临时目录；源 data/ 不被改动，退出时清理临时目录 ----
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
