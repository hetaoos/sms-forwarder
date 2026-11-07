#include "app/Outbox.h"

#include <Arduino.h>
#include <FS.h>
#include <time.h>

#include <algorithm>

#include "app/Psram.h"
#include "app/UserFs.h"
#include "log/LogStore.h"
#include "net/EmailService.h"
#include "net/MqttService.h"
#include "net/WifiManager.h"
#include "push/PushService.h"
#include "push/PushTypes.h"
#include "sms/SmsService.h"
#include "time/TimeSync.h"   // TimeSync::stamp()：统一事件时间戳供给

namespace {

// 短信正文落盘前截断，避免超长短信把单条记录撑到几十 KB
String clip(const String &s, size_t max) {
  return s.length() <= max ? s : s.substring(0, max);
}

// 把换行压成空格，便于日志与列表展示
String oneLine(const String &s, size_t max) {
  String o = clip(s, max);
  o.replace("\r", " ");
  o.replace("\n", " ");
  o.trim();
  return o;
}

}  // namespace

// ---------------- 生命周期 ----------------

void Outbox::begin(EmailService &email, MqttService &mqtt, PushService &push,
                   SmsService &sms, WifiManager &wifi, LogStore &log) {
  _email = &email;
  _mqtt = &mqtt;
  _push = &push;
  _sms = &sms;
  _wifi = &wifi;
  _log = &log;
  _mutex = xSemaphoreCreateMutex();
  _recs.reserve(cfg::kOutboxMaxRecords + 8);
  _incoming.reserve(16);

  if (userfs::mounted()) {
    userfs::fs().mkdir(cfg::kOutboxDir);
    _fsOk = true;
    load();
  }
  if (!_fsOk) {
    _log->addf(LogLevel::kError, "outbox",
               "用户数据分区不可用，持久化发件箱未启用（短信仍会走原有即时转发队列）");
    return;
  }
  int pending = pendingCount();
  _log->addf(LogLevel::kInfo, "outbox",
             "持久化发件箱就绪：恢复 %u 条记录，待投通道 %d 个",
             (unsigned)_recs.size(), pending);
}

void Outbox::load() {
  fs::FS &u = userfs::fs();
  File dir = u.open(cfg::kOutboxDir);
  if (!dir || !dir.isDirectory()) return;
  File f = dir.openNextFile();
  while (f) {
    String name = f.name();
    f.close();
    uint32_t id = parseId(name);
    if (id) {
      Rec r;
      if (readRec(id, r)) {
        // 崩溃恢复：转发类 sending 无法确认是否送达，回到 pending 重投（at-least-once）。
        // 发出类 sending 不同：短信可能已经发出，自动重投会重复扣费，故标记为
        // 「结果未知」终态，由用户在发件箱页手动决定是否重试。
        // nextAtMs 是运行时字段（millis 重启归零），按已尝试次数重新给一个短退避。
        for (auto &c : r.ch) {
          if (c.state == ChState::kSending) {
            if (c.key.startsWith("sms:")) {
              c.state = ChState::kSubmitted;
              c.err = "重启前发送结果未知，可手动重试";
              r.dirty = true;
            } else {
              c.state = ChState::kPending;
            }
          }
          if (c.state == ChState::kPending || c.state == ChState::kFailed) {
            c.nextAtMs = millis() + backoffMs(c.attempts);
          }
        }
        _recs.push_back(r);
        if (id > _maxId) _maxId = id;
      }
    }
    f = dir.openNextFile();
  }
  std::sort(_recs.begin(), _recs.end(),
            [](const Rec &a, const Rec &b) { return a.id < b.id; });
}

// ---------------- 落盘 ----------------

String Outbox::filePath(uint32_t id) const {
  return String(cfg::kOutboxDir) + "/" + String(id) + ".json";
}

uint32_t Outbox::parseId(const String &name) {
  int slash = name.lastIndexOf('/');
  String base = (slash >= 0) ? name.substring(slash + 1) : name;
  if (!base.endsWith(".json")) return 0;
  base = base.substring(0, base.length() - 5);
  if (!base.length()) return 0;
  for (size_t i = 0; i < base.length(); i++) {
    if (base[i] < '0' || base[i] > '9') return 0;
  }
  return (uint32_t)base.toInt();
}

