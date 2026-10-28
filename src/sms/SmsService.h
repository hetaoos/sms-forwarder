#pragma once

#include <Arduino.h>
#include <functional>
#include <vector>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <ArduinoJson.h>

#include "app/Config.h"
#include "serial/SerialBridge.h"
#include "sms/BlackList.h"
#include "sms/ModemSnapshot.h"
#include "sms/SmsCodec.h"
#include "sms/SmsRecord.h"
#include "sms/SmsStore.h"

// 短信服务：双通信模组的短信收发、存储与通信模组管理。
//
// 接收：SerialBridge 抽数任务把 +CMT: <len>\r\n<pdu> 段的通信模组自发数据
//   经 onUnsolicited 交给本类（返回 true = 已消费；无论是否消费，该段都会以
//   kUrc 类型记入 AT 历史，保证主动上报在控制台可见），
//   PDU 解码 + 长短信重组 + 落盘都在该上下文快速完成（不回调串口类，
//   避免与抽数任务持有的锁死锁）。
// 发送：经 SerialBridge::sendSmsPdu() 两段式完成 CMGS 握手（互斥锁保证
//   抽数任务不会抢走响应字节）；握手过程以 kBg 类型记入 AT 历史。
// 通信模组配置（启用/禁用）持久化到 NVS：appcfg / sms，
//   {"ver":1,"max":200,"modems":[{"port":1,"enabled":true},...]}
class SmsService {
 public:
  explicit SmsService(SerialBridge &bridge);

  // 载入配置、准备存储、对启用通信模组下发 CMGF/CNMI
  void begin();

  // 周期调用（App::tick）：长短信重组超时清理
  void tick();

  // 串口自发数据消费（SerialBridge 抽数任务上下文）；返回 true 表示已消费
  bool onUnsolicited(uint8_t port, const String &text);

  // ---------------- 通信模组管理 ----------------
  bool modemEnabled(uint8_t port) const;
  // 启用/禁用通信模组（热配置 + 持久化）；port 越界返回 false
  bool setModemEnabled(uint8_t port, bool enabled);
  // 设置通信模组别名（持久化）；port 越界返回 false
  bool setModemAlias(uint8_t port, const String &alias);
  // 触发通信模组重新初始化（仅启用态有效；置 needReinit 由抽数任务状态机重跑）
  bool reinitPort(uint8_t port);

  // 注入黑名单过滤器（App 装配时调用）；非空时在 handleCmt 内先于落盘/转发做匹配
  void setBlackList(BlackList *bl) { _blacklist = bl; }

  // ---------------- 事件回调 / 时间供给 ----------------
  // 通信模组就绪事件：初始化状态机进入 kReady 时于抽数任务上下文触发。
  // 回调内只应置标记或做轻量操作，禁止阻塞式 AT（时间同步等由 App 转 loop 任务执行）。
  using ModemReadyFn = std::function<void(uint8_t port)>;
  void setOnModemReadyCb(ModemReadyFn fn) { _onReady = fn; }

  // 短信接收事件：PDU 解码 / 长短信重组完成后于抽数任务上下文触发
  // （无论是否落盘成功都会触发——短信确实已收到）。
  // 回调内只应做轻量操作（如邮件通知入队），禁止阻塞式 AT 与长时间字符串构建。
  using SmsReceivedFn = std::function<void(const String &sender, const String &text,
                                           uint32_t ts, int8_t modem)>;
  void setOnSmsReceivedCb(SmsReceivedFn fn) { _onSms = fn; }

  // SIM 就绪状态变更事件：运行时插卡 / 拔卡（抽数任务上下文触发）。
  // 仅运行时切换才触发——开机首次就绪与通信模组重启后的重新就绪不通知
  // （分别由启动邮件覆盖、且并非真实的插卡事件）。
  using SimChangeFn = std::function<void(uint8_t port, bool ready)>;
  void setOnSimChangeCb(SimChangeFn fn) { _onSim = fn; }

  // 来电事件（tick 主循环上下文触发，回调内只允许轻量入队）：
  // number 为主叫号码（未获取到时为「未知号码」/「号码保密」）；
  // modem 为收到来电的通信模组串口号。命中黑名单（来电作用域）时不触发。
  using CallReceivedFn = std::function<void(const String &number, uint32_t ts,
                                           int8_t modem)>;
  void setOnCallReceivedCb(CallReceivedFn fn) { _onCall = fn; }

