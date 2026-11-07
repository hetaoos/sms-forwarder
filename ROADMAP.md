# 开发路线图与交接说明

> 面向后续接手的 AI / 开发者：本文说明「项目现在做到哪一步、下一步做什么、动手前必须知道什么」。
> 最后更新：2026-11-07 ｜ 固件版本：`1.0.0`（`VERSION`）｜ 平台：ESP32-S3 16MB Flash + 8MB PSRAM / Arduino
> 相关文档：[README.md](README.md)（功能与 API）、[debug-sms-store-parse-fail.md](debug-sms-store-parse-fail.md)（单缺陷取证记录）

---

## 0. 快速接手（先读这一节）

1. **当前状态**：核心链路已完整可运行；2026-11-07 完成 ① 存储模式 `CNMI=2,1` 收短信不入库缺陷修复（§2）、② 持久化不丢消息转发队列 / 发件箱（§2.5，含收到短信转发、网页主动发短信、两页发送弹窗统一）、③ 长短信分批 `+CMTI` 导致的重复接收修复（§2.6）、④ 显式任务 / 硬件看门狗（§2.7）、⑤ P1 半成品批次中的三项（§2.8：5 个模板占位符、编译开关、文档补齐）、⑥ BACK 长按行为与 Web 侧栏仓库入口（§2.9；「前端规划中菜单」按需求否决、已整段撤销）、⑦ 时区可配（默认东八区）与时间失真修复（§2.10）、⑧ 登录限流 + 口令哈希 + 前端 SHA-256 挑战应答（§2.11）。④ 的**编译验证已补跑通过**（`PLATFORMIO_BUILD_DIR=.pio/verify pio run -e esp32s3` → SUCCESS，RAM 36.7% / Flash 48.1%）；另验证 `ENABLE_MQTT=0` + `ENABLE_FIRMWARE_UPGRADE=0` 组合编译 SUCCESS（RAM 36.5% / Flash 45.8%）；⑦ 的两种配置编译结果（默认 RAM 36.7% / Flash 48.2%，双开关关闭 36.5% / 45.9%）见 §2.10。全部改动**尚未刷机实测**、**未提交 git**。
2. **接手第一件事**：按 §2.4 做设备验证，通过后才把 `debug-sms-store-parse-fail.md` 的 Status 改为 `CLOSED`。**不要跳过验证就宣称修好**。
3. **验证通过后的下一步**：按 §4 做「可靠转发」三件套（持久化投递队列 + 投递回执 + 规则引擎），这是收益最高的一块。
4. **不要重复劳动**：§1.2 列出的能力都已实现，别再作为「新功能」提出。
5. **动手前必读 §5**：任务上下文、端口锁、PSRAM、分区表、OTA tag 这几处踩错会直接导致重启或配置丢失。

---

## 1. 项目基线

### 1.1 代码地图

| 路径 | 职责 |
|---|---|
| `src/main.cpp` | `setup/loop` 仅调 `gApp.begin()` / `gApp.tick()` |
| `src/app/App.cpp/.h` | 模块装配与主循环调度；`Config.h` 是**唯一集中配置**（引脚/超时/容量/NVS 键全在此） |
| `src/app/TaskWatchdog.cpp/.h` | TWDT 运行期重配置、常驻任务订阅/喂狗、卡死原因 RTC + NVS 留痕、故障注入自检 |
| `src/app/UIMenu.cpp`、`src/ui/Ui.cpp`、`src/hal/Display.cpp` | LVGL 9 屏幕菜单、5 个页面、JPEG Web 预览 |
| `src/serial/SerialBridge.cpp/.h`、`AtHistory.cpp` | 双 UART 抽数任务、端口锁、AT 命令/URC 分流、自愈 |
| `src/sms/SmsService.cpp/.h` | **最核心**（约 3000 行）：双模组状态机、PDU 收发、主动查询、来电、管理员命令 |
| `src/sms/SmsCodec.cpp`、`lib/pdulib` | PDU 编解码、长短信重组 |
| `src/sms/SmsStore.cpp`、`BlackList.cpp` | `/data/sms/<id>.txt` 落盘（FIFO 1000）、黑名单 |
| `src/push/PushService.cpp`、`PushChannels.cpp`、`MsgContext.cpp` | 13 种 Webhook、广播/故障转移、模板占位符 |
| `src/net/WebPanel.cpp`、`StatusBus.cpp`、`StateJson.h` | HTTP 路由（约 2500 行）、SSE、状态序列化 |
| `src/net/WifiManager.cpp`、`AdminStore.cpp`、`EmailService.cpp`、`MqttService.cpp` | 配网、账号、SMTP、MQTT |
| `src/ota/Ota.cpp`、`src/app/SystemService.cpp`、`Coredump.cpp`、`UserFs.cpp` | OTA、定时重启、崩溃转储、`/data` 挂载 |
| `src/log/LogStore.cpp`、`src/time/TimeSync.cpp`、`src/utils/*` | 日志环、NTP/NITZ、HTTPS RAII、TLS 通道信号量、PSRAM 分配 |
| `data/` | 15 个 Web 页面 + `assets/app.js`（Alpine.js，本地内置）+ `app.css` + 中文字库 |
| `script/`、`partitions/esp32s3_16mb.csv`、`.cnb.yml` | 构建/发布脚本、分区表、CI |

### 1.2 已实现能力（不要当成待办）

- 双 Cat.1 模组纯 AT 控制（ML307A / SIMCOM / UNISOC 兼容）、PDU（UCS2/GSM7/8bit）、长短信重组（5 槽 / 20 段 / 30s）。
- 两种接收模式可切换：直投 `CNMI=2,2`（`+CMT:`）与存储 `CNMI=2,1`（`+CMTI:` + 主动 `AT+CMGR`）；60s 兜底轮询、10 分钟去重、未读/已读补删。
- 短信收/发/落盘/分页/搜索/按模组过滤/今日计数；管理员短信命令（`SMS:`/`RESET`/`PING`/`HELP`）；来电检测与提醒；黑名单（精确/前缀/通配符，短信+来电作用域）。
- 转发：SMTP 邮件（HTML 卡片 + 验证码高亮 + 5 类通知位 + 定时在线）、MQTT（esp-mqtt，9 类主题 + 6 种远程命令）、13 种 Webhook（含「SMS 短信备份」断网兜底 + 广播/故障转移）。
- Web 控制台（端口 80、LittleFS、gzip、hash 路由）、Cookie 会话 + Bearer + 持久化 API 令牌、SSE 实时推送、屏幕远程按键注入、JPEG 屏幕预览。
- WiFi 多组配置（NVS 5 组）、AP 配网、扫描/排序/试连；NTP + SIM NITZ 时间同步；定时重启；OTA 在线/本地双槽升级；coredump 查询/导出/擦除；配置导入导出；PSRAM 日志环与 AT 历史环；CPU/温度遥测。
- 任务看门狗（§2.7）：4 个常驻任务订阅 TWDT（60s / panic 自动重启）、长阻塞全程喂狗、复位原因与疑似卡死任务持久化并可在 Web / `/api/status` 查询、`/api/wdt/selftest` 故障注入自检。
- 推送模板占位符全部可用（§2.8）：`{carrier}` / `{to}` / `{iccid}` / `{signal}` 取模组快照（新增就绪后一次性 `AT+CCID`），`{remark}` 取 NVS `appcfg/remark`（推送通道页配置）；Web 占位符表与 README 已同步。
- 两个编译期开关均生效（§2.8）：`ENABLE_MQTT=0` / `ENABLE_FIRMWARE_UPGRADE=0` 都能编译并真正裁掉实现，前端按 `mqtt.compiled` / `otaAvailable` 隐藏入口。
- Web 仓库入口（§2.9）：侧栏底部与登录页显示 `cnb.cool/null/sms-forwarder`，**不展示项目简介**；按需求**不在 Web 展示「规划中 / 待办」**，路线图只在本文维护。
- BACK 长按全场景一致（§2.9）：状态页 / 菜单 / 文本页 / 弹窗统一熄屏，长按期间的 `kRepeat` 全部吞掉，修掉了「熄屏后立刻被自己的连发唤醒」这一实际缺陷。
- 时区可配、默认东八区（§2.10）：Web「系统设置 - 设备时区」可改 UTC-12:00 ~ +14:00（含半时区 / 45 分钟时区），落盘 NVS `appcfg/tz`，即时生效不重启；`/api/timezone` 与配置备份 `timezone` 节；所有 Web 列表按**设备时区**而非浏览器时区显示。
- 时间戳口径统一（§2.10）：`cfg::kEpochFloorSec` 一处定义、`TimeSync::stamp()` 一处供给，消除裸 `time(nullptr)` 的 1970 小值与多套阈值造成的失真；未同步仍兼容「开机秒数 / 开机 X」。
- 登录安全（§2.11）：NVS `auth` 只存 `salt + verifier`（v1 明文开机自动迁移）、Web 默认走 `salt/nonce/ts` 的 SHA-256 挑战应答（请求体不含密码、nonce 一次性消费）、全局/IP/账号三层限流（429 + `retryAfter`）、三层浏览器能力降级（原生 Web Crypto → 内置纯 JS SHA-256 → v1 明文兼容）、会话 7 天惰性过期 + LRU；配置导出 `auth` 节不再含明文密码。

### 1.3 构建与验证命令

```bash
pio run -e esp32s3                    # 编译（约 16s 增量）
pio run -e esp32s3 -t upload         # 烧固件（成功后自动上传一次 FS 镜像）
pio run -e esp32s3 -t uploadfs       # 仅烧 Web 资源（改过 data/ 必须执行）
pio device monitor -e esp32s3        # 串口监视 115200
```

- `extra_scripts`：`script/uploadfs_gz.py`（打包时自动 gzip，字库 `ui_cn.ttf` 除外）、`script/inject_app_version.py`（注入 OTA tag，本地按 `<VERSION>-<提交时间>-<sha7>` 自拼）。
- 编译验证时注意：PlatformIO/SCons 用**内容签名**判断增量，`touch` 源文件不会触发重编；要确认新代码进固件，可在 `.pio/build/esp32s3/firmware.elf` 里搜新增字符串（macOS `grep -a` 对中文不可靠，用 `python3` 读 bytes 比较稳妥）。
- 若构建日志出现 `[safe-delete][SAFE_DELETE_BULK_CONFIRM_REQUIRED]` 后卡住（环境的批量删除保护拦住 PlatformIO 清理 `.o`），用全新构建目录绕开：`PLATFORMIO_BUILD_DIR=.pio/verify pio run -e esp32s3`（约 1 分钟全量编译，结果等价）。
- 根目录 `compile_commands.json` 是**陈旧快照**（缺 LVGL 路径、OTA 宏与新增的 `.cpp`），不要用它做 `-fsyntax-only` 检查，会误报 `lvgl.h` 找不到、`../hal.h` 找不到、`JsonDocument` 一堆假错误。`ENABLE_MQTT` / `ENABLE_FIRMWARE_UPGRADE` 现已在 `src/app/Config.h` 归一化（未定义时默认 1），`MqttService incomplete` 那类假错误不再出现。一律以 `pio run` 完整编译为准。
- 验证编译开关时用独立构建目录 + 追加宏（叠加在 `platformio.ini` 之后，会有 redefined 警告，最终值以追加的为准）：`PLATFORMIO_BUILD_DIR=.pio/verify_off PLATFORMIO_BUILD_FLAGS="-D ENABLE_MQTT=0 -D ENABLE_FIRMWARE_UPGRADE=0" pio run -e esp32s3`。正式发布应直接改 `platformio.ini` 的 `build_flags`，避免宏重定义。
- 设备取证入口（不接 USB 也能用）：`/api/logs`、`/api/at/hist`、`/api/sms/query`、`/api/status`，Web 页「运行日志 / 模组调试 / 通信模组」。

---

## 2. 已完成：存储模式收短信缺陷修复（2026-11-07）

完整取证记录见 [debug-sms-store-parse-fail.md](debug-sms-store-parse-fail.md)。

### 2.1 现象与根因

现象：模组上报 `+CMTI: "SM",<idx>`，但短信未解析、未落盘 `/data/sms`、未推送，SIM 侧副本仍在。

从代码直接确认的 4 个根因：

| # | 根因 | 原位置 |
|---|---|---|
| 1 | `readResponse()` 仅靠 80ms 静默判停，`AT+CMGR` 头行与 PDU 行之间停顿稍久就提前返回，PDU 残留串进下一条命令 | `src/serial/SerialBridge.cpp` |
| 2 | `queryPort()` 在 CMGR **之前**就 `queryIndexCount = 0`，三个失败分支都不回退 → 一次失败永久丢失索引 | `src/sms/SmsService.cpp` |
| 3 | 初始化 `at()` 只判断端口锁是否取到，`AT+CMGF=0` / `AT+CNMI` 返回 ERROR 也推进到 `kReady`；模组可能停在 TEXT 模式，`AT+CMGR` 返回文本；且 CMGF 排在 CNMI 之后 | `SmsService.cpp::pollInit` |
| 4 | `waitingPdu` 无条件消费 `+CMT` 后的下一行，紧跟的 `+CMTI` 被吞 → `queryPending` 永不置位 | `SmsService.cpp::onUnsolicited` |

另确认：调试记录里写的「+CMTI 优先直接 CMGR、CMGL 降级」**当时并未真正落地**（原实现每轮固定发 `AT+CMGL=0/1`，各 8s 超时）。

### 2.2 改动清单（均未提交 git）

| 文件 | 改动 |
|---|---|
| `src/serial/SerialBridge.h/.cpp` | 新增 `drainRxLocked()`；`readResponse()` 见 `OK`/`ERROR`/`+CMS ERROR` 终止行即结束，静默兜底 80→300ms；`send()`/`trySend()` 下发前抽干 RX 残留交回 `pend`；`sendSmsPdu()` 复用抽干逻辑 |
| `src/sms/SmsService.h` | `ModemState` 新增 `waitingPduAtMs`、`configFailCount`、`queryFailStreak`、`cmgfChecked`、`cmgfOk`、`lastCmgrResp`、`lastQueryErr` |
| `src/sms/SmsService.cpp` | `queryPort()` 重写为 CMTI 优先 / CMGL 补充扫描；索引「成功才移除」+ 保留查询期间新 URC + 3s→60s 指数退避 + 连续 3 轮转全量扫描；新增 `AT+CMGF?` 复核与纠正；`extractPduHex()` 重写；`onUnsolicited()` 的 `waitingPdu`/`+CMTI` 加固；`pollInit` 关键配置校验且 CMGF 前置；`sendAt()` 还原校验；`setStoreMode()` 清标志并立即查询；`queryStatusJson()` 增加 6 个诊断字段；新增 `briefResp()` / `parseCmgfMode()` / `isHexOnly()` |
| `src/app/Config.h` | 新增 `kSmsPduWaitMs`(5s)、`kSmsQueryFailMax`(3)；更新查询链路注释 |
| `data/pages/modem.html` | 异常时在「通信模组」页显示 `indexes`/`failStreak`/`cmgfOk`/`lastError`/`lastCmgr` |
| `README.md` | 补 `GET|POST /api/sms/query` 文档与诊断字段说明 |
| `debug-sms-store-parse-fail.md` | 状态改为 `FIX-IMPLEMENTED`，写入修复说明、验证清单、关闭条件 |

