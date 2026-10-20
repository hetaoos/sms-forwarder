#include "sms/SmsStore.h"

#include "log/LogStore.h"

#include <algorithm>

#include <Arduino.h>
#include <time.h>    // time / localtime_r / mktime（今日统计用）

namespace {

// 本模块日志出口：全局日志环（串口由 LogStore 统一镜像输出）
LogStore &_log = devLog();

}  // namespace

// 确保目录存在；max 为条数上限
void SmsStore::begin(uint16_t max) {
  _mutex = xSemaphoreCreateMutex();
  _max = max;
  if (!userfs::mounted()) {
    _enabled = false;
    _log.addf(LogLevel::kError, "sms", "短信存储已停用：userdata 分区未挂载");
    return;
  }
  if (!userfs::fs().exists(_dir.c_str())) {
    // mkdir 失败必须发现：父目录不存在时后续 open(create=false) 全部 ENOENT，
    // 表现为「短信收发正常但记录存不进去」。这里显式报错并关闭存储。
    if (!userfs::fs().mkdir(_dir.c_str())) {
      _enabled = false;
      _log.addf(LogLevel::kError, "sms", "短信存储已停用：创建目录 %s 失败",
                _dir.c_str());
      return;
    }
  }
  _enabled = true;

  // 启动校准：计数 + 最大/最小 ID（仅开机一次扫描）
  _count = 0;
  _maxId = 0;
  _minId = 0xFFFFFFFF;
  lock();
  File root = userfs::fs().open(_dir.c_str());
  if (root) {
    File e = root.openNextFile();
    while (e) {
      uint32_t id = parseId(e.name());
      e.close();
      if (id) {
        _count++;
        if (id > _maxId) _maxId = id;
        if (id < _minId) _minId = id;
      }
      e = root.openNextFile();
    }
  }
  if (_count == 0) _minId = 0;
  unlock();
  _log.addf(LogLevel::kInfo, "sms",
            "短信存储就绪：%s（实际 /data%s），%d 条，最大 ID %u，上限 %u",
            _dir.c_str(), _dir.c_str(), _count, (unsigned)_maxId, (unsigned)_max);
}

// 写入一条记录（FIFO 淘汰最旧）
uint32_t SmsStore::add(const SmsMessage &msg) {
  if (!_enabled) return 0;
  lock();

  uint32_t id = _maxId + 1;
  String path = filePath(id);
  File f = userfs::fs().open(path.c_str(), "w");
  if (!f) {
    unlock();
    _log.addf(LogLevel::kError, "sms", "短信写入失败：无法创建文件 #%u", (unsigned)id);
    return 0;
  }
  f.printf("%u,%s,%s,%u,%d\r\n\r\n", (unsigned)id, msg.incoming ? "in" : "out",
           msg.phone.c_str(), (unsigned)msg.ts, (int)msg.modem);
  f.print(msg.content);
  f.close();
  _maxId = id;
  if (_minId == 0 || id < _minId) _minId = id;
  _count++;
  _todayValid = false;   // 记录集变化，今日统计缓存失效（下次查询重算）

  // 超限：id 单调递增，最旧者 id 最小，从 _minId 顺序推进删除（O(1)，无全目录扫描）。
  // 稳态下每条新增最多淘汰一条，故单次 add 循环必在删一条后退出。
  while (_count > _max && _minId < _maxId) {
    if (userfs::fs().exists(filePath(_minId).c_str())) {
      userfs::fs().remove(filePath(_minId).c_str());
      _count--;
      _log.addf(LogLevel::kWarn, "sms", "短信存储已满，淘汰最旧记录 #%u",
                (unsigned)_minId);
      _minId++;
      break;
    }
    _minId++;  // 该 id 已被删（不应发生），继续找下一个
  }
  unlock();
  return id;
}