  // 通信模组是否已就绪（启用且初始化完成，可收发短信）；port 越界返回 false
  bool modemReady(uint8_t port) const;

  // 所有启用通信模组的初始化是否已有结果：即不再处于「上电 / 等 AT / 等 SIM /
  // 射频 / 等注册 / 下发配置」等中间态（终态为 kReady 就绪或 kFault 故障）。
  // 供开机后需要等通信模组就绪再继续的流程使用（如启动邮件——邮件里要带上通信模组
  // 状态，太早发会显示「初始化中」）。通信模组缺失/损坏会走 kFault 终态，不会
  // 永久阻塞；无启用通信模组时返回 true。
  bool modemsSettled() const;

  // ---------------- 主动查询短信（通信模组存储兜底）----------------
  // 通信模组以 CNMI=2,1 接收时短信落在通信模组侧（本机收不到 +CMT 直投），
  // 由专职查询任务读回：AT+CMGL 列索引 -> AT+CMGR 读 PDU -> 复用收信处理链
  // 落盘 + 推送 -> 成功后 AT+CMGD 删除通信模组侧副本。
  // 请求异步执行（不阻塞调用方）；通信模组未就绪时请求保留，就绪后自动补跑。
  bool requestQuery(uint8_t port);
  // 对所有启用的通信模组各发起一次查询
  void requestQueryAll();
  // 查询状态 JSON（Web 展示）：各端口待查/执行中标记 + 上次查询结果
  String queryStatusJson() const;

  // ---------------- 定时主动查询（排程）----------------
  // 到点后由 tick() 置待查标记，查询任务异步执行——主循环内绝不做阻塞 AT。
  // 模式复用排程约定：cfg::kSchedModeInterval(1=固定间隔) / cfg::kSchedModeDaily(0=每日定点)。
  bool pollEnabled() const { return _pollEnabled; }
  uint8_t pollMode() const { return _pollMode; }
  uint16_t pollIntervalH() const { return _pollIntervalH; }
  uint16_t pollDailyMin() const { return _pollDailyMin; }
  // 应用定时查询配置并落盘；可含 enabled / mode / intervalH / hour / minute
  // 以及 safetySec（兜底轮询周期，秒，30-3600）
  bool applyPollConfig(JsonObjectConst o);
  // 兜底轮询周期（毫秒）：常开，与 +CMTI 自动上报互为补充
  uint32_t safetyPollMs() const { return _safetyPollMs; }

  // ---------------- 短信接收模式（CNMI）----------------
  // true = 存储模式（CNMI=2,1，短信落通信模组侧并上报 +CMTI 索引，配合主动查询）；
  // false = 直投模式（CNMI=2,2，新短信直接投递给设备）。
  // 切换后落盘保存，并由查询任务把新的 CNMI 热下发给已就绪的通信模组。
  bool storeMode() const { return _cnmiStore; }
  bool setStoreMode(bool on);

  // 时间戳供给：设备已同步时间时返回纪元秒，否则返回开机秒数。
  // 由 App 注入 TimeSync 的实现；未注入时退化为开机秒数。
  // 短信记录时间戳统一经此获取（前端按 >=1.6e9 区分真实时间/开机时长）。
  using TimeProviderFn = std::function<uint32_t()>;
  void setTimeProvider(TimeProviderFn fn) { _timeFn = fn; }
  uint32_t nowTs() const {
    return _timeFn ? _timeFn() : (uint32_t)(millis() / 1000);
  }
  // 系统状态供给（PING 命令回复用）：返回一段纯文本运行状态摘要。
  // 运行于 tick 主循环（pollAdminCmd），由 App 注入（可安全访问各子系统）。
  using SystemStatusFn = std::function<String()>;
  void setSystemStatusFn(SystemStatusFn fn) { _statusFn = fn; }
  // 通信模组列表 JSON：[{"port":1,"enabled":true,"ready":...,"state":"..."},...]
  String modemsJson() const;

