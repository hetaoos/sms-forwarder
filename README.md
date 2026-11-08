# 短信转发器（SMS Forwarder）

源码仓库：<https://cnb.cool/null/sms-forwarder>

基于 ESP32-S3 的短信转发设备：外接两个 4G 通信模组（Cat.1），通过 LVGL 驱动的
ST7789 屏幕与物理按键提供本地交互，并内置 Web 控制台做远程管理。短信、来电、
SIM 插拔、设备启动等事件可同时分发到 **SMTP 邮件 / MQTT / 13 种 webhook 推送通道**。

> 开发路线图、待办清单与后续工作交接见 [ROADMAP.md](ROADMAP.md)（内部开发文档）。

## 目录

- [功能特性](#功能特性)
- [硬件](#硬件)
- [快速开始](#快速开始)
- [编译期配置](#编译期配置)
- [屏幕 UI 与按键](#屏幕-ui-与按键)
- [时间与时区](#时间与时区)
- [MQTT 主题与命令](#mqtt-主题与命令)
- [推送模板占位符](#推送模板占位符)
- [Web API 参考](#web-api-参考)
- [脚本示例](#脚本示例)
- [项目结构](#项目结构)

## 功能特性

- **短信收发与存储**（`sms/SmsService`、`sms/SmsStore`、`sms/SmsCodec`）
  - 双通信模组独立收发，PDU 编解码（UCS2 / GSM7 / 8bit）+ 长短信自动重组
    （5 个并发重组槽位、单条最多 20 段、30 秒超时）
  - 落盘 `userdata` 分区（`/data/sms/<id>.txt`，一条一文件），FIFO 上限 1000 条
  - 列表分页 / 关键字搜索 / 按通信模组过滤 / 单条删除 / 清空，今日收发计数
  - 发送走独立任务执行 CMGS 两段式握手（不阻塞 HTTP 与抽数任务），
    POST 启动 + GET 轮询结果；长短信自动分段
  - 新短信在屏幕上弹窗提示（30 秒自动收起，任意键立即收起）
  - 收到的短信先登记**持久化发件箱**（`/data/outbox/<id>.json`）再逐通道转发，
    断电 / 重启后未送达通道自动续投，Web「发件箱」页可查各通道投递回执
  - 网页上主动发出的短信同样登记到发件箱（Web「短信」页与「发件箱」页均可发送），
    仍由专职 `sms_send` worker 执行 CMGS，不阻塞主循环
- **来电提醒**（`sms/SmsService`）
  - `RING`/`+CRING` 起一轮来电，`+CLIP`/主动 `+CLCC` 带主叫号码，
    3 秒后主动 `AT+CLCC` 兜底查询，10 秒窗口超时以「未知号码」派发；
    通话结束（NO CARRIER 等）立即收口，10 秒同号去重
  - 派发走邮件 / MQTT（`.../call` 主题）/ webhook 推送全链路，
    命中黑名单（来电作用域）时不派发；号码可能为「未知号码」/「号码保密」
- **管理员短信远程控制**（`sms/SmsService`）：
  在「管理员」页有一个**总开关（默认禁用，须显式开启）**与**最多 5 个**管理员手机号；
  仅当开关开启且配置了号码时，命令短信才会被执行（不落盘、不转发）。
  开关关闭或号码清空时，所有短信按普通短信处理（入库 + 转发）：
  - `SMS:<号码>:<内容>`：从收到命令的那个模组代发一条短信，
    执行结果（成功 / 失败原因）从同一模组回复给**发命令的那个管理员**
  - `RESET`：2 秒后重启设备（重启前先回复确认短信）
  - `PING [主机]`：经通信模组做网络连通性测试（默认 `8.8.8.8`，可指定如 `PING 1.1.1.1`），
    并回附系统运行状态（在线 / WiFi / IP / 运行时间 / 内存 / 模组 / MQTT 等）；测完自动还原短信模式
  - `HELP`：回复命令清单与参数说明
  - 关键字（RESET / PING / HELP 与 `SMS:` 前缀）大小写不敏感
  - 号码匹配忽略 `+86` 前缀；黑名单优先级更高（某管理员把自己拉黑则其命令失效）；
    命令格式错误时按普通短信处理（入库并转发）。命令入队于抽数任务、执行于主循环，
    单命令队列（上一条未执行完时新的被丢弃并记日志）
- **黑名单**（`sms/BlackList`）：最多 100 条规则，匹配模式 **精确 / 前缀 / 通配符**
  （`*` 任意串、`?` 单字符），作用域按位覆盖 **短信 / 来电**，
  命中动作 **仅记录不推送**（默认，照常入库但不转发）或 **静默丢弃**（不入库不转发）；
  匹配前归一化号码（去空格与 `-()./` 等分隔符）；网页可输入号码做规则测试
- **通信模组管理**（`sms/SmsService`、`sms/ModemSnapshot`）
  - 每个模组可独立**启用 / 禁用**并设**别名**（落盘 NVS），支持网页触发**热重初始化**
  - 非阻塞初始化状态机：等 AT（~20s）→ 等 SIM（~22s）→ 等注册（~90s）→ 下发配置 → 就绪，
    任一阶段超时转故障态并 15 秒后重试；单个动作最多持端口锁 1.5s，
    故障模组绝不阻塞另一个模组与 Web
  - 就绪后心跳探活（30s，连续 3 次失败转重初始化）+ 动态遥测
    （`AT+CSQ` 15s / `AT+COPS?` 5 分钟兜底）
  - 快照含型号 / 固件版本 / 本机号码 / 运营商（中文化）/ 注册状态 / IMEI / ICCID / 信号强度；
    ICCID 由就绪后一次性 `AT+CCID` 采集（兼容 `+CCID:` / `+ICCID:` / 裸数字回显），
    随 `/api/sms/ports` 输出并用于推送模板 `{iccid}`
- **推送通道**（`push/PushService`）：13 种方式（Bark / 钉钉 / 飞书 /
  Telegram / 企业微信 / Gotify / PushPlus / Server酱 / Mailgun /
  通用 POST-JSON / GET / 自定义模板 / **SMS 短信备份**），最多 10 条通道，
  **广播**或**故障转移**策略，可按通道开启失败重试；SMS 备份通道不走 HTTP——
  网络不可用时经通信模组把消息以短信发到目标手机号（互联网全断的最后通知手段），
  并对 SIM / 启动 / 在线类事件自动跳过。自定义模板支持占位符，见
  [推送模板占位符](#推送模板占位符)
- **MQTT 上报与远程命令**（`net/MqttService`，编译期 `ENABLE_MQTT=1` 开启）
  - 基于 esp-idf 内置 esp-mqtt（无需额外库），支持 TLS、自定义 clientId / 保活 / LWT 遗嘱
  - 事件类型位掩码（启动 / SIM / 短信 / 上线 / 来电，另含 **运行日志实时推送**位，
    默认关闭以免日志风暴）
  - 可订阅命令主题远程执行 `send_sms` / `send_at` / `restart` / `reinit` / `info` / `help`，
    命令在主循环线程执行（不在 MQTT 网络任务里直接调用），结果回发响应主题，
    详见 [MQTT 主题与命令](#mqtt-主题与命令)
- **SMTP 邮件通知**（`net/EmailService`）
  - 统一 HTML 卡片模板（灰底白卡 + 渐变标题栏 + 状态徽标 + 公共信息块），
    自动识别短信验证码并在标题/正文大字高亮；客户端不支持 HTML 时附带纯文本兜底
  - 通知类型位掩码（可分别开关）：**启动完成** / **SIM 状态变更** /
    **收到短信** / **定时在线通知**（两种排程：固定间隔 N 小时 /
    每日定点时:分）/ **来电提醒**，出厂五类全开
  - 运行期插拔卡检测：遥测周期查询 `AT+CPIN?`，掉卡/插卡成对通知
    （开机首次就绪与通信模组重启不误报）
  - 非阻塞入队 + 主循环发送：短信/SIM 回调（串口抽数任务）只做入队，
    SMTP 会话在 `App::tick` 执行，失败退避重试 3 次，认证失败不重试
  - 配置存 NVS（`appcfg` / `email`），授权码不回显；网页可发送测试邮件
  - **总开关**：停用后一切邮件都不发（短信 / SIM / 启动 / 定时在线全部静默
    跳过，待发队列丢弃），已保存的服务器、账号与通知类型保留，重新开启立即恢复
  - 仅支持隐式 TLS 端口（465 等，握手即加密），不支持 587（STARTTLS）
- **定时在线通知排程**（由通知类型位 bit3 总控）：
  `schedMode=1` 固定间隔（`intervalH` 小时，1-720），基于 millis 开机起算；
  `schedMode=0` 每日定点（`schedHour:schedMinute`），基于本地时间，依赖
  NTP / SIM 时间同步，触发时刻落盘 NVS 故重启不会补发，10 分钟窗口内的
  迟到触发仍有效。保存配置后会立即补发一封在线通知便于确认生效
- **双串口 AT 透传**（`serial/SerialBridge`）
  - 独立 FreeRTOS 任务持续抽数，命令/响应同步模型，阻塞式发送 + 超时收集
  - 连续 3 次无响应自动复位模组串口（自愈），支持手动复位
  - 环形缓冲保存最近 AT 记录（含模组自发上报 URC，分命令/后台/上报三类），
    Web 端增量拉取；有 PSRAM 时 512 条（≈658KB），无 PSRAM 退化为 30 条
  - 收发字节数 / 错误 / 超时 / 复位次数统计
- **ST7789 240x320 屏幕**（`hal/Display` + `ui/Ui` + `app/UIMenu`）
  - LovyanGFX 作面板驱动，**LVGL 9.6** 作 UI 框架；LVGL 全屏渲染缓冲落在 PSRAM，
    该缓冲同时是 Web 实时预览（`GET /api/screen`，JPEG）的取帧源
  - 五个页面一次建好（开机页 / 实时状态页 / 菜单页 / 可滚动文本页 / 新短信弹窗），
    之后只更新文本，靠 LVGL 脏区计算局部刷新
  - 中文字库由 TinyTTF 运行时从 `/ui_cn.ttf`（GB2312）渲染，图标/Logo 为编译进
    flash 的位图字体；开机页不依赖字库，可在字库加载期间安全显示
  - 支持 Web 远程开关屏、背光亮度调节（0-100%）、自动熄屏与自动返回状态页定时
  - 4 个物理按键导航（详见 [屏幕 UI 与按键](#屏幕-ui-与按键)），
    Web 端 `POST /api/key` 可远程注入按键事件（与物理键同路径）
- **板载 RGB 灯**（`hal/RgbLed`）：WS2812（GPIO48），5 组预设 + 任意 RGB 值 +
  亮度调节（0-100%）
- **WiFi 管理 + 配网**（`net/WifiManager`）
  - 编译期内置默认账号密码（`-DWIFI_SSID_DEFAULT` / `-DWIFI_PASS_DEFAULT`，不设置则为空）：
    首次启动且 NVS 中无 WiFi 配置时，自动把该默认账号**写入 NVS（入库）**，之后作为
    普通保存项在网页可见并可编辑
  - NVS 按功能存 JSON 配置，最多 5 组，启动时按顺序依次尝试（每组等 8 秒），
    掉线 5 秒后重连
  - 全部候选失败 3 轮后自动切换 AP 配网模式（`SMS-Forwarder-Setup`）；
    AP 热点密码可配置（编译期 `-DAP_PASS_DEFAULT` 或网页 `/api/wifi/appass`），
    未设置时为开放热点
  - 网页与 API 在所有模式下统一认证，支持 **Cookie 会话**与 **Bearer 访问令牌**
    两种方式；管理账号（用户名/密码）可在「系统设置」页修改，NVS
    `appcfg/auth` 只存 `salt` + `verifier = SHA256(salt:pass)`，**不存明文密码**，
    修改后全部登录态立即作废
  - Web 登录默认走 **SHA-256 挑战应答**：`GET /api/login/challenge` 下发
    `salt/nonce/ts`，前端提交 `proof = SHA256(verifier:ts:nonce)`，请求体不含
    明文密码；nonce 30 秒有效且一次性消费，登录失败有全局 / IP / 账号三层限流
    （429 + `retryAfter`）。v1 明文登录为兼容分支，编译期
    `-DLOGIN_REQUIRE_PROOF=1` 可禁用；详见「约定与认证」
  - 出厂管理账号可编译期注入（`-DWEB_AUTH_USER_DEFAULT` / `-DWEB_AUTH_PASS_DEFAULT`），
    两者必须同时有值才采用，任一为空/缺失则整体回落 `admin` / `admin123`
  - 配网采用 AP+STA 双栈：连网成功后页面不中断，重启后按配置直连；
    AP 模式下仍每 30 秒后台静默重试 STA
  - Web 支持扫描附近 WiFi、选中填密码、保存并连接、删除、上移/下移排序
- **Web 控制台**（`net/WebPanel`，端口 80）：Alpine.js 多页应用，打包进
  LittleFS 资源分区（`data/login.html` + `data/index.html` 外壳 +
  `data/pages/*.html` 内容页 + `data/assets/` 资源，支持 gzip）。
  左右布局：左侧分组菜单，右侧按 hash 路由懒加载内容页；
  侧栏底部只放源码仓库入口 `cnb.cool/null/sms-forwarder`
  （由 `renderShell()` 生成，不随菜单滚动、不调用 API；链接在新标签页打开），
  登录页卡片底部是同一个链接；**不展示项目简介文案**；
  开发路线图只在仓库 `ROADMAP.md` 维护，**Web 界面不展示待办 / 规划中内容**；
  全部接口统一在 `/api/` 前缀下，页面与静态资源公开，接口需登录
  （Cookie 会话或 Bearer 令牌），防止第三方盗用
- **SSE 实时推送**（`net/StatusBus`，`/api/sse`）：状态变化时主动推送给
  全部已连接浏览器（1 秒 diff 比对，只推变化，并发上限 8），网页端不再轮询状态接口；
  REST 接口原样保留供第三方客户端使用
- **OTA 在线升级**（`ota/Ota`，编译期 `ENABLE_FIRMWARE_UPGRADE=1` 开启）
  - 版本标识即 Release tag（`<VERSION>-<提交时间>-<短 SHA>`），
    由 `script/inject_app_version.py` 编译期注入
  - 「检查更新」双渠道：Release 列表 JSON 为主，Release 徽章 SVG 兜底
  - 在线下载固件写入 ota_0/ota_1 双槽，或本地上传固件 / LittleFS 镜像分片烧录；
    全过程在独立任务执行，进度经 `/api/ota/status` 轮询
  - 所有出网 TLS 阶段先取 TLS 通道（全局最多 4 个并发），避免握手内存不足
- **设备日志**（`log/LogStore`）：运行日志存入 PSRAM 定长记录环（有 PSRAM 时
  2048 条 ≈467KB，无 PSRAM 退化为 256 条），写满 FIFO 淘汰最旧；
  支持按级别 / 关键字过滤的分页查询、增量拉取与一键清空
- **时间同步**（`time/TimeSync`）：NTP 为主（24 小时周期校准）、
  SIM NITZ（`AT+CCLK?`）兜底（30 秒重试），每 10 分钟落盘 NVS，重启可恢复；
  显示时区可在「系统设置 - 设备时区」配置（**默认东八区 UTC+08:00**，落盘
  `appcfg/tz`，支持半时区 / 45 分钟时区），所有页面按设备时区显示时间；
  未同步时日志 / 短信 / AT 历史统一按约定记为开机秒数并显示「开机 X」，
  详见 [时间与时区](#时间与时区)
- **定时重启（系统保活）**（`app/SystemService`）：固定间隔（1-720 小时）或
  每日定点 + 星期位掩码，配置存 NVS（`appcfg` / `sched`），默认禁用
- **崩溃转储**（`app/Coredump`）：`coredump` 分区（128KB）由 ESP-IDF panic 处理器
  自动写入；NVS 独立命名空间记录崩溃时间与固件版本（不随出厂复位清除）。
  Web「系统工具」页可查询 / 导出 bin / 擦除分区
- **任务看门狗**（`app/TaskWatchdog`）：ESP-IDF TWDT（硬件定时器）运行期重配为
  60 秒并订阅 4 个常驻任务（`loopTask` / `uart-pump` / `sms_send` / `sms_query`）；
  长 AT 等待、CMGS 握手、端口锁等待处持续喂狗，卡死超时由 panic 自动重启。
  触发时在 RTC 内存留下各任务心跳，下次开机写运行日志 + NVS（`wdt` 命名空间），
  Web「系统工具」页与 `/api/status` 可查复位原因、疑似卡死任务、累计次数与固件版本；
  `POST /api/wdt/selftest` 可故障注入验证整条自动重启链路
- **用户数据分区**（`app/UserFs`）：5MB `userdata` 挂载 `/data`，存放短信记录等，
  **重烧固件不丢失**；仅「出厂复位」会格式化
- **配置导入导出**（`/api/config/export`、`/api/config/import`）：一键备份 / 恢复
  WiFi、管理账号、邮件、短信、MQTT、推送、黑名单、屏幕与定时重启配置
- **资源治理**（`app/Psram.h`、`utils/TlsTurn`、`utils/tls_mem`、`utils/HttpSession`）
  - PSRAM（8MB）承载大块数据：邮件正文构造、短信列表分页与响应 JSON 内存池、
    日志环、AT 历史环、LVGL 渲染缓冲；无 PSRAM 或空间不足时自动回退内部堆
  - mbedTLS 大块分配重定向到 PSRAM；全局 TLS 并发通道计数信号量（最多 4 个），
    OTA / 推送 / SMTP / MQTT-TLS 共用，避免 `MBEDTLS_ERR_SSL_ALLOC_FAILED`
  - CPU 使用率（`hal/CpuMonitor`，基于 FreeRTOS 运行时间统计）与芯片温度遥测

## 硬件

板型 `esp32s3_120_16_8-qio_opi`（Arduino 框架，pioarduino 平台），引脚定义集中在
`src/app/Config.h`：

| 功能 | 引脚 | 说明 |
|------|------|------|
| 模组1 TX / RX | GPIO17 / GPIO18 | 外接通信模组 |
| 模组2 TX / RX | GPIO41 / GPIO40 | 外接通信模组 |
| TFT CS / RST / DC | GPIO15 / GPIO5 / GPIO14 | ST7789 控制 |
| TFT MOSI / SCLK | GPIO13 / GPIO12 | SPI（SPI2，40MHz） |
| TFT BLK | GPIO8 | PWM 背光，默认 80% |
| RGB LED | GPIO48 | 板载 WS2812 |
| 按键 OK / UP / DOWN / BACK | GPIO42 / GPIO47 / GPIO38 / GPIO2 | 低电平有效（接上拉，按键到 GND） |

分区表 `partitions/esp32s3_16mb.csv`（16MB Flash）布局；app 分区起点与大小均
64KB 对齐，`userdata` 刷固件后仍保留：

| 分区 | 地址范围 | 大小 | 用途 |
|------|---------|------|------|
| nvs | 0x009000~0x018FFF | 64KB | 配置存储（WiFi 等，按功能存 JSON） |
| otadata | 0x019000~0x01AFFF | 8KB | OTA 启动槽位记录 |
| （保留） | 0x01B000~0x01FFFF | 20KB | 64KB 对齐补白 |
| app0 / app1 | 0x020000 / 0x420000 | 各 4MB | 双 OTA 固件槽位 |
| userdata | 0x820000~0xD1FFFF | 5MB | 用户数据，刷固件保留（挂载 `/data`） |
| spiffs | 0xD20000~0xFDFFFF | 2.75MB | 资源分区：Web 页面 / 字体 / 图标（挂载 `/`） |
| coredump | 0xFE0000~0xFFFFFF | 128KB | 崩溃转储 |

注意事项：

- `pio run -t uploadfs` 的目标是表中最后一个 FS 分区（spiffs 资源分区），
  只影响 Web 资源；`userdata` 仅由设备运行期读写，重烧固件不会丢失。
- `userdata` 与 `spiffs` 两个分区 subtype 同为 `spiffs`，代码里一律按 **label**
  查找分区（`cfg::kPartLabelSpiffs` / `kPartLabelUserdata` / `kPartLabelCoredump`），
  严禁传 `nullptr`（会误命中地址更小的 `userdata`）。
- `platformio.ini` 里 `board_upload.arduino.boot_app0 = 0x19000` 把框架默认写在
  0xe000 的 boot_app0 重定向到真实 otadata 位置——否则每次 `upload` 都会抹掉
  nvs 尾部导致配置丢失。

## 快速开始

### 编译与烧录

依赖 PlatformIO Core（>= 6），库版本以 `platformio.ini` 声明为准：

- pioarduino platform-espressif32（Arduino 框架）
- ESP32Async/ESPAsyncWebServer 3.12.1
- ESP32Async/AsyncTCP 3.5.0
- lovyan03/LovyanGFX 1.2.34
- lvgl/lvgl 9.6.0（屏幕 UI 框架）
- bblanchon/ArduinoJson 7.4.3
- mobizt/ReadyMail 0.4.2（SMTP 邮件发送，需 C++17，见 `platformio.ini` 的
  `-std=gnu++17` 与 `build_unflags`）
- 本地库：`lib/pdulib`（PDU 编解码）、`lib/jpge`（屏幕预览 JPEG 编码）

```bash
# 编译
pio run -e esp32s3

# 烧录固件（串口见 platformio.ini 的 upload_port）
pio run -e esp32s3 -t upload

# 上传 Web 页面到 LittleFS 资源分区（改过 data/ 后需重新执行）
pio run -e esp32s3 -t uploadfs

# 串口监视
pio device monitor -e esp32s3
```

**资源自动 gzip**：`data/` 下的文件在打包时由 `script/uploadfs_gz.py`
自动 gzip 成 `<文件名>.gz` 后再写入镜像（构建前后自动切换目录，源文件始终
保持可读；进程异常退出后下次运行会自动恢复现场）。设备端 `serveStatic`
与 `WebPanel` 都会优先读取 `.gz` 并返回 `Content-Encoding: gzip`，浏览器
自动解压。字库 `data/ui_cn.ttf` 在不压缩名单内。烧录固件（`pio run -t upload`）
成功后也会自动上传一次 FS 镜像。

**版本与 OTA 地址注入**：`script/inject_app_version.py` 在编译前把
`OTA_RELEASE_TAG` / `OTA_RELEASES_BASE_URL` / `OTA_LATEST_VERSION_URL`
编进固件（缺宏则直接编译失败）。CI 通过环境变量注入；本地编译按
`<VERSION>-<HEAD 提交时间>-<短 SHA>` 自拼一份同格式 tag，保证能过编译。

### 首次配网

1. 烧录后首次启动：若 NVS 中完全没有 WiFi 配置且固件内置了默认账号
  （`-DWIFI_SSID_DEFAULT` 非空），设备会先把该默认账号写入 NVS 并尝试连接；
   未内置默认账号、或默认连接失败时，才开启热点 **`SMS-Forwarder-Setup`**
   （默认开放，若内置/设置过 AP 密码则需密码）
2. 连接该热点，浏览器打开 **`http://192.168.4.1/login`**，在独立登录页输入
   `admin` / `admin123` 登录（登录响应同时返回 Bearer 令牌，见仪表盘页面）
3. 进入「WiFi 设置」卡片：**扫描附近 WiFi** → 点击扫描结果填入 SSID →
   输入密码 → **保存并连接**（内置默认账号也会显示在此列表，可随时修改/删除）
4. 连接成功后页面持续可用（双栈模式）；重启后将按保存的顺序自动连接

已保存配置存放在 NVS：命名空间 `appcfg`，键 `wifi`，值为 JSON 对象（管理账号
另存于同命名空间的键 `auth`：
`{"ver":2,"user":"..","salt":"<32hex>","verifier":"<64hex>"}`，不含明文密码；
旧版 v1 明文记录 `{"ver":1,"user":"..","pass":".."}` 开机自动迁移）：

```json
{
  "ver": 1,
  "list": [
    { "ssid": "HomeWiFi", "pass": "12345678" },
    { "ssid": "OfficeWiFi", "pass": "" }
  ]
}
```

启动时按 `list` 顺序依次尝试，每组等待 8 秒，全部失败 3 轮后进入 AP 配网模式。
Web 界面可随时添加（同 SSID 覆盖密码）、删除、上移/下移调整顺序，上限 5 组。

其余 NVS 键（同命名空间 `appcfg`）：`appass`（AP 热点密码）、`sms`（通信模组启用/别名、
管理员号码）、`apitoken`（API 访问令牌）、`email`、`mqtt`、`push`、`blacklist`、
`disp`（屏幕设置）、`sched`（定时重启）、`lastts`（最后已知时间）、
`tz`（时区偏移，分钟整数，默认 `480` = UTC+8，首次启动自动写入）、
`remark`（设备备注，纯字符串，供推送模板 `{remark}` 使用，最长 64 字节）。
崩溃信息另存于独立命名空间 `coredump`；看门狗诊断另存于 `wdt`
（`reset` / `reason` / `count` / `wdtTask` / `wdtUptime` / `wdtVer`）。
两者都不随出厂复位清除。

## 编译期配置

以下宏在 `platformio.ini` 的 `build_flags` 中注入，均为可选（有默认值）。

### 默认 WiFi 账号

首次启动且 NVS 中无任何 WiFi 配置时，固件会自动把这里的默认账号写入 NVS（入库）并优先
尝试连接；之后该账号作为普通保存项出现在「WiFi 设置」列表，可随时修改或删除。若未设置
（留空），则完全依赖 NVS 配置，首次无配置直接进入 AP 配网模式。

```ini
build_flags =
    -Isrc
    -DLGFX_USE_V1=1
    -DWIFI_SSID_DEFAULT=\"MyWiFi\"
    -DWIFI_PASS_DEFAULT=\"MyPass\"
```

### 出厂 Web 管理账号

**用户名与密码必须同时有值**才采用编译期值；任一为空/缺失（或长度超限：
用户名 >32、密码 >64 字符）则整体回落 `admin` / `admin123`
（在启动时初始化 NVS 账号时判定，见 `AdminStore::begin`）。
仅在 NVS 中无有效账号记录时生效（首次启动写入 NVS，之后以 NVS 为准，可在网页修改）：

```ini
build_flags =
    -DWEB_AUTH_USER_DEFAULT=\"admin\"
    -DWEB_AUTH_PASS_DEFAULT=\"admin123\"
```

### AP 配网热点密码

8~63 字符（WPA2 要求），留空表示**开放热点**（默认）。非法长度会回落开放并告警
（避免 softAP 静默失败）。首次启动（NVS 无记录）采用编译期值并落盘，之后以 NVS
为准，可在网页或用 `/api/wifi/appass` 修改：

```ini
build_flags =
    -DAP_PASS_DEFAULT=\"setup1234\"
```

### 功能开关

| 宏 | 默认 | 说明 |
|----|------|------|
| `ENABLE_FIRMWARE_UPGRADE` | `1` | OTA 在线升级 / 本地上传（分区表需具备 ota 双槽）。置 `0` 时整个检查/下载/烧录实现不编译，仅保留版本标识与 `/api/ota/status`（`available=false`），Web 升级入口与 LCD 升级动作隐藏 |
| `ENABLE_MQTT` | `1` | MQTT 服务（esp-mqtt 为 esp-idf 内置组件，无需额外 lib_deps）。置 `0` 时 `MqttService.cpp` 全实现不编译，改由头文件里的空实现替身顶住装配代码；`/api/status` 返回 `mqtt.compiled=false`，前端 MQTT 菜单自动隐藏，配置类 API 返回 404「MQTT 未编译进固件」 |
| `ENABLE_LCD` | `1` | ST7789 屏幕 + LVGL UI + 菜单状态机（4 键导航）整体开关。置 `0` 时 `Display` / `ui::Ui` / `UIMenu` 的实现整体不编译（LVGL、LovyanGFX 不再链接），改由头文件里的空实现替身顶住装配代码；`/api/status` 返回 `lcdCompiled=false`，前端 LCD 菜单、屏幕预览、按键导航入口隐藏，**LCD 相关接口（`/api/disp`、`/api/screen`、`/api/key` 等）统一返回 404「LCD 未编译进固件」**。**关闭后 4 个物理按键与 LCD 菜单整体失效**（菜单状态机已裁掉，按键不再响应） |
| `ENABLE_LED` | `1` | 板载 RGB 灯（WS2812）。置 `0` 时 `RgbLed` 实现不编译，同样由空实现替身顶住装配代码；`/api/status` 返回 `ledCompiled=false`，前端 RGB 灯菜单隐藏，**RGB 相关接口（`/api/led`、`/api/rgb`）统一返回 404「LED 未编译进固件」** |
| `APP_PLATFORM` | `esp32s3` | 平台标识，决定 OTA 产物文件名后缀，须与 CI Release 文件名一致 |

四个宏在 `src/app/Config.h` 里归一化：命令行未定义时按默认 `1` 处理，避免 IDE
用陈旧的 `compile_commands.json` 时功能被静默裁掉或误报 `incomplete`。
改开关后建议用全新构建目录验证：`PLATFORMIO_BUILD_DIR=.pio/verify pio run -e esp32s3`。

### 编译目标（多环境）

`platformio.ini` 在公共 `[env]` 段下放公共配置，再用两个具体环境覆盖差异：

| 环境 | `ENABLE_LCD` | `ENABLE_LED` | `APP_PLATFORM` | 说明 |
|------|--------------|--------------|----------------|------|
| `esp32s3` | `1` | `1` | `esp32s3` | 标准版：带屏幕 + LED，字库 `ui_cn.ttf` 随 Web UI 资源打进 FS |
| `esp32s3-slim` | `0` | `0` | `esp32s3-slim` | 精简版：无屏无灯。`lib_deps` 仅含公共库，**不声明也不下载** LVGL/LovyanGFX（标准版才叠加 `[common].lcd_lib_deps`）；字体位图与 `ui_cn.ttf` 均不打进 FS（省约 1.2MB） |

`slim` 版的 `APP_PLATFORM` 后缀 `esp32s3-slim` 会进入 OTA 产物名（`ota-<tag>-esp32s3-slim.bin`），
设备端据此下载对应固件。CI（`.cnb.yml`）对每个环境各起一个并行 job 编译并发布到同一 Release。
本地验证：`pio run -e esp32s3-slim -t buildfs`（确认 FS 镜像不含 `ui_cn.ttf`）。


## 屏幕 UI 与按键

（仅当 `ENABLE_LCD=1` 时生效；置 `0` 时屏幕、菜单与 4 键整体不编译，物理按键不再响应——
其余后台功能不受影响，详见上文「功能开关」的 `ENABLE_LCD`）

4 个物理按键（OK / UP / DOWN / BACK，低电平有效）驱动 `app/UIMenu` 的菜单状态机，
页面由 `ui/Ui`（LVGL）绘制。`hal/Button` 长按阈值 600ms，长按期间每 180ms 连发一次
（`cfg::kBtnLongMs` / `cfg::kBtnRepeatMs`）。

| 输入 | 行为 |
|------|------|
| 熄屏时任意键 | 唤醒并停在当前页面，这一下不导航；自动熄屏与长按熄屏共用此路径 |
| 状态页任意键 | 进入主菜单，回到上次选中的那一项 |
| BACK 短按 | 返回上一级；根页面再按回状态页；文本页退出文本页；弹窗时收起弹窗 |
| BACK 长按 | **任意显示态统一熄屏**（状态页 / 菜单 / 可滚动文本页 / 短信弹窗）；弹窗顺带收起，内容保留在帧缓冲 |
| BACK 长按期间的连发 | 全部忽略，否则刚熄掉的屏会被自己的 `kRepeat` 当成唤醒键立刻点亮 |
| UP / DOWN | 有选项时移动光标；纯信息页 / 文本页滚动正文（长按翻页） |
| OK | 进入子页 / 执行动作；弹窗中打开短信全文；长按连发便于亮度等值调节 |

菜单一屏 12 行，动态页每次渲染时按最新状态快照重建文本：

| 页面 | 内容 |
|------|------|
| 短信 | 最近 8 条预览，OK 进入可滚动详情 |
| SOC 信息 | 芯片 / 固件版本 / 内存 / PSRAM / CPU / 温度 |
| 通信模组 1 / 2 | 型号 / 固件 / 号码 / 运营商 / 注册 / IMEI / 信号 |
| WiFi | 当前连接与 IP |
| 邮件 / MQTT / 推送 | 各通道配置与连接状态 |
| RGB 灯 | 预设切换与亮度 |
| 关于 / 屏幕信息 | 设备信息与面板接线 |
| 工具箱 | 检查新版本 → 确认升级、重启、恢复出厂（连按 OK 5 次确认）、重置 Web 登录密码 |

屏幕设置（落盘 NVS `appcfg` / `disp`，也可经 `POST /api/disp` 修改）：

- `idleSec`：无操作自动退回实时状态页（默认 10s，0 = 关闭）
- `autoOffSec`：无操作自动熄屏，内容保留，任意键唤醒（默认 0 = 关闭）
- `bright`：背光亮度 0-100%

Web 端 `POST /api/key` 可注入按键事件，远程操作菜单（与物理键同一路径）。

## 时间与时区

设备系统时钟始终是 **UTC 纪元秒**，时区只影响 `localtime_r` / `strftime` 的本地显示与「今日 / 每日定点」计算。

- **默认东八区 UTC+08:00**（`cfg::kTzOffsetMinDefault = 480` 分钟）。可在 Web「系统设置 - 设备时区」选择，或调用 `GET|POST /api/timezone`；范围 UTC-12:00 ~ UTC+14:00（`-720` ~ `840` 分钟，东为正），支持半时区（`330` = +05:30）与 45 分钟时区（`345` = +05:45）。
- 配置存 NVS `appcfg/tz`，开机由 `TimeSync::loadTimezone()` 读取；**首次启动无该键（或值越界）时会把默认 `480` 显式写回 NVS**，因此设备、Web 设置页与配置导出从一开始就有确定的东八区。改时区即时生效、**不重启、不需要重新对时**。内部用 POSIX TZ 变量实现（符号与 UTC 偏移相反，东八区写 `UTC-8`），每次写系统时钟（`settimeofday` / SNTP 回调）后重新应用，避免 libc 的 TZ 状态被重置为 UTC；设备不做夏令时切换。
- Web 列表（日志 / 短信 / AT 历史 / 发件箱 / coredump）的时间戳一律按**设备时区**格式化（取 `/api/status` 的 `tzOffsetMin`），不跟随浏览器本地时区；旧固件无该字段时按 +480 兜底。
- 配置备份新增 `timezone` 节（`tzOffsetMin` / `tzText`）；导入旧备份（无该节）保持默认 / 现值。

**时间失真与统一口径**：NTP 未同步时 `TimeSync::stamp()` / `epochOrUptime()` 返回**开机秒数**（约定值小于 `cfg::kEpochFloorSec = 1600000000`，即 2020-09-13），前端显示成「开机 X」；同步后返回真实纪元秒。全项目共用这一个可信下限，不再散落 `1.6e9` / `100000` / `1000000` 等不同阈值；发件箱、测试推送、邮件 Date 头等处不再直接拿 `time(nullptr)` 的 1970 小值当记录时间戳，避免同一列表里开机秒数与无效纪元秒混排、或配置导出文件名拼出 `19700101`。崩溃时刻估算保留更宽松的 2020-01-01 下限（仅为兼容历史 coredump 记录）。未同步时推送模板 `{date}` 渲染为空串、`{timestamp}` 为开机秒数。

## MQTT 主题与命令

主题结构：`<topicPrefix>/<deviceId>/<sub>`，`topicPrefix` 默认 `smsf`，
`deviceId` 为芯片 eFuse MAC 的 12 位十六进制（与邮件里的设备 ID 一致）。

| 主题 | 方向 | 说明 |
|------|------|------|
| `.../status` | 发布 | `online` / `offline`（后者由 LWT 遗嘱发出） |
| `.../sms` | 发布 | 收到短信（含发件人、正文、时间戳、来源模组） |
| `.../call` | 发布 | 来电提醒（号码可能为「未知号码」） |
| `.../sim` | 发布 | SIM 卡插拔 |
| `.../boot` | 发布 | 设备启动完成（开机后首次连上发布一次） |
| `.../online` | 发布 | （重新）上线通知 |
| `.../log` | 发布 | 运行日志实时推送（默认关闭，限流 20 条/秒） |
| `.../command` | 订阅 | 远程命令（需开启「订阅命令主题」） |
| `.../command/response` | 发布 | 命令执行结果 |

命令为 JSON，可选带 `reqId`（结果中原样回传）：

```json
{"action":"send_sms","phone":"13800138000","content":"你好","modem":1}
{"action":"send_at","cmd":"AT+CSQ","modem":1,"timeout":5000}
{"action":"restart"}
{"action":"reinit","modem":1}
{"action":"info"}
{"action":"help"}
```

## 推送模板占位符

自定义通道（`PUSH_TYPE_CUSTOM`）的请求体模板、以及各通道的 URL / 标题等可配置字段
中用 `{字段名}` 引用，**未识别的占位符按原样保留**以便排查；已识别但当前取不到值的
字段渲染为**空串**（模组未就绪、SIM 不含本机号、模组不支持 `AT+CCID`、未配置备注等）。
Mailgun 通道的自定义请求体（`customBody`）渲染时取值会先做 HTML 转义，其余字段原样替换。

| 占位符 | 含义 | 数据来源 |
|---|---|---|
| `{from}` `{message}` | 发件人号码 / 消息正文 | 事件本身 |
| `{timestamp}` `{date}` | 事件时间戳（已同步为纪元秒，未同步为开机秒数）/ 本地时间字符串 `YYYY-MM-DD HH:mm:ss`（未同步时空串） | `PushJob.timestamp` / `localDateStr()` |
| `{deviceId}` `{deviceName}` `{uptime}` | 设备 ID / 设备名 / 运行时长 | 芯片 eFuse MAC、`millis()` |
| `{simSlot}` `{modemIndex}` | `SIM1` / `SIM2`、模组序号 `1` / `2` | 事件来源模组（测试推送为空） |
| `{message_type}`（旧别名 `{trigger_type}`） | 消息类型 | `MsgTypeInfo` |
| `{channelName}` `{channelType}` | 当前通道名称 / 类型中文名 | 通道配置 |
| `{carrier}` | 运营商（优先中文短名，如「中国移动」） | `AT+COPS?` → `ModemSnapshot.oper` |
| `{to}` | 本机号码（SIM 号） | `AT+CNUM` → `ModemSnapshot.num` |
| `{iccid}` | SIM 卡 ICCID | 就绪后一次性 `AT+CCID` → `ModemSnapshot.iccid` |
| `{signal}` | 信号强度，如 `-73 dBm (CSQ 20)` | `AT+CSQ`（15s 轮询），未知时空串 |
| `{remark}` | 设备备注 | NVS `appcfg/remark`，在「推送通道」页「设备备注」卡片配置（最长 64 字节） |

模组专属字段按事件的来源模组取值：测试推送无来源模组，这四项与 `simSlot` /
`modemIndex` 均为空串；所有取值都是推送时刻的快照，不会为了模板额外发 AT 命令。

## Web API 参考

### 约定与认证

**统一约定：请求与响应均为 JSON**——POST 请求体为 JSON
（`Content-Type: application/json`，非 JSON 的 POST 不匹配处理器）；
所有响应（含错误）均为 JSON，错误形如
`{"ok":false,"error":"描述"}` 并带合适的状态码（400 参数错 /
401 未认证 / 403 禁止 / 413 请求体过大 / 429 登录限流 / 500 服务端失败），未匹配路径返回
`{"ok":false,"error":"接口不存在"}`（404）。

页面与静态资源（`/`、`/login`、`/assets/*`、`/pages/*`）公开（不含敏感数据）；
全部 API 统一在 `/api/` 前缀下，除登录挑战 / 登录 / 登出 / 会话探测外均需认证。支持两种认证方式：

1. **会话 Cookie（浏览器）**：`POST /api/login` 成功后服务端下发 `sid`
   Cookie（`HttpOnly; SameSite=Strict; Path=/`，有效期 7 天），浏览器自动携带；
   `HttpOnly` 使 JS 读不到、`SameSite=Strict` 使 Cookie 不随跨站请求发送，
   第三方网页无法借登录态发起 CSRF 调用。服务端会话同样按 7 天惰性过期，
   表满时淘汰最旧（不再出现「第 9 次登录返回 500」）。
2. **Bearer 访问令牌（脚本/第三方客户端）**：登录响应体返回 `token`（随机串，内存态、7 天 TTL），
   之后请求带 `Authorization: Bearer <token>` 头即可，无需 Cookie。另有
   **持久化 API 令牌**（默认关闭，在系统设置页启用）：NVS `apitoken` 只存
   `verifier = SHA256(raw)`，**不存明文**；推荐用 `GET /api/token/challenge` +
   `POST /api/token/exchange` 换得**短时效票据**后再调业务接口（纯 HTTP 下不暴露 raw 令牌）；
   raw Bearer 作为兼容分支默认保留，可编译期 `-DAPI_TOKEN_REQUIRE_EXCHANGE=1` 关闭。

会话与临时令牌均为服务端随机生成，仅存内存（上限 8 个），**重启后全部失效**。
`POST /api/logout` 作废当前会话与令牌。未认证访问返回 401
`{"ok":false,"error":"未授权"}`。

#### 登录：SHA-256 挑战应答（v2，Web 默认）

管理账号在 NVS `appcfg/auth` 里只存
`{"ver":2,"user":"..","salt":"<32hex>","verifier":"<64hex>"}`，
`verifier = SHA256(salt + ":" + pass)`，**不存明文密码**；旧版 v1 明文记录
（`{"ver":1,"user","pass"}`）开机自动迁移，密码不变。

1. `GET /api/login/challenge`（免认证，`Cache-Control: no-store`）返回
   `{"ok":true,"v":2,"alg":"sha256-verifier","salt":"..","nonce":"<32hex>","ts":<设备millis>,"ttlMs":30000}`。
   `ts` 是设备 `millis()` 回显值，不依赖 NTP / 浏览器时钟。
2. 前端算 `verifier = SHA256(salt + ":" + pass)`、
   `proof = SHA256(verifier + ":" + ts + ":" + nonce)`，均为 64 位小写 hex；
   `POST /api/login` 提交
   `{"v":2,"user":"..","ts":<ts>,"nonce":"..","proof":"<64hex>"}`，**请求体不含密码**。
   服务端用 NVS 中的 verifier 重算 proof 做恒定时间比较；nonce 30 秒有效、
   **无论成败一次性消费**，重放 / 超时 / 篡改 `ts` 均按登录失败计。
3. 浏览器能力分级：`crypto.subtle` 只在安全上下文（HTTPS / localhost）暴露，
   设备是 `http://<局域网IP>:80`，用 IP 访问时必然不可用；因此登录页内置
   `assets/sha256.js`（纯 JS SHA-256，自带 UTF-8 编码）作为默认实现，
   原生 Web Crypto 可用时才走 `crypto.subtle.digest`。两者都不可用（无 JS 的脚本）
   时自动降级到 v1，并打印控制台告警。
4. **v1 兼容分支**：`{"user":"..","pass":".."}` 明文登录默认保留（脚本 / 旧前端），
   服务端用 `SHA256(salt + pass)` 重算 verifier 校验；编译期
   `-DLOGIN_REQUIRE_PROOF=1` 可彻底禁用，禁用后 v1 一律 401。
5. **限流**：全局（每分钟 20 次「失败 + 挑战」）、来源（IP + 用户名，5 分钟内
   失败 5 次锁 60 秒、再犯翻倍封顶 15 分钟）、挑战端点（每 IP 每分钟 30 次）三层；
   命中返回 `429 {"ok":false,"error":"尝试过于频繁，请稍后再试","retryAfter":<秒>}`
   + `Retry-After` 头，前端按秒倒计时。计数只用 `millis()` 且仅存内存、重启清零，
   不会在 HTTP handler 里 `delay()`。

> **安全边界**：设备是纯 HTTP，`salt/nonce/ts/proof` 仍随包可见，proof 可离线字典爆破；
> 本机制的实际收益是密码明文不再进入网络 / NVS / 日志 / 备份，以及防重放 + 限流，
> **不能替代 HTTPS**。`POST /api/auth` 的新密码 / `passOld` 与保留的 v1 分支仍是明文。
> **不支持跨该版本回滚**：NVS 升级为 v2 后刷回旧固件，旧固件读不到 `pass`
> 会回落出厂 `admin/admin123` 并覆写账号；OTA 只升不降，回滚前先导出备份。

### 认证与令牌

| 方法与路径 | 请求体 | 响应 |
|-----------|--------|------|
| `GET /api/login/challenge` | 无 | `{"ok":true,"v":2,"alg":"sha256-verifier","salt":"..","nonce":"..","ts":<millis>,"ttlMs":30000}`；免认证，受每 IP / 全局限流，超限 429 |
| `POST /api/login` | v2 `{"v":2,"user":"..","ts":<ts>,"nonce":"..","proof":"<64hex>"}`；兼容 v1 `{"user":"admin","pass":"admin123"}` | 200 + `Set-Cookie: sid=...`，`{"ok":true,"user":"admin","token":"..."}`；账密 / proof 错或 nonce 失效 401、格式错 400、限流 429、会话写入失败 500 |
| `POST /api/logout` | 无 | `{"ok":true}` + 清除 Cookie，作废当前会话/令牌 |
| `GET /api/session` | 无 | 已登录 `{"ok":true,"user":"admin"}`，否则 401 |
| `GET /api/auth` | 无 | 当前用户名 `{"ok":true,"user":"admin"}`（密码不回传） |
| `POST /api/auth` | `{"user":"新用户","pass":"新密码","passOld":"当前密码"}` | `{"ok":true}`；当前密码不符 403、长度超限 400、NVS 写入失败 500；成功后全部会话/令牌作废。新密码 / 旧密码字段仍是**明文 HTTP**（与 v1 兼容同属已知残留） |
| `GET /api/token/challenge` | 无（公开） | `{"ok":true,"v":2,"alg":"sha256-verifier","nonce":"..","ts":<millis>,"ttlMs":30000}`；未启用 403、限流 429 |
| `POST /api/token/exchange` | `{"v":2,"nonce":"..","ts":<ts>,"proof":"<64hex>"}` | `{"ok":true,"ticket":".."}`；proof 错 / nonce 失效 401、限流 429、未启用 403。ticket 为短时效票据（1 小时），后续 `Bearer <ticket>` 调业务接口 |
| `GET /api/token` | 无 | `{"ok":true,"enabled":bool,"token":"••••"}`（**不再回 raw**，真实值仅 `reset` 响应一次性给） |
| `POST /api/token` | `{"enabled":bool}` | 开关持久化 API 令牌（启用且无 verifier 时自动生成），返回 `{"ok":true,"enabled":bool}` |
| `POST /api/token/reset` | 无（普通 POST） | 重新生成令牌并写 NVS（只存 verifier），旧令牌立即失效；未启用 403；响应一次性回 raw |

### 系统状态与维护

| 方法与路径 | 请求体 | 说明 |
|-----------|--------|------|
| `GET /` | - | 控制台页面（公开） |
| `GET /login` | - | 独立登录页（公开，已登录自动跳回） |
| `GET /api/status` | - | 状态 JSON：`ssid/ip/rssi/uptime/heap/heapSize/psram/psramSize/romSize/color/rgb[]/disp/dispBright/rgbBright/connected/mode/time/timeSynced/timeSrc/tzOffsetMin/tzText/otaCompiled/otaAvailable/mqtt{compiled,enabled,configured,connected,error}/cpuAvail/cpu/tempAvail/temp/chip/hwVer/swVer/mac/todaySms{recv,sent}/sched{enabled,mode,daily,intervalH,weekdays}/wdt{enabled,timeoutMs,tasks,reset,resetText,bootWasWdt,wdtCount,lastWdtTask,lastWdtUptimeSec,lastWdtVersion}` |
| `GET /api/timezone` | - | `{"ok":true,"tzOffsetMin":480,"tzText":"UTC+08:00","tzOffsetMinMin":-720,"tzOffsetMinMax":840,"time":"..","timeSynced":bool,"timeSrc":"ntp"}` |
| `POST /api/timezone` | `{"tzOffsetMin":480}` | 修改设备时区（UTC 偏移分钟，东为正），立即重新应用 TZ 并落盘 NVS，`timezone` 节随之可导出；不重启、无需重新对时。越界 400、NVS 写失败 500，成功回显同 GET |
| `POST /api/system/restart` | 无 | `{"ok":true,"msg":"设备即将重启"}`，2 秒后重启 |
| `POST /api/system/reset` | 无 | 清空 NVS `appcfg` 全表 + 格式化 `userdata`，2 秒后重启（崩溃记录保留） |
| `POST /api/sched` | `{enabled,mode,dailyMin,intervalH,weekdays}` 或 `{"sched":{...}}` | 定时重启配置，即时生效不重启。`mode`：`0` 每日定点（`dailyMin` 为当日分钟数）/ `1` 固定间隔（`intervalH` 小时，1-720）；`weekdays` 位掩码（bit0=周日）；无效 400 |
| `POST /api/wdt/selftest` | `{"task":"loopTask","timeoutMs":10000}` | **会触发整机重启**：让指定受监控任务停止喂狗，验证 WDT 自动重启与原因记录。`task` 取 `wdt.tasks` 中的名字；`timeoutMs` 可选（0 / 缺省 = 沿用 60s，允许 3000~60000 临时缩短）；任务名无效或越界 400 |

### 屏幕 / RGB / 按键

| 方法与路径 | 请求体 | 说明 |
|-----------|--------|------|
| `GET /api/disp` | - | `{"ok":true,"on":bool,"bright":0-100,"autoOffSec":n,"idleSec":n}` |
| `POST /api/disp` | `{"on":bool?,"bright":0-100?,"autoOffSec":n?,"idleSec":n?}` | 字段均可选，越界钳制；屏幕设置落盘 NVS，返回最新状态 |
| `GET /api/screen` | - | **image/jpeg** 帧缓冲实时预览（`no-store`）；无帧缓冲 503 |
| `POST /api/key` | `{"key":"up"\|"down"\|"ok"\|"back","event":"click"\|"long"\|"repeat"?}` | 远程按键注入（与物理键同路径），返回 `{"ok":true}` |
| `GET /api/led` | - | 切换下一个预设颜色，返回最新 RGB 状态 |
| `GET /api/rgb` | - | `{"ok":true,"color":名,"rgb":[r,g,b],"index":n,"bright":0-100}` |
| `POST /api/rgb` | `{"c":索引?,"r":n?,"g":n?,"b":n?,"bright":0-100?}` | 三者可组合（`c` 越界 400，其余钳制），返回最新状态 |

### 模组调试（AT 命令）

| 方法与路径 | 说明 |
|-----------|------|
| `GET /api/at/send?port=1\|2&baud=115200&eol=0..3&cmd=...&t=1000` | 发送 AT 命令，返回该条记录 JSON；超时 `t` 钳制到 100-5000ms |
| `GET /api/at/hist?after=-1&n=20` | 增量拉取历史（after 之后，n 条） |
| `GET /api/at/clear` | 清空历史，返回 `{"ok":true}` |
| `GET /api/at/stats` | AT 环形缓冲统计（与 `/api/logs/stats` 对称） |
| `GET /api/at/reset?port=1\|2` | 手动复位模组串口，返回 `{"ok":true}`；端口非法 400 |
| `GET /api/uart/stats` | 两个模组串口统计 JSON |
| `GET /api/at/baud` | 默认波特率 `{"default":115200}` |

`eol` 取值：`0`=CRLF，`1`=CR，`2`=LF，`3`=无。
AT 命令经 `SmsService::sendAt` 下发，与短信收发共用端口锁，发完自动还原
CMGF/CNMI，避免控制台命令误改短信配置。

### 设备日志

运行日志存于 PSRAM 的定长记录环（`log/LogStore`，有 PSRAM 时 2048 条 ≈467KB，
无 PSRAM 退化为 256 条；写满 FIFO 淘汰最旧）。时间戳与短信记录一致：
时钟已同步为纪元秒，未同步为开机秒数。级别：`0` 调试 / `1` 信息 / `2` 警告 /
`3` 错误。

| 方法与路径 | 说明 |
|-----------|------|
| `GET /api/logs?page=1&pageSize=20&level=2&q=关键字` | 分页查询：`page` 从 1 起，`pageSize` 上限 100；`level` 为最低级别过滤（缺省不过滤，传 `2` 即仅看警告以上）；`q` 对 tag/msg 大小写不敏感匹配；结果按 ID 倒序（最新在前）。返回 `{"ok":true,"total":n,"page":p,"pageSize":ps,"logs":[{id,ts,level,tag,msg}...]}` |
| `GET /api/logs?after=<id>&limit=100&level=&q=` | 增量拉取（轮询追加用，避免整页重绘闪烁）：只返回 ID 大于 `after` 的匹配记录，按 ID **升序**（旧→新），最多 `limit` 条（上限 100）；返回 `{"ok":true,"last":服务端最新ID,"total":n,"count":n,"logs":[...]}`；`count < limit` 时表示后面已无匹配记录 |
| `GET /api/logs/stats` | 存储统计：`{"ok":true,"total":n,"max":n,"psram":bool,"bytes":n,"psramTotal":n,"psramFree":n}`；`psram=false` 表示缓冲未落在 PSRAM（已退化为内部堆，功能不受影响），由 `psramTotal` 区分原因（0 = 设备无 PSRAM） |
| `DELETE /api/logs` | 清空全部日志（逻辑清空，ID 计数保留）；清空动作本身补记一条 |

### WiFi 配网

| 方法与路径 | 请求体（JSON） | 说明 |
|-----------|---------------|------|
| `GET /api/wifi/status` | - | 模式（connecting/sta/ap）、连接、AP 信息、已保存数量 |
| `GET /api/wifi/scan` | - | 扫描中返回 `{"busy":1}`；完成返回 `{"busy":0,"nets":[...]}` |
| `GET /api/wifi/list` | - | 已保存配置（密码不回传，仅 `hasPass` 标记） |
| `POST /api/wifi/save` | `{"ssid":"..","pass":"..","connect":true}` | 保存（同 SSID 覆盖）并按需立即试连；满 5 组返回 500 |
| `POST /api/wifi/delete` | `{"index":n}` | 按序号删除；越界返回 400 |
| `POST /api/wifi/move` | `{"from":n,"to":m}` | 调整启动顺序；越界返回 400 |
| `POST /api/wifi/connect` | `{"ssid":"..","pass":".."}` | 仅试连，不保存 |
| `GET /api/wifi/appass` | - | AP 热点密码：`{"secure":bool,"pass":".."}` |
| `POST /api/wifi/appass` | `{"pass":".."}` | 修改 AP 热点密码（空串=开放，非空须 8-63 字符），返回 `{"ok":true,"secure":bool}` |

### 短信

| 方法与路径 | 请求体（JSON） | 说明 |
|-----------|---------------|------|
| `GET /api/sms?page=1&pageSize=20&q=&port=` | - | 列表：`{"ok":true,"total":n,"page":p,"pageSize":ps,"messages":[{id,dir:"in"\|"out",phone,content,ts,modem}]}`；`pageSize` 上限 100 |
| `DELETE /api/sms?id=n` / `?all=1` | - | 删除单条（不存在 404）或清空 |
| `GET /api/sms/stats` | - | `{"ok":true,"total":n,"max":n,"used":bytes,"fsTotal":bytes}` |
| `GET /api/sms/latest` | - | 最新一条摘要（仪表盘用，与 SSE `sms` 事件同格式）；无记录返回 `{}` |
| `POST /api/sms/query` | `{"port":1}`（不传或 0 = 全部） | 立即触发一次主动查询，异步执行 |
| `GET /api/sms/query` | - | 主动查询状态：`storeMode/deleteAfterRead/lastMs/safetySec` 与 `ports[]`（含 `pending/running/lastFound/lastHandled/lastDeleted` 及清空状态）。存储模式链路诊断字段：`indexes`（保留待重试的 +CMTI 索引数）、`cmgfChecked`/`cmgfOk`（PDU 模式确认结果）、`failStreak`、`lastCmgr`（最近一次 `AT+CMGR` 原始响应片段）、`lastError`——排查「+CMTI 可见但短信没入库」时先查这里 |
| `GET /api/sms/poll` | - | 定时主动查询配置：`{"ok":true,"enabled":bool,"mode":n,"intervalH":n,"hour":n,"minute":n,"safetySec":n}`。`mode`=`1` 固定间隔（`intervalH` 小时）/ `0` 每日定点（`hour`:`minute`）；`safetySec` 是 +CMTI 丢失时的兜底轮询周期（30-3600 秒） |
| `POST /api/sms/poll` | `{"enabled":bool,"mode":n,"intervalH":n,"hour":n,"minute":n,"safetySec":n}` | 保存定时主动查询配置并落盘，返回同 GET 的最新配置；保存失败 500。到点由 `tick()` 置待查标记，查询任务异步执行，不在主循环做阻塞 AT |
| `GET /api/sms/store` | - | 短信接收模式：`{"ok":true,"store":bool}`。`true`=存储模式（`CNMI=2,1`，`+CMTI` + 主动 `AT+CMGR`），`false`=直投模式（`CNMI=2,2`，`+CMT`） |
| `POST /api/sms/store` | `{"store":bool}` | 切换短信接收模式并落盘，查询任务会给已就绪模组热下发新的 `CNMI`；字段非 bool 返回 400，保存失败 500，成功回显 `{"ok":true,"store":bool}` |
| `GET /api/sms/ports` | - | 通信模组列表：`{"ok":true,"modems":[{"port":1,"enabled":true,"alias":"..","csq":n,"oper":"..","num":"..","imei":"..","iccid":"..",...}]}`；ICCID 需模组就绪且完成 `AT+CCID` 采集 |
| `POST /api/sms/ports` | `{"port":1,"enabled":bool?,"alias":".."?}` | 启用/禁用与设别名（可只传其一），返回最新列表 |
| `POST /api/sms/reinit` | `{"port":1}` | 热重初始化通信模组（仅启用态有效） |
| `POST /api/sms/clear` | `{"port":1}` | 清空该通信模组 SIM 上的全部短信（含未读、未转发，不可恢复）；异步执行，返回 `{"ok":true,"started":true}`，模组未就绪或已有清空任务返回 400 |
| `GET /api/sms/clear` | - | 清空状态：与 `GET /api/sms/query` 同结构，各端口含 `clearBusy` / `clearLastOk` / `clearLastDeleted` / `clearLastMs` / `clearLastErr` |
| `POST /api/sms/send` | `{"port":1,"phone":"..","content":".."}` | **先登记持久化发件箱**（`kind=1`，单通道 `sms:<port>`）再启动发送，立即返回 `{"ok":true,"started":true}`；发件箱不可用时回退原有直投队列。`port` 越界返回 400，在途返回 409，队列满返回 503 |
| `GET /api/sms/send` | - | 轮询发送结果：`{"busy":bool,"ok":bool,"id":n,"stored":bool,"warn":"..","err":".."}`（契约不变；结果由发件箱的发送回调回写，`busy` 只表示本次 CMGS 在途，不绑定发件箱退避重试）。Web 两个发送弹窗均为「提交即返回、不轮询」，此接口保留兼容 |
| `GET /api/sms/admin` | - | 管理员远程控制：`{"ok":true,"enabled":bool,"phones":[".."]}` |
| `POST /api/sms/admin` | `{"enabled":bool?,"phones":[".."]}` | 保存管理员配置（上限 5 个号码；空 `phones` 视为关闭），校验失败 400 |
| `POST /api/sms/ping` | `{"port":1,"host":"8.8.8.8"?}` | 经通信模组做连通性测试（启动） |
| `GET /api/sms/ping` | - | 轮询测试结果：`{"busy":bool,"ok":bool,"report":".."}` |

### 持久化发件箱（不丢消息转发队列）

发件箱有两类记录（`records[].kind`）：

- **`kind=0` 收到的短信**：落盘事件 + 「当时启用且有效的通道快照」（`email` / `mqtt` /
  `push:<idx>`），主循环逐通道投递；断电 / 重启后扫描目录恢复未送达通道并退避续投，
  转发队列不再随重启清空、也不再因队列满丢最旧。语义为**至少一次**：若在「发送
  成功」与「状态落盘」之间断电，重启后可能重发一条。
- **`kind=1` 主动发出的短信**（Web「短信」页与「发件箱」页的发送按钮都走这里）：
  只生成一个 `sms:<port>` 专用通道，**不生成 email/mqtt/push 通道**——否则会经
  「SMS 短信备份」push 通道再发一条短信形成回环。实际阻塞式 CMGS 仍由
  `SmsService` 的常驻 `sms_send` worker 执行，发件箱只做记账与异步结果回写。
  为避免重复扣费，**发送结果未知时不自动重发**：若发送中重启或 `done` 回调超时，
  通道记为 `submitted`（结果未知），由用户在发件箱页手动点「重试」决定是否补发。
  仅「入队发送队列失败」会自动退避重试（此时模组尚未发出，安全）。

非短信事件（启动 / SIM / 来电 / 在线 / 测试邮件）以及 MQTT `send_sms`、管理员
`SMS:` 命令、SMS 备份推送仍走各自原有队列，不进入发件箱。
Web「发件箱」页可查看每条记录在各通道的投递回执、重试失败或结果未知的通道。
「短信」页与「发件箱」页的发送弹窗为同款界面（通信模组/号码/内容、「加入发件箱」按钮、
结果未知提示）：提交后即关闭并提示，实际投递结果统一到发件箱页查看。

| 方法与路径 | 请求体 / 参数 | 说明 |
|-----------|---------------|------|
| `GET /api/outbox` | `?page=1&pageSize=20&id=n`（均可省略） | `{"ok":true,"enabled":bool,"total":n,"pending":n,"failed":n,"done":n,"records":[...]}`；传 `id` 时只返回该条并附完整 `text`。每条记录含 `kind`(0 收到 / 1 发出) / `phone` / `port` / `sender` / `preview` / `ts` / `modem` / `state`；`channels[]` 含 `k`(键，如 `email` / `mqtt` / `push:0` / `sms:1`) / `n`(名称) / `s`(状态) / `a`(尝试次数) / `e`(最近错误) / `ms`(耗时)，状态为 `pending` / `sending` / `ok` / `submitted` / `failed` / `skipped` / `dead` |
| `POST /api/outbox/retry` | `{"id":n}`（省略或 0 = 全部） | 把 `failed` / `dead`、以及 `sms:` 通道的 `submitted`（结果未知）重新排队并清零尝试次数。短信类重试前请确认对方未收到，避免重复发送 |
| `POST /api/outbox/clear` | - | 清空已完成回执，未送达记录一律保留 |
| `POST /api/outbox/delete` | `{"id":n}` | 删除单条（该记录未送达通道不再补发） |

### 黑名单

| 方法与路径 | 请求体（JSON） | 说明 |
|-----------|---------------|------|
| `GET /api/blacklist` | - | `{"ok":true,"enabled":bool,"action":n,"count":n,"list":[{"pattern":"..","mode":n,"scope":n,"note":"..","enabled":bool}]}` |
| `POST /api/blacklist` | `{"enabled":bool,"action":0\|1,"list":[{pattern,mode,scope,note,enabled}...]}` | 整体保存，逐条校验（mode 0-2、scope 非 0、pattern 非空）+ 去重，落盘后返回最新配置 |
| `POST /api/blacklist/enabled` | `{"enabled":bool}` | 只翻转总开关，不动规则与动作 |
| `GET /api/blacklist/test?num=...` | - | 测试某号码是否命中（忽略总开关）：`{"ok":true,"matched":bool,"scope":n,"note":".."}` |

`mode`：`0` 精确 / `1` 前缀 / `2` 通配符（`*` `?`）；
`scope`：位掩码 `1`=短信、`2`=来电；`action`：`0` 仅记录不推送 / `1` 静默丢弃。

### 邮件通知（SMTP）

| 方法与路径 | 请求体（JSON） | 说明 |
|-----------|---------------|------|
| `GET /api/email` | - | 邮件配置：`{"ok":true,"enabled":bool,"configured":bool,"server":"..","port":465,"user":"..","sendTo":"..","hasPass":bool,"notifyTypes":n,"schedMode":0\|1,"schedHour":n,"schedMinute":n,"intervalH":n}`；授权码不回显 |
| `POST /api/email` | `{"server":"..","port":465,"user":"..","pass":"..","sendTo":"..","enabled":true,"notifyTypes":31,"schedMode":1,"schedHour":9,"schedMinute":0,"intervalH":24}` | 保存配置并落盘 NVS；`pass` 为空串表示不修改已保存授权码；保存成功返回最新配置 |
| `POST /api/email/enabled` | `{"enabled":true\|false}` | 只翻转总开关，不动其余配置；停用时丢弃待发队列，重新开启后立即恢复 |
| `POST /api/email/test` | 无 | 入队一封测试邮件并**立即返回** `{"ok":true,"queued":true}`；总开关未开、配置不完整或 WiFi 未连接返回 400，入队失败 500 |

**通知类型位掩码** `notifyTypes`：`bit0` 设备启动 / `bit1` SIM 状态 /
`bit2` 收到短信 / `bit3` 定时在线 / `bit4` 来电提醒（出厂五类全开，缺省值 31）。

**总开关**（`enabled`）：false 时一切邮件都不发送——短信 / SIM / 启动 /
定时在线 / 来电通知全部静默跳过，待发队列丢弃；已保存的 SMTP 参数与通知类型保留，
重新开启后立即恢复。与「配置完整」（`configured`，四项参数齐全）相互独立：
实际能否发邮件由 `enabled && configured` 共同决定。NVS 无该字段时默认开启。

邮件模板：所有通知邮件共用一套卡片式 HTML（`EmailService::buildHtml`）——
灰底 + 白色圆角卡片 + 渐变标题栏 + 状态徽标 + 公共信息块（系统时间 / 各通信模组
本机号码或设备 IP / 运行时长）+ 底部标识；短信邮件额外带验证码高亮块
（识别「验证码 / OTP / code」等关键词附近的 4~6 位数字串，标题形如
「验证码: 123456 来自 示例」）。HTML 超长（>6000 字符）时自动降级为纯文本。

**不可在 HTTP 任务里同步发邮件**：`async_tcp` 任务栈不足以支撑 TLS 握手，会栈溢出重启。
`/api/email/test` 只入队，实际构建与 SMTP 发送在设备主循环（loop 任务）完成，
失败退避重试 3 次（2s / 4s），认证失败（账号或授权码错）直接丢弃并提示。

### MQTT

| 方法与路径 | 请求体（JSON） | 说明 |
|-----------|---------------|------|
| `GET /api/mqtt` | - | `{"ok":true,"enabled":bool,"configured":bool,"server":"..","port":1883,"useTls":bool,"user":"..","hasPass":bool,"clientId":"..","topicPrefix":"..","keepAlive":n,"subscribeEnabled":bool,"notifyTypes":n}`；密码不回显 |
| `POST /api/mqtt` | `{"server":"..","port":1883,"useTls":bool,"user":"..","pass":"..","clientId":"..","topicPrefix":"..","keepAlive":n,"subscribeEnabled":bool,"notifyTypes":n,"enabled":bool}` | 保存并落盘，立即重建客户端生效（无需重启）；`pass` 空串表示不改密码 |
| `POST /api/mqtt/enabled` | `{"enabled":bool}` | 只翻转总开关（停用时销毁客户端停止连接，配置保留） |
| `POST /api/mqtt/test` | 无 | 发布一条测试事件（经主循环 tick 发出），返回 `{"ok":true}` |

`notifyTypes` 位掩码：`bit0` 启动 / `bit1` SIM / `bit2` 短信 / `bit3` 上线 /
`bit4` 来电 / `bit16` 运行日志（MQTT 专属，默认关闭）；除日志外出厂全开。
主题与命令见 [MQTT 主题与命令](#mqtt-主题与命令)。

### 推送通道

| 方法与路径 | 请求体（JSON） | 说明 |
|-----------|---------------|------|
| `GET /api/push/types` | - | 13 种通道类型的字段 schema（前端动态渲染表单用）：`{"ok":true,"types":[...]}` |
| `GET /api/push` | - | `{"ok":true,"enabled":bool,"strategy":n,"remark":"..","configured":bool,"channels":[{"type":n,"name":"..","url":"..","hasKey1":bool,"key2":"..",...,"customBody":"..","retryOnFail":bool,"enabled":bool,"valid":bool}]}`；密钥字段只回显 `hasKey1` |
| `POST /api/push` | `{"enabled":bool,"strategy":n,"remark":"..","channels":[{...}]}` | 整体保存（最多 10 条），`key1` 空串表示保持已保存值；`remark` 为设备备注（可选，最长 64 字节，缺省时保持原值），供模板 `{remark}` 使用；保存后立即生效 |
| `POST /api/push/enabled` | `{"enabled":bool}` | 只翻转总开关，返回完整配置 |
| `POST /api/push/test` | 无 | 入队一条测试推送并立即返回 `{"ok":true,"queued":true}`；未启用 / 无 WiFi / 无有效通道返回 400 |

`strategy`：`0` 广播（同时发所有已启用通道）/ `1` 故障转移（按序尝试，首个成功即停）。
通道 `type`：`1` POST-JSON / `2` Bark / `3` GET / `4` 钉钉 / `5` PushPlus /
`6` Server酱 / `7` 自定义模板 / `8` 飞书 / `9` Gotify / `10` Telegram /
`11` 企业微信 / `12` SMS 短信备份 / `13` Mailgun。
`modemSlot` 指定 SMS 备份通道从哪个模组发出（<0 = 自动挑第一个就绪模组）。

### OTA 升级

| 方法与路径 | 请求体 | 说明 |
|-----------|--------|------|
| `GET /api/ota/status` | - | `{"state":n,"progress":0-100,"message":"..","currentVersion":"..","latestVersion":"..","available":bool}`；`state`：`0` 空闲 / `1` 检查中 / `2` 下载中 / `3` 写入中 / `4` 成功待重启 / `5` 失败 / `6` 烧录 LittleFS |
| `GET /api/ota/version` | - | 触发后台版本检查，立即返回 `{"ok":true,"version":"..","latestVersion":"..","available":bool}` |
| `POST /api/ota/start` | `{"tag":"<完整版本标识>"}` | 启动在线升级（后台任务下载写入），返回 `{"ok":true,"started":true}`，进度经 status 轮询 |
| `POST /api/ota/upload` | multipart `file` | 本地固件上传，分片写入 OTA 分区，成功后自动重启 |
| `POST /api/ota/upload-fs` | multipart `file` | 本地 LittleFS（Web UI）镜像上传 |

`ENABLE_FIRMWARE_UPGRADE=0` 时只有 `/api/ota/status` 存在，返回
`available=false` 与 `message="OTA 升级未编译进固件"`，其余四个接口不注册。

产物文件名：`ota-<tag>-<platform>.bin` / `littlefs-<tag>-<platform>.bin`，
下载地址由编译期注入的 `OTA_RELEASES_BASE_URL` 拼接。**Release tag 即完整版本标识**
（`<VERSION>-<提交时间>-<短 SHA>`），设备自报版本、下载地址与比较用的都是同一个值。

### 配置导入导出

| 方法与路径 | 请求体 | 说明 |
|-----------|--------|------|
| `GET /api/config/export` | - | 下载 `sms_forwarder_config_YYYYMMDD.json`（`application/octet-stream`），含 `wifi/auth/email/sms/mqtt/push/blacklist/disp/sched/timezone`；`auth` 节已脱敏（`ver/user/salt/verifier`，**不含明文管理密码**，仍可凭备份恢复登录），但 `wifi`/`email`/`mqtt` 等节仍含明文密码，备份需妥善保管；导出文件名按设备时区生成，时钟不可信时用默认名 |
| `POST /api/config/import` | 原始 JSON（`application/json`，>64KB 返回 413） | 导入并落盘，返回 `{"ok":true,"msg":".."}` 后 2 秒重启；导入即作废全部会话 |

### 崩溃转储

| 方法与路径 | 请求体 | 说明 |
|-----------|--------|------|
| `GET /api/tools/coredump/info` | - | `{"hasCoredump":bool,"size":n,"crashTime":epoch,"crashVersion":"..","elfUrl":".."}`；无数据只返回 `hasCoredump:false` |
| `GET /api/tools/coredump/export` | - | 分块流式下载 `sms-forwarder-coredump-<ts>.bin`；无数据 404 |
| `POST /api/tools/coredump/clear` | 无 | 整分区擦除（0xFE0000 起 128KB）；NVS 里的崩溃时间与版本记录保留 |

### 任务看门狗

ESP-IDF 开机已初始化 TWDT（`CONFIG_ESP_TASK_WDT_PANIC`，panic = 打印 + 重启），
但默认 5 秒超时短于一次合法的 SMTP 会话、发件箱单次投递或 CMGS 握手，
因此 `App::begin()` 用 `esp_task_wdt_reconfigure()` 放宽到
`cfg::kWdtTimeoutMs`（60 秒，`src/app/Config.h`），并显式订阅 4 个常驻任务：
`loopTask`（主循环）、`uart-pump`（串口抽数）、`sms_send`（发送）、`sms_query`（主动查询）。
`async_tcp` 由 AsyncTCP 库自行订阅并喂狗，其余一次性任务不在监控范围。

喂狗点覆盖所有长阻塞：`App::tick()` 每轮、`readResponse()` 的 AT 等待循环、
`sendSmsPdu()` 的 `>` 与 `+CMGS/OK` 等待循环、`lockPort()` 的等锁循环，
以及 `sms_send` 的空闲队列等待（已从 `portMAX_DELAY` 改为 1 秒超时）。
`TaskWatchdog::feed()` 内部按 1 秒节流，未订阅 TWDT 的任务调用它是 no-op。

超时后由 IDF panic 自动重启；超时 ISR 覆写 `esp_task_wdt_isr_user_handler()`
把各任务心跳写进 RTC_NOINIT 内存，下次开机结合 `esp_reset_reason()` 写一条
`wdt` 标签运行日志，并持久化到 NVS `wdt` 命名空间（断电也不丢）。
`/api/status` 的 `wdt` 字段（SSE `status` 事件同源）：

| 字段 | 含义 |
|------|------|
| `enabled` / `timeoutMs` / `tasks` | TWDT 是否启用、当前超时（ms）、已登记的常驻任务名 |
| `reset` / `resetText` / `bootWasWdt` | 本次开机复位原因枚举名、中文说明、是否由看门狗触发 |
| `wdtCount` | 累计看门狗重启次数（NVS 持久） |
| `lastWdtTask` / `lastWdtUptimeSec` / `lastWdtVersion` | 最近一次 WDT 的疑似卡死任务（空 = 心跳同时停止，未能定位）、触发时开机秒数、固件版本 |

**设备验收**：Web「系统工具」→「任务看门狗」→ 选任务与超时（10 秒）→ 点
「触发卡死自检」（等价 `POST /api/wdt/selftest {"task":"loopTask","timeoutMs":10000}`）。
设备约 10 秒后自动重启；重启后日志出现
`上次由看门狗重启：任务看门狗超时；卡死任务 loopTask…`，
`/api/status` 的 `wdt.reset=TASK_WDT`、`wdt.bootWasWdt=true` 且 `wdtCount` 递增，
Web 卡片显示该次记录。自检会中断服务，**勿在无人值守时误触**。

> 取舍：60 秒是为「最长合法阻塞」留的余量，若新增同步阻塞超过该值，
> 必须同步调大 `cfg::kWdtTimeoutMs` 或在阻塞循环里补喂狗点；
> 断电后 RTC 心跳证据丢失时仍可由复位原因判定 WDT 重启，但
> `lastWdtTask` 为空（显示「未能定位」）。

### SSE 实时推送

| 方法与路径 | 说明 |
|-----------|------|
| `GET /api/sse` | SSE 事件流（需认证，`Accept: text/event-stream`）。浏览器端 `EventSource` 自动携带会话 Cookie；Bearer 令牌客户端无法使用 SSE（无法自定义头），请继续用 REST 接口轮询 |

事件列表（data 均为对应 GET 接口同款 JSON）：

| 事件名 | 内容 | 推送时机 |
|--------|------|---------|
| `status` | 同 `/api/status` | 每秒比对，变化才推 |
| `stats` | 同 `/api/uart/stats` | 变化才推 |
| `at` | 新 AT 记录数组 | 有新记录才推（增量） |
| `wifi` | 同 `/api/wifi/status` | 变化才推 |
| `saved` | 同 `/api/wifi/list` | 变化才推 |
| `rgb` | 同 `/api/rgb` | 变化才推 |
| `disp` | 同 `/api/disp` | 变化才推 |
| `sms` | 同 `/api/sms/latest` | 新短信才推 |
| `smsports` | 同 `/api/sms/ports` | 变化才推 |
| `smsstats` | 同 `/api/sms/stats` | 变化才推 |
| `ota` | 同 `/api/ota/status`（`state`/`progress`/`message`/`currentVersion`/`latestVersion`/`available`） | 变化才推；升级长任务的进度/阶段/失败原因实时驱动「系统工具」页，替代原有 1 秒轮询 |
| `call` | `{"number":..,"ts":..,"modem":..}`（最近一次实际派发的来电，与 `sms` 事件对称） | 有新派发（非黑名单拦截）才推；仪表盘「最近来电」卡片实时呈现 |
| `log` | `{"total":n,"logs":[{id,ts,level,tag,msg}...]}`（同 `/api/logs` 增量，按 ID 升序） | 有新日志才推（增量，仿 `at`）；前端按自身级别/关键字过滤并去重并入 |
| `outbox` | `{}`（脏信号） | 发件箱任意可见变更（入队 / 投递结果 / 重试 / 删除 / 清空 / 淘汰）即推，前端收到即刷新当前视图 |

新客户端接入立即收到全量快照；并发上限 8（超出顶掉最旧）。断线由浏览器自动重连，
重连后前端会做一次 REST 兜底拉取补齐断线期数据。网页端
（`assets/app.js` 的 `Alpine.store('live')`）由此驱动，不再定时轮询；
SSE 不可用时自动降级为轮询（状态 2 秒、日志 / 发件箱 5 秒）。

## 脚本示例

下面示例用 Bearer 令牌调用各接口。脚本/第三方客户端推荐先换短期票据（纯 HTTP 下不暴露 raw 令牌）：

```bash
RAW=<你的持久化 API 令牌>          # 在「系统设置 - API 访问令牌」启用后获得
CH=$(curl -s http://设备IP/api/token/challenge)
NONCE=$(echo "$CH" | jq -r .nonce); TS=$(echo "$CH" | jq -r .ts)
V=$(printf %s "$RAW" | sha256sum | cut -d' ' -f1)
PROOF=$(printf "%s:%s:%s" "$V" "$TS" "$NONCE" | sha256sum | cut -d' ' -f1)
TOKEN=$(curl -s -X POST http://设备IP/api/token/exchange -H 'Content-Type: application/json' \
  -d "{\"v\":2,\"nonce\":\"$NONCE\",\"ts\":$TS,\"proof\":\"$PROOF\"}" | jq -r .ticket)
# 兼容（默认开启）：直接带 raw 持久化令牌：TOKEN=$RAW
# 临时登录令牌（v1 兼容，浏览器走 v2 挑战应答）：TOKEN=$(curl -s -X POST -H 'Content-Type: application/json' \
#   -d '{"user":"admin","pass":"admin123"}' http://设备IP/api/login | grep -o '"token":"[^"]*"' | cut -d'"' -f4)
```

> 浏览器默认走 v2 登录挑战应答，无需关心上述流程；`TOKEN` 拿到后即可像下面一样调用各接口：

# 查询状态（GET）
curl -H "Authorization: Bearer $TOKEN" http://设备IP/api/status

# 保存 WiFi（POST + JSON 请求体）
curl -X POST -H "Authorization: Bearer $TOKEN" \
  -H 'Content-Type: application/json' \
  -d '{"ssid":"MyWiFi","pass":"12345678","connect":true}' \
  http://设备IP/api/wifi/save

# 查询短信列表（分页 + 关键字）
curl -H "Authorization: Bearer $TOKEN" \
  "http://设备IP/api/sms?page=1&pageSize=20&q=验证码"

# 从通信模组 1 发短信（POST 启动 + GET 轮询结果）
curl -X POST -H "Authorization: Bearer $TOKEN" \
  -H 'Content-Type: application/json' \
  -d '{"port":1,"phone":"13800138000","content":"你好"}' \
  http://设备IP/api/sms/send
curl -H "Authorization: Bearer $TOKEN" http://设备IP/api/sms/send

# 查询邮件配置（GET）
curl -H "Authorization: Bearer $TOKEN" http://设备IP/api/email

# 保存邮件配置（POST + JSON 请求体；pass 留空则不修改授权码）
# 示例：每日 09:00 发送在线通知（schedMode=0），五类通知全开
curl -X POST -H "Authorization: Bearer $TOKEN" \
  -H 'Content-Type: application/json' \
  -d '{"server":"smtp.qq.com","port":465,"user":"user@qq.com","pass":"授权码","sendTo":"me@example.com","enabled":true,"notifyTypes":31,"schedMode":0,"schedHour":9,"schedMinute":0}' \
  http://设备IP/api/email

# 示例：固定间隔 24 小时（schedMode=1 + intervalH）
curl -X POST -H "Authorization: Bearer $TOKEN" \
  -H 'Content-Type: application/json' \
  -d '{"server":"smtp.qq.com","port":465,"user":"user@qq.com","pass":"授权码","sendTo":"me@example.com","enabled":true,"notifyTypes":31,"schedMode":1,"intervalH":24}' \
  http://设备IP/api/email

# 总开关：停用 / 重新启用邮件通知（只翻开关，不动其余配置）
curl -X POST -H "Authorization: Bearer $TOKEN" \
  -H 'Content-Type: application/json' -d '{"enabled":false}' \
  http://设备IP/api/email/enabled

# 发送测试邮件
curl -X POST -H "Authorization: Bearer $TOKEN" http://设备IP/api/email/test

# 推送通道：查询类型 schema / 当前配置 / 发送测试
curl -H "Authorization: Bearer $TOKEN" http://设备IP/api/push/types
curl -H "Authorization: Bearer $TOKEN" http://设备IP/api/push
curl -X POST -H "Authorization: Bearer $TOKEN" http://设备IP/api/push/test

# OTA：检查更新 -> 启动升级（tag 取 status 里的 latestVersion）
curl -H "Authorization: Bearer $TOKEN" http://设备IP/api/ota/version
curl -H "Authorization: Bearer $TOKEN" http://设备IP/api/ota/status
curl -X POST -H "Authorization: Bearer $TOKEN" \
  -H 'Content-Type: application/json' \
  -d '{"tag":"1.0.0-20261004T121442-f2b513a"}' \
  http://设备IP/api/ota/start

# 配置备份 / 恢复
curl -H "Authorization: Bearer $TOKEN" -o backup.json \
  http://设备IP/api/config/export
curl -X POST -H "Authorization: Bearer $TOKEN" \
  -H 'Content-Type: application/json' --data-binary @backup.json \
  http://设备IP/api/config/import

# 远程按键注入（等价于按一次 OK）
curl -X POST -H "Authorization: Bearer $TOKEN" \
  -H 'Content-Type: application/json' -d '{"key":"ok"}' \
  http://设备IP/api/key
```

## 项目结构

```
src/
├── main.cpp              Arduino 入口（setup/loop 驱动 App）
├── lv_conf.h             LVGL 9 配置（内存走 PSRAM、色彩深度、启用组件）
├── app/                  应用层
│   ├── App.h/.cpp        模块装配与主循环调度（WiFi / SSE / 短信 / 屏幕 / 队列 / 定时重启）
│   ├── Config.h          全局配置：引脚 / WiFi / AP / NVS / 超时 / 通知位掩码 / 环容量 / 分区 label
│   ├── Psram.h           PSRAM 工具：可增长缓冲区 + 容器 / ArduinoJson 分配器
│   ├── StatusInfo.h      App -> LVGL 的状态快照结构（零堆分配）
│   ├── UIMenu.h/.cpp     LCD 菜单状态机（12 槽位、动态页、屏幕设置持久化）
│   ├── SystemService.h/.cpp  定时重启（间隔 / 每日定点 + 星期位掩码）与配置导入导出
│   ├── Coredump.h/.cpp   崩溃元信息记录、分区查询与清除
│   ├── TaskWatchdog.h/.cpp  TWDT 运行期重配置、常驻任务订阅/喂狗、卡死原因 NVS 留痕
│   ├── UserFs.h/.cpp     userdata 分区挂载 /data、容量查询、格式化
│   └── KeyInject.h/.cpp  Web /api/key -> 主循环 UI 的按键事件队列
├── hal/                  硬件抽象层
│   ├── Display.h/.cpp    ST7789 + LovyanGFX 面板 + LVGL 显示端口（pimpl 隔离）、JPEG 取帧
│   ├── RgbLed.h/.cpp     WS2812 RGB 灯
│   ├── Button.h/.cpp     4 键扫描与 click/long/repeat 事件
│   └── CpuMonitor.h/.cpp 基于 FreeRTOS 运行时间统计的整机 CPU 使用率
├── ui/                   LVGL 界面层
│   ├── Ui.h/.cpp         开机页 / 状态页 / 菜单页 / 文本页 / 新短信弹窗的部件树
│   ├── UiFonts.h/.cpp    TinyTTF 中文字库（ui_cn.ttf）+ 图标 / Logo 位图字体
│   ├── UiIcons.h         图标码点宏
│   └── fonts/            编译进 flash 的图标与 Logo 字体 C 文件
├── net/                  网络层
│   ├── WifiManager.h/.cpp  WiFi 连接管理 + NVS 配网 + AP 模式 + 热点密码
│   ├── AdminStore.h/.cpp  管理账号（NVS 只存 salt+verifier，v1 明文自动迁移）
│   ├── LoginLimiter.h/.cpp 登录三层限流 + 一次性挑战 nonce（纯内存）
│   ├── WebPanel.h/.cpp   HTTP 路由 / 认证中间件 / 登录挑战 / LittleFS 静态服务
│   ├── StatusBus.h/.cpp   SSE 实时推送（/api/sse，diff 比对只推变化）
│   ├── StateJson.h        状态快照 JSON 序列化（REST 与 SSE 共用）
│   ├── EmailService.h/.cpp  SMTP 邮件通知：HTML 模板 + 通知类型 + 发送队列 + 排程
│   └── MqttService.h/.cpp  MQTT 事件上报 + 命令订阅（esp-mqtt）
├── sms/                  短信与通信模组
│   ├── SmsService.h/.cpp  双模组收发、PDU 处理、来电检测、管理员命令、模组管理
│   ├── SmsStore.h/.cpp    短信持久化（/data/sms/<id>.txt，FIFO 1000，分页 / 搜索 / 今日统计）
│   ├── SmsCodec.h/.cpp    PDU 编解码（lib/pdulib）+ 长短信重组
│   ├── BlackList.h/.cpp   号码黑名单（精确 / 前缀 / 通配符，短信 + 来电作用域）
│   ├── SmsRecord.h        SmsMessage / SmsConcatSlot / SmsPduPart 数据结构
│   └── ModemSnapshot.h    模组状态 POD（型号 / 版本 / 号码 / 运营商 / 注册 / IMEI / 信号）
├── push/                 推送通道
│   ├── PushService.h/.cpp 调度：多通道、广播 / 故障转移、队列 + 主循环发送、NVS
│   ├── PushChannels.h/.cpp 13 种通道的具体 HTTP 发送实现（TLS 经 TlsTurn 串行化）
│   ├── PushTypes.h/.cpp    通道类型与策略枚举、类型 schema JSON
│   └── MsgContext.h/.cpp   自定义模板可用字段与占位符渲染
├── serial/               串口层
│   ├── SerialBridge.h/.cpp  双串口 AT 透传（任务 + 互斥锁 + 自愈 + 统计）
│   └── AtHistory.h/.cpp     AT 记录环形缓冲（cmd/bg/urc 三类，JSON 增量导出）
├── ota/
│   └── Ota.h/.cpp         OTA 状态机：版本检查、在线升级、本地固件 / LittleFS 分片烧录
├── time/
│   └── TimeSync.h/.cpp    NTP 为主 + SIM NITZ 兜底，NVS 持久化，可配置时区（默认 UTC+8）
├── log/
│   └── LogStore.h/.cpp    PSRAM 环形日志缓冲（定长条目 / FIFO / 分级 / 分页查询）
└── utils/
    ├── HttpSession.h/.cpp  HTTPS 会话 RAII 封装（跳过证书校验）
    ├── CryptoUtil.h/.cpp   SHA-256 / 随机 hex / 恒定时间比较 / FNV 哈希
    ├── TlsTurn.h/.cpp      全局 TLS 并发通道计数信号量（最多 4 个）
    └── tls_mem.h/.cpp      mbedTLS 大块分配重定向到 PSRAM
data/
├── login.html            独立登录页（v2 挑战应答 / 429 倒计时 / 记住凭据存 verifier）
├── index.html            控制台外壳：左侧分组菜单 + 顶栏 + hash 路由内容区
├── ui_cn.ttf             中文字库（GB2312，TinyTTF 运行时渲染，不参与 gzip）
├── pages/                内容页片段（按菜单懒加载，纯静态无敏感数据）
│   ├── dashboard.html    仪表盘：SOC / 状态指标 / 屏幕 / RGB / 模组流量 / 最近动态 / API 令牌
│   ├── sms.html          短信：收发记录 / 发送 / 搜索 / 详情 / 删除 / 清空
│   ├── admin.html        管理员：授权手机号（短信远程控制总开关 + 号码）
│   ├── modem.html        通信模组：状态 / 信号 / 运营商 / 号码 / 启用与别名 / 重初始化
│   ├── blacklist.html    黑名单：精确 / 前缀 / 通配符，短信与来电作用域，命中动作
│   ├── push.html         推送通道：13 种方式（含 SMS 备份），广播 / 故障转移策略
│   ├── mqtt.html         MQTT：连接配置 / 事件类型 / 命令订阅
│   ├── email.html        邮件通知：SMTP 配置 / 通知类型 / 排程 / 测试发送
│   ├── wifi.html         WiFi：扫描 / 保存 / 排序 / 删除 / AP 热点密码
│   ├── serial.html       模组调试：命令发送 / 历史日志 / 统计 / 复位
│   ├── logs.html         运行日志：分页查看 / 级别与关键字过滤 / 自动刷新 / 清空
│   ├── display.html      屏幕显示：开关 / 背光亮度 / 自动熄屏 / 自动返回 / Web 预览
│   ├── rgb.html          RGB 灯：预设 / 自定义颜色 / 亮度
│   ├── settings.html     系统设置：管理账号 / API 访问令牌 / 定时重启
│   └── tools.html        系统工具：OTA 升级 / 配置导入导出 / 崩溃转储 / 任务看门狗状态与自检 / 重启 / 出厂复位
└── assets/               静态资源（改后需重新 uploadfs）
    ├── app.css           设计系统与布局样式
    ├── app.js            菜单配置 / hash 路由 / 认证封装 / 各页 Alpine 组件
    ├── sha256.js         纯 JS SHA-256（纯 HTTP 下 crypto.subtle 不可用时的默认实现）
    └── vendor/
        └── alpine.min.js Alpine.js 3.x 运行时（本地内置，AP 模式无外网可用）
lib/
├── pdulib                PDU 编解码库（短信 PDU <-> 文本）
└── jpge                  JPEG 编码器（屏幕实时预览）
partitions/
└── esp32s3_16mb.csv      16MB 分区表（含双 OTA 槽、userdata、LittleFS 资源分区、coredump）
script/
├── uploadfs_gz.py        data/ 打包时自动 gzip 并上传的 PlatformIO 脚本
├── inject_app_version.py 编译期注入 Release tag 与 OTA 发布源地址
├── gen_version.sh        生成版本标识（<VERSION>-<提交时间>-<短 SHA>）
├── build_release.py      发布产物构建
├── gen_release_notes.py  生成 Release 说明
├── prune_releases.py     清理历史 Release
├── gen_iconfont.py       生成图标字体 C 文件
└── print_version_tag.sh  打印当前版本 tag
```

代码注释为中文，覆盖类职责、方法、参数与字段含义。
