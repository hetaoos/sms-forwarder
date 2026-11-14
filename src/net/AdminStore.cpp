#include "net/AdminStore.h"

#include "log/LogStore.h"
#include "utils/CryptoUtil.h"

#include <ArduinoJson.h>
#include <Preferences.h>

#include <Arduino.h>

#include <cstring>

namespace {

// 本模组日志出口：全局日志环（串口由 LogStore 统一镜像输出）
LogStore &_log = devLog();

// 账号长度上限（与前端校验一致，防 NVS 被异常长字符串塞满）
constexpr size_t kUserMaxLen = 32;
constexpr size_t kPassMaxLen = 64;
constexpr size_t kSaltHexLen = 32;      // 16 字节 salt
constexpr size_t kVerifierHexLen = 64; // SHA-256
constexpr uint8_t kAuthVer = 2;        // NVS / 配置备份的账号节格式版本

bool isHexString(const String &s, size_t len) {
  if (s.length() != len) return false;
  for (size_t i = 0; i < s.length(); i++) {
    char c = s[i];
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
  }
  return true;
}

}  // namespace

String AdminStore::deriveVerifier(const String &salt, const String &pass) {
  return crypto::sha256Hex(salt + ":" + pass);
}

// 从 NVS 加载；无有效记录时确定出厂账号并持久化
void AdminStore::begin() {
  if (loadFromNvs()) {
    _log.addf(LogLevel::kInfo, "auth", "已从 NVS 载入管理账号：%s（口令哈希 v2）",
              _user.c_str());
    return;
  }
  // 无有效记录：优先用编译期注入的账号，但必须用户名与密码同时有效
  // （非空且长度合规），否则整体回落出厂默认 admin / admin123，
  // 避免只配了用户名/密码之一导致设备无法登录。
  const size_t userLen = strlen(cfg::kWebAuthUserDefault);
  const size_t passLen = strlen(cfg::kWebAuthPassDefault);
  const bool useBuildIn = userLen > 0 && userLen <= kUserMaxLen &&
                          passLen > 0 && passLen <= kPassMaxLen;
  String user = useBuildIn ? cfg::kWebAuthUserDefault : cfg::kWebAuthUserFallback;
  String pass = useBuildIn ? cfg::kWebAuthPassDefault : cfg::kWebAuthPassFallback;
  _log.addf(LogLevel::kInfo, "auth", "出厂管理账号：%s（%s）", user.c_str(),
            useBuildIn ? "编译期注入" : "内置默认 admin/admin123");
  _user = user;
  _salt = crypto::randomHex(16);
  _verifier = deriveVerifier(_salt, pass);
  if (persist()) {
    _log.addf(LogLevel::kInfo, "auth", "默认管理账号已写入 NVS（口令哈希 v2）");
  } else {
    _log.addf(LogLevel::kError, "auth", "默认管理账号写入 NVS 失败");
  }
}

// 从 NVS 读取 JSON 填充：v2 直接载入，v1 明文自动派生 verifier 并写回
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
  if (!doc["user"].is<const char *>()) {
    _log.addf(LogLevel::kWarn, "auth", "NVS 账号记录字段无效，回退默认值");
    return false;
  }
  String u = doc["user"].as<String>();
  if (!u.length() || u.length() > kUserMaxLen) {
    _log.addf(LogLevel::kWarn, "auth", "NVS 账号记录长度异常，回退默认值");
    return false;
  }

  const int ver = doc["ver"] | 1;
  if (ver >= kAuthVer && doc["salt"].is<const char *>() &&
      doc["verifier"].is<const char *>()) {
    String s = doc["salt"].as<String>();
    String v = doc["verifier"].as<String>();
    if (!isHexString(s, kSaltHexLen) || !isHexString(v, kVerifierHexLen)) {
      _log.addf(LogLevel::kWarn, "auth", "NVS 账号哈希格式无效，回退默认值");
      return false;
    }
    _user = u;
    _salt = s;
    _verifier = v;
    return true;
  }

  // v1 明文记录（旧固件 / 旧备份）：派生 verifier 并写回 v2，密码保持不变。
  // 落盘失败不致命：本次运行仍用新格式的内存态，下次开机会再迁移一次。
  String p = doc["pass"] | "";
  if (!p.length() || p.length() > kPassMaxLen) {
    _log.addf(LogLevel::kWarn, "auth", "NVS 账号记录字段无效，回退默认值");
    return false;
  }
  _user = u;
  _salt = crypto::randomHex(16);
  _verifier = deriveVerifier(_salt, p);
  if (persist()) {
    _log.addf(LogLevel::kInfo, "auth", "管理账号已从明文迁移为加盐哈希（v1 → v2）");
  } else {
    _log.addf(LogLevel::kWarn, "auth", "账号哈希迁移落盘失败，下次开机重试");
  }
  return true;
}