  // 单个通信模组的 LCD 简报（状态页一行用，短文本）：
  //   "未启用" / "等AT" / "等SIM" / "等注册" / "配置中" / "故障" /
  //   "就绪 <运营商> <CSQ>"
  // port 越界返回空串。与 modemsJson 同样不加锁：只读若干 POD 字段，
  // 抽数任务并发改写最多让读数差一拍，不存在一致性风险。
  String modemBrief(uint8_t port) const;

  // 单个通信模组的完整状态快照（LCD 菜单「通信模组」页用）。
  // 与 modemBrief 同策略不加锁：只读 POD + String 字段，最多让读数差一拍。
  // port 越界返回 false（out 保持原样）。
  bool modemSnapshot(uint8_t port, ModemSnapshot &out) const;

  // 运营商短代码 -> 中文名（CMCC/CUCC/CTCC/CBN）；未知返回空串
  static String operCn(const String &code);

  // 导出通信模组配置（启用/别名，供备份用）到给定 JSON 对象：
  // o["max"]=n, o["adminPhones"]=["..",".."], o["modems"]=[{"port":1,"enabled":..,"alias":".."},...]
  void exportTo(JsonObject &o) const;

  // 从给定 JSON 对象导入通信模组配置并落盘；json 同 exportTo；成功返回 true
  bool importFrom(const JsonObject &o);

  // ---------------- 串口调试 ----------------
  // 经短信通信模组发送一条 AT 命令（供 Web 串口调试）。与短信收发共用同一端口锁，
  // 串行无锁层面冲突；发送期间令初始化/心跳轮询让行；通信模组就绪时自动还原
  // CMGF/CNMI，防止控制台命令误改短信配置导致收发失效。返回 AT 历史记录（含响应）。
  SerialBridge::Result sendAt(uint8_t port, uint32_t baud, const String &cmd,
                              SerialBridge::Eol eol, uint32_t timeoutMs, String &err);

  // ---------------- 发送 ----------------
  // 发送短信（自动拆分长短信并逐段 CMGS）。
  // 返回值：0 = 通信模组未发出（失败，err 有原因）；入库记录 ID = 成功；
  //   kIdSentUnstored = 通信模组已发出但记录未入库（userdata 异常）——不算失败，
  //   此时 stored 出参为 false，短信确实已发出，只是列表不会显示本条。
  // 参考实现同款语义：存储失败不影响发送结果。
  static constexpr uint32_t kIdSentUnstored = 0xFFFFFFFEu;
  uint32_t send(uint8_t port, const String &phone, const String &text, String &err,
                bool *stored = nullptr);

  // ---------------- 发送（异步队列）----------------
  // 统一发送入口：Web / 推送 / MQTT / 管理员命令都只入队，由专职 worker 任务串行
  // 执行阻塞式 CMGS 握手——没有任何调用方任务因等端口锁而被长时间阻塞（消除任务
  // 看门狗风险），同端口也不会字节交错。完成回调 done 在 tick 主循环上下文触发，
  // 可安全访问 Web/MQTT 等共享资源；done 可为空（仅发送不关心结果）。
  // 返回 true 表示已入队；队列满（默认 8）返回 false，调用方自行决定重试/拒绝。
  using SendDoneFn = std::function<void(uint32_t id, bool stored, const String &err)>;
  bool enqueueSend(uint8_t port, const String &phone, const String &content,
                   SendDoneFn done = nullptr);

  // ---------------- 存储 ----------------
  SmsStore &store() { return _store; }

  // ---------------- 来电检测参数（移植自 esp32-sms-forwarding 的 Call）----------------
  // 同号码去重窗口（ms）：模组每个响铃周期都会重发 RING/+CLIP，一通电话不能变成多条
  static constexpr uint32_t kCallDedupMs     = 10000;
  // RING 后等待 +CLIP / 主动 +CLCC 的最大时长（ms），超时以「未知号码」派发
  static constexpr uint32_t kCallClipWaitMs  = 10000;
  // RING 后多久主动发 AT+CLCC 查询来电号码（ms）。实测 ML307A + 电信不发 +CLIP，
  // 而是在 RING 后主动报一条 +CLCC:——那条才是即时的号码来源，本查询只是兜底
  static constexpr uint32_t kCallClccDelayMs = 3000;
  // 主动 AT+CLCC 查询的响应收集超时（ms）
  static constexpr uint32_t kCallClccTimeoutMs = 2000;

