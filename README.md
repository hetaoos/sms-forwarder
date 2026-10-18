# 短信转发器（SMS Forwarder）

基于 ESP32-S3 的双串口 AT 透传设备：外接两个 AT 模组串口，通过 ST7789 屏幕
实时显示运行状态与最近一条 AT 交互，并内置 Web 控制台进行远程管理。

## 功能特性

- **双串口 AT 透传**（`serial/SerialBridge`）
  - 独立 FreeRTOS 任务持续抽数，命令/响应同步模型，阻塞式发送 + 超时收集
  - 连续 3 次无响应自动复位串口（自愈），支持手动复位
  - 环形缓冲保存最近 30 条 AT 记录（含串口自发上报），Web 端增量拉取
  - 收发字节数 / 错误 / 超时 / 复位次数统计
- **ST7789 240x320 屏幕**（`hal/Display`）
  - LovyanGFX 驱动，静态框架只画一次，数值行局部重绘（无整屏闪烁）
  - 显示 WIFI / IP / RSSI / UPTIME / HEAP / RGB 与最近一条 AT 命令
  - 支持 Web 远程开关屏与背光亮度调节（0-100%）
- **板载 RGB 灯**（`hal/RgbLed`）：WS2812，5 组预设 + 任意 RGB 值 + 亮度调节（0-100%）
- **WiFi 管理 + 配网**（`net/WifiManager`）
  - 编译期内置默认账号密码（`-DWIFI_SSID_DEFAULT` / `-DWIFI_PASS_DEFAULT`，不设置则为空）
  - NVS 按功能存 JSON 配置，最多 5 组，启动时按顺序依次尝试
  - 全部候选失败 3 轮后自动切换 AP 配网模式（`SMS-Forwarder-Setup`，开放热点）
  - 网页与 API 在所有模式下统一认证，支持 **Cookie 会话**与 **Bearer 访问令牌**
    两种方式；管理账号（用户名/密码）可在「系统设置」页修改，以 JSON 对象
    形式存于 NVS（`appcfg` / `auth`），修改后全部登录态立即作废
  - 配网采用 AP+STA 双栈：连网成功后页面不中断，重启后按配置直连
  - Web 支持扫描附近 WiFi、选中填密码、保存并连接、删除、上移/下移排序
- **Web 控制台**（`net/WebPanel`，端口 80）：Alpine.js 多页应用，打包进
  LittleFS 专有分区（`data/login.html` + `data/index.html` 外壳 +
  `data/pages/*.html` 内容页 + `data/assets/` 资源，支持 gzip）。
  左右布局：左侧分组菜单，右侧按 hash 路由懒加载内容页；
  全部接口统一在 `/api/` 前缀下，页面与静态资源公开，接口需登录
  （Cookie 会话或 Bearer 令牌），防止第三方盗用
- **SSE 实时推送**（`net/StatusBus`，`/api/sse`）：状态变化时主动推送给
  全部已连接浏览器（1 秒 diff 比对，只推变化），网页端不再轮询状态接口；
  REST 接口原样保留供第三方客户端使用
- **设备日志**（`log/LogStore`）：运行日志存入 PSRAM 定长记录环（默认 512 条，
  写满 FIFO 淘汰最旧，约 70KB；板子无 PSRAM 时自动退化为内部堆），
  支持按级别 / 关键字过滤的分页查询与一键清空
- **SMTP 邮件通知**（`net/EmailService`，参考 esp32-sms-forwarding 实现）
  - 统一 HTML 卡片模板（灰底白卡 + 渐变标题栏 + 状态徽标 + 公共信息块），
    自动识别短信验证码并在标题/正文大字高亮；客户端不支持 HTML 时附带纯文本兜底
  - 通知类型位掩码（可分别开关）：**收到短信** / **SIM 状态变更** /
    **设备启动完成** / **定时在线通知**（两种排程：固定间隔 N 小时 /
    每日定点时:分，未勾选则不触发）
  - 运行期插拔卡检测：遥测周期查询 `AT+CPIN?`，掉卡/插卡成对通知
    （开机首次就绪与模组重启不误报）
  - 非阻塞入队 + 主循环发送：短信/SIM 回调（串口抽数任务）只做入队，
    SMTP 会话在 `App::tick` 执行，失败退避重试 3 次，认证失败不重试
  - 配置存 NVS（`appcfg` / `email`），授权码不回显；网页可发送测试邮件
  - **总开关**：停用后一切邮件都不发（短信 / SIM / 启动 / 定时在线全部静默
    跳过，待发队列丢弃），已保存的服务器、账号与通知类型保留，重新开启立即恢复
  - 仅支持隐式 TLS 端口（465 等，握手即加密），不支持 587（STARTTLS）