### 2.3 验证结果（已做）

- `pio run -e esp32s3` → **SUCCESS**（RAM 36.3% / Flash 47.2%）；`read_lints` 0 错误。
- 已在 `firmware.elf` 中确认包含新增字符串（「全量扫描自愈」「不处于 PDU 模式」「按普通上报继续处理」「读回失败」）。

### 2.4 待办：设备取证（未关闭）

1. 刷入修复固件 → Web「通信模组」页应无异常诊断（PDU 模式正常、待重试索引 0、无最近错误）。
2. 发一条普通短信，日志依次出现「收到 +CMTI 索引通知」→「待读取短信索引」→「收到短信 #N」→「查询完成…读回失败 0 条」，且短信入库并转发到已启用通道。
3. 连发多条（含长短信），确认 `+CMTI` 索引不互相覆盖、SIM 副本被删除。
4. 控制台发 `AT+CMGF=1` 后立刻收短信，验证 CMGF 自动纠正仍可入库。
5. 若仍失败：看 `/api/sms/query` 的 `lastCmgr`/`lastError`/`cmgfOk`/`indexes`/`failStreak` 区分假设 A~E（判读表见调试记录 §设备验证清单 5）。
6. 上述 1-4 通过且 24h 内无「读回失败」日志后，把调试记录 Status 改为 `CLOSED` 并填写 Hypothesis Conclusions。

> **风险提示**：`readResponse()` 的改动影响**所有 AT 命令**（含初始化 `trySend`），若现场出现 AT 变慢/超时，优先复查终止行判定；`extractPduHex()` 现在要求先见 `+CMGR:` 头，极少数不回头的模组会一直读不到，日志与 `lastCmgr` 会明确留痕。

---

### 2.5 已完成：持久化「不丢消息」转发队列（§3 P0 第一项）

**状态**：代码完成，完整编译通过（`SUCCESS`，RAM 36.3% / Flash 47.7%），**未刷机实测**。验收标准两项均已覆盖。

**方案：薄 Outbox + 复用现有单次发送原语**，不改动三个服务原有队列与发送链路。

- 短信回调（抽数 / 查询任务）只做浅入队；`Outbox::tick()`（loop 任务）把事件连同
  「当时启用且有效的通道快照」**先落盘** `/data/outbox/<id>.json`，再逐通道投递。
- 新增薄接口复用现有发送原语：`EmailService::deliverSmsOnce()`、
  `MqttService::publishSmsOnce()`、`PushService::deliverOnce()`；另加
  `PushService::channelNeedsNet()`，使 SMS 备份通道在断网时仍能投递。
- 每条记录按通道记账：`pending/sending/ok/submitted/failed/skipped/dead` +
  尝试次数、最近错误、耗时。失败按 2s→60s 指数退避，最多 6 次；网络未就绪时
  推迟且不消耗次数；重启扫描目录恢复，`sending` 视为结果未知重投。
- 故障转移策略在发件箱内实现（首个 push 通道成功后其余标 `skipped`）；
  已完成回执 FIFO 保留 80 条，**未完成记录永不淘汰**。
- Web：`data/pages/outbox.html`「发件箱」页 + `/api/outbox{,/retry,/clear,/delete}`；
  API 与状态字段见 `README.md`「持久化发件箱」节。
- 用户数据分区不可用时 `App` 自动回退到原有即时转发队列，转发功能不中断。

**关键文件**：`src/app/Outbox.h/.cpp`（新增）、`src/app/Config.h`（`kOutbox*` 常量）、
`src/app/App.h/.cpp`（装配 + 回调）、`src/net/WebPanel.h/.cpp`（4 个 handler）、
`src/push/PushService.*`、`src/net/EmailService.*`、`src/net/MqttService.*`（单次投递接口）、
`data/assets/app.js`、`data/pages/outbox.html`、`data/pages/sms.html`（后者发送弹窗与
发件箱页同款：提交即登记、不轮询 `GET /api/sms/send`，结果去发件箱页看）。

**发出类记录（网页发送短信，同日追加）**：
- `POST /api/sms/send` 改为先调 `Outbox::enqueueOutgoingSms()` 登记；Web「短信」页与
  「发件箱」页的发送按钮都走该接口，发件箱不可用时回退原直投队列。
- `kind=1` 记录只生成 `sms:<port>` 单通道，**跳过 `snapshotChannels()`**，从根上切断
  「SMS 短信备份 push 通道 → 再发一条短信」的回环（`README.md` 已说明）。
- 复用 `SmsService::enqueueSend()`：CMGS 仍在常驻 `sms_send` worker 里执行
  （该任务现已订阅任务看门狗，空闲与 CMGS 握手期间持续喂狗，见 §2.7），
  `Outbox::deliver()` 绝不阻塞 loop；`done` 经 `_sms.tick()` →
  `Outbox::onSendDone()` 回写状态。`applyResult()` 把已入队通道落盘为 `kSending`，
  `load()` 遇到它记 `kSubmitted`（结果未知、不自动重发），`checkSendTimeout()`
  兜底 `done` 丢失（3 分钟）。
- 真正发送失败标 `kDead` 且**不自动重试**（避免重复扣费），仅「入队发送队列失败」
  自动退避；`retry()` 允许手动重试 `sms:` 的 `submitted`/`dead`。
- `/api/sms/send` 的 POST/GET 六字段契约不变，新增 `port` 越界 400；`busy` 只表示
  本次 CMGS 在途，不绑定发件箱退避重试。MQTT `send_sms`、管理员 `SMS:`、SMS 备份
  推送**未**纳入发件箱（避免破坏命令响应语义与引入回环）。

**已知取舍**：at-least-once（「发送成功」与「状态落盘」之间断电可能重发一条，**仅转发类**；发出类遇结果未知不自动重发）；
MQTT QoS1 与 SMS 备份只有「已入队」确认，记为 `submitted` 而非 `ok`；
通道列表是短信到达瞬间的快照，之后新增的通道不补发给旧记录；
单条正文落盘上限 1024 字节（长短信截断）。

**验收（待设备验证）**：
1. 发一条短信 → 发件箱页出现记录，各通道状态由 `pending` 变为 `ok` / `submitted` / `skipped`。
2. 投递过程中断电重启 → 重启后未完成通道自动续投，最终全部到达终态。
3. 关掉某个通道后重发 → 该通道标 `skipped`，其余通道正常。
4. `POST /api/outbox/retry {"id":n}` 能让 `dead` 通道重新投递。
5. 回归：启动邮件 / SIM / 来电 / 在线 / 测试邮件等**非短信**事件仍走原队列，行为不变。
6. 在「发件箱」页点「发送短信」→ 出现 `kind=1` / `sms:<port>` 单通道记录，成功标
   `ok`；Web「短信」页发送弹窗为同款界面、同样提交即返回（不轮询
   `GET /api/sms/send`），两页都**不会额外产生转发记录**（无回环）。
7. 发送中途断电重启 → 该记录标 `submitted`（结果未知），**不自动重发**；
   手动点「重试」才补发（重试前需确认对方未收到）。

### 2.6 已完成：存储模式长短信「重复接收」修复（2026-11-07）

**现象**：一条 5 段长短信到达后先落盘一次完整内容，约 60s 后又出现一条
「[长短信不完整 4/5 段]」并重复推送。AT 历史显示 `AT+CMGL=1` 每 30s 把索引
1~4 反复读回（09:09:14 / 09:09:44 / 09:10:14 三轮），直到 09:10:15 才
`AT+CMGD=4,3,2,1`。

**根因**：5 个分段由 5 个 `+CMTI` 分批上报（UDH `05000372 05 xx`：ref=0x72 /
共 5 段 / 序号 5,4,2,1,3 乱序），每个 URC 立刻触发一轮 `queryPort()`，每轮只
读回一个索引、只把这一段喂进 `SmsCodec::reassemble()`。最后一段到齐后完整
短信已落盘，但 `deleteGroup()` 只删「本轮 groups 里」的那一个索引；前 4 段
仍留在 SIM 上。兜底 `AT+CMGL=1`（已读列表）把它们反复捞回，而重组槽在
`reassemble()` 收齐后已清空，4 段凑不齐 5 段 → `kKeep` 不删；`StuckGroup`
的 `kSmsConcatStuckMs`（60s）滞留窗口到期后按「残缺长短信」二次入库并推送。

**修复**（`src/sms/SmsService.h/.cpp`，`queryPort()`）：
- `ModemState::partIdx[]`（24 项）登记每个长短信分段对应的 SIM 索引，键为
  `(端口|发送方|ref|总段数)` 的 FNV 哈希——**与 `reassemble()` 的槽位匹配键
  一致**，故意不含 SCTS：分段时间戳相差 1 秒时仍能归到同一组；登记项带
  10 分钟 TTL，防止 SIM 索引被复用后残留误删。
- 拼接成功（`last == kDelete`）、「已按不完整入库只补删」、滞留收敛三条路径
  统一改为 `deleteGroup(grp, pkey)`：先 `takePartIdx()` 取出同组历史索引，
  与本轮索引合并去重后逐条 `AT+CMGD`。删除失败会把索引重新登记，下轮补删。
- `_deleteAfterRead=false` 时不登记也不补删，保持用户配置语义；普通短信
  （`pkey=0`）行为完全不变。
- 新增日志「长短信拼接完成，连同前几轮残留共删除 N 个分段副本」便于取证。

**设备验证要点**（尚未实测）：收一条多段长短信后
① 日志只出现一次「收到短信 #N」，且无「长短信不完整」；
② 出现「…共删除 5 个分段副本」；
③ `AT+CPMS="SM"` 回到 `0,50`，后续 `AT+CMGL=1` 不再列出旧索引；
④ 普通短信与真缺段长短信（仍应收敛为**一条**不完整记录）不受影响。

---

### 2.7 已完成：显式任务 / 硬件看门狗（§3 P0 最后一项）

**状态**：代码完成，**编译验证已补跑通过**（§2.8：`PLATFORMIO_BUILD_DIR=.pio/verify pio run -e esp32s3`
→ SUCCESS，RAM 36.7% / Flash 48.1%）、**未刷机实测**；因改过 `data/`，刷机时仍需执行 `uploadfs`。

**问题**：原先全项目没有任何 `esp_task_wdt_*` 调用，仅靠「约定不写死循环」
规避卡死。框架开机其实已初始化 TWDT（5 秒 / panic / 订阅两个 Idle），
但 5 秒远短于合法阻塞：SMTP/TLS 15s、Webhook 10s、发件箱单次投递 15s、
单条 CMGS 握手最坏 5+30s；直接订阅项目任务只会误重启。

**方案：运行期重配置 + 显式订阅 + 全程喂狗 + 两级原因留痕**（新增 `app/TaskWatchdog`）。

- `TaskWatchdog::begin()`（`App::begin()` 早期、日志环就绪后、任何常驻任务创建前）：
  `esp_task_wdt_reconfigure()` 把超时放宽到 `cfg::kWdtTimeoutMs`（60s，
  保持 `trigger_panic=true` 与两个 Idle 订阅），并把 `loopTask` 订阅进去。
  重配置失败则**不订阅任何项目任务**，只留框架默认行为（宁可不监控也不误重启）。
- 另 3 个常驻任务在各自入口 `registerCurrent()`：`uart-pump`、`sms_send`、
  `sms_query`（`async_tcp` 由 AsyncTCP 库自行订阅并喂狗，一次性任务不纳入）。
- 喂狗点：`App::tick()` 每轮、`SerialBridge::readResponse()` 的 AT 等待循环、
  `sendSmsPdu()` 的 `>` / `+CMGS/OK` 等待循环、`lockPort()` 的等锁循环
  （由 `portMAX_DELAY` 改为 1s 超时循环）、`sms_send` 的队列等待
  （由 `portMAX_DELAY` 改为 1s 超时）、`sms_query` / `uart-pump` 每轮。
  `feed()` 按 1s 节流，且只对「当前已订阅 TWDT 的任务」生效，多任务共用路径安全。
- **卡死自动重启**：任一订阅任务超过 60s 未喂狗 → IDF panic
  （`CONFIG_ESP_TASK_WDT_PANIC`）→ 自动重启并写 `ESP_RST_TASK_WDT` 复位提示；
  panic 时 coredump 分区同样有快照。
- **记原因**：覆盖弱符号 `esp_task_wdt_isr_user_handler()`（ISR 上下文，只做数值
  比较 + RTC_NOINIT 写，无日志 / NVS / FreeRTOS 调用），把各任务最近心跳与
  「心跳明显落后」的槽位写进 RTC 内存；下次开机结合 `esp_reset_reason()` 打一条
  `wdt` 运行日志，并把复位原因、疑似卡死任务、触发时开机秒数、固件版本、
  累计次数持久化到 NVS `wdt` 命名空间（不随出厂复位清除）。
  `/api/status` 的 `wdt` 子对象（SSE 同源）与 Web「系统工具」页可查。
- **故障注入自检**：`POST /api/wdt/selftest {"task":"loopTask","timeoutMs":10000}`
  让指定任务停止喂狗（可临时把超时缩到 3~60s），用于设备验收；需认证 + 前端
  二次确认，Web「系统工具」页有卡片与按钮。**该接口会触发整机重启**。

**关键文件**：`src/app/TaskWatchdog.h/.cpp`（新增）、`src/app/Config.h`（`kWdt*` 常量
与 NVS 键）、`src/app/App.cpp`（装配 + loop 心跳）、`src/serial/SerialBridge.h/.cpp`
（等锁 / AT 等待 / CMGS 握手喂狗 + 抽数任务注册）、`src/sms/SmsService.cpp`
（两个 worker 注册 + 空闲等待续喂）、`src/net/StateJson.h`（`wdt` 字段）、
`src/net/WebPanel.h/.cpp`（自检 API）、`data/pages/tools.html`、`data/assets/app.js`
（状态卡片与自检 UI）、`README.md`（「任务看门狗」节与 API 表）。