  // ---------------- 管理员短信远程控制（移植自 esp32-sms-forwarding）----------------
  // 总开关 _adminEnabled（默认 false = 禁用，须显式开启）。仅当总开关开启且配置了
  // 管理员号码（_adminPhoneCount > 0）时，命令短信才会被拦截执行；任一条件不满足则
  // 所有短信按普通短信处理。这样默认出厂/升级后功能处于关闭态，避免误触发。
  // 管理员号码列表（空 = 功能关闭），上限 cfg::kAdminPhoneMax 条。
  // 匹配时任一条命中即为管理员（双方剥 +86 前缀后精确相等）。
  // 命令在 handleCmt 内、黑名单之后、落盘/转发之前判定（与旧实现同序）：
  //   SMS:<号码>:<内容>   从收到命令的那个模组代发一条短信，并向【发命令的管理员】回复执行结果
  //   RESET              2 秒后重启设备（重启前先回复确认短信）
  //   PING               回复当前系统运行状态（在线/WiFi/IP/运行时间/内存/模组/MQTT…）
  //   HELP               回复命令清单与参数说明
  // 关键字（RESET/PING/HELP 与 SMS: 前缀）大小写不敏感。
  // 命中的命令本身不落盘、不转发；其余内容按普通短信处理（入库 + 转发）。
  // 代发与回复都走「收到命令的那个模组」——管理员发过去的是哪张卡的号码，
  // 回来就应是同一张卡，否则会被当垃圾短信。
  // 执行放在 tick 主循环：发送是阻塞式 AT 交互、重启要起延时任务，
  // 都禁止在抽数任务上下文（handleCmt）里跑。
  struct AdminCmd {
    enum Kind : uint8_t { kSms, kReset, kPing, kHelp };
    Kind     kind;
    uint8_t  port;    // 收到命令的通信模组串口号
    String   from;    // 发命令的管理员号码（执行结果回复给它）
    String   phone;   // kSms：代发目标号码
    String   content; // kSms：代发内容
  };

  // 当前管理员号码列表的 JSON 数组字符串（Web 读取用）：["138...","139..."]
  String adminPhonesJson() const;

  // 以给定列表整体替换：逐条校验格式（同发送号码规则）、归一化去重、
  // 超上限拒绝；空列表 = 关闭。校验失败返回 false 且运行态保持不变；
  // 成功后持久化到 NVS（写盘失败也返回 false，但内存已更新——与别名保存同策略）。
  bool setAdminPhones(const std::vector<String> &phones);

  // 管理员短信远程控制总开关（默认 false = 禁用）。仅当开启且配置了号码时生效。
  // 置位后持久化到 NVS；写盘失败返回 false（内存已更新——与别名保存同策略）。
  bool adminEnabled() const { return _adminEnabled; }
  bool setAdminEnabled(bool on);

  // 经通信模组执行一次 ICMP ping（AT+MPING=<host>,<count>,<timeout>,<size>,<ttl>，
  // 覆盖 SIMCOM/UNISOC 等常见 Cat.1 模组）：先尽力激活 PDP 上下文，再发 MPING；
  // 所有 AT 经 sendAt 发出，发完自动还原短信模式（CMGF/CNMI），不影响短信收发。
  // host 空时回退 8.8.8.8，默认 count=4/timeout=10000/size=32/ttl=15（见参数范围
  // AT+MPING=?：count(1-60) timeout(1-65535) size(1-1400) ttl(1-15)）。成功返回 true
  // （至少收到一个回包），report 写回人类可读结果；失败返回 false（含原因与原始响应片段）。
  bool pingHost(uint8_t port, const String &host, String &report);
  // 系统状态文本（PING 回复附带，复用 App 注入的 _statusFn；未注入返回空串）
  String systemStatusText() const;

  // 最新短信记录 ID（SSE diff 用）
  uint32_t lastSmsId() const { return _lastSmsId; }

  // 最新一条短信的摘要 JSON（SSE sms 事件用）；无记录返回 "{}"
  String latestJson();

