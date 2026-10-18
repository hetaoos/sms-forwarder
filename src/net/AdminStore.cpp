#include "net/AdminStore.h"

#include <ArduinoJson.h>
#include <Preferences.h>

#include <Arduino.h>

namespace {

// 账号长度上限（与前端校验一致，防 NVS 被异常长字符串塞满）
constexpr size_t kUserMaxLen = 32;
constexpr size_t kPassMaxLen = 64;

}  // namespace

// 从 NVS 加载；无有效记录时用编译期默认值并持久化
void AdminStore::begin() {
  if (loadFromNvs()) {
    Serial.printf("[auth] loaded from nvs, user=%s\r\n", _user.c_str());
    return;
  }
  // 无有效记录：落入编译期默认值并落盘，之后所有修改都基于它
  _user = cfg::kWebAuthUser;
  _pass = cfg::kWebAuthPass;
  if (persist()) {
    Serial.println("[auth] default credentials persisted to nvs");
  } else {
    Serial.println("[auth] persist default credentials failed");
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
    Serial.println("[auth] nvs record parse failed, fallback to default");
    return false;
  }
  if (!doc["user"].is<const char *>() || !doc["pass"].is<const char *>()) {
    Serial.println("[auth] nvs record fields invalid, fallback to default");
    return false;
  }
  String u = doc["user"].as<String>();
  String p = doc["pass"].as<String>();
  if (!u.length() || u.length() > kUserMaxLen ||
      !p.length() || p.length() > kPassMaxLen) {
    Serial.println("[auth] nvs record length invalid, fallback to default");
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
  Serial.printf("[auth] credentials updated, user=%s\r\n", _user.c_str());
  return true;
}
