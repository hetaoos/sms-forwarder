#include "net/AdminStore.h"

#include "log/LogStore.h"

#include <ArduinoJson.h>
#include <Preferences.h>

#include <Arduino.h>

#include <cstring>

namespace {

// 本通信模组日志出口：全局日志环（串口由 LogStore 统一镜像输出）
LogStore &_log = devLog();

// 账号长度上限（与前端校验一致，防 NVS 被异常长字符串塞满）
constexpr size_t kUserMaxLen = 32;
constexpr size_t kPassMaxLen = 64;

}  // namespace

// 从 NVS 加载；无有效记录时确定出厂账号并持久化
void AdminStore::begin() {
  if (loadFromNvs()) {
    _log.addf(LogLevel::kInfo, "auth", "已从 NVS 载入管理账号：%s", _user.c_str());
    return;
  }
  // 无有效记录：优先用编译期注入的账号，但必须用户名与密码同时有效
  // （非空且长度合规），否则整体回落出厂默认 admin / admin123，
  // 避免只配了用户名/密码之一导致设备无法登录。
  const size_t userLen = strlen(cfg::kWebAuthUserDefault);
  const size_t passLen = strlen(cfg::kWebAuthPassDefault);
  const bool useBuildIn = userLen > 0 && userLen <= kUserMaxLen &&
                          passLen > 0 && passLen <= kPassMaxLen;
  _user = useBuildIn ? cfg::kWebAuthUserDefault : cfg::kWebAuthUserFallback;
  _pass = useBuildIn ? cfg::kWebAuthPassDefault : cfg::kWebAuthPassFallback;
  _log.addf(LogLevel::kInfo, "auth", "出厂管理账号：%s（%s）", _user.c_str(),
            useBuildIn ? "编译期注入" : "内置默认 admin/admin123");
  if (persist()) {
    _log.addf(LogLevel::kInfo, "auth", "默认管理账号已写入 NVS");
  } else {
    _log.addf(LogLevel::kError, "auth", "默认管理账号写入 NVS 失败");
  }
}

// 从 NVS 读取 JSON 填充；无记录 / 解析失败 / 字段非法返回 false
bool AdminStore::loadFromNvs() {
  Preferences prefs;
  if (!prefs.begin(cfg::kNvsNamespace, true)) return false;  // 只读方式打开
  String json = prefs.getString(cfg::kNvsKeyAuth, "");
  prefs.end();
  if (!json.length()) return false;

  JsonDocument doc;
  if (deserializeJson(doc, json) != DeserializationError::Ok) {
    _log.addf(LogLevel::kWarn, "auth", "NVS 账号记录解析失败，回退默认值");
    return false;
  }
  if (!doc["user"].is<const char *>() || !doc["pass"].is<const char *>()) {
    _log.addf(LogLevel::kWarn, "auth", "NVS 账号记录字段无效，回退默认值");
    return false;
  }
  String u = doc["user"].as<String>();
  String p = doc["pass"].as<String>();
  if (!u.length() || u.length() > kUserMaxLen ||
      !p.length() || p.length() > kPassMaxLen) {
    _log.addf(LogLevel::kWarn, "auth", "NVS 账号记录长度异常，回退默认值");
    return false;
  }
  _user = u;
  _pass = p;
  return true;
}

// 序列化为 JSON 写入 NVS
bool AdminStore::persist() const {
  JsonDocument doc;
  doc["ver"] = 1;  // 配置格式版本，便于后续升级
  doc["user"] = _user;
  doc["pass"] = _pass;

  String out;
  serializeJson(doc, out);

  Preferences prefs;
  if (!prefs.begin(cfg::kNvsNamespace, false)) return false;  // 读写方式打开
  size_t written = prefs.putString(cfg::kNvsKeyAuth, out);
  prefs.end();
  return written == out.length();
}

// 校验账号密码是否与当前存储一致
bool AdminStore::check(const String &user, const String &pass) const {
  return user == _user && pass == _pass;
}

// 导出账号（含密码，供备份用）到给定 JSON 对象
void AdminStore::exportTo(JsonObject &o) const {
  o["user"] = _user;
  o["pass"] = _pass;
}

// 从给定 JSON 对象导入账号并落盘（长度校验与 update 一致）
bool AdminStore::importFrom(const JsonObject &o) {
  String u = o["user"] | "";
  String p = o["pass"] | "";
  if (!u.length() || u.length() > kUserMaxLen ||
      !p.length() || p.length() > kPassMaxLen) {
    _log.addf(LogLevel::kWarn, "auth", "导入的账号字段非法，已跳过");
    return false;
  }
  _user = u;
  _pass = p;
  if (!persist()) {
    _log.addf(LogLevel::kError, "auth", "账号导入落盘失败");
    return false;
  }
  _log.addf(LogLevel::kInfo, "auth", "已从备份导入管理账号：%s", _user.c_str());
  return true;
}

// 修改用户名 / 密码并持久化到 NVS
bool AdminStore::update(const String &user, const String &pass, String *err) {
  if (!user.length() || user.length() > kUserMaxLen) {
    if (err) *err = "用户名长度需为 1-" + String(kUserMaxLen) + " 字符";
    return false;
  }
  if (!pass.length() || pass.length() > kPassMaxLen) {
    if (err) *err = "密码长度需为 1-" + String(kPassMaxLen) + " 字符";
    return false;
  }
  // 先暂存旧值：落盘失败时回滚，保证内存与 NVS 一致
  String oldUser = _user, oldPass = _pass;
  _user = user;
  _pass = pass;
  if (!persist()) {
    _user = oldUser;
    _pass = oldPass;
    if (err) *err = "NVS 写入失败";
    return false;
  }
  _log.addf(LogLevel::kInfo, "auth", "管理账号已更新：%s", _user.c_str());
  return true;
}