**已知取舍**：
- 60s 是「最长合法阻塞 + 余量」的折中，卡死最长需 60s 才恢复；新增同步阻塞
  超过该值必须调大 `kWdtTimeoutMs` 或在阻塞循环里补喂狗点。
- 放宽后 Idle 任务的饿死检测同样变成 60s（框架默认 5s），换来不误杀 TLS / CMGS。
- 疑似卡死任务由 RTC 心跳推断（精度 1s）；若所有任务心跳同时停止（CPU 整体挂死）
  或断电导致 RTC 证据丢失，`lastWdtTask` 为空（显示「未能定位」），但
  `reset=TASK_WDT` 仍能确定是看门狗重启；串口 panic 日志与 coredump 有更精确的信息。
- 自检接口是危险操作，只在验收时使用；普通运行不会调用它。

**验收（待设备验证）**：
1. `pio run -e esp32s3` → SUCCESS；改过 `data/` 再 `pio run -e esp32s3 -t uploadfs`。
2. 开机日志依次有「上次复位原因…」或「上次由看门狗重启…」、
   「任务 loopTask 已注册看门狗」…「任务看门狗已启用：超时 60 秒，监控 …」，
   `/api/status` 的 `wdt.enabled=true`、`tasks` 含 4 个任务名。
3. 空载运行 30 分钟（含收/发短信、发邮件、OTA 检查）→ **无意外重启**；
   `wdt.reset` 保持上电 / 软件重启原因，日志无「看门狗重启」。
4. Web「系统工具」→「任务看门狗」选 `loopTask` + 10 秒 → 触发自检：
   设备约 10s 后自动重启；重启后日志出现
   「上次由看门狗重启：任务看门狗超时；卡死任务 loopTask…」，
   `wdt.reset=TASK_WDT`、`wdt.bootWasWdt=true`、`wdtCount` 递增、
   `wdt.lastWdtTask=loopTask`，Web 卡片显示该次记录。
5. 对 `uart-pump` / `sms_send` / `sms_query` 各重复一次第 4 步，确认原因指向对应任务。
6. 回归：自检重启后收一条普通短信 + 一条长短信、网页发一条短信、
   `/api/at/send` 后配置还原，确认 AT 历史无截断 / 串包（喂狗点改动覆盖了
   `readResponse` / `lockPort`，须重点回归 §6 第 3 项）。

---

### 2.8 已完成：P1 半成品批次三项（2026-11-07）

**状态**：代码完成，**两种编译配置均 SUCCESS**、**未刷机实测**、**未提交 git**。

| 配置 | 命令 | 结果 |
|---|---|---|
| 默认（两个开关都开） | `PLATFORMIO_BUILD_DIR=.pio/verify pio run -e esp32s3` | SUCCESS，RAM 36.7% / Flash 48.1%（顺带把 §2.7 欠的编译验证补跑了） |
| 两个开关都关 | `PLATFORMIO_BUILD_DIR=.pio/verify_off PLATFORMIO_BUILD_FLAGS="-D ENABLE_MQTT=0 -D ENABLE_FIRMWARE_UPGRADE=0" pio run -e esp32s3` | SUCCESS，RAM 36.5% / Flash 45.8%（Flash 少 ~95KB，确认实现被裁掉） |

另：`node --check data/assets/app.js` 通过；`read_lints` 0 错误；在 `firmware.elf` 里确认
`AT+CCID` 进入两种固件，而「MQTT 未编译进固件」「OTA 升级未编译进固件」只出现在关闭配置。

**① 补齐 5 个模板占位符**
- `MsgContext.cpp::fieldValue()` 原本就认识这 5 个名字，缺的只是取值。`PushService::buildMsgContext()`
  现在从 `_sms->modemSnapshot(job.modem + 1, ...)` 取 `{carrier}`（`oper`，中文短名优先）、
  `{to}`（`num`）、`{iccid}`；`{signal}` 由 `csq` 换算为 `-73 dBm (CSQ 20)`（与 Web 页 `csqDbm()` 同口径）。
  注意 `PushJob.modem` 是 0 基而 `modemSnapshot()` 要 1 基；测试推送 `modem=-1` 时四项为空。
  快照里的 `-` / 未知 CSQ 统一归一化为空串，不会把减号塞进模板。
- 新增 ICCID 采集：`ModemState.iccid` + `ModemSnapshot.iccid[24]` + 就绪后一次性 `AT+CCID`
  （静态采集 `idStep` 6→7 步，重初始化时清空），解析函数兼容 `+CCID: "…"` / `+ICCID: …` /
  裸数字行三种回显；`modemsJson()` 与 Web「通信模组」页均已展示。
- `{remark}` 用独立 NVS 键 `appcfg/remark`（`cfg::kNvsKeyRemark`，最长 64 字节），由 `PushService`
  自管：随 `/api/push` GET/POST、配置导入导出走，放在「推送通道」页「设备备注」卡片。
  独立键的好处是只改推送通道配置或空配置时备注不丢。
- `MsgContext.h` 注释、Web 占位符表（补齐 `{deviceId}` / `{message_type}` 并修正示例值）、
  README 占位符表格全部同步。

**② 修编译开关**
- `src/app/Config.h` 归一化两个宏：未定义时默认 `1`（IDE 用陈旧 `compile_commands.json`
  也不会静默裁功能），全项目统一 `#if ENABLE_XXX`。
- `ENABLE_MQTT=0`：`MqttService.h` 的 `#else` 分支提供一个纯 inline 的空实现替身，
  覆盖 `App` / `WebPanel` / `Outbox` 所有未被守卫的调用点；`MqttService.cpp` 整个实现
  （esp-mqtt 客户端、TLS、命令队列）不再编译。配置类 API 返回 404「MQTT 未编译进固件」，
  发件箱不再生成 `mqtt` 通道（旧记录残留时判 `skipped`），配置导入遇到 `mqtt` 节直接忽略。
  `/api/status` 新增 `mqtt.compiled`，前端 `applyFeatureFlags()` 据此隐藏 MQTT 菜单并把
  `#/mqtt` 哈希跳回仪表盘 —— README 原先声称的「前端自动隐藏」现在是真的。
- `ENABLE_FIRMWARE_UPGRADE=0`：`Ota.cpp` 从「远端版本探测」到文件末尾整段包进 `#if`，
  只保留 `version()` / `status()` / `init()` 与四个空实现入口；`otaPartitionAvailable()` 恒 false，
  LCD 升级动作不注册，Web 只保留 `/api/ota/status`（`available=false`，`message` 明确提示）。
  `/api/status` 新增 `otaCompiled`。
- 顺带修正 `WebPanel.cpp` / `WebPanel.h` 两处屏幕预览注释：实际返回 **JPEG**（`image/jpeg`），原文写的是 BMP。

**③ 文档补齐**
- README「短信」API 表补 `GET|POST /api/sms/poll` 与 `GET|POST /api/sms/store` 四行
  （含 `mode` / `safetySec` / `CNMI` 语义与错误码）；`/api/sms/ports` 补 `iccid`。
- README 补齐推送模板占位符表格（含数据来源）、`/api/push` 的 `remark` 字段、NVS 键 `remark`、
  `/api/status` 的 `otaCompiled` / `mqtt.compiled`、功能开关表的真实行为与 OTA 关闭时的 API 表现。

**验收（待设备验证）**：
1. 刷默认固件 → 「通信模组」页出现 ICCID（模组支持 `AT+CCID` 时），AT 历史里有就绪后一条 `AT+CCID`。
2. 在「推送通道」页填设备备注并保存 → 自定义模板用 `{remark}` 发一条测试推送，值正确；重启后备注仍在。
3. 收一条短信并让自定义模板引用 `{carrier}` / `{to}` / `{iccid}` / `{signal}` → 值来自对应模组，
   单模组可用、测试推送（无来源模组）时这四项为空串而不是字面量占位符。
4. 配置导出/导入后备注不丢；`/api/sms/poll` 与 `/api/sms/store` 按 README 契约读写成功。
5. 若烧关闭配置的固件：MQTT 菜单消失、`/api/status` 的 `mqtt.compiled=false` / `otaCompiled=false`、
   「系统工具」页 OTA 按钮禁用且提示「OTA 升级未编译进固件」，短信 / 邮件 / 推送主链路不受影响。

---

### 2.9 已完成：BACK 长按行为 + Web 侧栏仓库入口（2026-11-07）

**状态**：代码完成，**两种编译配置均 SUCCESS**、**未刷机实测**、**未提交 git**。

**① Web 不展示「规划中」，改为侧栏仓库入口**（§3 P1 的「前端规划中菜单」按需求否决）

本次先按 §3 P1 做过一版「规划中」菜单（`MENU` 末组 9 项 + `todoPage()` 占位页），
随后按需求**整段撤销**：Web 控制台不展示路线图 / 待办，侧栏恢复为 16 个已实现页面，
`todo` 徽标渲染与 `.menu-item .todo` 样式一并删除，`route()` 回到统一的
`pages/<id>.html` 懒加载。路线图只维护在仓库 `ROADMAP.md`，不进设备 Web UI。

替代做法是在**侧栏底部**固定放一个源码仓库入口（`renderShell()` 生成、不随菜单滚动、
所有页面可见）；按后续需求**不放任何项目简介文案**：

- 源码仓库：`https://cnb.cool/null/sms-forwarder`（`target="_blank" rel="noopener noreferrer"`；
  AP 配网模式无外网时链接打不开，但地址仍可见 / 可复制），点击后顺带收起窄屏抽屉；
- 登录页（`data/login.html`）卡片底部是同一个链接，未登录时同样可见；
- 新增 `.side-foot` / `.login-repo` 样式（`data/assets/app.css`）；README 顶部补仓库地址、
  「Web 控制台」章节明确写出「Web 不展示待办 / 规划中内容、不展示项目简介」；
- 菜单项 `title` 悬停说明与 `hidden` 项注释保留（与路线图无关的通用改进），
  `applyFeatureFlags()`（§2.8）不受影响；
- **不新增页面、不调用 API**。

**② BACK 长按行为**（§3 P1）

原实现有两处问题：`UIMenu.cpp` 在可滚动文本页里明确写着「Back 长按暂未定义，忽略」，
而菜单页的长按 BACK 熄屏**实际上根本不生效** —— `hal/Button` 在 `kLongPress` 之后
每 `kBtnRepeatMs`（180ms）还会发一次 `kRepeat`，该事件落到 `handle()` 开头的
「熄屏时任意键唤醒」分支，刚熄掉的屏立刻被自己的连发点亮。

- 把「长按 BACK 熄屏」提升到 `handle()` 开头（唤醒分支之后、弹窗分支之前），
  状态页 / 菜单 / 可滚动文本页 / 短信弹窗**四种显示态行为一致**；弹窗可见时顺带
  `dismissPopup()`，避免唤醒后又看到一条已经不看的旧短信。熄屏后内容仍保留在帧缓冲。
- 长按 BACK 期间的 `kRepeat` 一律吞掉，修掉上述自唤醒缺陷；菜单 / 文本页里
  「BACK 连发忽略」的注释同步改为指向统一处理处。
- 删除文本页里「无定义操作」的特判；`UIMenu.h` 状态机注释与 README 按键表
  （短按 / 长按 / 连发 / 唤醒 / 状态页 / 信息页滚动）全部补齐，并删掉与实现不符的
  「文本页长按 OK 刷新」旧描述（`_viewLive` 从未被置位，文本页由 `renderView()` 重绘）。
- 行为边界：熄屏时按住 BACK 只唤醒不导航；短按 BACK 仍是逐层返回 / 退出文本页 /
  收起弹窗；UP/DOWN/OK 的 180ms 连发不受影响。

**验证**：C++ 侧（`UIMenu.cpp/.h`）默认配置与 `ENABLE_MQTT=0` +
`ENABLE_FIRMWARE_UPGRADE=0` 组合均编译 SUCCESS（RAM 36.7% / Flash 48.1% 与
36.5% / 45.8%）；`node --check data/assets/app.js` 通过；`read_lints` 0 错误；
前端沙箱校验确认侧栏 16 项、无「规划中」字样、`side-foot` 含仓库地址。

**验收（待设备验证）**：
1. Web 控制台侧栏**没有任何「规划中」条目或徽标**，16 个页面仍正常加载；
   `ENABLE_MQTT=0` 固件里 MQTT 菜单依旧隐藏。
2. 侧栏底部只有「源码仓库」标签 + `cnb.cool/null/sms-forwarder`，**没有简介文案**；
   滚动菜单时底部不动，点击链接在新标签页打开，窄屏下抽屉收起；未登录时在登录页
   卡片底部也能看到同一个链接。
3. 状态页 / 菜单页 / 短信详情文本页 / 新短信弹窗四种状态下**按住 BACK 超过 600ms 不放**
   → 屏幕熄灭并**保持熄灭**（旧固件会自己亮回来）；松手后短按任意键唤醒、仍停在当前页面。
4. 短按 BACK 逐层返回 / 退出文本页 / 收起弹窗不变；UP/DOWN/OK 长按连发调值不变。

---

### 2.10 已完成：时区可配（默认东八区）+ 时间失真修复（2026-11-07）

**状态**：代码完成，**两种编译配置均 SUCCESS**（默认 RAM 36.7% / Flash 48.2%，
双开关关闭 RAM 36.5% / Flash 45.9%）、**未刷机实测**、**未提交 git**。

**① 时区可配，默认东八区**（§3 P1）

- `Config.h` 新增 `kTzOffsetMinDefault = 480`、`[-720, 840]` 范围与 NVS 键 `tz`；
  `TimeSync` 新增 `_tzOffsetMin` / `timezoneMin()` / `timezoneText()` / `setTimezone()`
  / `loadTimezone()` / `saveTimezone()`。开机**先读时区再恢复时间**：
  `restoreFromNvs()` 会写系统时钟并重新应用 TZ，顺序不能反。
  `loadTimezone()` 用读写模式打开 `appcfg`：**首次启动无 `tz` 键（或值越界）时把 480
  显式写回 NVS**，默认值不再只存在于内存；写入失败不致命，内存默认仍生效。