 private:
  // 通信模组初始化状态机：从「上电」到「短信就绪」的全异步流程
  // （适配 esp32-sms-forwarding：等 AT 就绪 -> SIM -> 射频 -> 网络注册 ->
  //  配置逐条下发 -> 心跳探活；任一环节失败转故障态慢速重试，运行时通信模组重启自愈）
  enum class ModemInitState : uint8_t {
    kDown = 0,    // 未启用/已禁用
    kPowerOn,     // 硬复位（EN 引脚上下电）
    kWaitAt,      // 等 AT 就绪
    kWaitCpin,    // 等 SIM 就绪（CPIN READY）
    kBringRf,     // CFUN=1 射频上线
    kWaitReg,     // 等网络注册（CEREG/CREG）
    kConfig,      // 下发通信模组配置（COPS/CEREG/CGDCONT/CNMI/CMGF...）
    kReady,       // 完全就绪，可收发短信
    kFault,       // 故障/未连接：达最大重试，慢速重试直至恢复
  };

  // 每通信模组运行时状态（下标 = 串口号 1..kSmsMaxModems）
  struct ModemState {
    bool enabled = false;
    String alias;               // 通信模组别名（用户可设，展示用；如「客厅卡槽」）
    uint8_t csmsRef = 1;       // 长短信参考号计数器（每通信模组独立，非零）
    bool waitingPdu = false;   // +CMT 头之后的下一段即 PDU
    ModemInitState initState = ModemInitState::kDown;  // 初始化状态机当前态
    uint8_t initAttempts = 0;  // 等 AT 就绪尝试次数
    uint8_t regAttempts = 0;   // 网络注册尝试次数
    uint8_t configStep = 0;    // 配置逐条下发索引（kPowerOn 时复用为阶段）
    uint8_t failCount = 0;     // 心跳/就绪后连续失败计数
    uint32_t nextPollMs = 0;   // 下次动作时间（节流，避免 AT 风暴）
    uint32_t lastActivityMs = 0;  // 上次模组有活动(响应/URC 上报)的时刻；心跳仅在其过期(30s)时才发 AT
    uint32_t cmdPauseUntil = 0;  // 暂停其他后台指令的绝对时刻；短信发送前标记、每收到短信延长（见使用处）
    bool needReinit = false;   // 运行时检测到通信模组重启 -> 重新开始初始化
    bool everReady = false;    // 是否曾进入过就绪态（区分开机与运行时 SIM 事件）
    bool     simRemovedNotified = false;  // 已通知过「SIM 拔出」（重新就绪时补发「插入」）

    // ---- 主动查询（通信模组存储兜底）----
    bool    queryPending = false;    // 有待执行的存储查询（+CMTI / 定时 / 手动触发）
    bool    queryRunning = false;    // 查询任务正在处理本端口
    uint32_t queryLastFound = 0;    // 上次查询发现的存量条数
    uint32_t queryLastHandled = 0;  // 上次查询落盘处理的条数

    // ---- 来电检测（RING / +CLIP / +CLCC，移植自 esp32-sms-forwarding 的 Call）----
    bool     callPending = false;        // RING 已到：等待号码或通话结束
    bool     callDispatch = false;       // 有来电待派发（tick 消费后清位）
    String   callNumber;                 // 已获取到的主叫号码（空=未知）
    uint32_t callClipWaitUntilMs = 0;    // 等 +CLIP/+CLCC 的窗口截止时刻
    uint32_t callLastNotifyMs = 0;       // 上次派发时刻（去重窗口，见 kCallDedupMs）
    bool     callClccTried = false;      // 本轮是否已主动查询过 AT+CLCC
    uint32_t callClccDueMs = 0;          // 主动查询 AT+CLCC 的时刻