- **PSRAM 大块内存**（`app/Psram.h`）：ESP32-S3 片外 8MB PSRAM 用于承载瞬时大块
  数据，内部 DRAM（327KB，还要与任务栈、LWIP 缓冲共用）只留短字符串：
  - 邮件正文构造：CSS + 卡片 + 多模组表格直接拼进 PSRAM 缓冲区，
    并以「静态内容」形式交给 ReadyMail（`body(const char*, size_t)`），全程不回拷 DRAM
  - 短信列表分页：结果容器与响应 JSON 内存池均分配在 PSRAM（一页最多 100 条）
  - 设备日志环（见上）
  - 无 PSRAM 或空间不足时自动回退内部堆，功能不变（构造失败则丢弃本次通知并打日志）

## 硬件与引脚

板型 `esp32s3_120_16_8-qio_opi`（Arduino 框架），引脚定义集中在
`src/app/Config.h`：

| 功能 | 引脚 | 说明 |
|------|------|------|
| 串口1 TX / RX | GPIO17 / GPIO18 | 外接 AT 模组 |
| 串口2 TX / RX | GPIO41 / GPIO40 | 外接 AT 模组 |
| TFT CS / RST / DC | GPIO15 / GPIO5 / GPIO14 | ST7789 控制 |
| TFT MOSI / SCLK | GPIO13 / GPIO12 | SPI（SPI2，40MHz） |
| TFT BLK | GPIO8 | PWM 背光，默认 80% |
| RGB LED | GPIO48 | 板载 WS2812 |

## 编译与烧录

依赖 PlatformIO Core（>= 6），库版本以 `platformio.ini` 声明为准：

- ESP32Async/ESPAsyncWebServer 3.12.1
- ESP32Async/AsyncTCP 3.5.0
- lovyan03/LovyanGFX 1.2.33
- bblanchon/ArduinoJson 7.x
- mobizt/ReadyMail 0.4.x（SMTP 邮件发送，需 C++17，见 `platformio.ini` 的
  `-std=gnu++17` 与 `build_unflags`）

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
自动解压。烧录固件（`pio run -t upload`）成功后也会自动上传一次 FS 镜像。

分区表 `partitions/esp32s3_16mb.csv`（16MB Flash）布局；app 分区起点与大小均
64KB 对齐，`userdata` 刷固件后仍保留：

| 分区 | 地址范围 | 大小 | 用途 |
|------|---------|------|------|
| nvs | 0x009000~0x018FFF | 64KB | 配置存储（WiFi 等，按功能存 JSON） |
| otadata | 0x019000~0x01AFFF | 8KB | OTA 启动槽位记录 |
| app0 / app1 | 0x020000 / 0x420000 | 各 4MB | 双 OTA 固件槽位 |
| userdata | 0x820000~0xD1FFFF | 5MB | 用户数据，刷固件保留（挂载 `/data`） |
| spiffs | 0xD20000~0xFDFFFF | 2.75MB | 资源分区：Web 页面 / 字体 / 图标（挂载 `/`） |
| coredump | 0xFE0000~0xFFFFFF | 128KB | 崩溃转储 |

`pio run -t uploadfs` 的目标是表中最后一个 FS 分区（spiffs 资源分区），
只影响 Web 资源；`userdata` 仅由设备运行期读写，重烧固件不会丢失。

### 内置默认 WiFi 账号（可选）

在 `platformio.ini` 的 `build_flags` 中修改；不设置则默认为空串，
完全依赖 NVS 配置（首次无配置时自动进入 AP 配网）：

```ini
build_flags =
    -Isrc
    -DLGFX_USE_V1=1
    -DWIFI_SSID_DEFAULT=\"MyWiFi\"
    -DWIFI_PASS_DEFAULT=\"MyPass\"
```

## 首次配网流程

1. 烧录后若无任何 WiFi 配置，设备开启热点 **`SMS-Forwarder-Setup`**（开放，无密码）
2. 连接该热点，浏览器打开 **`http://192.168.4.1/login`**，在独立登录页输入
   `admin` / `admin123` 登录（登录响应同时返回 Bearer 令牌，见仪表盘页面）