- `applyTimezone()` 由固定 `UTC-8` 改为按配置生成 POSIX TZ（符号与 UTC 偏移相反：
  `+480 → UTC-8`、`+330 → UTC-5:30`、`-300 → UTC+5`），支持半时区与 45 分钟时区，
  不做夏令时；`syncNTP()` 用 `_tzOffsetMin * 60`，NTP 日志显示当前时区。
- `GET|POST /api/timezone`（`WebPanel`）：范围校验、即时生效、落盘 NVS、**不重启、
  不需要重新对时**（系统时钟是 UTC epoch，改时区只影响 `localtime_r` / `strftime`
  与「今日 / 每日定点」计算）。配置备份新增 `timezone` 节，导入旧备份（无该节）
  保持默认 / 现值。`/api/status` 新增 `tzOffsetMin` / `tzText`。
- Web「系统设置 - 设备时区」卡片：`-12:00 ~ +14:00` 每 30 分钟一档 + 3 个 45 分钟
  时区，常用时区带中文别名；`pageTimezone()` 保存后同步 `live.status` 让列表立刻
  按新时区重绘。仪表盘时间卡片显示 `tzText`。`<option>` 由 `x-for` 动态生成、晚于
  `x-model` 初始化，首屏下拉会停在「UTC-12:00」而不是当前时区；现加
  `x-ref="tzSelect"` + `syncSelect()`（`$nextTick` 后按 `tzOffsetMin` 对齐
  `select.value`），GET 失败也按默认 480 放行，保证首次打开就选中东八区。
- **前端时间显示改按设备时区**：`fmtDateTime()` / `fmtDateShort()` 以及发件箱 /
  coredump 里两处自写的 `new Date(ts*1000) + getHours()` 全部统一为按
  `status.tzOffsetMin` 平移后用 `getUTC*` 取值，不再跟随浏览器本地时区；
  旧固件 / 状态未到达时按 +480 兜底。

**② 时间失真修复**（§3 P1）

- 统一可信下限 `cfg::kEpochFloorSec = 1600000000`（2020-09-13），替换散落在
  `TimeSync` / `MqttService` / `EmailService` / `SmsService` / `SmsStore` /
  `SystemService` / `App` 里的 `1600000000` / `1000000` / `100000` 阈值；
  `dateStr()` 额外要求 `isSynced()`，未同步不再拼出看似真实的 1970 日期。
  崩溃时刻估算保留更宽松的 2020-01-01 下限并加注释（兼容历史 coredump）。
- 新增静态 `TimeSync::stamp()`（取全局唯一实例的 `epochOrUptime()`）：发件箱
  `in.ts` / `r.created`、测试推送 `job.timestamp` 改走它，不再直接拿
  `time(nullptr)` 的 1970 小值当记录时间戳，避免同一列表里开机秒数与无效纪元秒混排。
- 邮件 Date 头只在时钟可信时填真实 epoch（否则留给库默认）；`localDateStr()`
  改用统一阈值，未同步时模板 `{date}` 为空串；配置导出文件名从 `now > 100000`
  改为 `isSynced() && now >= kEpochFloorSec`，不再生成 `19700101` 这种误导性文件名。
- **保留兼容约定**：未同步时仍记「开机秒数」、前端显示「开机 X」，历史记录与
  前端 `>= 1.6e9` 判断都不需要迁移。这次修的是「裸 `time(nullptr)` 混入」与
  「多套阈值」造成的真实失真，不改成伪造时间。

**验证**：两种编译配置 SUCCESS（数值见上）；`node --check data/assets/app.js` 通过；
前端沙箱校验 `+480 / 0 / +330 / -300` 四个时区的 `fmtDateTime` 输出均符合预期，
`tzOptions()` 56 档且含默认 +480 与三个 45 分钟时区；设置页 `<select>` 有
`x-ref="tzSelect"`、组件含 `syncSelect()`。

**验收（待设备验证）**：
1. 首次烧录（无 `appcfg/tz`）→ 开机 `loadTimezone()` 自动写入 `tz=480`，
   `/api/timezone` 返回 `480` / `UTC+08:00`；**首次打开「系统设置」时区下拉即选中
   `UTC+08:00 · 北京 / 新加坡 / 香港`**，不是停在 UTC-12:00；NTP 同步后日志、
   短信、AT 历史显示东八区日期时间。
2. 在「系统设置」改成 `UTC+05:30`（330）并保存 → 无需重启，`/api/status` 的
   `tzText` 变化、列表时间立即按 +5:30 重绘；重启后仍为 330。
3. 配置导出 JSON 含 `timezone.tzOffsetMin = 330`；导入后时区恢复；导入旧备份
   （无该节）保持当前时区且不报错。
4. 未同步（无 NTP / SIM）时收一条短信 → 列表显示「开机 X」、模板 `{date}` 为空串、
   导出文件名用默认名（不带 1970 日期）；同步后新记录切换为真实日期。

---

### 2.11 已完成：登录限流 + 口令哈希 + 前端 SHA-256 挑战应答（2026-11-07）

**状态**：代码完成，三种编译配置均 SUCCESS（默认 RAM 37.0% / Flash 48.4%；双开关关闭 SUCCESS；`LOGIN_REQUIRE_PROOF=1` SUCCESS）、`buildfs` SUCCESS、前端语法与 SHA-256 对照测试通过，**未刷机实测**、**未提交 git**；改过 `data/`，刷机时需 `uploadfs`。按 §3.1「已确认决策」**A + B 同批** 落地（跳过 `sha256-pass` 中间态）。

**① NVS 不再存明文密码**（`AdminStore`）
- `appcfg/auth` 升为 v2 `{"ver":2,"user","salt","verifier"}`，`verifier = SHA256(salt + ":" + pass)`；`begin()` 读到 v1 明文时派生 verifier、生成 salt 并写回 v2（密码不变；落盘失败只告警，下次开机重试）；`update()` 每次换新 salt；`exportTo()` 只导出 `user/salt/verifier`、`importFrom()` 同时接受 v2 与 v1 备份（v1 自动升级）。
- `check()` 改为派生比对，v1 登录 / `POST /api/auth` 的 `passOld` / LCD 随机重置三条路径自动适配，故 `settings.html`、`app.js` **未改**（见 §3.1「定稿调整」）。新增 `checkProof()` 与 `salt()`。

**② 挑战应答**（`WebPanel`）
- `GET /api/login/challenge`：免认证（已加白名单，长路径先注册）、`Cache-Control: no-store`，返回 `v=2 / alg=sha256-verifier / salt / nonce / ts / ttlMs`。`ts` 是设备 `millis()` 回显值，不用 `TimeSync::stamp()`；nonce 表 16 条、30s TTL、惰性清理、满时淘汰最旧、**无论成败一次性消费**。
- `POST /api/login` 按 `v` 分流：v2 提交 `{"v":2,"user","ts","nonce","proof"}`，`proof = SHA256(verifier + ":" + ts + ":" + nonce)`，服务端用 NVS verifier 重算 + 恒定时间比较，**请求体不含密码**；v1 明文兼容**默认保留**，编译期 `LOGIN_REQUIRE_PROOF=1` 可禁用（禁用后统一 401）。失败（含 nonce 重放 / 过期 / ts 不符）统一 `401 "用户名或密码错误"`。

**③ 三层限流**（新增 `src/net/LoginLimiter.h/.cpp`）
- 全局 20 次/分钟（失败 + 挑战）、来源 `remoteIP() + FNV(user)` 5 次/5 分钟锁 60s 起 ×2 封顶 15 分钟、挑战端点每 IP 30 次/分钟；命中 `429 {"ok":false,"error":"..","retryAfter":<秒>}` + `Retry-After` + `no-store`。
- 全部只用 `millis()`、纯内存、重启清零，不落 NVS、不 `delay()`；进入锁定时打一条 `auth` 日志（含 IP 末段）。`kLogin*` 常量集中在 `Config.h`。

**④ 前端**（`data/login.html` + 新增 `data/assets/sha256.js`）
- `sha256.js` 是自带 UTF-8 编码的纯 JS SHA-256（约 2KB）；`crypto.subtle` 可用（HTTPS / localhost）时优先原生，否则走内置实现，两者都不可用降级 v1 并 `console.warn`。提交时**才**取挑战（用户输密码时间不占 30s 窗口）；429 按 `retryAfter` 倒计时；「保存登录凭据」改存 `{user, verifier}`，旧明文记录丢弃，401 且来自保存凭据时清缓存并提示重新输入（设备换 salt 后旧 verifier 失效）。
- 密码框去掉 `required` 改由脚本校验，否则使用已保存 verifier 时空密码框会被原生约束验证拦住、无法提交。

**⑤ 会话表修复**（`WebPanel`）
- 会话 / 令牌表加签发时间戳，按 `kSessionTtlMs`（7 天）惰性过期 + 满时淘汰最旧（修掉「第 9 次登录返回 500」）；登录时两张表任一写入失败都回滚已写入的 sid（不再遗留有效会话）。

**关键文件（实际）**：`src/utils/CryptoUtil.h/.cpp`（新增：mbedtls 单次 SHA-256 / 随机 hex / 恒定时间比较 / FNV）、`src/net/LoginLimiter.h/.cpp`（新增）、`src/net/AdminStore.h/.cpp`、`src/net/WebPanel.h/.cpp`、`src/app/Config.h`（`kLogin*`、`kSessionTtlMs`、`LOGIN_REQUIRE_PROOF`）、`data/login.html`、`data/assets/sha256.js`（新增）、`README.md`、本文。

**验证结果（已做）**
- 默认：`PLATFORMIO_BUILD_DIR=.pio/verify_login pio run -e esp32s3` → **SUCCESS**（RAM 37.0% / Flash 48.4%）。
- 双开关关闭：`PLATFORMIO_BUILD_DIR=.pio/verify_login_off PLATFORMIO_BUILD_FLAGS="-D ENABLE_MQTT=0 -D ENABLE_FIRMWARE_UPGRADE=0" pio run -e esp32s3` → **SUCCESS**。
- 禁用 v1：`PLATFORMIO_BUILD_DIR=.pio/verify_login_proof PLATFORMIO_BUILD_FLAGS="-D LOGIN_REQUIRE_PROOF=1" pio run -e esp32s3` → **SUCCESS**。
- `PLATFORMIO_BUILD_DIR=.pio/verify_login pio run -e esp32s3 -t buildfs` → **SUCCESS**，`littlefs.bin` 含 `sha256.js.gz` / `login.html.gz`；`firmware.elf` 含 `/api/login/challenge`、`sha256-verifier`、限流与「v1 → v2」迁移日志字符串。
- `node --check data/assets/sha256.js` 通过；纯 JS SHA-256 与 Node `crypto.createHash('sha256')` 对 9 组用例（空串 / 短串 / 中文 / emoji / 55、56、64、119、1000 字节的补齐边界）输出完全一致；`login.html` 内联脚本 `new Function()` 语法检查通过；`read_lints` 0 错误。
- 协议一致性人工核对：前端 `sha256(salt + ':' + pass)` ↔ `AdminStore::deriveVerifier()`；前端 `sha256(verifier + ':' + c.ts + ':' + c.nonce)` ↔ `AdminStore::checkProof()`；`ts` JSON number ↔ `as<uint32_t>()`、`String(ts)` 十进制拼接，分隔符与 UTF-8/小写 hex 口径一致。

**验收（待设备验证，按 §3.1「验收」9 项 / §6 第 11 项）**
1. 刷固件 + `uploadfs` 后，浏览器用设备 IP 直连：`window.crypto.subtle` 为 `undefined`，走 L1 内置 SHA-256 仍能登录；抓包确认 v2 请求体只有 `salt/nonce/ts/proof`、**无明文密码**。
2. 重放同一 `{nonce,ts,proof}` → 401；篡改字段 → 401；challenge 后等 35s 再提交 → 401。
3. 连续 5 次错密码 → 第 6 次 429 + `Retry-After`、前端倒计时，到期恢复；换 IP 仍受全局桶；限流期间不产生有效 sid。
4. **NVS 迁移**：旧固件（`auth` v1 明文）升上来 → 开机日志「管理账号已从明文迁移为加盐哈希（v1 → v2）」、原密码仍可登录；导出 `auth` 节无 `pass`；导入 v1 旧备份与新备份都能登录；改密码、LCD 随机重置、`passOld` 校验正常。
5. 旧脚本 v1 明文登录可用；刷 `LOGIN_REQUIRE_PROOF=1` 固件后 v1 返回 401；Bearer 令牌客户端与 SSE 不受影响。
6. 记住凭据：`localStorage` 只有 `verifier`；重开登录页免输密码可登录；设备改密码后旧 verifier 失效并提示重新输入。
7. 连续 10 次成功登录不再 500；`logout` / `/api/session` / AP 配网模式回归正常。

**已知取舍**（详见 §3.1「已知取舍与风险」）：纯 HTTP 下 `proof` 仍可离线字典爆破（单次 SHA-256 加盐不是慢 KDF）；**不支持跨该版本回滚**（旧固件读不到 `pass` 会回落 `admin/admin123` 并覆写账号）；v1 兼容与 `POST /api/auth` 的新密码 / `passOld` 仍是明文；`localStorage` 的 verifier 是密码等价物、只限制爆炸半径；导出脱敏**只覆盖 `auth` 节**，`wifi/email/mqtt` 明文仍在。

---

### 2.12 已完成：API 访问令牌传输保护（2026-11-07）

**状态**：代码完成，默认 + `API_TOKEN_REQUIRE_EXCHANGE=1` 两种编译均 SUCCESS、`buildfs` SUCCESS、`node --check` 通过，**未刷机实测**、**未提交 git**；改过 `data/`，刷机时需 `uploadfs`。按 §3.2「待确认」5 项**全部采纳**落地。

**① NVS 只存 verifier（不存 raw）**（`WebPanel`）
- `apitoken` 升 v2 `{"ver":2,"enabled","verifier"}`，`verifier = SHA256(raw)`；`loadApiToken()` 读到旧 v1 `{"ver":1,"enabled","token"}` 时派生 verifier 并写回 v2（明文令牌不变；仅开机一次，落盘失败下次重试）；`resetApiToken()` 生成 raw、只存 verifier、返回 raw（一次性披露）；`saveApiTokenCfg()` 只写 verifier。