bool Outbox::writeRec(const Rec &r) const {
  if (!_fsOk) return false;
  JsonDocument doc(psramJsonAllocator());
  doc["id"] = r.id;
  doc["kind"] = r.kind;
  doc["phone"] = r.phone;
  doc["port"] = r.port;
  doc["sender"] = r.sender;
  doc["text"] = r.text;
  doc["ts"] = r.ts;
  doc["modem"] = (int)r.modem;
  doc["created"] = r.created;
  doc["failover"] = r.failover;
  JsonArray arr = doc["ch"].to<JsonArray>();
  for (const auto &c : r.ch) {
    JsonObject o = arr.add<JsonObject>();
    o["k"] = c.key;
    o["n"] = c.label;
    o["e"] = c.err;
    o["s"] = (int)c.state;
    o["a"] = c.attempts;
    o["ms"] = c.costMs;
  }
  File f = userfs::fs().open(filePath(r.id), "w");
  if (!f) return false;
  size_t n = serializeJson(doc, f);
  f.close();
  return n > 0;
}

bool Outbox::readRec(uint32_t id, Rec &out) const {
  File f = userfs::fs().open(filePath(id), "r");
  if (!f) return false;
  JsonDocument doc(psramJsonAllocator());
  DeserializationError e = deserializeJson(doc, f);
  f.close();
  if (e || !doc["id"].is<uint32_t>()) return false;
  out.id = doc["id"] | 0;
  if (!out.id) return false;
  // kind/phone/port 为后加字段：旧记录缺省即按「收到的转发」处理，向后兼容
  out.kind = (uint8_t)(doc["kind"] | 0);
  out.phone = doc["phone"] | "";
  out.port = (uint8_t)(doc["port"] | 0);
  out.sender = doc["sender"] | "";
  out.text = doc["text"] | "";
  out.ts = doc["ts"] | 0;
  out.modem = (int8_t)(doc["modem"] | -1);
  out.created = doc["created"] | 0;
  out.failover = doc["failover"] | false;
  JsonArray arr = doc["ch"].as<JsonArray>();
  for (JsonObject o : arr) {
    Channel c;
    c.key = o["k"] | "";
    c.label = o["n"] | "";
    c.err = o["e"] | "";
    c.state = (ChState)(o["s"] | 0);
    c.attempts = (uint8_t)(o["a"] | 0);
    c.costMs = o["ms"] | 0;
    if (c.key.length()) out.ch.push_back(c);
  }
  return true;
}

// ---------------- 状态工具 ----------------

const char *Outbox::stateName(ChState s) {
  switch (s) {
    case ChState::kPending:   return "pending";
    case ChState::kSending:   return "sending";
    case ChState::kOk:        return "ok";
    case ChState::kSubmitted: return "submitted";
    case ChState::kFailed:    return "failed";
    case ChState::kSkipped:   return "skipped";
    case ChState::kDead:      return "dead";
    default:                  return "unknown";
  }
}

uint32_t Outbox::backoffMs(uint8_t attempts) {
  uint32_t v = cfg::kOutboxBackoffBaseMs;
  for (uint8_t i = 1; i < attempts && i < 8; i++) v *= 2;
  if (v > cfg::kOutboxBackoffMaxMs) v = cfg::kOutboxBackoffMaxMs;
  return v;
}

bool Outbox::recDone(const Rec &r) {
  for (const auto &c : r.ch) {
    if (c.state == ChState::kPending || c.state == ChState::kSending ||
        c.state == ChState::kFailed) return false;
  }
  return true;
}

