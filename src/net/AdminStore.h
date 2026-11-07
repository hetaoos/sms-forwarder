#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>

#include "app/Config.h"

// 管理账号存储：用户名 + 加盐口令哈希以 JSON 对象形式存 NVS（与 WiFi 配置同一
// 命名空间、同一模式），启动时读取；无有效记录时确定出厂账号并立即落盘。
//
// 存储格式 v2：{"ver":2,"user":"..","salt":"<32hex>","verifier":"<64hex>"}
//   verifier = SHA256(salt + ":" + pass)，NVS 不存明文密码。
// 兼容 v1：{"ver":1,"user":"..","pass":".."}，开机读到时派生 verifier 并写回 v2
//   （密码不变，用户无感）；配置导入遇到 v1 备份同样自动升级。
//
// 出厂账号来源：编译期注入的 WEB_AUTH_USER_DEFAULT / WEB_AUTH_PASS_DEFAULT
// 必须同时非空且长度合规才生效，否则整体回落 admin / admin123。
// 注意：修改成功后调用方（WebPanel）应作废全部会话与访问令牌。
class AdminStore {
 public:
  // 从 NVS 加载（含 v1 → v2 迁移）；无有效记录时按上述规则取出厂账号并持久化
  void begin();

  // 校验账号密码（v1 明文登录 / 修改账号时的 passOld / LCD 重置共用）
  // user：用户名；pass：明文密码
  bool check(const String &user, const String &pass) const;

  // 校验挑战应答：proof = SHA256(verifier + ":" + ts + ":" + nonce)
  // ts：challenge 下发的设备 millis() 回显值；nonce：一次性随机串
  bool checkProof(const String &user, uint32_t ts, const String &nonce,
                  const String &proof) const;

  // 修改用户名 / 密码并持久化到 NVS（每次换新 salt）
  // user：新用户名（1..32 字符）；pass：新密码（1..64 字符）；
  // err：出参，失败时写入原因（可空）；成功返回 true
  bool update(const String &user, const String &pass, String *err = nullptr);

  // 当前用户名（口令哈希不回传）
  const String &user() const { return _user; }

  // 当前 salt（由 /api/login/challenge 下发给前端派生 verifier）
  const String &salt() const { return _salt; }

  // 导出账号到给定 JSON 对象：o["ver"]/["user"]/["salt"]/["verifier"]，
  // 不含明文密码；verifier 足以让备份恢复登录，但不可用于其它站点撞库。
  void exportTo(JsonObject &o) const;

  // 从给定 JSON 对象导入账号并落盘；同时接受 v2 与 v1 明文备份
  bool importFrom(const JsonObject &o);

 private:
  // 从 NVS 读取 JSON 填充；无记录 / 解析失败 / 字段非法返回 false
  bool loadFromNvs();

  // 序列化为 v2 JSON 写入 NVS
  bool persist() const;

  // verifier = SHA256(salt + ":" + pass)，64 位小写 hex
  static String deriveVerifier(const String &salt, const String &pass);

  String _user;      // 当前用户名
  String _salt;      // 16 字节随机 salt 的 hex（32 字符）
  String _verifier;  // 口令哈希的 hex（64 字符）
};
