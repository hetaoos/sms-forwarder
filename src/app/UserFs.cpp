#include "app/UserFs.h"

#include "log/LogStore.h"

#include <LittleFS.h>

#include <Arduino.h>

namespace {

// 本模块日志出口：全局日志环（串口由 LogStore 统一镜像输出）
LogStore &_log = devLog();

}  // namespace

namespace userfs {

// 全局唯一挂载实例：App 启动时初始化，SmsStore 等模块直接复用
static fs::LittleFSFS g_fs;
static bool g_mounted = false;

// 挂载用户数据分区；重复调用返回首次的结果
bool begin() {
  if (g_mounted) return true;
  // formatOnFail=true 只在注册返回 ESP_FAIL（分区存在但无有效 FS）时自动格式化；
  // 若仍失败（如全新分区首挂偶发失败），再显式格式化重试一次兜底。
  g_mounted = g_fs.begin(true, "/data", 10, "userdata");
  if (!g_mounted) {
    _log.addf(LogLevel::kWarn, "fs", "userdata 挂载失败，正在格式化并重试");
    if (g_fs.format()) {
      g_mounted = g_fs.begin(false, "/data", 10, "userdata");
    }
  }
  if (g_mounted) {
    _log.addf(LogLevel::kInfo, "fs", "userdata 已挂载于 /data（%u 字节）",
              (unsigned)userfs::totalBytes());
  } else {
    _log.addf(LogLevel::kError, "fs",
              "userdata 挂载失败（检查分区表是否含 userdata 分区）");
  }
  return g_mounted;
}

// 挂载后的文件系统实例
fs::FS &fs() { return g_fs; }

// 分区总容量字节（totalBytes/usedBytes 不在 fs::FS 基类，需具体类型）
size_t totalBytes() { return g_mounted ? g_fs.totalBytes() : 0; }

// 已用字节
size_t usedBytes() { return g_mounted ? g_fs.usedBytes() : 0; }

// 是否已挂载
bool mounted() { return g_mounted; }

// 格式化用户数据分区：卸载 -> 格式化 -> 重新挂载
bool format() {
  if (g_mounted) {
    g_fs.end();
    g_mounted = false;
  }
  bool ok = g_fs.format();
  if (ok) g_mounted = g_fs.begin(true, "/data", 10, "userdata");
  _log.addf(LogLevel::kInfo, "fs", ok ? "userdata 已格式化并重新挂载"
                                      : "userdata 格式化失败");
  return g_mounted;
}

}  // namespace userfs