    // ---- 就绪后遥测（周期轮询，供 UI 展示）----
    int8_t  csq = -1;          // 信号质量 0..31（99=未知），-1 表示未查询
    String  oper;               // 运营商名称（AT+COPS? 长格式）
    uint8_t act = 0;           // 接入技术（AT+CREG/CEREG 末字段：0 GSM,2 UTRAN,4 E-UTRAN...）
    String  reg;               // 网络注册状态文本（已注册/搜索中/未注册...）
    bool    simReady = false;  // SIM 卡是否就绪（AT+CPIN? READY）
    String  imei;              // 通信模组 IMEI（就绪后查一次）
    String  num;               // 本机号码（SIM 号码，AT+CNUM；部分 SIM 不含则为空）
    String  vendor;            // 通信模组厂商（AT+CGMI，如 MLINK）
    String  model;             // 通信模组型号（AT+CGMM，如 ML307A-DSLN）
    String  revision;          // 通信模组版本/完整料号（AT+CGMR，常含后缀如 -MTSH1S00 或固件版本）
    String  operCode;          // 运营商短代码（CMCC/CUCC/CTCC/CBN，由 AT+COPS 解析）
    // 一次性静态采集（IMEI/号码/厂商/型号/版本/运营商）：就绪(reinit)后仅执行一次并永久缓存
    bool     identityQueried = false;  // 静态信息是否已采集并缓存
    uint8_t  idStep = 0;               // 静态采集步骤（0 IMEI / 1 CNUM / 2 CGMI / 3 CGMM / 4 CGMR / 5 COPS）
    uint32_t nextIdMs = 0;             // 下条静态采集命令时间（节流）
    // 动态遥测：仅轮询真正变化的数据，各查询独立计时（每 tick 至多一条，避免持锁过久）
    uint32_t nextCsqMs = 0;            // AT+CSQ 下次查询时间
    uint32_t nextOpsMs = 0;            // AT+COPS? 下次查询时间（运营商，注册跃迁 URC 触发刷新）
    int8_t   lastRegStat = -1;         // 上次注册 stat，用于检测「未注册↔已注册」跃迁以刷新运营商
    bool     operRefreshPending = false; // 注册跃迁触发运营商刷新（成功后清除）
    bool     regSynced = false;        // 就绪后一次性 AT+CEREG? 同步是否完成（仅执行一次）
  };

  // 启用/禁用后启动/停止异步初始化（状态机交由 tick 驱动）
  void startInit(uint8_t port);
  void stopInit(uint8_t port);

  // 状态机每轮驱动（由 SerialBridge 抽数任务轮询回调调用，运行于抽数任务上下文，
  // 非阻塞：内部用 trySend，端口忙即跳过；受 nextPollMs 节流）；port 越界忽略
  void pollInit(uint8_t port);

  // 解析注册状态响应中的 stat 字段（+CEREG:/+CREG: 第二字段），失败返回 -1
  static int parseRegStat(const String &resp, const char *prefix);

  // 初始化态 -> 可读字符串（SSE/JSON 用）
  static const char *initStateName(ModemInitState s);

  // 通信模组对应 EN 复位引脚 / 有效电平（来自 Config）
  int8_t enPin(uint8_t port) const;
  bool enActiveLow() const;

  // 对通信模组异步下发 PDU 模式 / 新短信直投配置（仅启动状态机，不阻塞）
  bool configureModem(uint8_t port, bool enabled);

  // 发送一段 PDU（CMGS 两段式握手）；失败写入 err
  bool sendPduPart(uint8_t port, const SmsPduPart &part, String &err);

  // 处理一条收到的 PDU（解码 + 重组 + 落盘）
  // 返回值决定通信模组侧副本能否删除（存储模式查询链路据此决定是否发 AT+CMGD）
  enum class CmtOutcome : uint8_t {
    kKeep,    // 保留：解码失败 / 长短信未收齐 / 落盘失败 -> 下轮查询重试
    kDelete,  // 可删：已落盘 / 命中黑名单丢弃 / 管理员命令已消费
  };
  CmtOutcome handleCmt(uint8_t port, const String &pduHex);

  // 处理一条已解码的短信（长短信分段在此重组）；调用方须持有 _codecMutex
  CmtOutcome processDecodedLocked(uint8_t port, const String &sender,
                                  const String &ts, const String &text,
                                  const int *concat);

  // 当前 CNMI 配置串：存储模式（2,1，落存储 + 上报 +CMTI 索引）/ 直投模式（2,2）
  const char *cnmiCmd() const {
    return _cnmiStore ? "AT+CNMI=2,1,0,0,0" : "AT+CNMI=2,2,0,0,0";
  }