void Outbox::snapshotChannels(Rec &r) const {
  // 主动发出的短信：只生成专用单通道。绝不能走全量快照——否则会经过
  // PUSH_TYPE_SMS「短信备份」通道再发一条短信，形成无限回环，且重复扣费。
  if (r.kind == kSend) {
    Channel sms;
    sms.key = String("sms:") + String(r.port);
    sms.label = String("通信模组 ") + String(r.port) + " · 短信发送";
    r.ch.push_back(sms);
    r.failover = false;
    return;
  }

  Channel mail;
  mail.key = "email";
  mail.label = "邮件通知";
  r.ch.push_back(mail);
#if ENABLE_MQTT
  Channel mq;
  mq.key = "mqtt";
  mq.label = "MQTT";
  r.ch.push_back(mq);
#endif

  // 快照当下启用且字段齐全的 webhook 通道，索引固化在 key 里（push:<idx>）。
  // 用户随后改配置不会影响已入队记录：新增通道不补发，删掉的通道投递时判跳过。
  PushService::ChannelBrief briefs[MAX_PUSH_CHANNELS];
  int n = _push ? _push->channelBriefs(briefs, MAX_PUSH_CHANNELS) : 0;
  for (int i = 0; i < n; i++) {
    if (!briefs[i].valid) continue;
    Channel c;
    c.key = String("push:") + String(i);
    c.label = briefs[i].label;
    r.ch.push_back(c);
  }
  r.failover = _push && _push->strategy() == PUSH_STRATEGY_FAILOVER;
}

// ---------------- 入队与主循环 ----------------

void Outbox::enqueueSms(const String &sender, const String &text, uint32_t ts,
                        int8_t modem) {
  if (!_mutex || !_fsOk) return;  // FS 不可用时由 App 回退到原有即时转发队列
  lock();
  if (_incoming.size() >= 32) {
    unlock();
    _log->addf(LogLevel::kWarn, "outbox", "入队缓冲已满，本次短信未能登记发件箱");
    return;
  }
  Incoming in;
  in.sender = sender;
  in.text = clip(text, cfg::kOutboxTextLimit);
  in.ts = ts;
  in.modem = modem;
  _incoming.push_back(in);
  unlock();
}

// 网页主动发出短信：只做浅入队（HTTP / async_tcp 任务上下文安全），
// 落盘与真正入队发送都由 tick() 在 loop 任务完成。
bool Outbox::enqueueOutgoingSms(uint8_t port, const String &phone,
                                const String &content, SendDoneFn done) {
  if (!_mutex || !_fsOk) return false;  // FS 不可用时调用方回退到原有发送队列
  if (port < 1 || port > cfg::kSmsMaxModems || !phone.length() ||
      !content.length()) return false;
  lock();
  if (_incoming.size() >= 32) {
    unlock();
    _log->addf(LogLevel::kWarn, "outbox", "发件箱入队缓冲已满，本次发送短信未登记");
    return false;
  }
  Incoming in;
  in.kind = kSend;
  in.phone = phone;
  in.port = port;
  in.sender = phone;  // 列表里统一用 sender 展示号码
  in.text = clip(content, cfg::kOutboxTextLimit);
  in.ts = TimeSync::stamp();  // 已同步=纪元秒，未同步=开机秒数（与日志/短信/AT 历史同口径）
  in.modem = (int8_t)port;
  in.done = done;
  _incoming.push_back(in);
  unlock();
  return true;
}