3. 进入「WiFi 设置」卡片：**扫描附近 WiFi** → 点击扫描结果填入 SSID →
   输入密码 → **保存并连接**
4. 连接成功后页面持续可用（双栈模式）；重启后将按保存的顺序自动连接

已保存配置存放在 NVS：命名空间 `appcfg`，键 `wifi`，值为 JSON 对象（管理账号
另存于同命名空间的键 `auth`：`{"ver":1,"user":"..","pass":".."}`）：

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

## HTTP API

**统一约定：请求与响应均为 JSON**——POST 请求体为 JSON
（`Content-Type: application/json`，非 JSON 的 POST 不匹配处理器）；
所有响应（含错误）均为 JSON，错误形如
`{"ok":false,"error":"描述"}` 并带合适的状态码（400 参数错 /
401 未认证 / 413 请求体过大 / 500 服务端失败），未匹配路径返回
`{"ok":false,"error":"not found"}`。

页面与静态资源（`/`、`/login`、`/assets/*`、`/pages/*`）公开（不含敏感数据）；
全部 API 统一在 `/api/` 前缀下，除登录/登出/会话探测外均需认证。支持两种认证方式：

1. **会话 Cookie（浏览器）**：`POST /api/login` 成功后服务端下发 `sid`
   Cookie（`HttpOnly; SameSite=Strict; Path=/`），浏览器自动携带；
   `HttpOnly` 使 JS 读不到、`SameSite=Strict` 使 Cookie 不随跨站请求发送，
   第三方网页无法借登录态发起 CSRF 调用。
2. **Bearer 访问令牌（脚本/第三方客户端）**：登录响应体返回 `token`，
   之后请求带 `Authorization: Bearer <token>` 头即可，无需 Cookie。

会话与令牌均为服务端随机生成，仅存内存（上限 8 个），**重启后全部失效**。
`POST /api/logout` 作废当前会话与令牌。未认证访问返回 401
`{"ok":false,"error":"unauthorized"}`。

### 认证

| 方法与路径 | 请求体 | 响应 |
|-----------|--------|------|
| `POST /api/login` | `{"user":"admin","pass":"admin123"}` | 200 + `Set-Cookie: sid=...`，`{"ok":true,"user":"admin","token":"..."}`；账密错 401 |
| `POST /api/logout` | 无 | `{"ok":true}` + 清除 Cookie，作废当前会话/令牌 |
| `GET /api/session` | 无 | 已登录 `{"ok":true,"user":"admin"}`，否则 401 |
| `GET /api/auth` | 无 | 当前用户名 `{"ok":true,"user":"admin"}`（密码不回传） |
| `POST /api/auth` | `{"user":"新用户","pass":"新密码","passOld":"当前密码"}` | `{"ok":true}`；当前密码不符 403、长度超限 400、NVS 写入失败 500；成功后全部会话/令牌作废 |

### 系统 / 屏幕 / RGB

| 方法与路径 | 说明 |
|-----------|------|
| `GET /` | 控制台页面（公开） |
| `GET /login` | 独立登录页（公开，已登录自动跳回） |
| `GET /api/led` | 切换下一个预设颜色，返回最新 RGB 状态 |
| `GET /api/rgb` | 查询 RGB 状态：`{"ok":true,"color":名,"rgb":[r,g,b],"index":n,"bright":0-100}` |
| `POST /api/rgb` | JSON `{"c":索引}` / `{"r":n,"g":n,"b":n}` / `{"bright":0-100}`，三者可组合（`c` 越界 400），返回最新状态 |
| `GET /api/disp` | 查询屏幕状态，返回 `{"ok":true,"on":bool,"bright":0-100}` |
| `POST /api/disp` | JSON `{"on":bool?,"bright":0-100?}`（字段可选，越界钳制），返回最新状态 |
| `GET /api/status` | 状态 JSON（ssid/ip/rssi/uptime/heap/color/rgb/disp/dispBright/rgbBright/mode/connected） |

### SSE 实时推送

| 方法与路径 | 说明 |
|-----------|------|
| `GET /api/sse` | SSE 事件流（需认证，`Accept: text/event-stream`）。浏览器端 `EventSource` 自动携带会话 Cookie；Bearer 令牌客户端无法使用 SSE（无法自定义头），请继续用上面的 REST 接口轮询 |

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

