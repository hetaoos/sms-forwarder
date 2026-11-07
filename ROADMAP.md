# 开发路线图与交接说明

> 面向后续接手的 AI / 开发者：本文说明「项目现在做到哪一步、下一步做什么、动手前必须知道什么」。
> 最后更新：2026-11-07 ｜ 固件版本：`1.0.0`（`VERSION`）｜ 平台：ESP32-S3 16MB Flash + 8MB PSRAM / Arduino
> 相关文档：[README.md](README.md)（功能与 API）、[debug-sms-store-parse-fail.md](debug-sms-store-parse-fail.md)（单缺陷取证记录）

---

## 0. 快速接手（先读这一节）

1. **当前状态**：核心链路已完整可运行；2026-11-07 完成两项代码工作（① 存储模式 `CNMI=2,1` 收短信不入库缺陷修复，见 §2；② 持久化不丢消息转发队列 / 发件箱，见 §2.5，含「收到短信转发」与「网页主动发短信」两类记录），**完整编译通过**（RAM 36.3% / Flash 47.7%），但**尚未刷机实测**，改动**未提交 git**。
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
- 根目录 `compile_commands.json` 是**陈旧快照**（缺 `-D ENABLE_MQTT=1`、OTA 宏与 LVGL 路径），不要用它做 `-fsyntax-only` 检查，会误报 `MqttService` incomplete / `lvgl.h` 找不到；新增的 `.cpp` 也不在其中，IDE 的 clang 诊断会报 `../hal.h` 找不到、`JsonDocument` 一堆假错误。一律以 `pio run` 完整编译为准。
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
- 复用 `SmsService::enqueueSend()`：CMGS 仍在不入任务看门狗的常驻 `sms_send` worker
  里执行，`Outbox::deliver()` 绝不阻塞 loop；`done` 经 `_sms.tick()` →
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

---

## 3. 待办清单

难度：小 = 半天内；中 = 1~2 天；大 = 需分阶段。**验收标准**用于判断是否做完。

### P0 必须补的短板

| 项 | 价值 | 涉及文件 | 难度 | 验收标准 |
|---|---|---|---|---|
| ~~持久化「不丢消息」转发队列~~ ✅ 已实现，见 §2.5 | 短信事件已改走发件箱；**非短信事件**仍是邮件/推送队列深 6、满则丢最旧、重启即丢，后续可扩展 | `src/app/Outbox.*`、三个服务的单次投递接口 | 大 | 断电重启后未送达消息继续补发；每条短信可查各通道投递结果 |
| TLS 证书校验 + OTA 固件验签 | 全部出网 TLS 跳过校验，OTA 无签名/哈希 | `src/utils/HttpSession.cpp:27`、`src/net/EmailService.cpp:200`、`src/net/MqttService.cpp:533`、`src/ota/Ota.cpp` | 中 | 可配置 CA/指纹；校验失败有明确日志；保留兼容开关 |
| 登录限流 + 口令哈希 + 导出脱敏 | 默认 `admin/admin123`、NVS 明文存密码、导出含明文、`handleLogin` 无失败计数 | `src/net/AdminStore.cpp`、`src/net/WebPanel.cpp:885`、`src/net/WifiManager.cpp:415` | 中 | 连续失败锁定；NVS 存加盐哈希；导出可选脱敏 |
| MQTT 命令面设备侧鉴权 | 任何能发 `<prefix>/<id>/command` 的主体可执行 `send_at`/`restart`/`send_sms` | `src/net/MqttService.cpp:300-360` | 小 | 命令带签名或白名单，非法命令被拒且记日志 |
| 显式任务/硬件看门狗 | 全项目无 `esp_task_wdt_*`，靠约定规避卡死 | `src/app/App.cpp` | 中 | 关键任务注册 WDT，卡死自动重启并记原因 |

### P1 半成品补齐（性价比最高）

