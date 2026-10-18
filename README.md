# ESP32-S3 短信转发 / AT 桥接器

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
  - 支持 Web 远程开关屏
- **板载 RGB 灯**（`hal/RgbLed`）：WS2812，5 组预设 + 任意 RGB 值
- **WiFi 管理 + 配网**（`net/WifiManager`）
  - 编译期内置默认账号密码（`-DWIFI_SSID_DEFAULT` / `-DWIFI_PASS_DEFAULT`，不设置则为空）
  - NVS 按功能存 JSON 配置，最多 5 组，启动时按顺序依次尝试
  - 全部候选失败 3 轮后自动切换 AP 配网模式（`SMS-FWD-Setup`，开放热点）
  - 网页与 API 在所有模式下统一认证（`admin` / `admin123`），支持
    **Cookie 会话**与 **Bearer 访问令牌**两种方式，详见「HTTP API」一节
  - 配网采用 AP+STA 双栈：连网成功后页面不中断，重启后按配置直连
  - Web 支持扫描附近 WiFi、选中填密码、保存并连接、删除、上移/下移排序
- **Web 控制台**（`net/WebPanel`，端口 80）：Alpine.js 多页应用，打包进
  LittleFS 专有分区（`data/login.html` + `data/index.html` 外壳 +
  `data/pages/*.html` 内容页 + `data/assets/` 资源，支持 gzip）。
  左右布局：左侧分组菜单，右侧按 hash 路由懒加载内容页；
  全部接口统一在 `/api/` 前缀下，页面与静态资源公开，接口需登录
  （Cookie 会话或 Bearer 令牌），防止第三方盗用

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

1. 烧录后若无任何 WiFi 配置，设备开启热点 **`SMS-FWD-Setup`**（开放，无密码）
2. 连接该热点，浏览器打开 **`http://192.168.4.1/login`**，在独立登录页输入
   `admin` / `admin123` 登录（登录响应同时返回 Bearer 令牌，见仪表盘页面）
3. 进入「WiFi 设置」卡片：**扫描附近 WiFi** → 点击扫描结果填入 SSID →
   输入密码 → **保存并连接**
4. 连接成功后页面持续可用（双栈模式）；重启后将按保存的顺序自动连接

已保存配置存放在 NVS：命名空间 `appcfg`，键 `wifi`，值为 JSON 对象：

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

### 系统 / 屏幕 / RGB

| 方法与路径 | 说明 |
|-----------|------|
| `GET /` | 控制台页面（公开） |
| `GET /login` | 独立登录页（公开，已登录自动跳回） |
| `GET /api/led` | 切换下一个 RGB 颜色，返回 `{"color":"颜色名"}` |
| `GET /api/set?c=索引` 或 `?r=&g=&b=` | 设置 RGB（预设索引或任意 RGB），返回 `{"ok":true}` |
| `GET /api/disp?on=0\|1` | 屏幕开关，返回 `{"on":bool}` |
| `GET /api/status` | 状态 JSON（ssid/ip/rssi/uptime/heap/color/rgb/disp/mode/connected） |

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
│   ├── Config.h          全局配置：引脚 / WiFi / AP / NVS / 超时
│   └── StatusInfo.h      App -> Display 的状态快照结构
├── hal/                  硬件抽象层
│   ├── Display.h/.cpp    ST7789 屏幕（pimpl 隔离 LovyanGFX）
│   └── RgbLed.h/.cpp     WS2812 RGB 灯
├── net/                  网络层
│   ├── WifiManager.h/.cpp  WiFi 连接管理 + NVS 配网 + AP 模式
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
│   ├── rgb.html          RGB 灯：预设 / 自定义颜色
│   ├── display.html      屏幕显示开关
│   ├── sms.html          短信记录（规划中）
│   ├── push.html         推送通道（规划中，13 种方式）
│   ├── mqtt.html         MQTT（规划中）
│   ├── blacklist.html    黑名单（规划中）
│   ├── logs.html         运行日志（规划中）
│   ├── settings.html     系统设置（规划中）
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
