#include "app/UserFs.h"

#include <LittleFS.h>

#include <Arduino.h>

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
    Serial.println("[fs] userdata mount failed, formatting and retrying");
    if (g_fs.format()) {
      g_mounted = g_fs.begin(false, "/data", 10, "userdata");
    }
  }
  if (g_mounted) {
    Serial.printf("[fs] userdata mounted at /data (%u bytes)\r\n",
                  (unsigned)userfs::totalBytes());
  } else {
    Serial.println("[fs] userdata mount failed (check partition table has 'userdata')");
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

}  // namespace userfs
