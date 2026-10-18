#pragma once

#include <FS.h>

// 用户数据文件系统：5MB userdata 分区，挂载于 /data。
// 与 Web 资源所在的 spiffs 分区相互独立，刷固件与 OTA 均保留，
// 供业务模块持久化用户数据（短信记录等）。
// 全工程共用这一个挂载实例（同一分区不能重复挂载）。
namespace userfs {

// 挂载（重复调用无副作用）；formatOnFail=true，首次或损坏时自动格式化
bool begin();

// 挂载后的文件系统实例（未挂载时行为未定义，调用前确保 begin 成功）
fs::FS &fs();

// 分区总容量 / 已用字节（未挂载返回 0）
size_t totalBytes();
size_t usedBytes();

// 是否已挂载
bool mounted();

}  // namespace userfs