**② 挑战交换替代每请求明文**（`WebPanel`，复用 `LoginLimiter` nonce 表 + 限流桶）
- `GET /api/token/challenge`（公开，需已启用）：返回 `v=2 / alg=sha256-verifier / nonce / ts / ttlMs`，`Cache-Control: no-store`，受每 IP 挑战限流。
- `POST /api/token/exchange`：`{"v":2,"nonce","ts","proof"}`，`proof = SHA256(verifier + ":" + ts + ":" + nonce)`；服务端用 `_apiTokenVerifier` 重算 + 恒定时间比较，nonce 一次性消费，命中限流返回 429 + `retryAfter`；成功签发**短时效票据**（1 小时，`storeAdd(_tokens,...,ttl)`），返回 `{"ok":true,"ticket":".."}`。脚本后续 `Bearer <ticket>`。
- `authorized()` 改为**只认 `_tokens` 表（ticket + 登录临时 token）**，不再直接比 raw；`kApiTokenRawCompat`（默认 true）保留 raw Bearer 兼容分支：用 `SHA256(raw)` 与 verifier 比对（不持有 raw），每次打节流告警；`-DAPI_TOKEN_REQUIRE_EXCHANGE=1` 可彻底关掉（编译期消除该分支）。

**③ 端点脱敏**：`GET /api/token` 不再回 raw（返回掩码占位），真实值仅 `POST /api/token/reset` 响应一次性给；`handleTokenPost` 改用 verifier。

**④ 前端**（`data/assets/app.js` + `data/pages/settings.html`）
- `pageApiToken()` 的 `tokenMasked` 在持有 raw（reset 后瞬时）时掩码、否则提示「已隐藏」；`curlCmd` 示例改为 challenge→exchange 换票据（含兼容 raw 行）；提示文案说明 raw 仅重置后可见一次。

**关键文件**：`src/net/WebPanel.h/.cpp`（挑战/交换/脱敏/`authorized` 改 ticket-only）、`src/app/Config.h`（`kApiTicketTtlMs`、`API_TOKEN_REQUIRE_EXCHANGE`/`kApiTokenRawCompat`）、`data/assets/app.js`、`data/pages/settings.html`、README、本文。复用 `src/utils/CryptoUtil.*`、`src/net/LoginLimiter.*`。

**验证结果（已做）**
- 默认：`PLATFORMIO_BUILD_DIR=.pio/verify_apitoken pio run -e esp32s3` → SUCCESS；`API_TOKEN_REQUIRE_EXCHANGE=1` 变体 → SUCCESS。
- `PLATFORMIO_BUILD_DIR=.pio/verify_apitoken pio run -e esp32s3 -t buildfs` → SUCCESS，`littlefs.bin` 含 `settings.html.gz`/`app.js.gz`；`firmware.elf` 含 `/api/token/challenge`、`/api/token/exchange`、`API 令牌已从明文迁移为 verifier`、`收到 raw Bearer 令牌`、`令牌或签名无效` 等字符串。
- `node --check data/assets/app.js` 通过；令牌 proof 公式（`SHA256(SHA256(raw):ts:nonce)`）与 curl `sha256sum` 示例拼接一致（Node 仿真通过）。

**验收（待设备验证，按 §3.2 验收 5 项）**
1. 启用持久化令牌后，脚本走 challenge→exchange 拿 ticket 能调 API；ticket 过期（1h）后 401；重放 ticket → 401；challenge 后等 35s 再 exchange → 401。
2. raw `Bearer <token>` 默认仍可用（有「raw Bearer」warn 日志）；刷 `API_TOKEN_REQUIRE_EXCHANGE=1` 固件后 raw → 401；浏览器 SPA 用 Cookie 正常。
3. `GET /api/token` 不再回 raw（掩码）；NVS `apitoken` 只含 verifier、无 raw；`reset` 仍一次性回 raw；旧 v1 备份（token 明文）开机自动升级。
4. 回归：登录 / 会话 / SSE / 收发音信不受影响；限流命中返回 429。

**已知取舍**（详见 §3.2）：纯 HTTP 下 exchange 的 proof / ticket 仍可嗅探；兼容模式 raw-Bearer 存在时仍暴露（同 v1 登录）；**不支持跨该版本回滚**（NVS 升 v2 后旧固件读不到 token 字段会按未启用处理，旧脚本 raw 调用失效；建议回滚前先重置/导出）；浏览器 SPA 不受影响（Cookie 已受登录 v2 保护）。

---

## 3. 待办清单

难度：小 = 半天内；中 = 1~2 天；大 = 需分阶段。**验收标准**用于判断是否做完。

### P0 必须补的短板

| 项 | 价值 | 涉及文件 | 难度 | 验收标准 |
|---|---|---|---|---|
| ~~持久化「不丢消息」转发队列~~ ✅ 已实现，见 §2.5 | 短信事件已改走发件箱；**非短信事件**仍是邮件/推送队列深 6、满则丢最旧、重启即丢，后续可扩展 | `src/app/Outbox.*`、三个服务的单次投递接口 | 大 | 断电重启后未送达消息继续补发；每条短信可查各通道投递结果 |
| TLS 证书校验 + OTA 固件验签 | 全部出网 TLS 跳过校验，OTA 无签名/哈希 | `src/utils/HttpSession.cpp:27`、`src/net/EmailService.cpp:200`、`src/net/MqttService.cpp:533`、`src/ota/Ota.cpp` | 中 | 可配置 CA/指纹；校验失败有明确日志；保留兼容开关 |
| ~~登录限流 + 口令哈希 + 导出脱敏~~ ✅ 已实现，见 §2.11（方案 §3.1） | NVS `auth` v2 只存 salt+verifier、v1 自动迁移；登录默认挑战应答 + 三层限流；导出 `auth` 节脱敏 | `src/net/AdminStore.*`、`src/net/LoginLimiter.*`、`src/utils/CryptoUtil.*`、`src/net/WebPanel.*`、`data/login.html` | 中 | 连续失败锁定；NVS 存加盐哈希；导出可选脱敏（**待设备验证**） |
| MQTT 命令面设备侧鉴权 | 任何能发 `<prefix>/<id>/command` 的主体可执行 `send_at`/`restart`/`send_sms` | `src/net/MqttService.cpp:300-360` | 小 | 命令带签名或白名单，非法命令被拒且记日志 |
| ~~显式任务/硬件看门狗~~ ✅ 已实现，见 §2.7 | 4 个常驻任务已订阅 TWDT（60s / panic 自动重启）、长阻塞全程喂狗、原因持久化 + 自检 API | `src/app/TaskWatchdog.*`、`App.cpp`、`SerialBridge.*`、`SmsService.cpp` | 中 | 关键任务注册 WDT，卡死自动重启并记原因 |

### P1 半成品补齐（性价比最高）

| 项 | 说明 | 涉及文件 | 难度 |
|---|---|---|---|
| ~~补齐 5 个模板占位符~~ ✅ 已实现，见 §2.8 | `{carrier}` / `{to}` / `{iccid}` / `{signal}` 取模组快照（新增 `AT+CCID`），`{remark}` 取 NVS 备注 | `src/push/*`、`src/sms/SmsService.*`、`data/pages/push.html` | 小 |
| 联系人簿 / 号码备注 | 让推送显示姓名而非裸号码（现在 `{remark}` 是**设备**备注，不是号码备注，此项仍未做） | `src/sms/`、`src/push/`、`data/pages/` | 中 |
| SSE 补全 | 全量快照缺 `sms`/`smsports`；`Last-Event-ID` 续传只做了 AT 历史；Bearer 客户端无法用 SSE（可加 query token） | `src/net/StatusBus.cpp:139-185` | 小 |
| SMTP 587 / STARTTLS | README 明确的已知限制 | `src/net/EmailService.cpp` | 中 |
| ~~修编译开关~~ ✅ 已实现，见 §2.8 | 两个宏归一化并真正裁掉实现，两种配置均编译通过，前端按 `compiled` 隐藏入口 | `src/app/Config.h`、`src/net/MqttService.h`、`src/ota/Ota.cpp`、`src/net/WebPanel.cpp` | 小 |
| ~~前端「规划中」菜单~~ ❌ 按需求不做，见 §2.9 | Web 不展示路线图 / 待办；改为侧栏底部显示源码仓库入口（不含简介），路线图只在本文维护 | `data/assets/app.js`、`app.css` | 小 |
| ~~时区可配 + 时间失真~~ ✅ 已实现，见 §2.10 | 默认 UTC+8 可配（`/api/timezone`、NVS、备份节），前端按设备时区显示；统一可信下限与时间戳供给，消除裸 `time(nullptr)` 与多套阈值 | `src/time/TimeSync.*`、`src/net/WebPanel.cpp`、`data/pages/settings.html` | 中 |
| ~~Back 长按行为~~ ✅ 已实现，见 §2.9 | 四种显示态统一熄屏 + 吞掉 `kRepeat`（原实现熄屏后会被自己的连发唤醒） | `src/app/UIMenu.cpp`、`UIMenu.h` | 小 |
| ~~文档补齐~~ ✅ 已实现，见 §2.8 | 四个 `poll`/`store` 接口进 API 表；BMP 注释改 JPEG；顺带补齐占位符 / `remark` / 编译开关文档 | `README.md`、`src/net/WebPanel.cpp/.h` | 小 |

### P2 新功能

**短信智能化（业务价值最高）**
- **条件转发规则引擎**：按发件人 / 关键词 / 是否验证码 / 模组 / 时间段决定转发到哪些通道、是否入库、优先级。现有黑名单只有「全转/全不转」两档。
- **自动回复 / 双向网关**：管理员命令现为固定 4 个且单槽队列；改成自定义关键词 → 自动回短信 / 转发指定号码 / 触发 HTTP。
- **来电动作**：来电检测已完整，可加白名单自动接听、黑名单自动挂断（`+CHUP`）、挂断后自动回短信。
- **验证码中心**：邮件已能识别验证码，抽成独立页/接口（倒序、一键复制、过期倒计时、正则可配）。
- **投递回执**：每条短信附投递表（各通道成功/失败/耗时），Web 展示 + 失败一键重发；与 P0 持久化队列配套。
- 短信导出 CSV/JSON、按日期批量删除、远端备份。

**可观测 / 运维**
- 一键全链路体检（AT → SIM → 网络 → TLS → SMTP → MQTT → Webhook）生成诊断报告；可复用 §2.4 的诊断字段。
- 信号强度 / 收发条数 / 通道成功率历史曲线（现只有瞬时值与今日计数）。
- SIM 余额与流量查询（`AT+CUSD` 等）+ 低余额告警；免打扰时段、通知风暴抑制、重复验证码去重。
- OTA 启动失败自动回滚、升级前自动备份配置（双槽已具备）。
- 单元测试：`test/` 目前只有模板；先给 `lib/pdulib` 做 host 原生测试再进 CI。

**通道与协议**
- 新增 Ntfy / Discord / Slack / LINE / Home Assistant 通道；Webhook 加 HMAC 签名、自定义 Header、代理。
- 局域网 mDNS 发现、本地 Web 可选 HTTPS、WebSocket 替代 SSE。
- 4G 数据链路备份：现联网只靠 WiFi，模组只收发短信；断网可用模组 PPP 拨号保住 Webhook/OTA（改动大）。

**硬件 / 电源**
- `kModemEnPin1/2 = -1` 预留未用：接 EN 引脚做硬复位，替代仅串口自愈。
- 电池/ADC 电量、低压告警、light sleep 定时唤醒（全项目无 `esp_sleep`）。
- 崩溃后自动把 coredump 摘要推送到通道（coredump 已有，只差主动上报）。

**前端体验**
- 深色主题、短信详情页、图表页、手机号脱敏显示（验证码打码 + 二次确认查看）。

### 3.1 方案存档（已实施，待设备验证）：登录限流 + 前端 SHA-256 挑战应答（2026-11-07）

**状态**：**已按 A + B 同批实施**，代码实现、编译与前端验证结果见 §2.11；本章保留为设计依据与协议契约，**设备验收仍待执行**。5 项决策于 2026-11-07 确认（见「已确认决策」）。「API 访问令牌传输保护」列入「下一步讨论」，**不在本批范围**。注意下文「现状与硬约束」记录的是**改造前**的旧实现，勿据此判断当前代码。

#### 现状与硬约束

| # | 事实 | 证据 |
|---|---|---|
| 1 | `POST /api/login` 收 JSON `{"user","pass"}`，`AdminStore::check()` 明文 `==` 比对，**无任何失败计数 / 延时** | `WebPanel.cpp:931-958`、`AdminStore.cpp:94-96` |
| 2 | 设备是 `http://<局域网IP>:80`；**`window.crypto.subtle` 只在安全上下文（HTTPS / localhost）暴露**，用 IP 访问时是 `undefined`（可在设备页面 `console.log(window.crypto.subtle)` 实测）。所以「浏览器支不支持 SHA-256」在本项目几乎必然落到**不支持原生 API** 的分支 | `Config.h:251`、§5；`crypto.getRandomValues` 在非安全上下文仍可用，但本方案不需要前端随机源 |
| 3 | 设备时钟可能未同步，`TimeSync::stamp()` 未同步时返回开机秒数，**不能拿来做绝对时间窗口**；`millis()` 才是单调可靠的计时源 | §2.10、`TimeSync.h:72-75` |
| 4 | 所有 handler 跑在 `async_tcp` 小栈任务，禁止 `delay()` / 大栈 / 多轮哈希；限流只能「记时间 → 提前返回」 | `WebPanel.h:117-119`、§5 |
| 5 | 会话表 `String[8]` 无过期、满即拒绝登录（8 次登录就能把合法用户挡在门外）；`handleLogin` 令牌表写失败返回 500 时，已写入的 sid 不会回退（遗留有效会话） | `WebPanel.h:100-104`、`WebPanel.cpp:943-949` |
| 6 | 认证白名单是 3 个精确 URL，新增端点必须加入；且长路径必须先注册 | `WebPanel.cpp:908-919`、`398-415` |
| 7 | 本库可用 `req->client()->remoteIP()` 做按 IP 维度限流 | `.pio/libdeps/esp32s3/ESPAsyncWebServer/src/Middleware.cpp:180` |
| 8 | 固件已有 mbedtls（HMAC-SHA256 在 `PushChannels.cpp:57-67`，文件内 `static` 不可直接复用）；单次 SHA-256 用 `mbedtls/sha256.h` 即可，handler 内耗时 <1ms | §5 TLS 段 |

#### 目标与非目标