void Outbox::tick() {
  if (!_fsOk || !_mutex) return;

  // 1) 后台任务投进来的新事件 -> 生成记录并先落盘（先记账再发送，才谈得上不丢）
  std::vector<Incoming> batch;
  lock();
  batch.swap(_incoming);
  unlock();
  for (auto &in : batch) {
    Rec r;
    r.kind = in.kind;
    r.sender = in.sender;
    r.phone = in.phone;
    r.port = in.port;
    r.text = in.text;
    r.ts = in.ts;
    r.modem = in.modem;
    r.created = TimeSync::stamp();
    r.done = in.done;
    snapshotChannels(r);
    lock();
    r.id = ++_maxId;
    bool ok = writeRec(r);
    if (ok) {
      _recs.push_back(r);
      unlock();
      _log->addf(LogLevel::kInfo, "outbox", "发件箱登记 #%u：%s %s，%u 个通道待投",
                 (unsigned)r.id, r.kind == kSend ? "发出至" : "来自",
                 in.sender.c_str(), (unsigned)r.ch.size());
    } else {
      unlock();
      // 发出类登记失败：不能静默吞掉，必须让外部回调收口（否则 Web 轮询会一直 busy）
      if (r.kind == kSend && r.done) r.done(0, false, "发件箱登记落盘失败");
      _log->addf(LogLevel::kError, "outbox", "发件箱记录 #%u 落盘失败，无法保证不丢消息",
                 (unsigned)r.id);
    }
  }

  // 1b) sms_send worker 的 done 回调若丢失（_doneQ 满），超时兜底收口
  //     （函数自己管理锁：回调必须在释放锁之后执行）
  checkSendTimeout();

  // 2) 处理 Web 侧删除/重试留下的标记 + 淘汰旧回执
  lock();
  sweep();
  unlock();

  uint32_t now = millis();
  if ((uint32_t)(now - _lastTickMs) < cfg::kOutboxTickGapMs) return;

  // 3) 挑一条到期通道；只投一个，避免单轮 tick 里串起多个 15s 的 SMTP 会话
  Rec job;
  Channel ch;
  lock();
  bool has = pickDue(job, ch);
  unlock();
  if (!has) return;
  _lastTickMs = millis();

  // 4) 网络未就绪：推迟且不消耗尝试次数（重启后 WiFi 还没连上时不浪费重试额度）。
  //    SMS 备份推送通道不需要 WiFi，由 PushService::channelNeedsNet 例外放行。
  bool needsNet = true;
  if (ch.key.startsWith("push:")) {
    needsNet = _push->channelNeedsNet(ch.key.substring(5).toInt());
  } else if (ch.key.startsWith("sms:")) {
    needsNet = false;  // 经通信模组发短信不依赖 WiFi
  }
  if (needsNet && !_wifi->connected()) {
    applyResult(job.id, ch.key, false, false, false, true, "网络未就绪", 0);
    return;
  }

  // 5) 单次投递（可能阻塞，此时不持 Outbox 锁）
  bool submitted = false, skipped = false;
  String err;
  uint32_t t0 = millis();
  bool ok = deliver(job, ch, submitted, skipped, err);
  applyResult(job.id, ch.key, ok, submitted, skipped, false, err,
              (uint32_t)(millis() - t0));
}

bool Outbox::deliver(const Rec &r, const Channel &c, bool &submitted,
                     bool &skipped, String &err) {
  if (c.key == "email") {
    return _email->deliverSmsOnce(r.sender, r.text, r.ts, r.modem, skipped, err);
  }
  if (c.key == "mqtt") {
    bool ok = _mqtt->publishSmsOnce(r.sender, r.text, r.ts, r.modem, skipped, err);
    submitted = ok;  // esp-mqtt QoS1 仅表示已入队，无端到端投递确认
    return ok;
  }
  if (c.key.startsWith("push:")) {
    int idx = c.key.substring(5).toInt();
    bool skip = false;
    bool ok = _push->deliverOnce(idx, r.sender, r.text, r.ts, r.modem,
                                 MSG_TYPE_SMS, skip, err);
    if (skip) skipped = true;
    return ok;
  }
  if (c.key.startsWith("sms:")) {
    if (!_sms) { err = "短信服务未就绪"; return false; }
    uint8_t port = (uint8_t)c.key.substring(4).toInt();
    if (!port) port = r.port;
    // 阻塞式 CMGS 只能由 SmsService 的常驻 sms_send worker 执行；这里只入队，
    // done 由 _sms.tick()（loop 任务）回调后回写发件箱状态。
    // 只按值捕获 id/key，不捕获记录引用（记录可能已被删除或淘汰）。
    const uint32_t recId = r.id;
    const String key = c.key;
    bool queued = _sms->enqueueSend(port, r.phone, r.text,
        [this, recId, key](uint32_t sid, bool stored, const String &serr) {
          onSendDone(recId, key, sid, stored, serr);
        });
    if (!queued) {
      err = "短信发送队列已满";
      return false;  // 尚未交给模组，退避重试不会造成重复短信
    }
    submitted = true;  // applyResult 会把 sms: 的 submitted 落成 kSending 等结果
    return true;
  }
  skipped = true;
  err = "未知通道";
  return false;
}

bool Outbox::pickDue(Rec &out, Channel &outCh) {
  // 调用方须持有 _mutex
  uint32_t now = millis();
  for (auto &r : _recs) {
    if (r.removed) continue;
    for (auto &c : r.ch) {
      if (c.state != ChState::kPending && c.state != ChState::kFailed) continue;
      if (c.nextAtMs && (int32_t)(c.nextAtMs - now) > 0) continue;
      out = r;
      outCh = c;
      c.state = ChState::kSending;  // 仅内存态；崩溃后按 pending 重投
      c.nextAtMs = 0;
      return true;
    }
  }
  return false;
}