// 分页 + 关键字 + 模组过滤
// 设计：加锁只做目录枚举（取文件名）；之后解锁，只读首行头做过滤/排序；
// 仅对命中且进入当前页的记录按需补读正文。全程不持锁读取上千个文件正文，
// 避免阻塞抽数任务（短信写入）与并发删除。
void SmsStore::list(int page, int pageSize, const String &q, int8_t modem,
                    SmsList &out, int &total) {
  out.clear();
  total = 0;
  if (!_enabled) return;
  if (page < 1) page = 1;
  if (pageSize < 1) pageSize = 20;
  if (pageSize > cfg::kSmsPageSizeMax) pageSize = cfg::kSmsPageSizeMax;

  // 1) 加锁枚举目录，仅收集文件名后立刻解锁（枚举很快，不含正文读取）
  std::vector<uint32_t> ids;
  {
    lock();
    File root = userfs::fs().open(_dir.c_str());
    if (root) {
      File e = root.openNextFile();
      while (e) {
        uint32_t id = parseId(e.name());
        e.close();
        if (id) ids.push_back(id);
        e = root.openNextFile();
      }
    }
    unlock();
  }

  // 2) 读首行头（不加锁，偶发与写入并发仅影响快照一致性，无破坏）
  std::vector<SmsMessage> cand;
  cand.reserve(ids.size());
  for (uint32_t id : ids) {
    SmsMessage m;
    if (readHeader(id, m)) cand.push_back(m);
  }

  // 3) 过滤（模组 + 关键字）；关键字命中正文时按候选补读正文
  String ql = q;
  ql.toLowerCase();
  bool hasQ = q.length() > 0;
  std::vector<SmsMessage> matched;
  matched.reserve(cand.size());
  for (const SmsMessage &m : cand) {
    if (modem >= 0 && m.modem != modem) continue;
    if (!hasQ) {
      matched.push_back(m);
      continue;
    }
    String pl = m.phone;
    pl.toLowerCase();
    if (pl.indexOf(ql) >= 0) {
      matched.push_back(m);  // 号码命中（正文尚未读，分页时再补）
      continue;
    }
    // 需匹配正文：补读该条正文
    SmsMessage full;
    if (readFull(m.id, full)) {
      String cl = full.content;
      cl.toLowerCase();
      if (cl.indexOf(ql) >= 0) matched.push_back(full);
    }
  }

  // 时间倒序，相同按 ID 倒序
  std::sort(matched.begin(), matched.end(), [](const SmsMessage &a, const SmsMessage &b) {
    if (a.ts != b.ts) return a.ts > b.ts;
    return a.id > b.id;
  });
  total = (int)matched.size();

  // 4) 分页：仅对当前页补读正文（不加锁）
  int start = (page - 1) * pageSize;
  if (start >= total) return;
  int end = start + pageSize;
  if (end > total) end = total;
  for (int i = start; i < end; i++) {
    SmsMessage m = matched[i];
    if (!m.content.length()) readFull(m.id, m);  // 仅按号码命中的补读正文
    out.push_back(m);
  }
}

// 取最近 n 条（ID 倒序）：从 _maxId 向下扫，跳过缺失（已删除/已淘汰）的 ID。
// 扫的次数带上限，存储被清空后不会退化成整段遍历。
int SmsStore::latest(int n, SmsList &out) {
  out.clear();
  if (!_enabled || n <= 0) return 0;
  lock();
  uint32_t id = _maxId;
  const uint32_t floor = _minId ? _minId : 1;
  int guard = n * 4 + 16;
  while ((int)out.size() < n && guard-- > 0 && id >= floor) {
    SmsMessage m;
    if (readFull(id, m)) out.push_back(m);
    if (id == 0) break;
    id--;
  }
  unlock();
  return (int)out.size();
}

// 读取单条
bool SmsStore::detail(uint32_t id, SmsMessage &out) {
  if (!_enabled || !id) return false;
  lock();
  bool ok = readFull(id, out);
  unlock();
  return ok;
}

// 删除单条
bool SmsStore::deleteOne(uint32_t id) {
  if (!_enabled || !id) return false;
  lock();
  String path = filePath(id);
  bool existed = userfs::fs().exists(path.c_str());
  if (existed) {
    userfs::fs().remove(path.c_str());
    _count--;
    if (id == _minId) _minId = id + 1;  // 删的是最旧者，顺推最小 ID
  }
  unlock();
  return existed;
}

// 清空全部
void SmsStore::clear() {
  if (!_enabled) return;
  lock();
  File root = userfs::fs().open(_dir.c_str());
  if (root) {
    File e = root.openNextFile();
    while (e) {
      uint32_t id = parseId(e.name());  // 用 ID 重建路径，兼容 name() 返回全路径
      e.close();
      if (id) userfs::fs().remove(filePath(id).c_str());
      e = root.openNextFile();
    }
  }
  _count = 0;
  _maxId = 0;
  _minId = 0;
  _todayValid = false;
  unlock();
  _log.addf(LogLevel::kInfo, "sms", "短信存储已清空");
}

// 存储统计
String SmsStore::statsJson() {
  if (!_enabled) return "{\"ok\":false,\"error\":\"store disabled\"}";
  size_t total = userfs::totalBytes();
  size_t used = userfs::usedBytes();
  return String("{\"ok\":true,\"total\":") + _count + ",\"max\":" + _max +
         ",\"used\":" + used + ",\"fsTotal\":" + total + "}";
}

