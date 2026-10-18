#pragma once

#include <Arduino.h>

#include "app/Config.h"

// 管理账号存储：用户名 / 密码以 JSON 对象形式存 NVS（与 WiFi 配置同一
// 命名空间、同一模式），启动时读取；无有效记录时确定出厂账号并立即落盘，
// 保证 NVS 与运行态一致。
// 出厂账号来源：编译期注入的 WEB_AUTH_USER_DEFAULT / WEB_AUTH_PASS_DEFAULT
// 必须同时非空且长度合规才生效，否则整体回落 admin / admin123。
// 存储格式：{"ver":1,"user":"..","pass":".."}
// 注意：修改成功后调用方（WebPanel）应作废全部会话与访问令牌。
class AdminStore {
 public:
  // 从 NVS 加载；无有效记录时按上述规则取出厂账号并持久化
  void begin();

  // 校验账号密码是否与当前存储一致
  // user：用户名；pass：密码
  bool check(const String &user, const String &pass) const;

  // 修改用户名 / 密码并持久化到 NVS
  // user：新用户名（1..32 字符）；pass：新密码（1..64 字符）；
  // err：出参，失败时写入原因（可空）；成功返回 true
  bool update(const String &user, const String &pass, String *err = nullptr);

  // 当前用户名（密码不回传）
  const String &user() const { return _user; }

 private:
  // 从 NVS 读取 JSON 填充；无记录 / 解析失败 / 字段非法返回 false
  bool loadFromNvs();

  // 序列化为 JSON 写入 NVS
  bool persist() const;

  String _user;  // 当前用户名
  String _pass;  // 当前密码
};