  // ---------------- 主动查询任务（通信模组存储兜底）----------------
  // 专职任务：轮询各端口的待查标记 -> 阻塞 AT 读回模组存储 -> 复用收信处理链
  // -> 成功后删除模组侧副本。查询失败不删除任何内容，下轮重试。
  static void queryTaskEntry(void *arg);
  void queryTask();
  // 查询一个通信模组的存储短信；返回 false = 查询失败（AT 无响应/协议异常）
  bool queryPort(uint8_t port);
  // 解析 AT+CMGL 响应中的短信索引列表
  static void parseSmsIndexes(const String &resp, std::vector<int> &out);
  // 从 AT+CMGR 响应中提取 PDU 十六进制串（无有效 PDU 返回空串）
  static String extractPduHex(const String &resp);
  // 去重表：命中（TTL 内同键）返回 true；仅在落盘成功后登记
  bool dedupHit(const String &key);
  void dedupAdd(const String &key);

  // 判断定时查询是否到期；到期仅置待查标记（真正的 AT 由查询任务执行）
  void pollSchedule();
  // 解析并应用定时查询配置字段（不落盘；载入与设置共用）
  void setPollFields(JsonObjectConst o);

  // 设置 SIM 就绪态并检测运行时切换（插卡/拔卡通知）。
  // 开机首次就绪（everReady=false）与通信模组重启后的重新就绪（未通知过拔出）
  // 均不触发事件，避免每次开机/通信模组重启都误报「SIM 已插入」。
  void setSimReady(uint8_t port, bool ready);

  // ---- 来电检测（URC 在抽数任务上下文解析，派发在 tick 主循环）----
  // RING / +CRING：开始一轮来电（去重窗口内的重复 RING 忽略）
  void handleCallRing(uint8_t port);
  // +CLIP: 主动上报主叫号码；无先导 RING 时也按来电处理（该行已带齐通知所需信息）
  void handleCallClip(uint8_t port, const String &line);
  // 主动上报的 +CLCC:（ML307A + 电信的即时号码来源）；解析格式同 AT+CLCC 响应
  void handleCallClcc(uint8_t port, const String &line);
  // 通话结束上报（NO CARRIER / BUSY / NO ANSWER）：已有号码立即派发，不再等满窗口
  void handleCallEnd(uint8_t port, const String &line);
  // tick 驱动：派发待通知来电、超时收口、主动 AT+CLCC 兜底查询
  void pollCall(uint8_t port);
  // 实际派发（黑名单判定 + 回调）；运行于 tick 主循环上下文
  void dispatchCall(uint8_t port);
  // 从 +CLCC: 行/响应中提取主叫号码（第 4 个字段后的首个引号对），失败返回空串
  static String parseClccNumber(const String &text);

  // ---- 管理员短信远程控制 ----
  // 号码归一化后精确相等（剥 +86 前缀、去空白）；任一为空返回 false
  static bool phonesEqual(const String &a, const String &b);
  // sender 是否命中管理员列表（持锁遍历；抽数任务上下文调用）
  bool isAdmin(const String &sender);
  // 解析并把管理员命令入队（抽数任务上下文调用，只置状态不执行）。
  // 返回 false = 不是有效命令（SMS: 格式错误），调用方应按普通短信继续处理；
  // 队列被占用（上一条尚未执行）时丢弃新的并记日志，但仍算已消费（返回 true）。
  bool enqueueAdminCmd(uint8_t port, const String &sender, const String &cmd);
  // 执行队首管理员命令（tick 主循环上下文：代发/回复/重启都在这里做）
  void pollAdminCmd();
  // 向发命令的管理员回复一条执行结果短信（从同一模组发出）；失败仅记日志
  void replyToAdmin(uint8_t port, const String &to, const String &text);

  // 载入 / 保存 NVS 配置；返回是否有有效记录
  bool loadConfig();
  bool saveConfig();

  // 取/放互斥锁（保护 _lastSmsId 与配置读写）
  void lock() const { xSemaphoreTake(_mutex, portMAX_DELAY); }
  void unlock() const { xSemaphoreGive(_mutex); }

  // ---- 统一发送队列（见 enqueueSend）----
  static constexpr uint8_t kSendQueueLen = 8;    // 待发送任务容量
  static constexpr uint8_t kDoneQueueLen = 16;   // 完成事件容量（tick 每轮清空）
  QueueHandle_t _sendQ = nullptr;   // 元素为 SendJob*（入队用 xQueueSend(...,0) 不阻塞）
  QueueHandle_t _doneQ = nullptr;   // 元素为 DoneEvent*（worker 完成 -> tick 消费）
  TaskHandle_t  _sendTask = nullptr;
  // 待发送任务（堆分配，指针经队列传递，避免 std::function 直接跨队列拷贝）
  struct SendJob {
    uint8_t    port;
    String     phone;
    String     content;
    SendDoneFn done;
  };
  // 完成事件：worker 执行完 SendJob 后投递，由 tick 主循环取出并触发 done 回调
  struct DoneEvent {
    SendDoneFn done;
    uint32_t   id;
    bool       stored;
    String     err;
  };
  static void sendTaskEntry(void *arg);
  void sendTask();