void Outbox::applyResult(uint32_t id, const String &key, bool ok, bool submitted,
                         bool skipped, bool noNet, const String &err,
                         uint32_t costMs) {
  lock();
  bool found = false;
  for (auto &r : _recs) {
    if (r.id != id || r.removed) continue;
    for (auto &c : r.ch) {
      if (c.key != key) continue;
      found = true;
      c.costMs = costMs;
      c.err = clip(err, cfg::kOutboxErrLimit);
      c.nextAtMs = 0;
      if (noNet) {
        c.state = ChState::kPending;
        c.nextAtMs = millis() + cfg::kOutboxNoNetRetryMs;
      } else if (skipped) {
        c.state = ChState::kSkipped;
      } else if (ok) {
        c.attempts++;
        if (submitted && key.startsWith("sms:")) {
          // 已交给 sms_send worker，真实结果等 done 回调。落盘成 kSending 很关键：
          // 断电重启后 load() 会记为「结果未知」而非自动重发（避免重复扣费）。
          c.state = ChState::kSending;
          r.inflightAtMs = millis();
          _log->addf(LogLevel::kInfo, "outbox",
                     "发件箱 #%u 短信已交给发送队列，等待模组回执", (unsigned)id);
        } else {
          c.state = submitted ? ChState::kSubmitted : ChState::kOk;
          _log->addf(LogLevel::kInfo, "outbox", "发件箱 #%u 通道 %s 投递成功（%u ms）",
                     (unsigned)id, stateName(c.state), (unsigned)costMs);
        }
      } else {
        c.attempts++;
        if (c.attempts >= cfg::kOutboxMaxAttempts) {
          c.state = ChState::kDead;
          _log->addf(LogLevel::kWarn, "outbox",
                     "发件箱 #%u 通道 %s 连续 %u 次失败，已放弃：%s",
                     (unsigned)id, c.label.c_str(), (unsigned)c.attempts, c.err.c_str());
        } else {
          c.state = ChState::kFailed;
          c.nextAtMs = millis() + backoffMs(c.attempts);
          _log->addf(LogLevel::kWarn, "outbox",
                     "发件箱 #%u 通道 %s 第 %u 次失败：%s",
                     (unsigned)id, c.label.c_str(), (unsigned)c.attempts, c.err.c_str());
        }
      }
      // 网络未就绪只是「推迟」，盘上状态仍是 pending/failed，无需反复回写：
      // 否则长时间断网时每 kOutboxNoNetRetryMs 就写一次同一个文件，白白磨损 flash。
      if (!noNet) {
        r.dirty = true;
        writeRec(r);
        r.dirty = false;
      }
      if (ok && r.failover && key.startsWith("push:")) markFailoverSkipped(id);
      break;
    }
    break;
  }
  unlock();
  if (!found) {
    _log->addf(LogLevel::kWarn, "outbox", "发件箱 #%u 回写失败：记录已不存在",
               (unsigned)id);
  }
}

// sms_send worker 的完成回调，由 SmsService::tick() 在 loop 任务触发。
// 这里把 kSending 升级为最终结果并落盘；外部 done 回调在释放锁之后执行。
void Outbox::onSendDone(uint32_t recId, const String &key, uint32_t sid,
                        bool stored, const String &err) {
  SendDoneFn cb;
  lock();
  for (auto &r : _recs) {
    if (r.id != recId || r.removed) continue;
    for (auto &c : r.ch) {
      if (c.key != key || c.state != ChState::kSending) continue;
      if (sid != 0) {
        c.state = ChState::kOk;
        c.err = stored ? String("已发出 · 短信记录 #") + String(sid)
                       : "已发出，但短信记录落盘失败";
      } else {
        // 模组可能已发出却没回执，自动重投会重复扣费：直接结单，
        // 需要时由用户在发件箱页手动点「重试」。
        c.state = ChState::kDead;
        c.err = err.length() ? err : String("模组发送失败");
      }
      r.inflightAtMs = 0;
      r.dirty = true;
      writeRec(r);
      r.dirty = false;
      _log->addf(LogLevel::kInfo, "outbox", "发件箱 #%u 短信发送结果：%s（%s）",
                 (unsigned)recId, stateName(c.state), c.err.c_str());
      break;
    }
    cb = r.done;
    r.done = nullptr;  // 只回调一次
    break;
  }
  unlock();
  if (cb) cb(sid, stored, err);
}

