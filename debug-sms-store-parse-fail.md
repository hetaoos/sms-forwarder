# Debug Session: sms-store-parse-fail

> 本文只记录此缺陷；完整功能路线图、后续工作与接手指引见 [ROADMAP.md](ROADMAP.md)。

- **Status**: FIX-IMPLEMENTED（代码修复已落地，待设备取证后关闭）
- **progress_status**: verifying
- **Issue**: 通信模组已收到短信（串口可见），但未解析成功、未入库 LittleFS、未推送
- **Evidence Source**: 设备本地日志路径（LogStore 环形缓冲，Web「运行日志」/「模组调试」）
- **Evidence Directory**: /tmp/trae-debug-sms-forwarder/sms-store-parse-fail
- **Log File**: （复用设备 LogStore，不落 NDJSON；设备未接 USB，证据取自 Web 日志页）
- **Debug Server**: 未启用（设备离线采集，走本地日志出口）

## Reproduction Steps
1. 设备刷入 CNMI=2,1 存储模式固件（短信落通信模组侧，仅上报 +CMTI 索引）
2. 向设备号码发送一条短信 → 串口出现 `+CMTI: "SM",<idx>`
3. 预期：查询任务下发 AT+CMGL/AT+CMGR 读回短信 → 落盘 → 推送
4. 实际：短信未入库、未推送（模组侧副本应仍在，说明未走到删除）

## Hypotheses & Verification
| ID | Hypothesis | Likelihood | Effort | Evidence |
|----|------------|------------|--------|----------|
| A | `AT+CMGL=0` 在该模组不被支持 / 返回非预期格式 → 索引列表为空 → 查询静默结束 | High | Low | Pending |
| B | `AT+CMGR=<idx>` 响应格式与预期不符（无 PDU 行 / 带引号 / 空响应）→ extractPduHex 返回空 → 跳过 | High | Low | Pending |
| C | 拿到 PDU 但 `SmsCodec::decode` 解码失败（pdulib 不接受该 PDU） | Med | Low | Pending |
| D | 查询任务从未执行（+CMTI 未识别 / queryPending 未置位 / 任务未创建 / 模组未 kReady） | Med | Low | Pending |
| E | 已解码并落盘，但推送通道异常（推送服务/通道失败） | Low | Med | Pending |

## 已实施的修复（2026-11-07，代码侧）

按「+CMTI 可见 → 未解析 / 未入库 / 未推送 / SIM 副本仍在」的链路逐段加固：

1. **串口响应不再被 80ms 静默截断**（`src/serial/SerialBridge.cpp`）
   - `readResponse()` 改为「见到标准终止行 `OK` / `ERROR` / `+CMS ERROR` 即结束」，
     静默兜底从 80ms 放宽到 300ms。`AT+CMGR` 头行与 PDU 行之间停顿较久时，
     原实现会提前返回，把 PDU 字节留给下一条命令 → 串包、解析失败。
   - `send()` / `trySend()` 下发命令前抽干 RX 残留并交回 `pend`（复用
     `drainRxLocked()`，与 `sendSmsPdu()` 一致），避免上一条命令的尾部混进本次响应。
2. **`+CMTI` 不再被 `waitingPdu` 吞掉**（`src/sms/SmsService.cpp::onUnsolicited`）
   - 只有纯 hex 行才作为 `+CMT` 的 PDU 消费；其它行清标志后继续按行路由。
   - `waitingPdu` 加 `kSmsPduWaitMs`(5s) 超时自清；`setStoreMode()` 切换时清零。
   - `+CMTI` 分支补上 `cmdPauseUntil` 让后台 AT 让行（原仅 `+CMT` 有）。
3. **初始化必须确认 PDU 模式**（`pollInit` 的 `kConfig`）
   - `AT+CMGF=0` 移到 `AT+CNMI` 之前；这两条校验 `OK`，失败最多重试 3 次、
     停在本步不推进到 `kReady`（原实现只看端口锁是否取到，ERROR 也照样就绪）。
   - `sendAt()` 的 CMGF/CNMI 还原同样校验响应，失败留 WARN。
   - `queryPort()` 每轮初始化后首次查询用 `AT+CMGF?` 复核，非 PDU 模式立即纠正。
4. **`+CMTI` 优先、索引不再一次性丢失**（`queryPort()`）
   - 有待查索引时直接 `AT+CMGR`，不再每轮固定发两条 8s 超时的 `AT+CMGL=0/1`；
     CMGL 降级为「无 CMTI 索引 / 兜底轮询 / 连续失败自愈」时的补充扫描。
   - 原实现在 CMGR 之前就 `queryIndexCount = 0`，一次读失败即永久丢失索引。
     现改为**只有成功读回并解码才移除**，失败的索引保留并按 3s→60s 指数退避重试；
     连续 3 轮失败自动转 CMGL 全量扫描，避免快速路径被异常索引永久占住。
   - 队列重建时保留查询期间新到达的 `+CMTI`，不覆盖。
5. **`extractPduHex()` 重写**：要求先见 `+CMGR:`/`+CMGL:` 头（拒绝串包残留的 hex 行），
   支持长 PDU 分行拼接、去行内空格与引号，遇 `OK`/`ERROR` 收口，跳过混入的 URC。