新客户端接入立即收到全量快照；断线由浏览器自动重连，重连后前端会做一次
REST 兜底拉取补齐断线期数据。网页端（`assets/app.js` 的 `Alpine.store('live')`）
由此驱动，不再定时轮询；SSE 不可用时自动降级为 2 秒轮询。

### 串口 AT

| 方法与路径 | 说明 |
|-----------|------|
| `GET /api/at/send?port=1\|2&baud=115200&eol=0..3&cmd=...&t=1000` | 发送 AT 命令，返回该条记录 JSON |
| `GET /api/at/hist?after=-1&n=20` | 增量拉取历史（after 之后，n 条） |
| `GET /api/at/clear` | 清空历史，返回 `{"ok":true}` |
| `GET /api/at/reset?port=1\|2` | 手动复位串口，返回 `{"ok":true}` |
| `GET /api/at/baud` | 默认波特率 |
| `GET /api/uart/stats` | 两串口统计 JSON |

`eol` 取值：`0`=CRLF，`1`=CR，`2`=LF，`3`=无。

### 设备日志（PSRAM 环形缓冲）

运行日志存于 PSRAM 的定长记录环（`log/LogStore`，默认 512 条，写满 FIFO
淘汰最旧；无 PSRAM 时自动退化为内部堆，功能不变）。时间戳与短信记录一致：
时钟已同步为纪元秒，未同步为开机秒数。级别：`0` 调试 / `1` 信息 / `2` 警告 /
`3` 错误。

| 方法与路径 | 说明 |
|-----------|------|
| `GET /api/logs?page=1&pageSize=20&level=2&q=关键字` | 分页查询：`page` 从 1 起，`pageSize` 上限 100；`level` 为最低级别过滤（缺省不过滤，传 `2` 即仅看警告以上）；`q` 对 tag/msg 大小写不敏感匹配；结果按 ID 倒序（最新在前）。返回 `{"ok":true,"total":n,"page":p,"pageSize":ps,"logs":[{id,ts,level,tag,msg}...]}` |
| `GET /api/logs?after=<id>&limit=100&level=&q=` | 增量拉取（轮询追加用，避免整页重绘闪烁）：只返回 ID 大于 `after` 的匹配记录，按 ID **升序**（旧→新），最多 `limit` 条（上限 100）；`level`/`q` 与分页模式同义。返回 `{"ok":true,"last":服务端最新ID,"total":匹配总数,"count":本次条数,"logs":[...]}`；`count < limit` 时表示后面已无匹配记录，前端可把基线直接推进到 `last` |
| `GET /api/logs/stats` | 存储统计：`{"ok":true,"total":n,"max":n,"psram":bool,"bytes":n,"psramTotal":n,"psramFree":n}`；`psram=false` 表示缓冲未落在 PSRAM（无 PSRAM 或分配失败，已退化为内部堆，功能不受影响），此时由 `psramTotal` 区分两种原因（0 = 设备无 PSRAM）。启动串口会打印 `[log] ring ready: ... in PSRAM/heap` |
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

### 邮件通知（SMTP）

| 方法与路径 | 请求体（JSON） | 说明 |
|-----------|---------------|------|
| `GET /api/email` | - | 邮件配置：`{"ok":true,"enabled":bool,"configured":bool,"server":"..","port":465,"user":"..","sendTo":"..","hasPass":bool,"notifyTypes":n,"schedMode":0\|1,"schedHour":n,"schedMinute":n,"intervalH":n}`；授权码不回显 |
| `POST /api/email` | `{"server":"..","port":465,"user":"..","pass":"..","sendTo":"..","enabled":true,"notifyTypes":15,"schedMode":1,"schedHour":9,"schedMinute":0,"intervalH":24}` | 保存配置并落盘 NVS；`pass` 为空串表示不修改已保存授权码；`enabled` 为 SMTP 邮件通知总开关（缺省 true，兼容不带该字段的旧客户端）；`notifyTypes` 为位掩码（bit0=设备启动 bit1=SIM状态 bit2=收到短信 bit3=定时在线）；排程 `schedMode=1` 固定间隔（`intervalH` 小时，1-720）/ `schedMode=0` 每日定点（`schedHour:schedMinute`，依赖时间同步）；保存成功返回最新配置 |
| `POST /api/email/enabled` | `{"enabled":true\|false}` | 只翻转总开关，不动其余配置；停用时丢弃待发队列，重新开启后立即恢复 |
| `POST /api/email/test` | 无 | 立即发送一封测试邮件（单次尝试，最长阻塞约 15s）；总开关未开、配置不完整或 WiFi 未连接返回 400，发送失败 500 |

