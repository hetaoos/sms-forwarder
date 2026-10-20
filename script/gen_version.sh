#!/usr/bin/env bash
# 生成版本元数据，并写入 stdout 键值对（供 print_version_tag.sh 解析）。
#
# 版本标识只有一套，别再拆：
#   RELEASE_TAG  = <VERSION>-<YYYYMMDD>T<HHMMSS>-<short_sha>
#                  时间部分取当前分支 HEAD 的提交时间（committer date，固定 CST
#                  时区渲染），不是编译时刻 —— 同一份代码重复构建得到同一个 tag。
#                  Release 的 tag，同时也是产物文件名里的 <tag>。设备端（若启用 OTA）
#                  直接拿它拼下载地址、也是「有没有新版本」的比较依据 —— 三处必须是
#                  同一个值，否则就会出现「检查到新版本但下载 404」。
#   APP_VERSION  = <VERSION>-<short_sha>，编进 esp_app_desc 供 esp_ota 等库判版本。
#
# 用法： ./script/gen_version.sh
set -euo pipefail

# 允许在 git 仓库根目录之外调用
if [ -f VERSION ]; then
  PREFIX=$(tr -d '[:space:]' < VERSION)
else
  PREFIX="1"
fi
[ -n "$PREFIX" ] || PREFIX="unknown"

SHA_SHORT=$(git rev-parse --short=7 HEAD 2>/dev/null || echo "local")
FULL_SHA=$(git rev-parse HEAD 2>/dev/null || echo "local")
# 时间戳取当前分支 HEAD 的提交时间（committer date），而不是编译时刻：
#   - 同一份代码编译多少次 tag 都不变，产物与 Release 可复现；
#   - CI 与本地拼出的 tag 只由「代码」决定，不依赖构建机器的墙上时钟。
# --date=format-local: 让 git 按 TZ 指定的时区渲染，macOS / Linux 行为一致。
# 取不到时（无 git、空仓库、源码包构建）退回当前系统时间，保证 tag 仍有唯一性。
APP_BUILD_TIMESTAMP=$(TZ=Asia/Shanghai git log -1 --format=%cd --date=format-local:%Y%m%dT%H%M%S 2>/dev/null || true)
if [ -n "$APP_BUILD_TIMESTAMP" ]; then
  TIMESTAMP=$(TZ=Asia/Shanghai git log -1 --format=%cd --date=format-local:%Y-%m-%dT%H:%M:%S+08:00 2>/dev/null || true)
  [ -n "$TIMESTAMP" ] || TIMESTAMP=$(TZ=Asia/Shanghai date +%Y-%m-%dT%H:%M:%S+08:00)
else
  APP_BUILD_TIMESTAMP=$(TZ=Asia/Shanghai date +%Y%m%dT%H%M%S)
  TIMESTAMP=$(TZ=Asia/Shanghai date +%Y-%m-%dT%H:%M:%S+08:00)
  echo "[gen_version] ⚠️  读取不到 git 提交时间，退回当前系统时间" >&2
fi

# 完整标识：VERSION + 提交时间 + 短 SHA。既是 Release tag，也是产物名里的 <tag>。
RELEASE_TAG="${PREFIX}-${APP_BUILD_TIMESTAMP}-${SHA_SHORT}"
APP_VERSION="${PREFIX}-${SHA_SHORT}"

cat <<ENVEOF
RELEASE_TAG=${RELEASE_TAG}
APP_VERSION=${APP_VERSION}
APP_BUILD_TIMESTAMP=${APP_BUILD_TIMESTAMP}
SHA_SHORT=${SHA_SHORT}
FULL_SHA=${FULL_SHA}
TIMESTAMP=${TIMESTAMP}
ENVEOF
