#pragma once

#include <Arduino.h>
#include <functional>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include "app/Config.h"
#include "serial/SerialBridge.h"
#include "sms/SmsCodec.h"
#include "sms/SmsRecord.h"
#include "sms/SmsStore.h"

// 短信服务：双 4G 模组的短信收发、存储与模组管理。
//
// 接收：SerialBridge 抽数任务把 +CMT: <len>\r\n<pdu> 段的模组自发数据
//   经 onUnsolicited 交给本类（返回 true = 已消费，不进 AT 历史），
//   PDU 解码 + 长短信重组 + 落盘都在该上下文快速完成（不回调串口类，
//   避免与抽数任务持有的锁死锁）。
// 发送：经 SerialBridge::send() 两段式完成 CMGS 握手（互斥锁保证
//   抽数任务不会抢走响应字节）；PDU 流量 quiet 记入，不冲刷 AT 历史。
// 模组配置（启用/禁用）持久化到 NVS：appcfg / sms，
//   {"ver":1,"max":200,"modems":[{"port":1,"enabled":true},...]}
class SmsService {
 public:
  explicit SmsService(SerialBridge &bridge);

  // 载入配置、准备存储、对启用模组下发 CMGF/CNMI
  void begin();

  // 周期调用（App::tick）：长短信重组超时清理
  void tick();

  // 串口自发数据消费（SerialBridge 抽数任务上下文）；返回 true 表示已消费
  bool onUnsolicited(uint8_t port, const String &text);

  // ---------------- 模组管理 ----------------
  bool modemEnabled(uint8_t port) const;
  // 启用/禁用模组（热配置 + 持久化）；port 越界返回 false
  bool setModemEnabled(uint8_t port, bool enabled);
  // 设置模组别名（持久化）；port 越界返回 false
  bool setModemAlias(uint8_t port, const String &alias);
  // 触发模组重新初始化（仅启用态有效；置 needReinit 由抽数任务状态机重跑）
  bool reinitPort(uint8_t port);

  // ---------------- 事件回调 / 时间供给 ----------------
  // 模组就绪事件：初始化状态机进入 kReady 时于抽数任务上下文触发。
  // 回调内只应置标记或做轻量操作，禁止阻塞式 AT（时间同步等由 App 转 loop 任务执行）。
  using ModemReadyFn = std::function<void(uint8_t port)>;
  void setOnModemReadyCb(ModemReadyFn fn) { _onReady = fn; }

  // 模组是否已就绪（启用且初始化完成，可收发短信）；port 越界返回 false
  bool modemReady(uint8_t port) const;

  // 时间戳供给：设备已同步时间时返回纪元秒，否则返回开机秒数。
  // 由 App 注入 TimeSync 的实现；未注入时退化为开机秒数。
  // 短信记录时间戳统一经此获取（前端按 >=1.6e9 区分真实时间/开机时长）。
  using TimeProviderFn = std::function<uint32_t()>;
  void setTimeProvider(TimeProviderFn fn) { _timeFn = fn; }
  uint32_t nowTs() const {
    return _timeFn ? _timeFn() : (uint32_t)(millis() / 1000);
  }
  // 模组列表 JSON：[{"port":1,"enabled":true,"ready":...,"state":"..."},...]
  String modemsJson() const;

  // ---------------- AT 控制台 ----------------
  // 经短信模块发送一条 AT 命令（供 Web AT 控制台）。与短信收发共用同一端口锁，
  // 串行无锁层面冲突；发送期间令初始化/心跳轮询让行；模组就绪时自动还原
  // CMGF/CNMI，防止控制台命令误改短信配置导致收发失效。返回 AT 历史记录（含响应）。
  SerialBridge::Result sendAt(uint8_t port, uint32_t baud, const String &cmd,
                              SerialBridge::Eol eol, uint32_t timeoutMs, String &err);

  // ---------------- 发送 ----------------
  // 发送短信（自动拆分长短信并逐段 CMGS）。
  // 返回值：0 = 模组未发出（失败，err 有原因）；入库记录 ID = 成功；
  //   kIdSentUnstored = 模组已发出但记录未入库（userdata 异常）——不算失败，
  //   此时 stored 出参为 false，短信确实已发出，只是列表不会显示本条。
  // 参考实现同款语义：存储失败不影响发送结果。
  static constexpr uint32_t kIdSentUnstored = 0xFFFFFFFEu;
  uint32_t send(uint8_t port, const String &phone, const String &text, String &err,
                bool *stored = nullptr);

  // ---------------- 存储 ----------------
  SmsStore &store() { return _store; }

  // 最新短信记录 ID（SSE diff 用）
  uint32_t lastSmsId() const { return _lastSmsId; }

  // 最新一条短信的摘要 JSON（SSE sms 事件用）；无记录返回 "{}"
  String latestJson();