**总开关**（`enabled`）：false 时一切邮件都不发送——短信 / SIM / 启动 /
定时在线通知全部静默跳过，待发队列丢弃；已保存的 SMTP 参数与通知类型保留，
重新开启后立即恢复。与「配置完整」（`configured`，四项参数齐全）相互独立：
实际能否发邮件由 `enabled && configured` 共同决定。NVS 无该字段时默认开启
（旧固件没有总开关，升级后保持原行为）。

邮件模板：所有通知邮件共用一套卡片式 HTML（`EmailService::buildHtml`）——
灰底 + 白色圆角卡片 + 渐变标题栏 + 状态徽标 + 公共信息块（系统时间 / 各模组
本机号码或设备 IP / 运行时长）+ 底部标识；短信邮件额外带验证码高亮块
（识别「验证码 / OTP / code」等关键词附近的 4~6 位数字串，标题形如
「验证码: 123456 来自 示例」）。HTML 超长（>6000 字符）时自动降级为纯文本。

通知类型默认四类全开；「SIM 状态变更」仅在运行时插拔卡时触发（开机首次
就绪、模组重启后的重新就绪均不通知）。发送走主循环队列，失败退避重试 3 次
（2s / 4s），认证失败（账号或授权码错）直接丢弃并提示，不消耗重试。

**定时在线通知排程**（由通知类型位 bit3 总控，勾选后按排程触发）：

| 模式 | 字段 | 说明 |
|------|------|------|
| 固定间隔 | `schedMode=1` + `intervalH`（1-720 小时） | 基于 millis 计时，开机后每满一个周期发送一次 |
| 每日定点 | `schedMode=0` + `schedHour`(0-23) + `schedMinute`(0-59) | 基于本地时间，每天到点发送一次；**依赖 NTP / SIM 时间同步**，未同步不触发；触发时刻落盘 NVS（`lastDaily`），重启不会补发当天已发过的通知；tick 被邮件发送阻塞时，触发点起 10 分钟窗口内的迟到触发仍有效 |

保存配置（含排程）后会立即补发一封在线通知，便于确认配置生效。

邮件配置脚本示例（Bearer 令牌）：

```bash
TOKEN=$(curl -s -X POST -H 'Content-Type: application/json' \
  -d '{"user":"admin","pass":"admin123"}' http://设备IP/api/login \
  | grep -o '"token":"[^"]*"' | cut -d'"' -f4)

# 查询邮件配置（GET）
curl -H "Authorization: Bearer $TOKEN" http://设备IP/api/email

# 保存邮件配置（POST + JSON 请求体；pass 留空则不修改授权码）
# 示例：每日 09:00 发送在线通知（schedMode=0）
curl -X POST -H "Authorization: Bearer $TOKEN" \
  -H 'Content-Type: application/json' \
  -d '{"server":"smtp.qq.com","port":465,"user":"user@qq.com","pass":"授权码","sendTo":"me@example.com","enabled":true,"notifyTypes":15,"schedMode":0,"schedHour":9,"schedMinute":0}' \
  http://设备IP/api/email

# 示例：固定间隔 24 小时（schedMode=1 + intervalH）
curl -X POST -H "Authorization: Bearer $TOKEN" \
  -H 'Content-Type: application/json' \
  -d '{"server":"smtp.qq.com","port":465,"user":"user@qq.com","pass":"授权码","sendTo":"me@example.com","enabled":true,"notifyTypes":15,"schedMode":1,"intervalH":24}' \
  http://设备IP/api/email

# 总开关：停用 / 重新启用邮件通知（只翻开关，不动其余配置）
curl -X POST -H "Authorization: Bearer $TOKEN" \
  -H 'Content-Type: application/json' -d '{"enabled":false}' \
  http://设备IP/api/email/enabled

# 发送测试邮件
curl -X POST -H "Authorization: Bearer $TOKEN" http://设备IP/api/email/test
```

脚本调用示例（Bearer 令牌）：