- **目标**：① 登录失败限流（防在线爆破 / 撞库）；② Web 默认（v2）登录请求不再携带明文密码，服务端一次性挑战防重放；③ 浏览器「有 / 没有原生 SHA-256」都有可用路径；④ 不破坏现有脚本客户端与配置备份。
- **非目标**：**不宣称等同 HTTPS**。纯 HTTP 下抓包者可见 `nonce`/`ts`/`proof`/`salt`，可离线字典爆破、抢先重放（只造成合法登录失败，不能提权）；根治要靠 §3 P2「本地 Web 可选 HTTPS」+ AP 热点 WPA2。本批的「导出脱敏」**只覆盖 `auth` 账号节**，`wifi`/`email`/`mqtt` 的明文 `pass` 导出、MQTT 命令鉴权、AP 开放热点仍是 §3 的其它待办。

#### 实施范围与协议（A + B 同批，已确认）

**一次性做到位**：限流 + 挑战应答 + NVS 口令哈希 + 导出脱敏 + 记住密码改存 verifier + 会话表修复，同一批改完、同一批验收（不再分两次动 `AdminStore`）。因同批做，**跳过 `sha256-pass` 中间态**：前端只实现 verifier 版 proof，不做「服务端用明文重算」的临时算法；challenge 仍带 `v`/`alg`，供后续换 KDF 时平滑升级。

**数据格式契约**（前后端必须一致：UTF-8 拼接、小写 hex、十进制 ts）：
- `salt`：`esp_random()` 16 字节 → 32 位小写 hex，随账号存 NVS / 由 challenge 下发；
- `verifier = SHA256(salt_hex + ":" + pass)` → 64 位小写 hex，**NVS 只存它，不存明文 `pass`**；
- `proof = SHA256(verifier_hex + ":" + ts + ":" + nonce)` → 64 位小写 hex；
- 服务端一律恒定时间比较（固定 64 字节逐字节异或累计）。

**协议**

1. `GET /api/login/challenge`（免认证：加入白名单；按 `WebPanel.cpp:398-415` 的注册顺序规则放在 `/api/login` **之前**）
   返回 `{"ok":true,"v":2,"alg":"sha256-verifier","salt":"<32hex>","nonce":"<32hex>","ts":<设备millis>,"ttlMs":30000}`。
   - `nonce` 用 `esp_random()` 16 字节（复用 `genSecret()`），存服务端表 `nonce[32]` + `issuedAtMs`；**随机串由设备下发而不是浏览器自生成**：不依赖浏览器随机源，且设备能用自身 `millis()` 判新鲜度，不受 NTP / 浏览器时钟偏差影响。
   - `ts` 是设备 `millis()` 回显值（不是纪元秒），前端原样回传；服务端按 `millis() - issuedAtMs <= ttlMs` 校验，**不用 `TimeSync::stamp()`**。
   - nonce 表 TTL 惰性清理、满时淘汰最旧；每次登录**无论成败一次性消费**，杜绝重放。
2. `POST /api/login` 两条路径（按请求体有无 `v`/`nonce`/`proof` 分流）：
   - **v2（Web 默认）**：`{"v":2,"user":"..","ts":<ts>,"nonce":"..","proof":"<64hex>"}`，**不含 `pass`**；服务端用 NVS 里的 `verifier` 重算 proof 比对（`AdminStore::checkProof()`）。
   - **v1（兼容，默认保留）**：原 `{"user","pass"}` 契约不变；服务端用 `SHA256(salt + pass)` 重算 verifier 比对，即只改 `AdminStore::check()` 内部实现。脚本客户端、旧前端、LCD 重置后的首次登录都走这条。
   - 校验顺序：限流 → 字段 → nonce 存在且未过期 → **消费 nonce** → 用户名 / proof；失败统一 `401 "用户名或密码错误"`（不区分用户不存在 / nonce 失效，防枚举），`429` / `400` 单独可区分；成功响应与 `Set-Cookie` 完全不变（`sendJsonDocWithCookie`），`goHome()` 不用改。
3. **NVS 迁移（同批）**：`appcfg/auth` 从 `{"ver":1,"user","pass"}` 升到 `{"ver":2,"user","salt","verifier"}`；`loadFromNvs()` 读到 v1 时派生 verifier、生成 salt 并写回 v2（开机一次性、低频）。`update()` 改密码时换新 salt 并派生新 verifier；`exportTo()` 输出 `user/salt/verifier`（**不含明文 `pass`**，仍可凭备份恢复登录）；`importFrom()` 同时接受 v2 与 v1 明文备份（v1 自动升级）。
4. **定稿调整（相对上一版方案）**：`POST /api/auth` 的 `passOld` **不再改走 proof**，保持明文契约，由 `check()` 一处改动覆盖 v1 登录 / `passOld` / LCD 重置三条路径，避免把「改密码」页也卷进协议改造；`data/pages/settings.html` 与 `data/assets/app.js` 因此**不改**。新密码字段仍是明文 HTTP，与已确认保留的 v1 兼容属于同一残留面（见「已知取舍」）。

#### 浏览器能力分级（「支持 sha256」的真实含义）

| 级别 | 判定 | 做法 | 本项目命中 |
|---|---|---|---|
| L0 | `window.crypto && crypto.subtle && crypto.subtle.digest` 可用（HTTPS / localhost） | 原生 `crypto.subtle.digest('SHA-256', ...)` | 极少 |
| L1 | `subtle` 不可用（**纯 HTTP 局域网 IP**） | **自带纯 JS SHA-256**：新增 `data/assets/sha256.js`，约 2KB（gzip 后 <1.5KB），自带 UTF-8 编码，不依赖 `TextEncoder` / secure context，兼容老浏览器 | **默认命中** |
| L2 | 连 JS 哈希都跑不了 / 命令行脚本 / 旧前端 | 保留 v1 明文 `{"user","pass"}` 兼容分支（同样受限流保护）；脚本客户端改用持久化 API 令牌（`/api/token`），不再用密码登录；提供 `requireProof` 开关（默认关闭，验收通过后可开启）一刀切禁掉 v1 | 第三方脚本 |

> 结论：**不是「不支持就放弃哈希」，而是「原生 API 不可用就换纯 JS 实现」**，覆盖率接近 100%；L2 只是给无 JS 的脚本留的后路，同样落在限流与统一 401 文案内。

#### 限流设计

- **计时一律 `millis()`**（相对窗口 / 单调），不落 NVS：重启清零，避免断电后把自己锁死；攻击者无法远程重启（`/api/system/restart` 需认证，见 `WebPanel.cpp:568`），物理接触场景本就不在本方案防护范围。计数只在内存，不写 NVS，也就没有磨损问题。
- **三个维度**（阈值已确认，见「已确认决策」）：
  - 全局桶：所有来源合计每分钟最多 `kLoginGlobalFailMax`（= 20）次失败 / 挑战，防伪造 IP 绕过；
  - 来源桶：键 = `remoteIP()` + 用户名 FNV，表 16 条惰性淘汰（NAT 后多人共 IP 时靠用户名再分一层）；
  - 挑战端点：每 IP 每分钟最多 `kLoginChallengePerIpMax`（= 30）次，防刷爆 nonce 表。
- **阶梯锁定**：窗口 `kLoginFailWindowMs`（5 分钟）内失败 `kLoginFailMax`（5）次 → 锁 `kLoginLockBaseMs`（60s），再犯 ×2，上限 `kLoginLockMaxMs`（15 分钟）；成功清掉该键。锁定期**直接返回** `429 {"ok":false,"error":"尝试过于频繁，请稍后再试","retryAfter":<秒>}` + `Retry-After` 头，**绝不 `delay()` / `vTaskDelay()`**。
- **计入失败**：账号或 proof 错、nonce 无效 / 过期 / 重放、v1 明文失败；纯字段格式错误（`400`）不计，避免误伤。前端按 `retryAfter` 倒计时禁用按钮（仅体验，不是安全边界）。
- **可观测**：`auth` 分类节流日志（含 IP 末段，不刷屏），可用 `/api/logs?q=限流` 取证。
- 常量全部集中 `src/app/Config.h`：`kLoginNonceTtlMs` / `kLoginNonceMax` / `kLoginFailWindowMs` / `kLoginFailMax` / `kLoginLockBaseMs` / `kLoginLockMaxMs` / `kLoginGlobalFailMax` / `kLoginChallengePerIpMax`（照现有 `k` 前缀 + 中文注释风格）。

#### 同批修复与「记住密码」（已确认）

- 会话表加 `issuedAtMs` + 7 天惰性过期 + 满时淘汰最旧：修掉「第 9 次登录返回 500」这个现成 DoS；
- `handleLogin` 先确认两张凭证表都有空位再一起写入，修令牌表写失败时遗留有效 sid；
- **记住密码改存 verifier**：`localStorage['savedLogin']` 从 `{user, pass}` 改为 `{user, verifier}`，不再存明文密码。回填时**不往密码框塞假密码**，置 `usingSaved=true` 直接拿保存的 verifier 算 proof；用户一旦在密码框触发 `input` 就置 `false`，改用 challenge 下发的 `salt` + 输入密码现场派生 verifier。设备改密码 / 重新迁移后 salt 变化会让旧 verifier 失效 → 401 时清掉保存项并提示「密码已变更，请重新输入」。
  > **必须讲清**：verifier 是「密码等价物」，拿到它能直接构造 v2 proof 登录本机，所以这**不是安全存储**，只是把泄露范围限制在本设备（不会把用户可能在其它站点复用的明文密码交出去）；`localStorage` 仍是 XSS / 本机用户可读的平面。

#### 涉及文件

| 文件 | 改动 |
|---|---|
| `src/net/WebPanel.h/.cpp` | 新增 `handleLoginChallenge`；`handleLogin` v1/v2 分流与 proof 校验；nonce 表 + 限流桶；中间件白名单 + 路由注册顺序；SHA-256 / hex / 恒定时间比较（匿名命名空间或 `src/utils` 薄封装，实现时择一）；会话过期 / LRU |
| `src/net/AdminStore.h/.cpp` | NVS v1→v2 迁移、`salt/verifier`、`check()` 改派生比对、`checkProof()`、`exportTo()` 脱敏（仅账号节）、`importFrom()` 兼容 v1、`update()` 换 salt |
| `src/app/Config.h` | `kLogin*` 限流常量与 `auth` 格式版本 |
| `data/login.html`、新增 `data/assets/sha256.js` | 能力检测 L0/L1/L2、challenge → verifier → proof、429 倒计时、v2 请求体去掉明文 `pass`、记住密码改存 verifier |
| `data/pages/settings.html`、`data/assets/app.js` | **不改**（`POST /api/auth` 契约保持，见「定稿调整」） |
| `README.md` | 新端点、v1/v2 协议、429 契约、NVS 迁移与**回滚警告**、脱敏口径、降级与安全边界 |
| 本文 | 实施后按 §2/§3/§4/§5/§6/§7 同步状态 |

#### 验收（编译 + 设备）

1. 两种编译配置均 SUCCESS（§1.3）；改过 `data/` 必须 `uploadfs`；`node --check data/assets/sha256.js` 通过（`app.js` 本批不改），`login.html` 内联脚本在浏览器控制台确认无报错。
2. 浏览器用设备 IP 直连：`window.crypto.subtle` 为 `undefined`，走 L1 纯 JS SHA-256 仍能登录；抓包确认 v2 请求体**无明文密码**，只有 `salt`/`nonce`/`ts`/`proof`。
3. 重放同一 `{nonce,ts,proof}` → 401（nonce 一次性）；篡改 `ts`/`nonce`/`proof` → 401；challenge 后等 35s 再提交 → 401。
4. 连续 5 次错密码 → 第 6 次 `429` + `Retry-After`，到期恢复；错用户名同样计数；换 IP 仍受全局桶；限流期间不产生有效 sid。
5. **NVS 迁移**：从旧固件（`auth` v1 明文）升级 → 开机自动写成 v2、原密码仍可登录；`GET /api/config/export` 的 `auth` 节只有 `user/salt/verifier`、无 `pass`；导入新备份与导入 v1 旧备份后都能登录；改密码、LCD 随机重置密码、`POST /api/auth` 的 `passOld` 校验均正常。
6. 旧脚本 v1 明文登录仍可用（默认）；打开 `requireProof` 后 v1 返回 401；Bearer API 令牌客户端不受影响。
7. 连续 10 次成功登录不再 500；`logout` / `/api/session` / SSE / AP 配网模式回归正常。
8. 记住密码：勾选后 `localStorage` 只有 `verifier`、无明文 `pass`；重开登录页免输密码可登录；设备改密码后旧 verifier 失效并提示重新输入。
9. 回归非认证链路：收 / 发短信、发件箱、时区显示等按 §6 不受影响。

#### 已知取舍与风险

- 纯 HTTP 下 `salt`/`nonce`/`ts`/`proof` 随包可见，`proof` 可离线字典爆破（单次 SHA-256 加盐**不是慢 KDF**）；实际收益是**密码明文不再进网络 / NVS / 日志 / 备份** + 防重放 + 限流，不能防嗅探后的离线爆破或会话劫持。
- **不支持跨该版本回滚**：NVS 改 v2 后刷回旧固件，会因读不到 `pass` 字段而回落出厂 `admin/admin123` 并覆写账号；OTA 只升不降，README / 升级说明必须写明，回滚前先导出备份。
- **v1 兼容是既定残留**：默认保留期间攻击者仍可直接发明文密码（同样被限流），`POST /api/auth` 的新密码 / `passOld` 也仍明文；只有 `requireProof` 能关掉登录面的明文。`wifi`/`email`/`mqtt` 明文导出、AP 开放热点、MQTT 命令鉴权仍是 §3 其它待办。
- `localStorage` 存 verifier **不是安全存储**（密码等价物，仅限制爆炸半径），salt 变化会让保存项失效。
- 按 IP 限流在 NAT 下可能误伤，故叠加用户名维度、锁定封顶 15 分钟；计数重启清零，不落 NVS。
- 单次 SHA-256 可在 handler 内做（<1ms）；派生 verifier 只在开机迁移 / 改密码 / 导入备份等低频路径，后续若引入 PBKDF2 多轮派生必须放主循环，别在 `async_tcp` 里迭代。
- 新增 JS 受 `script/uploadfs_gz.py` 自动 gzip；`Cache-Control: no-store`（`WebPanel.cpp:67-88`）保证不缓存旧登录页。

#### 已确认决策（2026-11-07）