// done 回调丢失（_doneQ 满）或 worker 卡死时的超时兜底：按「结果未知」结单，
// 不自动重发短信，同时用 kIdSentUnstored 让 WebPanel 的轮询状态收口。
void Outbox::checkSendTimeout() {
  std::vector<SendDoneFn> callbacks;
  const uint32_t now = millis();
  lock();
  for (auto &r : _recs) {
    if (r.removed || r.kind != kSend || !r.inflightAtMs) continue;
    if ((uint32_t)(now - r.inflightAtMs) < cfg::kOutboxSendWaitMs) continue;
    bool changed = false;
    for (auto &c : r.ch) {
      if (!c.key.startsWith("sms:") || c.state != ChState::kSending) continue;
      c.state = ChState::kSubmitted;
      c.err = "发送结果未回传（可能已发出，勿重复发送）";
      changed = true;
    }
    r.inflightAtMs = 0;
    if (!changed) continue;
    r.dirty = true;
    writeRec(r);
    r.dirty = false;
    if (r.done) { callbacks.push_back(r.done); r.done = nullptr; }
    _log->addf(LogLevel::kWarn, "outbox",
               "发件箱 #%u 短信发送结果超时未回传，按结果未知结单（不自动重发）",
               (unsigned)r.id);
  }
  unlock();
  for (auto &fn : callbacks) {
    fn(SmsService::kIdSentUnstored, false, "发送结果未回传（可能已发出）");
  }
}

void Outbox::markFailoverSkipped(uint32_t id) {
  // 调用方须持有 _mutex：故障转移策略下第一个成功的通道之后，其余 push 通道无需再投
  for (auto &r : _recs) {
    if (r.id != id || r.removed) continue;
    for (auto &c : r.ch) {
      if (!c.key.startsWith("push:")) continue;
      if (c.state == ChState::kPending || c.state == ChState::kFailed) {
        c.state = ChState::kSkipped;
        r.dirty = true;
      }
    }
    if (r.dirty) { writeRec(r); r.dirty = false; }
    break;
  }
}

void Outbox::sweep() {
  // 调用方须持有 _mutex
  fs::FS &u = userfs::fs();

  // 删除 Web 侧标记的记录
  for (auto it = _recs.begin(); it != _recs.end();) {
    if (!it->removed) { ++it; continue; }
    u.remove(filePath(it->id));
    it = _recs.erase(it);
  }
  // 回写待落盘的修改（retry / 手动删除之外的状态变更）
  for (auto &r : _recs) {
    if (!r.dirty) continue;
    if (writeRec(r)) r.dirty = false;
  }
  // FIFO 淘汰最旧的「已完成」回执；未完成记录永不淘汰（不丢消息的前提）
  while (_recs.size() > cfg::kOutboxKeepDone) {
    auto victim = _recs.end();
    for (auto it = _recs.begin(); it != _recs.end(); ++it) {
      if (!recDone(*it)) continue;
      if (victim == _recs.end() || it->id < victim->id) victim = it;
    }
    if (victim == _recs.end()) {
      // 全是未完成记录：宁可超出上限也不删，只节流告警
      static uint32_t lastWarn = 0;
      if (millis() - lastWarn > 60000) {
        lastWarn = millis();
        _log->addf(LogLevel::kWarn, "outbox",
                   "发件箱未完成记录已达 %u 条（上限 %u），暂停淘汰旧回执",
                   (unsigned)_recs.size(), (unsigned)cfg::kOutboxMaxRecords);
      }
      break;
    }
    u.remove(filePath(victim->id));
    _recs.erase(victim);
  }
}

// ---------------- Web API ----------------