  // ---------------- 主动查询（通信模组存储兜底）----------------
  TaskHandle_t _queryTask = nullptr;    // 查询任务句柄
  SemaphoreHandle_t _codecMutex = nullptr;  // 保护 PDU 解码（pdulib 实例非线程安全）与重组槽位
  bool _cnmiStore = true;               // true = CNMI=2,1（落存储 + +CMTI 索引）；false = 2,2 直投
  bool _cnmiDirty = false;              // 存储模式切换后待热下发 CNMI（由查询任务执行）
  bool _deleteAfterRead = true;         // 查询处理成功后删除通信模组侧副本
  // 去重表（环形）：键 = 发送方|时间戳|正文长度|正文 CRC，防删除失败后重复落盘
  struct DedupItem {
    String   key;
    uint32_t ms = 0;
  };
  DedupItem _dedup[cfg::kSmsDedupSlots];
  uint8_t   _dedupNext = 0;             // 环形写指针
  uint32_t  _queryLastMs = 0;           // 上次查询完成时刻
  uint32_t  _safetyLastMs = 0;          // 上次兜底轮询触发时刻（+CMTI 丢失时的第二重保险）
  uint32_t  _safetyPollMs = cfg::kSmsSafetyPollMs;  // 兜底轮询周期（毫秒，可在配置页调整）

  // ---------------- 定时主动查询（排程）----------------
  bool     _pollEnabled = false;                        // 总开关（默认关）
  uint8_t  _pollMode = cfg::kSchedModeInterval;         // 0=每日定点 1=固定间隔
  uint16_t _pollIntervalH = cfg::kSchedIntervalHDefault;// 间隔模式周期（小时）
  uint16_t _pollDailyMin = 0;                           // 每日定点（分钟，0..1439）
  uint32_t _pollBaseMs = 0;                             // 间隔模式基线（开机 millis）
  uint32_t _pollLastDay = 0;                            // 上次每日触发天序号（持久化防重复）

  SerialBridge &_bridge;    // AT 串口桥（send 通道）
  SmsStore _store;          // 文件存储
  SmsCodec _codec;          // PDU 编解码 + 重组
  ModemState _modems[cfg::kSmsMaxModems + 1];  // 下标 = 串口号
  SmsConcatSlot _concat[cfg::kSmsConcatSlots]; // 长短信重组槽位
  uint16_t _max = cfg::kSmsStoreMax;           // 存储条数上限
  uint32_t _lastSmsId = 0;                     // 最新记录 ID（SSE diff）
  mutable SemaphoreHandle_t _mutex = nullptr;  // 保护 _lastSmsId / 配置读写

  ModemReadyFn _onReady;    // 通信模组就绪回调（抽数任务上下文触发）
  SmsReceivedFn _onSms;     // 短信接收回调（抽数任务上下文触发）
  SimChangeFn _onSim;       // SIM 就绪状态变更回调（抽数任务上下文触发）
  CallReceivedFn _onCall;   // 来电视题回调（tick 主循环上下文触发）
  TimeProviderFn _timeFn;   // 时间戳供给（App 注入；为空则开机秒数）
  SystemStatusFn _statusFn; // 系统状态供给（App 注入；PING 命令回复用）

  String   _adminPhones[cfg::kAdminPhoneMax];  // 管理员号码列表
  uint8_t  _adminPhoneCount = 0;               // 有效条数（与 _adminPhones 同步更新）
  bool     _adminEnabled = false;              // 总开关（默认禁用，须显式开启）
  AdminCmd _adminCmd;          // 待执行的管理员命令（_adminCmdPending 为真时有效）
  bool     _adminCmdPending = false;  // 有管理员命令待 tick 执行
  BlackList *_blacklist = nullptr;  // 黑名单过滤器（App 注入；空表示不拦截）
};
