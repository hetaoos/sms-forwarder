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
  - AP 模式下网页用 `admin` / `admin123` Basic 认证保护
  - 配网采用 AP+STA 双栈：连网成功后页面不中断，重启后按配置直连
  - Web 支持扫描附近 WiFi、选中填密码、保存并连接、删除、上移/下移排序
- **Web 控制台**（`net/WebPanel`，端口 80）：状态页 + RGB 控制 + AT 命令 +
  WiFi 配网，单页应用，1s 轮询增量刷新

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

# 烧录（串口见 platformio.ini 的 upload_port）
pio run -e esp32s3 -t upload

# 串口监视
pio device monitor -e esp32s3
```

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
2. 连接该热点，浏览器打开 **`http://192.168.4.1`**，输入 `admin` / `admin123` 登录
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

AP 配网模式下所有接口均需 Basic 认证（`admin` / `admin123`）。

### 系统 / 屏幕 / RGB

| 方法与路径 | 说明 |
|-----------|------|
| `GET /` | 控制台页面 |
| `GET /led` | 切换下一个 RGB 颜色，返回颜色名 |
| `GET /set?c=索引` 或 `?r=&g=&b=` | 设置 RGB（预设索引或任意 RGB） |
| `GET /disp?on=0\|1` | 屏幕开关 |
| `GET /status` | 状态 JSON（ip/rssi/uptime/heap/color/disp） |

### 串口 AT

| 方法与路径 | 说明 |
|-----------|------|
| `GET /at/send?port=1\|2&baud=115200&eol=0..3&cmd=...&t=1000` | 发送 AT 命令，返回该条记录 JSON |
| `GET /at/hist?after=-1&n=20` | 增量拉取历史（after 之后，n 条） |
| `GET /at/clear` | 清空历史 |
| `GET /at/reset?port=1\|2` | 手动复位串口 |
| `GET /at/baud` | 默认波特率 |
| `GET /uart/stats` | 两串口统计 JSON |

`eol` 取值：`0`=CRLF，`1`=CR，`2`=LF，`3`=无。

### WiFi 配网

| 方法与路径 | 说明 |
|-----------|------|
| `GET /wifi/status` | 模式（connecting/sta/ap）、连接、AP 信息、已保存数量 |
| `GET /wifi/scan` | 扫描中返回 `{"busy":1}`；完成返回 `{"busy":0,"nets":[...]}` |
| `GET /wifi/list` | 已保存配置（密码不回传，仅 `hasPass` 标记） |
| `POST /wifi/save?ssid=&pass=&connect=1` | 保存（同 SSID 覆盖）并按需立即试连 |
| `POST /wifi/delete?index=n` | 按序号删除 |
| `POST /wifi/move?from=n&to=m` | 调整启动顺序 |
| `POST /wifi/connect?ssid=&pass=` | 仅试连，不保存 |

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
│   ├── WebPanel.h/.cpp   HTTP 路由与处理器
│   └── WebPage.h         单页应用模板（%占位符% 由服务端替换）
└── serial/               串口层
    ├── SerialBridge.h/.cpp  双串口 AT 透传（任务 + 互斥锁 + 自愈）
    └── AtHistory.h/.cpp     AT 记录环形缓冲（JSON 导出）
```

代码注释为中文，覆盖类职责、方法、参数与字段含义。