```bash
TOKEN=$(curl -s -X POST -H 'Content-Type: application/json' \
  -d '{"user":"admin","pass":"admin123"}' http://设备IP/api/login \
  | grep -o '"token":"[^"]*"' | cut -d'"' -f4)

# 查询状态（GET）
curl -H "Authorization: Bearer $TOKEN" http://设备IP/api/status

# 保存 WiFi（POST + JSON 请求体）
curl -X POST -H "Authorization: Bearer $TOKEN" \
  -H 'Content-Type: application/json' \
  -d '{"ssid":"MyWiFi","pass":"12345678","connect":true}' \
  http://设备IP/api/wifi/save
```

## 目录结构

```
src/
├── main.cpp              Arduino 入口（setup/loop 驱动 App）
├── app/                  应用层
│   ├── App.h/.cpp        模块装配与主循环调度
│   ├── Config.h          全局配置：引脚 / WiFi / AP / NVS / 超时 / 邮件通知 / 日志环
│   ├── Psram.h           PSRAM 工具：可增长缓冲区 + 容器 / ArduinoJson 分配器
│   └── StatusInfo.h      App -> Display 的状态快照结构
├── hal/                  硬件抽象层
│   ├── Display.h/.cpp    ST7789 屏幕（pimpl 隔离 LovyanGFX）
│   └── RgbLed.h/.cpp     WS2812 RGB 灯
├── log/                  设备日志
│   └── LogStore.h/.cpp   PSRAM 环形日志缓冲（定长条目 / FIFO / 分页查询）
├── net/                  网络层
│   ├── WifiManager.h/.cpp  WiFi 连接管理 + NVS 配网 + AP 模式
│   ├── AdminStore.h/.cpp  管理账号存储（NVS JSON，可网页修改）
│   ├── EmailService.h/.cpp  SMTP 邮件通知：HTML 模板 + 通知类型 + 发送队列
│   ├── StatusBus.h/.cpp   SSE 实时推送（/api/sse，diff 比对只推变化）
│   ├── StateJson.h        状态快照 JSON 序列化（REST 与 SSE 共用）
│   └── WebPanel.h/.cpp   HTTP 路由 / 认证中间件 / LittleFS 静态服务
└── serial/               串口层
    ├── SerialBridge.h/.cpp  双串口 AT 透传（任务 + 互斥锁 + 自愈）
    └── AtHistory.h/.cpp     AT 记录环形缓冲（JSON 导出）
data/
├── login.html            独立登录页（POST /api/login，成功后跳回目标页）
├── index.html            控制台外壳：左侧分组菜单 + 顶栏 + hash 路由内容区
├── pages/                内容页片段（按菜单懒加载，纯静态无敏感数据）
│   ├── dashboard.html    仪表盘：状态指标 / 屏幕 / RGB / 串口 / 最近动态 / API 令牌
│   ├── serial.html       串口 AT：命令发送 / 历史日志 / 统计 / 复位
│   ├── wifi.html         WiFi：扫描 / 保存 / 排序 / 删除
│   ├── email.html        邮件通知：SMTP 配置 / 通知类型 / 测试发送
│   ├── rgb.html          RGB 灯：预设 / 自定义颜色 / 亮度
│   ├── display.html      屏幕显示：开关 / 背光亮度调节
│   ├── sms.html          短信：收发记录 / 发送 / 搜索 / 详情 / 删除 / 清空
│   ├── modem.html        4G 模组：状态 / 信号 / 运营商 / 号码 / 启用与别名
│   ├── push.html         推送通道（规划中，13 种方式）
│   ├── mqtt.html         MQTT（规划中）
│   ├── blacklist.html    黑名单（规划中）
│   ├── logs.html         运行日志：分页查看 / 级别与关键字过滤 / 自动刷新 / 清空
│   ├── settings.html     系统设置：管理账号修改（用户名/密码存 NVS）
│   └── tools.html        系统工具（规划中）
└── assets/               静态资源（改后需重新 uploadfs）
    ├── app.css           设计系统与布局样式
    ├── app.js            菜单配置 / hash 路由 / 认证封装 / 各页 Alpine 组件
    └── vendor/
        └── alpine.min.js Alpine.js 3.14.9 运行时（本地内置，AP 模式无外网可用）
partitions/
└── esp32s3_16mb.csv      16MB 分区表（含 LittleFS 专有分区）
script/
└── uploadfs_gz.py        data/ 打包时自动 gzip 并上传的 PlatformIO 脚本
```

代码注释为中文，覆盖类职责、方法、参数与字段含义。