| 项 | 说明 | 涉及文件 | 难度 |
|---|---|---|---|
| 补齐 5 个模板占位符 | `{carrier}` `{to}` `{iccid}` `{signal}` `{remark}` 目前留空；运营商/本机号/信号 `ModemSnapshot` 已有，`iccid` 加一条 `AT+CCID`，备注加 NVS 字段 | `src/push/MsgContext.h:14-20`、`PushService.cpp:419` | 小 |
| 联系人簿 / 号码备注 | 顺带让 `{remark}` 有意义，推送显示姓名而非裸号码 | `src/sms/`、`src/push/`、`data/pages/` | 中 |
| SSE 补全 | 全量快照缺 `sms`/`smsports`；`Last-Event-ID` 续传只做了 AT 历史；Bearer 客户端无法用 SSE（可加 query token） | `src/net/StatusBus.cpp:139-185` | 小 |
| SMTP 587 / STARTTLS | README 明确的已知限制 | `src/net/EmailService.cpp` | 中 |
| 修编译开关 | `ENABLE_MQTT=0` 会因 `App` 无条件实例化 `MqttService` 而编译失败；`ENABLE_FIRMWARE_UPGRADE` 定义了却无任何 `#if` | `src/net/MqttService.h:32`、`src/app/App.h:63`、`src/ota/Ota.h:137` | 小 |
| 前端「规划中」菜单 | `data/assets/app.js:599-601` 已支持 `todo`/`hidden` 徽标但 MENU 无数据，正好标注本清单待办 | `data/assets/app.js:539-565` | 小 |
| 时区可配 + 时间失真 | UTC+8 硬编码；未同步时时间戳退化为开机秒数，日志/短信/AT 历史共用该口径 | `src/time/TimeSync.cpp`、`src/app/App.cpp:131` | 中 |
| Back 长按行为 | `src/app/UIMenu.cpp:245-247` 注明「暂未定义」 | `src/app/UIMenu.cpp` | 小 |
| 文档补齐 | `/api/sms/poll`、`/api/sms/store` 未进 API 表；`WebPanel.cpp:529` 注释仍写 BMP（实为 JPEG） | `README.md`、`src/net/WebPanel.cpp` | 小 |

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

---

## 4. 推荐实施顺序

1. **设备验证**：关闭 OPEN 缺陷（§2.4）+ 验证发件箱续投（§2.5）—— 核心链路可用性优先于一切新功能。
2. ✅ **持久化投递队列 + 投递回执已完成**（§2.5）→ 下一步做**条件转发规则引擎**，与发件箱的通道快照衔接（在 `snapshotChannels()` 处按规则裁剪通道），避免返工。
3. **半成品批次**：5 个占位符 + 联系人簿 + SSE 快照补全（改动小、体验提升明显）。
4. **安全三件套**：TLS 校验（可配置 CA/指纹，保留 `setInsecure` 兼容开关）、登录限流、口令哈希；顺带做 MQTT 命令鉴权。
5. **长期保底**：看门狗 + `pdulib` 单元测试 + CI。

> 每个阶段结束都应：编译通过 → 更新本文档 §2/§7 → 在 `README.md` 同步 API/配置变更 → 需要设备验证的写清楚验收步骤。

---

## 5. 架构约束与已知坑（必读）

**任务上下文与栈**
- `async_tcp`（Web）任务栈很小，**禁止在 HTTP handler 里做 TLS 握手 / 同步发邮件 / 长耗时操作**，必须「入队 + 主循环 tick 执行」（见 `WebPanel.cpp` 注释与 `/api/email/test` 的实现）。
- 串口抽数任务优先级 2、栈 8KB（`SerialBridge.cpp:121`），URC 回调里可做文件 I/O；`onReceiveError` 回调只有 2~4KB 栈，**只能计 O(1) 计数**，日志由抽数任务合并打印（曾因在回调里 printf 触发 DoubleException panic）。
- 短信查询任务 `sms_query` 栈 8192、优先级 3；pdulib 实例非线程安全，解码/重组必须持 `_codecMutex`。

