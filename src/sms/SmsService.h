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

  // 经通信模组执行一次 ICMP ping（AT+CGPING，覆盖 SIMCOM/UNISOC 等常见 Cat.1 模组）：
  // 先尽力激活 PDP 上下文，再发 CGPING；所有 AT 经 sendAt 发出，发完自动还原短信模式
  // （CMGF/CNMI），不影响短信收发。host 空时回退 8.8.8.8。成功返回 true（至少收到
  // 一个回包），report 写回人类可读结果；失败返回 false（含原因与原始模块响应片段）。
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
    uint32_t lastOkMs = 0;     // 上次 AT 成功响应时间（探活/诊断用）
    uint32_t lastSendMs = 0;   // 上次发送短信时间（发送窗口内 pollInit 让行，避免干扰 CMGS 握手）
    bool needReinit = false;   // 运行时检测到通信模组重启 -> 重新开始初始化
    bool everReady = false;    // 是否曾进入过就绪态（区分开机与运行时 SIM 事件）
    bool simRemovedNotified = false;  // 已通知过「SIM 拔出」（重新就绪时补发「插入」）

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
    uint32_t nextTeleMs = 0;   // 下次遥测查询时间（独立于心跳，节流）
    uint8_t  teleStep = 0;     // 遥测轮询步骤（0 CSQ / 1 COPS / 2 CEREG / 3 CGSN）
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
  void handleCmt(uint8_t port, const String &pduHex);

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