 private:
  // 模组初始化状态机：从「上电」到「短信就绪」的全异步流程
  // （适配 esp32-sms-forwarding：等 AT 就绪 -> SIM -> 射频 -> 网络注册 ->
  //  配置逐条下发 -> 心跳探活；任一环节失败转故障态慢速重试，运行时模组重启自愈）
  enum class ModemInitState : uint8_t {
    kDown = 0,    // 未启用/已禁用
    kPowerOn,     // 硬复位（EN 引脚上下电）
    kWaitAt,      // 等 AT 就绪
    kWaitCpin,    // 等 SIM 就绪（CPIN READY）
    kBringRf,     // CFUN=1 射频上线
    kWaitReg,     // 等网络注册（CEREG/CREG）
    kConfig,      // 下发模组配置（COPS/CEREG/CGDCONT/CNMI/CMGF...）
    kReady,       // 完全就绪，可收发短信
    kFault,       // 故障/未连接：达最大重试，慢速重试直至恢复
  };

  // 每模组运行时状态（下标 = 串口号 1..kSmsMaxModems）
  struct ModemState {
    bool enabled = false;
    String alias;               // 模组别名（用户可设，展示用；如「客厅卡槽」）
    uint8_t csmsRef = 1;       // 长短信参考号计数器（每模组独立，非零）
    bool waitingPdu = false;   // +CMT 头之后的下一段即 PDU
    ModemInitState initState = ModemInitState::kDown;  // 初始化状态机当前态
    uint8_t initAttempts = 0;  // 等 AT 就绪尝试次数
    uint8_t regAttempts = 0;   // 网络注册尝试次数
    uint8_t configStep = 0;    // 配置逐条下发索引（kPowerOn 时复用为阶段）
    uint8_t failCount = 0;     // 心跳/就绪后连续失败计数
    uint32_t nextPollMs = 0;   // 下次动作时间（节流，避免 AT 风暴）
    uint32_t lastOkMs = 0;     // 上次 AT 成功响应时间（探活/诊断用）
    uint32_t lastSendMs = 0;   // 上次发送短信时间（发送窗口内 pollInit 让行，避免干扰 CMGS 握手）
    bool needReinit = false;   // 运行时检测到模组重启 -> 重新开始初始化

    // ---- 就绪后遥测（周期轮询，供 UI 展示）----
    int8_t  csq = -1;          // 信号质量 0..31（99=未知），-1 表示未查询
    String  oper;               // 运营商名称（AT+COPS? 长格式）
    uint8_t act = 0;           // 接入技术（AT+CREG/CEREG 末字段：0 GSM,2 UTRAN,4 E-UTRAN...）
    String  reg;               // 网络注册状态文本（已注册/搜索中/未注册...）
    bool    simReady = false;  // SIM 卡是否就绪（AT+CPIN? READY）
    String  imei;              // 模组 IMEI（就绪后查一次）
    String  num;               // 本机号码（SIM 号码，AT+CNUM；部分 SIM 不含则为空）
    String  vendor;            // 模组厂商（AT+CGMI，如 MLINK）
    String  model;             // 模组型号（AT+CGMM，如 ML307A-DSLN）
    String  revision;          // 模组版本/完整料号（AT+CGMR，常含后缀如 -MTSH1S00 或固件版本）
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

  // 模组对应 EN 复位引脚 / 有效电平（来自 Config）
  int8_t enPin(uint8_t port) const;
  bool enActiveLow() const;

  // 对模组异步下发 PDU 模式 / 新短信直投配置（仅启动状态机，不阻塞）
  bool configureModem(uint8_t port, bool enabled);

  // 发送一段 PDU（CMGS 两段式握手）；失败写入 err
  bool sendPduPart(uint8_t port, const SmsPduPart &part, String &err);

  // 处理一条收到的 PDU（解码 + 重组 + 落盘）
  void handleCmt(uint8_t port, const String &pduHex);

  // 载入 / 保存 NVS 配置；返回是否有有效记录
  bool loadConfig();
  bool saveConfig();

  // 取/放互斥锁（保护 _lastSmsId 与配置读写）
  void lock() const { xSemaphoreTake(_mutex, portMAX_DELAY); }
  void unlock() const { xSemaphoreGive(_mutex); }

  SerialBridge &_bridge;    // AT 串口桥（send 通道）
  SmsStore _store;          // 文件存储
  SmsCodec _codec;          // PDU 编解码 + 重组
  ModemState _modems[cfg::kSmsMaxModems + 1];  // 下标 = 串口号
  SmsConcatSlot _concat[cfg::kSmsConcatSlots]; // 长短信重组槽位
  uint16_t _max = cfg::kSmsStoreMax;           // 存储条数上限
  uint32_t _lastSmsId = 0;                     // 最新记录 ID（SSE diff）
  mutable SemaphoreHandle_t _mutex = nullptr;  // 保护 _lastSmsId / 配置读写

  ModemReadyFn _onReady;    // 模组就绪回调（抽数任务上下文触发）
  TimeProviderFn _timeFn;   // 时间戳供给（App 注入；为空则开机秒数）
};
