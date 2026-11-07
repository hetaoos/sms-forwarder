#pragma once

#include <Arduino.h>
#include <stdint.h>

// 登录挑战应答 / 管理账号口令哈希共用的轻量密码学工具。
// 只做单次 SHA-256 与随机串生成：调用点要么在 async_tcp handler（必须 <1ms），
// 要么在开机迁移 / 改密码等低频路径；不要在这里加多轮 KDF。
namespace crypto {

// bytes 字节硬件随机数 -> 2*bytes 位小写十六进制字符串
String randomHex(size_t bytes);

// SHA-256(data) -> 64 位小写十六进制；失败返回空串
String sha256Hex(const String &data);

// 等长字符串的恒定时间比较（长度不等直接 false，不泄露内容长度差异）
bool constantTimeEqual(const String &a, const String &b);

// FNV-1a 32 位哈希（限流桶按「IP + 用户名」建键，避免存明文用户名）
uint32_t fnv1a(const String &s);

}  // namespace crypto