6. **可取证性**：`queryStatusJson()` 新增 `indexes` / `cmgfChecked` / `cmgfOk` /
   `failStreak` / `lastCmgr` / `lastError`，Web「通信模组」页在有异常时直接展示，
   无需接 USB 串口即可判断链路停在哪一步。
7. **失败退避**：CMGL 全无响应与读回失败都设置 `queryRetryAfterMs`，
   避免查询任务每 250ms 刷一轮 AT 风暴。

## Log Evidence
（待采集：设备 Web「运行日志」页 + 「模组调试」页）

### 取证日志（当前固件实际文案）

> 早期插桩清单的文案与当前固件已不一致，此处按修复后代码列出。完整日志序列与
> 判读方法见 [ROADMAP.md](ROADMAP.md) §2.4 / §6；不接 USB 时优先看
> `/api/sms/query` 的 `indexes` / `cmgfOk` / `failStreak` / `lastCmgr` / `lastError`
> 与 Web「通信模组」页诊断区。

- `sms`「主动查询任务已启动」/「主动查询任务创建失败」→ 查询任务是否存活
- `sms`「就绪（PDU 模式 + 短信存储已配置）」→ 初始化是否确认了 CMGF/CNMI
- `sms`「通信模组 %u 收到 +CMTI 索引通知（索引 %d，待查 %u 个），已置待查标记」→ URC 通路与索引解析
- `sms`「通信模组 %u 待读取短信索引 %u 个（未读 %u / 已读 %u / +CMTI %u）」→ 索引来源
- `sms`「通信模组 %u AT+CMGL=0/1 未解析到索引（响应前 100 字：…/…）」→ 模组原始响应（5 分钟节流）
- `sms` WARN「通信模组 %u AT+CMGR=%d 未取到 PDU：…」→ 读回响应格式异常
- `sms` WARN「通信模组 %u 索引 %d 的 PDU 解码失败：…」→ pdulib 兼容性
- `sms`「通信模组 %u 查询完成：存量 %u 条，处理 %u 条，删除 %u 条，读回失败 %u 条」

### 已实施的加固（绕开 CMGL 依赖）
- `+CMTI` 行末数字解析为索引，存入 `ModemState::queryIndexes[]`（8 槽、去重，无单值
  `m.queryIndex` 字段）；查询时**优先**直接 `AT+CMGR=<idx>`，`AT+CMGL=0/1` 降级为
  补充扫描（捞回已读残留 / 兜底轮询 / 连续失败自愈）
  → 即使模组不支持 `AT+CMGL`，新短信仍能读回。
- 读失败的索引**保留并 3s→60s 退避重试**，不再在 CMGR 之前一次性清空
  （原实现 `queryIndexCount = 0` 会让索引永久丢失）。

## Verification Conclusion
（待设备验证后补：刷入修复固件 → 按复现步骤 1-4 重测 → 采集下列证据）

## 设备验证清单（取证后据此关闭本 issue）

1. 刷入修复固件，确认 Web「通信模组」页无异常诊断（`PDU 模式 正常`、
   `待重试索引 0`、无「最近错误」）。
2. 向设备号码发一条普通短信，日志应依次出现：
   - 「收到 +CMTI 索引通知（索引 N，待查 1 个），已置待查标记」
   - 「待读取短信索引 1 个（… / +CMTI 1）」
   - 「收到短信 #N，来自 …」
   - 「查询完成：存量 1 条，处理 1 条，删除 1 条，读回失败 0 条」
   - 短信列表出现该记录，且邮件 / MQTT / Webhook 收到转发。
3. 连续发多条（含长短信）确认 `+CMTI` 索引不互相覆盖、SIM 侧副本被删除。
4. 在控制台发 `AT+CMGF=1` 后立刻收一条短信，确认 `AT+CMGF=0` 还原成功、
   短信仍能入库（验证 CMGF 复核与纠正）。
5. 若仍失败：查看 `/api/sms/query` 的 `lastError` / `lastCmgr` / `cmgfOk` /
   `indexes` / `failStreak`，以及 AT 历史里 `AT+CMGR=<idx>` 的完整响应，
   据此区分假设 A~E：
   - `lastCmgr` 是文本正文 → TEXT 模式（假设 B 的变体，已由 CMGF 复核覆盖）
   - `lastCmgr` 只有 `+CMGR:` 头无 PDU 行 → 响应收集仍被截断（假设 A/B）
   - `lastError` 为「PDU 解码失败」→ pdulib 兼容性（假设 C）
   - `indexes` 持续增长且不减少 → CMGR 索引失效或模组存储格式差异（假设 A/B）
   - 始终没有「收到 +CMTI」日志但 AT 历史有 `+CMTI` → URC 分流问题（假设 D）

## Final Evidence Gate
- **Evidence Sources**: 设备 Web 日志页 /api/logs、AT 历史 /api/at/hist、
  查询诊断 /api/sms/query（新增字段）、Web「通信模组」页诊断区
- **progress_status**: verifying
- **Hypothesis Conclusions**: 待设备取证后逐条填写（A~E）
- **关闭条件**: 上述验证清单 1-4 全部通过，且 24 小时内无「读回失败」日志