// 序列化为 v2 JSON 写入 NVS（不含明文密码）
bool AdminStore::persist() const {
  JsonDocument doc;
  doc["ver"] = kAuthVer;
  doc["user"] = _user;
  doc["salt"] = _salt;
  doc["verifier"] = _verifier;

  String out;
  serializeJson(doc, out);

  Preferences prefs;
  if (!prefs.begin(cfg::kNvsNamespace, false)) return false;  // 读写方式打开
  size_t written = prefs.putString(cfg::kNvsKeyAuth, out);
  prefs.end();
  return written == out.length();
}

// 校验账号密码：用内存中的 salt 派生 verifier 后恒定时间比较
bool AdminStore::check(const String &user, const String &pass) const {
  if (user != _user) return false;
  return crypto::constantTimeEqual(deriveVerifier(_salt, pass), _verifier);
}

// 校验挑战应答：服务端不接触明文密码，只用 NVS 中的 verifier 重算 proof
bool AdminStore::checkProof(const String &user, uint32_t ts, const String &nonce,
                            const String &proof) const {
  if (user != _user || proof.length() != kVerifierHexLen) return false;
  String expected = crypto::sha256Hex(_verifier + ":" + String(ts) + ":" + nonce);
  return crypto::constantTimeEqual(expected, proof);
}

// 导出账号（不含密码，供备份用）到给定 JSON 对象
void AdminStore::exportTo(JsonObject &o) const {
  o["ver"] = kAuthVer;
  o["user"] = _user;
  o["salt"] = _salt;
  o["verifier"] = _verifier;
}

// 从给定 JSON 对象导入账号并落盘：v2 直接载入，v1 明文自动升级
bool AdminStore::importFrom(const JsonObject &o) {
  String u = o["user"] | "";
  if (!u.length() || u.length() > kUserMaxLen) {
    _log.addf(LogLevel::kWarn, "auth", "导入的账号字段非法，已跳过");
    return false;
  }
  const int ver = o["ver"] | 1;
  String s, v;
  if (ver >= kAuthVer && o["salt"].is<const char *>() &&
      o["verifier"].is<const char *>()) {
    s = o["salt"].as<String>();
    v = o["verifier"].as<String>();
    if (!isHexString(s, kSaltHexLen) || !isHexString(v, kVerifierHexLen)) {
      _log.addf(LogLevel::kWarn, "auth", "导入的账号哈希格式非法，已跳过");
      return false;
    }
  } else {
    String p = o["pass"] | "";
    if (!p.length() || p.length() > kPassMaxLen) {
      _log.addf(LogLevel::kWarn, "auth", "导入的账号字段非法，已跳过");
      return false;
    }
    s = crypto::randomHex(16);
    v = deriveVerifier(s, p);
    if (v.length() != kVerifierHexLen) {
      _log.addf(LogLevel::kWarn, "auth", "导入账号派生哈希失败，已跳过");
      return false;
    }
    _log.addf(LogLevel::kInfo, "auth", "导入的明文账号已升级为加盐哈希（v1 → v2）");
  }

  // 先暂存旧值：落盘失败时回滚，保证内存与 NVS 一致
  String oldUser = _user, oldSalt = _salt, oldVerifier = _verifier;
  _user = u;
  _salt = s;
  _verifier = v;
  if (!persist()) {
    _user = oldUser;
    _salt = oldSalt;
    _verifier = oldVerifier;
    _log.addf(LogLevel::kError, "auth", "账号导入落盘失败");
    return false;
  }
  _log.addf(LogLevel::kInfo, "auth", "已从备份导入管理账号：%s", _user.c_str());
  return true;
}

// 修改用户名 / 密码并持久化到 NVS（换新 salt + 重新派生 verifier）
bool AdminStore::update(const String &user, const String &pass, String *err) {
  if (!user.length() || user.length() > kUserMaxLen) {
    if (err) *err = "用户名长度需为 1-" + String(kUserMaxLen) + " 字符";
    return false;
  }
  if (!pass.length() || pass.length() > kPassMaxLen) {
    if (err) *err = "密码长度需为 1-" + String(kPassMaxLen) + " 字符";
    return false;
  }
  String salt = crypto::randomHex(16);
  String verifier = deriveVerifier(salt, pass);
  if (verifier.length() != kVerifierHexLen) {
    if (err) *err = "口令哈希计算失败";
    return false;
  }
  // 先暂存旧值：落盘失败时回滚，保证内存与 NVS 一致
  String oldUser = _user, oldSalt = _salt, oldVerifier = _verifier;
  _user = user;
  _salt = salt;
  _verifier = verifier;
  if (!persist()) {
    _user = oldUser;
    _salt = oldSalt;
    _verifier = oldVerifier;
    if (err) *err = "NVS 写入失败";
    return false;
  }
  _log.addf(LogLevel::kInfo, "auth", "管理账号已更新：%s", _user.c_str());
  return true;
}
