# ESP32-S3 固件构建环境
#
# 预装 PlatformIO + esptool，避免每次流水线都走 pip 安装。
# CNB 会按文件内容计算镜像哈希并缓存，仅当本文件变化时才重建。
FROM python:3.12-slim

ENV PIP_NO_CACHE_DIR=1 \
    PIP_DISABLE_PIP_VERSION_CHECK=1 \
    PLATFORMIO_CORE_DIR=/root/.platformio

RUN apt-get update \
 && apt-get install -y --no-install-recommends git ca-certificates \
 && rm -rf /var/lib/apt/lists/*

# 必须用系统级 pip 安装：CNB 的 PlatformIO 工具链缓存卷（.cnb.yml 里声明的
# /root/.platformio）是跨流水线复用的，容器重建后 venv 里的 python3.x 等内容会
# 残缺/与镜像版本不匹配。PlatformIO 启动时发现 penv 异常就会跑 `venv --clear` 重建，
# 而 venv 模块不允许指向符号链接的 /usr/local/bin/python3.x（python:3.12-slim 默认如此），
# 于是报 `Error: [Errno 17] File exists: '/root/.platformio/penv/include/python3.12'`
# 并让构建失败。系统级 pip 安装不依赖 penv，可彻底绕开这条路径。
# 另：构建缓存卷里可能有并发任务残留的半成品 penv，故构建开始时先清掉。
RUN apt-get update \
 && apt-get install -y --no-install-recommends git ca-certificates python3-venv \
 && rm -rf /var/lib/apt/lists/* \
 && rm -rf /root/.platformio/penv

# PlatformIO 6.x 与 esptool 5.x（5.x 的 merge-bin 连字符选项在本项目脚本里已兼容）
RUN pip install --break-system-packages "platformio>=6.1" "esptool>=4.8" \
 && pio --version && esptool version