// 今日短信统计（收/发）：本地午夜为界。结果缓存，跨午夜失效后重算一次
// （扫描所有记录首行，最多 1000 条，仅在每天首次查询时触发，开销可控）。
void SmsStore::todayCount(int &recv, int &sent) {
  recv = 0;
  sent = 0;
  if (!_enabled) return;

  time_t now = time(nullptr);
  if (now < 1600000000) return;            // 时钟未同步，无法判定「今日」
  struct tm t;
  localtime_r(&now, &t);
  t.tm_hour = 0; t.tm_min = 0; t.tm_sec = 0;
  time_t midnight = mktime(&t);            // 本地午夜 epoch
  if (midnight < 0) return;

  if (_todayValid && _todayMidnight == (uint32_t)midnight) {
    recv = _todayRecv; sent = _todaySent;
    return;
  }

  int r = 0, s = 0;
  lock();
  for (uint32_t id = _minId; id != 0 && id <= _maxId; id++) {
    SmsMessage m;
    if (!readHeader(id, m)) continue;      // 已删除 / 已淘汰的 ID 跳过
    if (m.ts >= (uint32_t)midnight) {
      if (m.incoming) r++; else s++;
    }
  }
  unlock();

  _todayRecv = r; _todaySent = s;
  _todayMidnight = (uint32_t)midnight; _todayValid = true;
  recv = r; sent = s;
}

// 文件路径
String SmsStore::filePath(uint32_t id) const {
  return _dir + "/" + String(id) + ".txt";
}

// 从文件名解析 ID（取末尾 .txt 前的数字）
uint32_t SmsStore::parseId(const String &name) {
  // 取最后一段 '/' 之后的部分，兼容 name() 返回全路径的实现
  String base = name;
  int slash = base.lastIndexOf('/');
  if (slash >= 0) base = base.substring(slash + 1);
  if (base.endsWith(".txt")) base = base.substring(0, base.length() - 4);
  if (!base.length() || base.length() > 9) return 0;
  for (size_t i = 0; i < base.length(); i++) {
    if (base[i] < '0' || base[i] > '9') return 0;
  }
  return (uint32_t)base.toInt();
}

// 只读首行头（不含正文），不加锁
bool SmsStore::readHeader(uint32_t id, SmsMessage &out) {
  String path = filePath(id);
  File f = userfs::fs().open(path.c_str(), "r");
  if (!f) return false;
  String header = f.readStringUntil('\n');
  f.close();

  header.trim();
  int p1 = header.indexOf(',');
  if (p1 < 0) return false;
  int p2 = header.indexOf(',', p1 + 1);
  if (p2 < 0) return false;
  int p3 = header.indexOf(',', p2 + 1);
  if (p3 < 0) return false;
  int p4 = header.indexOf(',', p3 + 1);

  out = SmsMessage();
  out.id = id;
  out.incoming = header.substring(p1 + 1, p2) == "in";
  out.phone = header.substring(p2 + 1, p3);
  out.ts = (uint32_t)header.substring(p3 + 1, p4 < 0 ? header.length() : p4).toInt();
  out.modem = p4 < 0 ? -1 : (int8_t)header.substring(p4 + 1).toInt();
  return true;
}

// 读首行头 + 正文，不加锁（调用方按需加锁）
bool SmsStore::readFull(uint32_t id, SmsMessage &out) {
  String path = filePath(id);
  File f = userfs::fs().open(path.c_str(), "r");
  if (!f) return false;
  String header = f.readStringUntil('\n');
  String content = f.readString();  // 正文 = 空行之后的全部内容
  f.close();

  header.trim();
  int p1 = header.indexOf(',');
  if (p1 < 0) return false;
  int p2 = header.indexOf(',', p1 + 1);
  if (p2 < 0) return false;
  int p3 = header.indexOf(',', p2 + 1);
  if (p3 < 0) return false;
  int p4 = header.indexOf(',', p3 + 1);

  out = SmsMessage();
  out.id = id;
  out.incoming = header.substring(p1 + 1, p2) == "in";
  out.phone = header.substring(p2 + 1, p3);
  out.ts = (uint32_t)header.substring(p3 + 1, p4 < 0 ? header.length() : p4).toInt();
  out.modem = p4 < 0 ? -1 : (int8_t)header.substring(p4 + 1).toInt();
  // 去掉正文前的空行（readString 从当前位置继续，空行只剩一个 \r\n）
  if (content.startsWith("\n")) content = content.substring(1);
  else if (content.startsWith("\r\n")) content = content.substring(2);
  out.content = content;
  return true;
}