**串口与 AT**
- 端口是**每模组独立互斥锁**，`send()` 阻塞、`trySend()` 非阻塞（抽数任务驱动状态机，绝不能阻塞）。
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
- NVS 命名空间 `appcfg`，键：`wifi / appass / auth / sms / apitoken / email / mqtt / push / blacklist / disp / sched / lastts`；`coredump` 是独立命名空间（不随出厂复位清除）。
- 出厂管理账号 `admin/admin123`（编译期 `-DWEB_AUTH_*_DEFAULT` 可覆盖）；配置导出**含明文密码**。
- 所有 `/api/`（除 login/logout/session）需 Cookie `sid` 或 Bearer 或持久化 API 令牌。

**其它**
- 所有出网 TLS 目前 `setInsecure()`，OTA 无签名 → P0 待修。
- 队列普遍偏浅且满则丢弃：邮件 6、推送 6（丢最旧）、短信发送 8、管理员命令单槽、SSE 8 客户端（顶掉最旧）。**短信事件已改走持久化发件箱**（§2.5），不再受这些队列限制；其余事件仍在原队列里。
- 时间未同步时时间戳为开机秒数，前端以 `>= 1.6e9` 区分。
- `ENABLE_MQTT=0` 当前**编译会失败**；`ENABLE_FIRMWARE_UPGRADE` 是名义开关（无 `#if`）。
- `managed_components/` 有约 40 个 ESP-IDF 组件但源码无任何引用；`include/`、`test/` 基本是模板空目录。
- 代码注释与日志均为中文；配置常量集中在 `src/app/Config.h`，不要散落硬编码。

---

## 6. 验证与回归方法

1. `pio run -e esp32s3` 必须 SUCCESS；改过 `data/` 还要 `pio run -e esp32s3 -t uploadfs`。
2. 改 AT / 短信链路后，重点回归：直投模式（`CNMI=2,2`）与存储模式（`CNMI=2,1`）**各收一条普通短信 + 一条长短信**、发送短信、来电提醒、模组热重初始化、`/api/at/send` 后配置还原。
3. 串口响应判停相关改动（`readResponse`）影响所有 AT 命令，回归时看「模组调试」页 AT 历史是否有响应被截断/串包、初始化能否正常到 `kReady`。
4. 不接 USB 时用 Web 取证：`/api/logs`（按 `q=关键词` 过滤）、`/api/at/hist?after=<id>`、`/api/sms/query`、`/api/status`。
5. 正常收信日志序列（存储模式）：「主动查询任务已启动」→「就绪（PDU 模式…）」→「收到 +CMTI 索引通知」→「待读取短信索引」→「收到短信 #N」→「收到短信：…」→「查询完成…」。缺哪一步即定位到哪一段。

---

## 7. 变更记录

| 日期 | 内容 | 状态 |
|---|---|---|
| 2026-11-07 | 全面梳理现有功能，形成 P0/P1/P2 功能清单与实施顺序（本文 §3/§4） | 已记录 |
| 2026-11-07 | 修复存储模式 `CNMI=2,1` 收短信不入库缺陷（§2），编译通过 | 代码完成，**待设备验证** |
| 2026-11-07 | 实现持久化不丢消息转发队列 / 发件箱（§2.5），含 4 个 Web API 与「发件箱」页 | 代码完成，编译通过，**待设备验证** |
| 2026-11-07 | 网页发送短信接入发件箱（§2.5 发出类记录）；发件箱页新增「发送短信」按钮与弹窗 | 代码完成，编译通过，**待设备验证** |
| 2026-11-07 | 「短信」页发送弹窗统一为「发件箱」页同款（模组/号码/内容、「加入发件箱」按钮、结果未知提示）；提交即返回，去掉 `pollSend()` 轮询 | 代码完成，`node --check` 通过，**待设备验证** |
| — | 后续每次阶段性工作，请在此追加一行并同步 §0/§2/§3 的状态 | — |