String Outbox::statusJson(int page, int pageSize, uint32_t id) const {
  JsonDocument doc(psramJsonAllocator());
  doc["ok"] = true;
  doc["enabled"] = _fsOk;
  if (page < 1) page = 1;
  if (pageSize < 1) pageSize = 20;
  if (pageSize > 50) pageSize = 50;

  lock();
  int total = 0, pending = 0, failed = 0, done = 0;
  for (const auto &r : _recs) {
    if (r.removed) continue;
    total++;
    if (recDone(r)) done++;
    for (const auto &c : r.ch) {
      if (c.state == ChState::kPending || c.state == ChState::kSending) pending++;
      else if (c.state == ChState::kFailed ||
               (c.state == ChState::kSubmitted && c.key.startsWith("sms:"))) failed++;
    }
  }
  doc["total"] = total;
  doc["pending"] = pending;
  doc["failed"] = failed;
  doc["done"] = done;

  JsonArray arr = doc["records"].to<JsonArray>();
  // 倒序（最新在前），按 id 过滤；分页下标按逻辑顺序计
  int idx = 0, skip = (page - 1) * pageSize;
  for (auto it = _recs.rbegin(); it != _recs.rend(); ++it) {
    const Rec &r = *it;
    if (r.removed || (id && r.id != id)) continue;
    if (idx++ < skip) continue;
    if (arr.size() >= (size_t)pageSize) break;
    JsonObject o = arr.add<JsonObject>();
    o["id"] = r.id;
    o["kind"] = r.kind;       // 0=收到的短信转发，1=主动发出的短信
    o["phone"] = r.phone;
    o["port"] = r.port;
    o["sender"] = r.sender;
    o["preview"] = oneLine(r.text, 60);
    if (id) o["text"] = r.text;
    o["ts"] = r.ts;
    o["modem"] = (int)r.modem;
    o["created"] = r.created;
    o["failover"] = r.failover;
    o["state"] = recDone(r) ? "done" : "pending";
    JsonArray cs = o["channels"].to<JsonArray>();
    for (const auto &c : r.ch) {
      JsonObject co = cs.add<JsonObject>();
      co["k"] = c.key;
      co["n"] = c.label;
      co["s"] = stateName(c.state);
      co["a"] = c.attempts;
      co["e"] = c.err;
      co["ms"] = c.costMs;
    }
  }
  unlock();
  String out;
  serializeJson(doc, out);
  return out;
}

bool Outbox::retry(uint32_t id) {
  lock();
  bool found = false;
  for (auto &r : _recs) {
    if (r.removed || (id && r.id != id)) continue;
    for (auto &c : r.ch) {
      // sms: 通道的 kSubmitted 表示「结果未知 / 结果未回传」，同样允许手动重试
      // （用户自行确认是否已发出，避免自动重发重复扣费）
      const bool retryable = c.state == ChState::kFailed ||
                             c.state == ChState::kDead ||
                             (c.state == ChState::kSubmitted &&
                              c.key.startsWith("sms:"));
      if (!retryable) continue;
      c.state = ChState::kPending;
      c.attempts = 0;
      c.nextAtMs = 0;
      c.err = "";
      r.inflightAtMs = 0;
      r.dirty = true;
      found = true;
    }
  }
  unlock();
  return found;
}

void Outbox::clearDone() {
  lock();
  for (auto &r : _recs) {
    if (recDone(r)) r.removed = true;
  }
  unlock();
}

bool Outbox::deleteOne(uint32_t id) {
  lock();
  for (auto &r : _recs) {
    if (r.id == id && !r.removed) {
      r.removed = true;
      unlock();
      return true;
    }
  }
  unlock();
  return false;
}

int Outbox::pendingCount() const {
  lock();
  int n = 0;
  for (const auto &r : _recs) {
    if (r.removed) continue;
    for (const auto &c : r.ch) {
      if (c.state == ChState::kPending || c.state == ChState::kSending) n++;
    }
  }
  unlock();
  return n;
}

int Outbox::failedCount() const {
  lock();
  int n = 0;
  for (const auto &r : _recs) {
    if (r.removed) continue;
    for (const auto &c : r.ch) {
      if (c.state == ChState::kFailed || c.state == ChState::kDead ||
          (c.state == ChState::kSubmitted && c.key.startsWith("sms:"))) n++;
    }
  }
  unlock();
  return n;
}
