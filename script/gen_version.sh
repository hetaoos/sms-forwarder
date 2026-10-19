#!/usr/bin/env bash
# 生成构建元数据，并写入 stdout 键值对（供 print_version_tag.sh 解析）。
#
# 版本标识只有一套，别再拆：
#   RELEASE_TAG  = <VERSION>-<YYYYMMDD>T<HHMMSS>-<short_sha>
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
YYYYMMDD=$(TZ=Asia/Shanghai date +%Y%m%d)
HHMMSS=$(TZ=Asia/Shanghai date +%H%M%S)

APP_BUILD_TIMESTAMP="${YYYYMMDD}T${HHMMSS}"
# 完整标识：VERSION + 构建时间 + 短 SHA。既是 Release tag，也是产物名里的 <tag>。
RELEASE_TAG="${PREFIX}-${APP_BUILD_TIMESTAMP}-${SHA_SHORT}"
APP_VERSION="${PREFIX}-${SHA_SHORT}"
TIMESTAMP=$(TZ=Asia/Shanghai date +%Y-%m-%dT%H:%M:%S+08:00)

cat <<ENVEOF
RELEASE_TAG=${RELEASE_TAG}
APP_VERSION=${APP_VERSION}
APP_BUILD_TIMESTAMP=${APP_BUILD_TIMESTAMP}
SHA_SHORT=${SHA_SHORT}
FULL_SHA=${FULL_SHA}
TIMESTAMP=${TIMESTAMP}
ENVEOF
