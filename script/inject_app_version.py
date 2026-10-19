Import("env")
import os
import subprocess
from datetime import datetime, timezone, timedelta

project_dir = env["PROJECT_DIR"]


def cst_now():
    return datetime.now(timezone(timedelta(hours=8)))


def read_version_prefix():
    version_file = os.path.join(project_dir, "VERSION")
    if os.path.exists(version_file):
        prefix = open(version_file).read().strip()
        if prefix:
            return prefix
        print("[inject_app_version] ⚠️  VERSION 文件为空")
    return "1"


# --- 构建时间戳 ---
# CI 注入的 APP_BUILD_TIMESTAMP 形如 20261004T121442（CST）。它的两个用途：
#   1. 与短 SHA 一起拼出 RELEASE_TAG（见下）
#   2. 拆成 APP_BUILD_DATE / APP_BUILD_TIME 存进 esp_app_desc
ts = os.environ.get("APP_BUILD_TIMESTAMP", "").strip()

# --- 短 SHA ---
# 单拎一个变量出来。Ota::version() 要拼完整的版本标识，靠字符串去反解
# APP_VERSION 里的 '-' 分隔既脆又看不懂。
app_version_sha = os.environ.get("SHA_SHORT", "").strip()
if not app_version_sha:
    try:
        app_version_sha = subprocess.check_output(
            ["git", "rev-parse", "--short=7", "HEAD"],
            cwd=project_dir, stderr=subprocess.DEVNULL
        ).decode().strip()
    except Exception:
        app_version_sha = "local"
        print("[inject_app_version] ⚠️  git not available, using '-local' suffix")

# --- APP_VERSION ---
# 编进 esp_app_desc（esp_ota / 系统信息用）。OTA 的版本比较不用它，见 OTA_RELEASE_TAG。
app_version = os.environ.get("APP_VERSION", "").strip()
if not app_version:
    app_version = f"{read_version_prefix()}-{app_version_sha}"

# --- OTA_RELEASE_TAG ---
# 设备自报的版本、OTA 的下载地址与版本比较全都基于它，必须与 CI 的 Release tag
# 逐字一致：<VERSION>-<构建时间>-<短 SHA>，如 1.0.0-20261004T121442-f2b513a。
# CI 通过 RELEASE_TAG 注入；本地编译按下述规则自拼一份，保证能过编译。
#
# ⚠️ 若这里退回成 VERSION 字面值（如 "1.0.0"），设备就会去
# /releases/download/1.0.0/ota-1.0.0-esp32s3.bin 取固件 —— 而 CI 产出的文件名是
# ota-1.0.0-<时间>-<sha>-esp32s3.bin，必然 404。缺值时的兜底也必须保持同一套格式。
ota_release_tag = os.environ.get("RELEASE_TAG", "").strip()
if not ota_release_tag:
    local_ts = ts if (ts and len(ts) >= 15 and ts[8] == 'T') else cst_now().strftime("%Y%m%dT%H%M%S")
    ota_release_tag = f"{read_version_prefix()}-{local_ts}-{app_version_sha}"
    print(f"[inject_app_version] ⚠️  未注入 RELEASE_TAG，本地按 CI 规则拼出 {ota_release_tag}")

# --- APP_BUILD_DATE / APP_BUILD_TIME ---
if ts and len(ts) >= 15 and ts[8] == 'T':
    app_build_date = f"{ts[0:4]}-{ts[4:6]}-{ts[6:8]}"
    app_build_time = f"{ts[9:11]}:{ts[11:13]}:{ts[13:15]}"
else:
    if ts:
        print(f"[inject_app_version] ⚠️  APP_BUILD_TIMESTAMP 格式非法: '{ts}'，改用当前 CST 时间")
    now = cst_now()
    app_build_date = now.strftime("%Y-%m-%d")
    app_build_time = now.strftime("%H:%M:%S")

# --- OTA 发布源地址 ---
# 两个地址分工不同，都得注入：
#   OTA_RELEASES_BASE_URL   产物下载用，形如 .../<repo>/-/releases
#   OTA_LATEST_VERSION_URL  「检查更新」用，形如 .../<repo>/-/badge/release
# 注意徽章不在 /-/releases 下面，而是挂在仓库根上，所以拼不出来，只能各给一个。
# 默认值按当前仓库推出来，本地编译也能过；CI 显式传入优先（仓库改名时 CI 跟着改）。
DEFAULT_REPO_SLUG = "null/sms-forwarder"


def ota_urls(repo_slug):
    return (f"https://cnb.cool/{repo_slug}/-/releases",
            f"https://cnb.cool/{repo_slug}/-/badge/release")


repo_slug = os.environ.get("CNB_REPO_SLUG", "").strip() or DEFAULT_REPO_SLUG
default_releases_url, default_badge_url = ota_urls(repo_slug)

releases_base_url = os.environ.get("OTA_RELEASES_BASE_URL", "").strip() or default_releases_url
latest_version_url = os.environ.get("OTA_LATEST_VERSION_URL", "").strip() or default_badge_url

# --- Inject ---
# APP_VERSION_SHA 不再单独注入：它已经是 OTA_RELEASE_TAG 的一部分，固件里
# 没有任何地方单独用它了（Ota::version() 直接返回 tag）。留一个没人读的宏
# 只会让人以为「短 SHA 有两处来源」。
env.Append(CPPDEFINES=[
    ("APP_VERSION",     '\\"' + app_version     + '\\"'),
    ("OTA_RELEASE_TAG", '\\"' + ota_release_tag + '\\"'),
    ("OTA_RELEASES_BASE_URL",   '\\"' + releases_base_url   + '\\"'),
    ("OTA_LATEST_VERSION_URL",  '\\"' + latest_version_url  + '\\"'),
    ("APP_BUILD_DATE",  '\\"' + app_build_date  + '\\"'),
    ("APP_BUILD_TIME",  '\\"' + app_build_time  + '\\"'),
])

print(f"[inject_app_version] 🏷️  APP_VERSION={app_version} (sha={app_version_sha})")
print(f"[inject_app_version] 🚀 OTA_RELEASE_TAG={ota_release_tag}")
print(f"[inject_app_version] 🔗 OTA_RELEASES_BASE_URL={releases_base_url}")
print(f"[inject_app_version] 🔗 OTA_LATEST_VERSION_URL={latest_version_url}")
print(f"[inject_app_version] 📅 APP_BUILD_DATE={app_build_date}")
print(f"[inject_app_version] 🕐 APP_BUILD_TIME={app_build_time}")
