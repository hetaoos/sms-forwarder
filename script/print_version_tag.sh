#!/usr/bin/env bash
# 生成版本元数据，以 CNB 的 ##[set-output key=value] 格式输出，
# 由 .cnb.yml 的 resolve-version 任务 exports 成 Pipeline 级环境变量。
#
# 版本算法与 script/gen_version.sh 完全一致，避免两处漂移。
# 输出项（键名即 exports 的 from-key）：
#   release_tag   RELEASE_TAG  Release 的 tag，同时也是产物文件名里的 <tag>，
#                             形如 1.0.0-20260423T210356-551f992。设备端 OTA 用
#                             它拼下载地址、也用它比较新旧 —— 这三处必须同值。
#   short_sha     SHA_SHORT    短 SHA
#   full_sha      FULL_SHA     完整 commit SHA，供 release notes 显示
#   timestamp     TIMESTAMP    构建时间（CST，ISO8601）
set -euo pipefail

ENV_FILE=$(mktemp)
trap 'rm -f "$ENV_FILE"' EXIT

bash "$(dirname "$0")/gen_version.sh" > "$ENV_FILE"

get() { sed -n "s/^$1=//p" "$ENV_FILE"; }

echo "##[set-output release_tag=$(get RELEASE_TAG)]"
echo "##[set-output app_version=$(get APP_VERSION)]"
echo "##[set-output app_build_timestamp=$(get APP_BUILD_TIMESTAMP)]"
echo "##[set-output short_sha=$(get SHA_SHORT)]"
echo "##[set-output full_sha=$(get FULL_SHA)]"
echo "##[set-output timestamp=$(get TIMESTAMP)]"