| # | 决策 | 结论 |
|---|---|---|
| 1 | 阶段 B（NVS 口令哈希 + 导出脱敏） | **同批做**；跳过 `sha256-pass` 中间态，直接 verifier 版 v2 |
| 2 | v1 明文兼容 | **默认保留**；`requireProof` 作为可选开关保留（默认关），不影响现有脚本 |
| 3 | 记住密码 | **改存 verifier**，不存明文密码；按「同批修复与记住密码」契约实现 |
| 4 | 限流阈值 | **合适，不改**：5 次 / 5 分钟，锁 60s 起 ×2 封顶 15 分钟，全局 20 次/分钟，challenge 30 次/分钟·IP |
| 5 | 会话过期 / LRU 与遗留 sid | **同批修** |

#### 下一步讨论（不纳入本批实施）

- **API 访问令牌的传输保护**：本批（登录）的挑战应答只覆盖登录密码，令牌链路完全不动；现已展开为 **§3.2 方案（已实施存档）**——复用登录范式的 challenge→exchange→ticket，NVS 只存 verifier，raw-Bearer 作为兼容分支默认保留（同 v1 登录）。原待讨论方向：① 令牌也改成 verifier + 每请求签名 / 证明；② 用令牌换取短期票据（已采纳为方案主体）；③ 直接上 HTTPS 一并根治；④ 只收敛暴露面（默认关闭、掩码、缩短有效期、限流）。讨论时一并明确 `authorized()` 要求 `_apiTokenEnabled`（默认关闭，登录返回的临时 token 默认不可用）这一现状是否要改。
- 相关残留（同属后续讨论）：慢 KDF（PBKDF2-HMAC-SHA256）与前端同款实现；`POST /api/auth` 新密码 / `passOld` 的非明文改造；配置导出 `wifi`/`email`/`mqtt` 的 `pass` 脱敏；AP 开放热点、MQTT 命令鉴权、§3 P2 本地 HTTPS。

---

### 3.2 方案存档（已实施，待设备验证）：API 访问令牌传输保护（2026-11-07）

**状态**：**已按 5 项决策（全部采纳）实施**，代码实现、编译与前端验证结果见 §2.12；本章保留为设计依据与协议契约，**设备验收仍待执行**。对应 §3 P0「安全三件套」之「API 访问令牌传输保护」。注意下文「现状与硬约束」记录的是**改造前**的旧实现，勿据此判断当前代码。

#### 现状与硬约束

- 设备是 `http://<局域网IP>:80`，纯 HTTP；`Bearer <token>` 头、管理端点响应、NVS 均为明文（与登录改造前同源问题）。
- `GET /api/token`（需登录）直接回完整 raw token（`WebPanel.cpp:1136-1141`）；`POST /api/token/reset` 回 raw 新令牌（`resetApiToken()` 返回 `_apiToken`）。
- NVS `apitoken` 为 `{"ver":1,"enabled":bool,"token":"<32hex>"}`，**明文存 token**（`WebPanel.cpp:342-389`）。
- `authorized()` 直接 `t == _apiToken` 比对 Bearer，**raw token 每次请求明文进线**（`WebPanel.cpp:303-311`）；登录响应里的临时 `token` 也明文。
- **浏览器 SPA 走会话 Cookie（HttpOnly）调 API**，`app.js` 的 `apiFetch` 不带 `Authorization`，故明文令牌暴露面主要在「管理页读取 + 脚本/第三方客户端每请求 Bearer」，而非浏览器自身。
- 已具备可复用设施：`CryptoUtil`（SHA-256 / hex / 恒定时间比较）、`LoginLimiter`（nonce 表 + 限流桶）、`AdminStore` 的 verifier 模式、挑战端点注册范式（`/api/login/challenge`）；本方案直接复用。

#### 目标与非目标

- **目标**：① 脚本/第三方客户端认证时**不再每请求发 raw token**，改为一次挑战交换换得短时效 ticket，后续 `Bearer <ticket>`；② NVS 只存 `token verifier`，不存 raw（与密码同口径）；③ 管理端点 `GET /api/token` 默认只回掩码，raw 仅一次性披露（reset/reveal）；④ 不破坏浏览器 SPA（仍走 Cookie）。
- **非目标**：**不宣称等同 HTTPS**，纯 HTTP 下 proof / ticket 仍可嗅探；根治靠 §3 P2 本地 HTTPS。`wifi`/`email`/`mqtt` 明文导出、MQTT 命令鉴权、慢 KDF 不在本批。

#### 主体方案（复用登录链路范式）

1. **NVS 升级**：`apitoken` 升 `{"ver":2,"enabled":bool,"verifier":"<64hex>"}`；`loadApiToken()` 见到旧 `token` 时派生 `verifier = SHA256(token)` 并丢弃 raw（一次性迁移，落盘失败仅告警、下次重试）；`resetApiToken()` 生成 raw、`storeVerifier = SHA256(raw)`、`saveApiTokenCfg()` 只写 verifier、返回 raw（一次性披露）；服务端内存只留 `_apiTokenVerifier`。
2. **挑战交换（仿登录 v2）**：
   - `GET /api/token/challenge`（需已登录）：返回 `{"ok":true,"v":2,"alg":"sha256-verifier","nonce":"<32hex>","ts":<millis>,"ttlMs":30000}`，复用 `LoginLimiter` 的 nonce 表，`ts` 用 `millis()`。
   - `POST /api/token/exchange`：`{"v":2,"nonce","ts","proof"}`，`proof = SHA256(verifier + ":" + ts + ":" + nonce)`（`verifier = SHA256(rawToken)`）；服务端用 `_apiTokenVerifier` 校验、nonce 一次性消费、命中限流返回 429 + `retryAfter`；成功签发**短时效 ticket**（不透明随机串，`storeAdd(_tokens, ttl)`），返回 `{"ok":true,"ticket":".."}`。
   - 前端管理页「复制 curl 示例」与脚本：先 `challenge` → `exchange` 取 ticket，再带 `Bearer <ticket>` 调业务接口。
3. **`authorized()` 改为只认 ticket**：只比 `_tokens` 表（TTL 惰性过期），**不再直接比 `_apiToken`**。为兼容旧脚本，保留 `RAW_TOKEN_COMPAT`（默认 1）：`Bearer <rawToken>` 仍接受但打 `auth` warn 日志；编译期 `API_TOKEN_REQUIRE_EXCHANGE=1` 可彻底关掉（同 `LOGIN_REQUIRE_PROOF` 范式）。`_tokens` 表现在统一承载「登录临时 token + exchange ticket」两类短期凭证。
4. **端点脱敏**：`GET /api/token` 返回 `{"ok":true,"enabled":bool,"token":""}`（默认不再回 raw，前端用既有掩码 UI）；raw 仅在 `POST /api/token/reset` 响应一次性给（同初始设置的一次性披露）。
5. **前端**：`app.js` 的 `apiFetch` 仍走 Cookie，无需改；仅「API 访问令牌」页的 curl 示例更新为 exchange 流程（或保留 v1 明文示例并注明兼容）。浏览器自身认证链路不受影响。

#### 涉及文件

| 文件 | 改动 |
|---|---|
| `src/net/WebPanel.cpp/.h` | `handleTokenChallenge` / `handleTokenExchange`；`authorized()` 改 ticket-only；`loadApiToken`/`resetApiToken` 改 verifier；`GET /api/token` 脱敏；认证白名单加挑战端点 |
| `src/app/Config.h` | `kApiTicketTtlMs`（建议 3600000，即 1 小时）、`RAW_TOKEN_COMPAT` / `API_TOKEN_REQUIRE_EXCHANGE` 开关 |
| `data/pages/settings.html` 或 `data/assets/app.js` | curl 示例改 exchange 流程 |
| `README.md` | 新端点、v2 协议、429 契约、NVS 迁移与回滚警告、脱敏口径 |
| `ROADMAP.md` | 实施后按 §2/§3/§4/§7 同步 |

#### 验收（编译 + 设备）

1. 编译（默认 + 双开关关闭 + `API_TOKEN_REQUIRE_EXCHANGE=1`）SUCCESS；改过 `data/` 必须 `uploadfs`。
2. 脚本走 exchange 拿 ticket 后能调 API；ticket TTL 过期后 401；重放 ticket → 401；challenge 后等 35s 再 exchange → 401。
3. raw `Bearer <token>` 默认仍可用（有 warn 日志）；开 `REQUIRE_EXCHANGE` 后 raw → 401；浏览器 SPA 用 Cookie 正常。
4. `GET /api/token` 不再回 raw（掩码）；NVS `apitoken` 只含 verifier、无 raw；`reset` 仍一次性回 raw；导入旧 v1 备份（`token` 明文）自动升级。
5. 回归：登录 / 会话 / SSE / 收发音信不受影响；限流命中返回 429。

#### 已知取舍与风险

- 纯 HTTP 下 exchange 的 `proof` 同登录可被嗅探离线爆破（单次 SHA-256 加盐非慢 KDF）；收益是 raw token 不每请求进线 / 不落 NVS。
- **兼容模式 raw-Bearer 存在时仍暴露**（同 v1 登录），只有 `REQUIRE_EXCHANGE` 能彻底关。
- ticket TTL 内若被嗅探可复用（同会话 Cookie 风险，已有 HttpOnly+SameSite 兜底）。
- **不支持跨该版本回滚**：NVS 升 v2 后刷回旧固件读不到 `verifier`，API 令牌功能异常；OTA 只升不降，回滚前先导出/重置。
- 浏览器 SPA 不受影响（Cookie 已受登录 v2 保护）。

#### 已确认决策（2026-11-07，全部采纳）

| # | 决策 | 建议 |
|---|---|---|
| 1 | NVS 只存 verifier（不存 raw） | 是 |
| 2 | 引入 challenge→exchange→ticket，raw-Bearer 默认保留兼容 | 是（同 v1 登录） |
| 3 | `GET /api/token` 默认只回掩码 | 是 |
| 4 | ticket TTL | 1 小时 |
| 5 | 浏览器 SPA 维持 Cookie、仅脚本/第三方走 exchange | 是 |

---

## 4. 推荐实施顺序

1. **设备验证**：关闭 OPEN 缺陷（§2.4）+ 验证发件箱续投（§2.5）—— 核心链路可用性优先于一切新功能。
2. ✅ **持久化投递队列 + 投递回执已完成**（§2.5）→ 下一步做**条件转发规则引擎**，与发件箱的通道快照衔接（在 `snapshotChannels()` 处按规则裁剪通道），避免返工。
3. ✅ **半成品批次：5 个占位符 + 编译开关 + 文档补齐已完成**（§2.8）、BACK 长按 + 侧栏仓库入口（§2.9；规划中菜单按需求不做）、时区可配 + 时间失真（§2.10）→ 余下联系人簿（号码备注）、SSE 快照补全、SMTP 587 / STARTTLS。
4. **安全三件套**：✅ 登录限流 + 口令哈希 + 前端挑战应答已完成（§2.11，方案 §3.1，**待设备验证**）；✅ API 访问令牌传输保护已完成（§2.12，方案 §3.2，**待设备验证**）→ 余下 TLS 证书校验（可配置 CA/指纹，保留 `setInsecure` 兼容开关）、MQTT 命令鉴权。
5. ✅ **长期保底：看门狗已完成**（§2.7，编译验证已补跑通过，见 §2.8）→ 余下 `pdulib` 单元测试 + CI。

> 每个阶段结束都应：编译通过 → 更新本文档 §2/§7 → 在 `README.md` 同步 API/配置变更 → 需要设备验证的写清楚验收步骤。

---

## 5. 架构约束与已知坑（必读）

**任务上下文与栈**
- `async_tcp`（Web）任务栈很小，**禁止在 HTTP handler 里做 TLS 握手 / 同步发邮件 / 长耗时操作**，必须「入队 + 主循环 tick 执行」（见 `WebPanel.cpp` 注释与 `/api/email/test` 的实现）。
- 串口抽数任务优先级 2、栈 8KB（`SerialBridge.cpp:121`），URC 回调里可做文件 I/O；`onReceiveError` 回调只有 2~4KB 栈，**只能计 O(1) 计数**，日志由抽数任务合并打印（曾因在回调里 printf 触发 DoubleException panic）。
- 短信查询任务 `sms_query` 栈 8192、优先级 3；pdulib 实例非线程安全，解码/重组必须持 `_codecMutex`。
- **任务看门狗（§2.7）**：`loopTask` / `uart-pump` / `sms_send` / `sms_query` 已订阅 TWDT（60s / panic 自动重启）。新增常驻任务必须 `TaskWatchdog::registerCurrent()` 并在循环 / 阻塞等待处 `feed()`；新增超过 60s 的同步阻塞必须调大 `cfg::kWdtTimeoutMs` 或补喂狗点，否则会误重启。`async_tcp` 由库自行订阅，其 handler 里长阻塞也会让它无法回循环喂狗。

**串口与 AT**
- 端口是**每模组独立互斥锁**，`send()` 阻塞、`trySend()` 非阻塞（抽数任务驱动状态机，绝不能阻塞）。
- `lockPort()` 等锁循环每 1s 喂一次狗，不再 `portMAX_DELAY` 死等；`readResponse()` 与 `sendSmsPdu()` 的等待循环同理（§2.7）。
- 后台初始化/心跳只许用 `trySend`；长时间持锁会挡住 `pumpOnce` 的 URC 归档。
- `sendAt()` 会在就绪态自动还原 `AT+CMGF=0` + `cnmiCmd()`（现已校验响应），用户随意改 AT 可能短暂影响查询链路。

**内存 / PSRAM**
- 大缓冲、LVGL 帧缓冲、日志环、AT 历史环、mbedTLS 大块都优先 PSRAM；无 PSRAM 时容量退化（`Config.h` 有对照值）。
- 全局 TLS 并发通道信号量最多 4（`src/utils/TlsTurn.h`），OTA/推送/SMTP/MQTT-TLS 共用，超了会 `MBEDTLS_ERR_SSL_ALLOC_FAILED`。

**分区与 OTA**
- 分区表 `partitions/esp32s3_16mb.csv`：`userdata` 5MB 挂 `/data`（刷固件保留，只有出厂复位才格式化）、`spiffs` 2.75MB 挂 `/`（Web 资源）、`coredump` 128KB、双 OTA 槽各 4MB。
- 两个 FS 分区 subtype 同为 `spiffs`，**代码里一律按 label 查找**，传 `nullptr` 会误命中地址更小的 `userdata`。
- `board_upload.arduino.boot_app0 = 0x19000`：框架默认 0xe000 落在 nvs 内，每次 upload 会抹掉配置。
- OTA tag 格式 `<VERSION>-<提交时间>-<sha7>`，由 `script/inject_app_version.py` 编译期注入；产物名 `ota-<tag>-<platform>.bin`，`APP_PLATFORM` 必须与 CI 一致。

**配置与认证**
- NVS 命名空间 `appcfg`，键：`wifi / appass / auth / sms / apitoken / email / mqtt / push / remark / blacklist / disp / sched / tz / lastts`（`remark` 为设备备注纯字符串，`{remark}` 占位符取值，§2.8）；`coredump`（崩溃时间 / 版本）与 `wdt`（复位原因 / 卡死任务 / 累计次数）是独立命名空间（不随出厂复位清除）。
- 管理账号 `appcfg/auth` 为 **v2**：`{"ver":2,"user","salt","verifier"}`，`verifier = SHA256(salt:pass)`，**不存明文密码**；旧 v1 明文记录开机自动迁移（§2.11）。**不支持回滚到旧固件**：旧固件读不到 `pass` 会回落 `admin/admin123` 并覆写账号，OTA 只升不降、回滚前先导出备份。出厂账号 `admin/admin123`（编译期 `-DWEB_AUTH_*_DEFAULT` 可覆盖）。
- 配置导出 `auth` 节只含 `user/salt/verifier`；`wifi / email / mqtt` 等节**仍含明文密码**，备份需妥善保管。
- 所有 `/api/`（除 `login/challenge`、`login`、`logout`、`session` 四个端点）需 Cookie `sid` 或 Bearer 或持久化 API 令牌；登录限流、挑战应答与 v1 兼容开关见 §2.11 / §3.1。
- **前端哈希的硬约束**：`window.crypto.subtle` 只在安全上下文（HTTPS / localhost）暴露，设备是 `http://<局域网IP>:80`，用 IP 访问时必然是 `undefined`；登录页已内置 `data/assets/sha256.js` 纯 JS 实现（§2.11）。限流 / nonce / 会话过期一律用 `millis()`，不要用未同步时返回开机秒数的 `TimeSync::stamp()`；会话与令牌表 7 天惰性过期、满时淘汰最旧。

**其它**
- 所有出网 TLS 目前 `setInsecure()`，OTA 无签名 → P0 待修。
- 队列普遍偏浅且满则丢弃：邮件 6、推送 6（丢最旧）、短信发送 8、管理员命令单槽、SSE 8 客户端（顶掉最旧）。**短信事件已改走持久化发件箱**（§2.5），不再受这些队列限制；其余事件仍在原队列里。
- 时间未同步时时间戳仍为**开机秒数**（兼容约定，前端显示「开机 X」）；可信下限统一为 `cfg::kEpochFloorSec`（§2.10），记录时间戳一律经 `TimeSync::stamp()` / `epochOrUptime()`，**不要再直接 `time(nullptr)`**。时区可在 Web 配置，前端按设备时区格式化。
- 两个编译期开关已修复（§2.8）：宏在 `src/app/Config.h` 归一化（未定义默认 1），`ENABLE_MQTT=0` 用空实现替身顶住装配代码、`ENABLE_FIRMWARE_UPGRADE=0` 裁掉 `Ota.cpp` 升级主体；改开关必须两种组合都编译验证。
- 推送模板的模组专属字段（`carrier/to/iccid/signal`）取的是**推送时刻的模组快照**，不会为了渲染模板临时发 AT；`AT+CCID` 只在模组就绪后的一次性静态采集里发一次。
- `managed_components/` 有约 40 个 ESP-IDF 组件但源码无任何引用；`include/`、`test/` 基本是模板空目录。
- 代码注释与日志均为中文；配置常量集中在 `src/app/Config.h`，不要散落硬编码。

---

## 6. 验证与回归方法

1. `pio run -e esp32s3` 必须 SUCCESS；改过 `data/` 还要 `pio run -e esp32s3 -t uploadfs`。
2. 改 AT / 短信链路后，重点回归：直投模式（`CNMI=2,2`）与存储模式（`CNMI=2,1`）**各收一条普通短信 + 一条长短信**、发送短信、来电提醒、模组热重初始化、`/api/at/send` 后配置还原；长短信分段分多批 `+CMTI` 到达时只落盘一次、SIM 分段一次性删净（§2.6）。
3. 串口响应判停相关改动（`readResponse`）影响所有 AT 命令，回归时看「模组调试」页 AT 历史是否有响应被截断/串包、初始化能否正常到 `kReady`。
4. 不接 USB 时用 Web 取证：`/api/logs`（按 `q=关键词` 过滤）、`/api/at/hist?after=<id>`、`/api/sms/query`、`/api/status`。
5. 正常收信日志序列（存储模式）：「主动查询任务已启动」→「就绪（PDU 模式…）」→「收到 +CMTI 索引通知」→「待读取短信索引」→「收到短信 #N」→「收到短信：…」→「查询完成…」。缺哪一步即定位到哪一段。
6. 改看门狗 / 长阻塞路径后，按 §2.7「验收」做空载 30 分钟 + 四个任务的故障注入自检，确认无意外重启且原因记录正确。
7. 改编译开关相关代码后，除默认配置外还要按 §1.3 的命令验证 `ENABLE_MQTT=0` + `ENABLE_FIRMWARE_UPGRADE=0`，并在两个 `firmware.elf` 里确认应裁掉的字符串确实消失。改过 `data/` 别忘了 `uploadfs`。
8. 改推送模板 / 模组快照后，按 §2.8「验收」核对占位符取值、ICCID 采集与备注持久化；改过 `AT+CCID` 静态采集步骤（`idStep`）时重点看初始化仍能正常到 `kReady`。
9. 改按键状态机后按 §2.9「验收」回归四种显示态的 BACK 长按 / 短按、唤醒、UP/DOWN/OK 连发；改 `data/assets/app.js` 后跑 `node --check`，改过 `data/` 必须重新 `uploadfs` 才能在设备上看生效。
10. 改时间 / 时区后按 §2.10「验收」回归默认 +480、改成半时区、重启沿用、导入导出与未同步显示；新增记录时间戳必须用 `TimeSync::stamp()`，判断时钟可信一律用 `cfg::kEpochFloorSec`，不要再直接 `time(nullptr)`。
11. 改登录 / 认证链路后按 §3.1「验收」回归：v1 / v2 两条登录路径、nonce 重放与 TTL、429 限流、NVS `auth` v1→v2 迁移与回滚警告、导出脱敏、记住密码 verifier 失效、会话过期 / LRU、Bearer 令牌与 SSE 不受影响；改过 `data/` 必须 `uploadfs`。

---

## 7. 变更记录

| 日期 | 内容 | 状态 |
|---|---|---|
| 2026-11-07 | 全面梳理现有功能，形成 P0/P1/P2 功能清单与实施顺序（本文 §3/§4） | 已记录 |
| 2026-11-07 | 修复存储模式 `CNMI=2,1` 收短信不入库缺陷（§2），编译通过 | 代码完成，**待设备验证** |
| 2026-11-07 | 实现持久化不丢消息转发队列 / 发件箱（§2.5），含 4 个 Web API 与「发件箱」页 | 代码完成，编译通过，**待设备验证** |
| 2026-11-07 | 网页发送短信接入发件箱（§2.5 发出类记录）；发件箱页新增「发送短信」按钮与弹窗 | 代码完成，编译通过，**待设备验证** |
| 2026-11-07 | 「短信」页发送弹窗统一为「发件箱」页同款（模组/号码/内容、「加入发件箱」按钮、结果未知提示）；提交即返回，去掉 `pollSend()` 轮询 | 代码完成，`node --check` 通过，**待设备验证** |
| 2026-11-07 | 修复存储模式长短信分批 `+CMTI` 导致的重复接收（§2.6：分段索引登记 + 拼接成功后补删全部分段） | 代码完成，编译通过（RAM 36.6% / Flash 47.9%），**待设备验证** |
| 2026-11-07 | 实现显式任务 / 硬件看门狗（§2.7）：TWDT 60s 重配置、4 个常驻任务订阅与喂狗、卡死原因 RTC+NVS 留痕、`/api/wdt/selftest` 与 Web 卡片 | 代码完成，编译验证已补跑通过（§2.8），**待设备验证** |
| 2026-11-07 | 补齐 5 个推送模板占位符（§2.8）：新增 `AT+CCID` / `ModemSnapshot.iccid`，`{carrier}/{to}/{iccid}/{signal}` 取模组快照，`{remark}` 取 NVS `appcfg/remark`；推送页新增设备备注配置与占位符表 | 代码完成，两种配置编译通过，`node --check` 通过，**待设备验证** |
| 2026-11-07 | 修编译开关（§2.8）：`Config.h` 归一化宏、`ENABLE_MQTT=0` 空实现替身 + 前端菜单隐藏、`ENABLE_FIRMWARE_UPGRADE=0` 裁掉 OTA 升级主体；`/api/status` 增 `mqtt.compiled` / `otaCompiled` | 代码完成，默认 SUCCESS（RAM 36.7% / Flash 48.1%）、双开关关闭 SUCCESS（RAM 36.5% / Flash 45.8%），**待设备验证** |
| 2026-11-07 | 文档补齐（§2.8）：README 补 `/api/sms/poll` `/api/sms/store` 四行、占位符表格、`remark` / NVS 键 / 编译开关说明；`WebPanel.cpp` 屏幕预览注释 BMP→JPEG；本文 §0/§1.2/§1.3/§2.8/§3/§4/§5/§6/§7 同步 | 已记录，**待设备验证** |
| 2026-11-07 | 「前端规划中菜单」按需求否决并整段撤销（§2.9）：删掉 `MENU` 规划分组、`todoPage()`、路由 todo 分支与 `.todo` 样式，Web 恢复只显示 16 个已实现页面；路线图只在本文维护 | 已撤销，`node --check` 通过，**待设备验证** |
| 2026-11-07 | Web 仓库入口（§2.9）：`renderShell()` 侧栏底部与 `login.html` 卡片底部只放 `https://cnb.cool/null/sms-forwarder`（新标签页打开、窄屏收起抽屉），按需求**不放项目简介**；新增 `.side-foot` / `.login-repo` 样式；README 顶部补仓库地址并明确 Web 不展示待办 | 代码完成，**待设备验证**（需重新 `uploadfs`） |
| 2026-11-07 | BACK 长按行为（§2.9）：`handle()` 开头统一熄屏并吞掉 BACK 的 `kRepeat`，修掉「熄屏后立刻被自己连发唤醒」；文本页不再特判无定义操作，弹窗熄屏时顺带收起；`UIMenu.h` 按键注释与 README 按键表同步 | 代码完成，两种配置编译通过，**待设备验证** |
| 2026-11-07 | 时区可配 + 时间失真（§2.10）：`TimeSync` 可配置 POSIX TZ（默认 UTC+8，NVS `appcfg/tz`，支持半时区）、`GET\|POST /api/timezone`、Web 设置页与仪表盘显示、配置备份 `timezone` 节；新增 `cfg::kEpochFloorSec` 与 `TimeSync::stamp()` 统一时间戳口径，前端列表改按设备时区格式化；README 新增「时间与时区」章节 | 代码完成，两种配置编译 SUCCESS（36.7%/48.2% 与 36.5%/45.9%）、`node --check` 与沙箱校验通过，**待设备验证**（需 `uploadfs`） |
| 2026-11-07 | 按需求调整：Web 侧栏 / 登录页移除项目简介文案，只保留仓库链接（§2.9）；`TimeSync::loadTimezone()` 在首次启动或值越界时把默认东八区 `tz=480` 显式写回 NVS，并修 `x-for` / `x-model` 时序导致设置页首屏下拉未选中当前时区的问题（§2.10） | 代码完成，两种配置编译 SUCCESS、`node --check` 通过，**待设备验证**（需 `uploadfs`） |
| 2026-11-07 | 主题微调：侧栏菜单滚动条轨道改为与菜单同色 `#191c24`（标准 `scrollbar-color` + WebKit 两套写法）；模组调试「类型 / 模组」分段控件底色由硬编码 `#11131a` 改为 `var(--bg-soft)`，浅色文字对比度 3.10:1 → 5.58:1；表单控件 `color-scheme` 跟随页面主题，避免系统深色 + 页面浅色时原生下拉颜色错乱 | 代码完成，CSS 括号平衡 / 对比度校验通过，**待设备验证**（需 `uploadfs`） |
| 2026-11-07 | 新增「登录限流 + 前端 SHA-256 挑战应答」方案（§3.1）：服务端下发 nonce/ts 一次性挑战、浏览器原生或内置纯 JS SHA-256 双实现、全局/IP/账号三层限流；明确纯 HTTP 下 `crypto.subtle` 不可用、计时用 `millis()`、不阻塞 `async_tcp`；同步 §3 / §4 / §5 | **仅方案，待确认**，代码未动 |
| 2026-11-07 | §3.1 方案定稿：5 项决策确认（A+B 同批 / v1 默认保留 / 记住密码改存 verifier / 阈值不改 / 会话修复同批）；协议改为直接 verifier 版 v2，含 NVS `auth` v1→v2 迁移与回滚警告；新增「API 访问令牌传输保护」为下一步讨论项；同步 §3 / §4 / §6 | **方案已确认，待实施**，代码未动 |
| 2026-11-07 | 实现登录限流 + 口令哈希 + 前端 SHA-256 挑战应答（§2.11，按 §3.1 A+B 同批）：新增 `CryptoUtil` / `LoginLimiter` / `sha256.js`，`AdminStore` v1→v2 迁移与导出脱敏，v2 挑战应答 + 三层限流 + 429，v1 兼容开关 `LOGIN_REQUIRE_PROOF`，会话过期 / LRU 与遗留 sid 修复；README 与本文 §0 / §1.2 / §2.11 / §3 / §4 / §5 / §7 同步 | 代码完成，三种编译配置 + `buildfs` 均 SUCCESS，`node --check` / SHA-256 对照 / 语法检查通过，**待设备验证**（需 `uploadfs`），**未提交 git** |
| 2026-11-07 | 实现 API 访问令牌传输保护（§2.12，按 §3.2 全部采纳）：NVS 只存 verifier（v1 明文自动迁移）、新增 `/api/token/challenge`+`/api/token/exchange` 换短时效票据、`authorized()` 改 ticket-only（raw Bearer 兼容默认保留）、`GET /api/token` 脱敏；README 与本文 §3.2 / §2.12 / §4 同步 | 代码完成，两种编译 + `buildfs` SUCCESS、`node --check` 通过，**待设备验证**（需 `uploadfs`），**未提交 git** |
| — | 后续每次阶段性工作，请在此追加一行并同步 §0/§2/§3 的状态 | — |
