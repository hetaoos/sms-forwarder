#include "hal/Display.h"

#include <Arduino.h>
#include <LovyanGFX.hpp>
#include <esp_heap_caps.h>  // ps_malloc（帧缓冲抓图放 PSRAM）
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>  // 互斥量：保护屏幕预览 JPEG 缓存（主循环写 / HTTP 读）
#include <utility>
#include <vector>

#include "jpge.h"  // 单头+单cpp JPEG 编码器（lib/jpge），用于 /api/screen 实时预览

#include "app/Config.h"
#include "hal/FontCN.h"
#include "log/LogStore.h"

namespace {

// 本模块日志出口：全局日志环（串口由 LogStore 统一镜像输出）
LogStore &_log = devLog();

// ---------- LovyanGFX 自定义引脚面板 ----------
// 按 cfg 中的引脚/频率配置 SPI 总线、ST7789 面板与 PWM 背光
class St7789Panel : public lgfx::LGFX_Device {
 public:
  St7789Panel() {
    {  // ---- SPI 总线配置 ----
      auto cfgBus = _bus.config();
      cfgBus.spi_host = SPI2_HOST;        // 使用 SPI2 外设
      cfgBus.spi_mode = 0;                // SPI 模式 0
      cfgBus.freq_write = cfg::kTftSpiFreq;  // 写入时钟
      cfgBus.freq_read = 8000000;         // 读取时钟（保守值）
      cfgBus.pin_sclk = cfg::kTftSclk;    // 时钟引脚
      cfgBus.pin_mosi = cfg::kTftMosi;    // 数据引脚
      cfgBus.pin_miso = -1;               // 屏幕无 MISO
      cfgBus.pin_dc = cfg::kTftDc;        // 数据/命令引脚
      _bus.config(cfgBus);
      _panel.setBus(&_bus);
    }
    {  // ---- 面板配置 ----
      auto cfgPanel = _panel.config();
      cfgPanel.pin_cs = cfg::kTftCs;          // 片选
      cfgPanel.pin_rst = cfg::kTftRst;        // 复位
      cfgPanel.pin_busy = -1;                 // 无 BUSY
      cfgPanel.memory_width = cfg::kTftWidth; // 显存宽
      cfgPanel.memory_height = cfg::kTftHeight;// 显存高
      cfgPanel.panel_width = cfg::kTftWidth;  // 可见区宽
      cfgPanel.panel_height = cfg::kTftHeight;// 可见区高
      cfgPanel.offset_x = 0;                  // X 偏移
      cfgPanel.offset_y = 0;                  // Y 偏移
      cfgPanel.invert = true;                 // 该面板需要反色
      cfgPanel.rgb_order = false;             // RGB 顺序
      cfgPanel.dlen_16bit = false;            // 8 位传输宽度
      cfgPanel.bus_shared = false;            // 独占总线
      _panel.config(cfgPanel);
    }
    {  // ---- 背光配置 ----
      auto cfgLight = _light.config();
      cfgLight.pin_bl = cfg::kTftBlk;   // 背光引脚
      cfgLight.invert = false;          // 高电平点亮
      cfgLight.freq = 44100;            // PWM 频率
      cfgLight.pwm_channel = 7;         // PWM 通道
      _light.config(cfgLight);
      _panel.setLight(&_light);
    }
    setPanel(&_panel);
  }

 private:
  lgfx::Panel_ST7789 _panel;  // ST7789 面板驱动
  lgfx::Bus_SPI _bus;         // SPI 总线
  lgfx::Light_PWM _light;     // PWM 背光
};

// ---------- 配色 ----------
// r/g/b：8 位通道值，组装成 RGB565 供屏幕使用
uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b) {
  return ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);
}

// 字节数紧凑格式：>=1MB 显示 "x.xxM"，>=1KB 显示 "xxx.xK"，否则 "xxxB"
// 屏幕数值栏宽度有限，堆内存/PSRAM 用该格式可在不换行的情况下区分量级
String fmtBytes(uint32_t bytes) {
  char buf[16];
  if (bytes >= 1024u * 1024u) {
    snprintf(buf, sizeof(buf), "%.2fM", bytes / 1048576.0);
  } else if (bytes >= 1024u) {
    snprintf(buf, sizeof(buf), "%.1fK", bytes / 1024.0);
  } else {
    snprintf(buf, sizeof(buf), "%uB", (unsigned)bytes);
  }
  return String(buf);
}

const uint16_t kHeaderBg = rgb565(0x1E, 0x3A, 0x63);  // 标题栏底色
const uint16_t kLabel = rgb565(0x90, 0xA0, 0xB8);     // 标签文字
const uint16_t kValue = rgb565(0xE8, 0xEE, 0xF6);     // 数值文字
const uint16_t kOk = rgb565(0x3A, 0xD9, 0x7A);        // 正常状态（绿）
const uint16_t kErr = rgb565(0xE5, 0x5A, 0x5A);       // 异常状态（红）
const uint16_t kCmd = rgb565(0x5A, 0xC8, 0xFF);       // AT 命令文字（蓝）
const uint16_t kResp = rgb565(0xA8, 0xE6, 0xA0);      // AT 响应文字（绿）
const uint16_t kHint = rgb565(0x60, 0x68, 0x74);      // 页脚提示文字
const uint16_t kLine = rgb565(0x30, 0x38, 0x48);      // 分隔线
const uint16_t kBlack = rgb565(0, 0, 0);              // 背景黑

// ---------- 版式 ----------
constexpr int16_t kPad = 10;                    // 页面左右边距
constexpr int16_t kRowH = 20;                    // 状态行行高
constexpr int16_t kHeaderH = 32;                 // 标题栏高度
constexpr int16_t kRowTop = kHeaderH + 10;       // 42，第一行状态文字的 Y
constexpr int16_t kValueX = 100;                 // 数值栏起点，左侧留给标签（标签为静态内容）
constexpr int16_t kFooterH = 18;                 // 页脚预留高度

constexpr uint8_t kRowCount = 7;  // 状态行数量
// 各行左侧标签（静态内容，只画一次）
const char *const kLabels[kRowCount] = {"WIFI", "IP", "RSSI", "UPTIME", "HEAP",
                                        "PSRAM", "RGB"};

// 状态区与 AT 区分隔线 Y
constexpr int16_t divY() { return kRowTop + kRowCount * kRowH + 4; }
// "LAST AT" 标题 Y
constexpr int16_t atTitleY() { return divY() + 10; }
// AT 内容区首行 Y
constexpr int16_t atBodyY() { return atTitleY() + kRowH; }

const int16_t kAtBodyTop = atBodyY();

// 状态页页脚固定文案
constexpr const char *kStatusFooter = "HTTP :80  U1 17/18  U2 41/40";

// 页面框架类型：用于在状态页 / 屏幕显示页 / 菜单页之间切换时决定是否需要重铺框架
enum class Chrome { kStatus, kScreen, kMenu };

// ---------- 静态框架：只画一次 ----------
// 绘制标题栏 / 标签 / 分隔线 / 页脚；title/footer 由调用方决定（状态页或屏幕显示页）
void paintFrame(lgfx::LovyanGFX &g, const char *title, const char *footer) {
  g.fillScreen(kBlack);

  FontCN::instance().applyTo(g);  // 中文点阵字体（含 ASCII）；未就绪时回退 Font2
  g.setTextDatum(textdatum_t::top_left);

  // 标题栏
  g.fillRect(0, 0, g.width(), kHeaderH, kHeaderBg);
  g.setTextColor(kValue);
  g.drawString(title ? title : "", kPad, 8);

  // 状态行标签
  g.setTextColor(kLabel);
  for (uint8_t i = 0; i < kRowCount; i++) {
    g.drawString(kLabels[i], kPad, kRowTop + i * kRowH);
  }

  g.drawFastHLine(kPad, divY(), g.width() - kPad * 2, kLine);
  g.drawString("LAST AT", kPad, atTitleY());

  g.setFont(&lgfx::fonts::Font0);
  g.setTextColor(kHint);
  g.drawString(footer ? footer : "", kPad, g.height() - 14);

  g.setTextDatum(textdatum_t::top_left);
}

// ---------- 局部重绘：数值行 ----------
// 只擦除数值区，标签保持不动，所以不会有整屏擦写导致的闪烁
// 返回是否发生了重绘（帧缓冲有变化，需要推送面板）
// g：绘制目标；idx：行号（0..kRowCount-1）；value：新数值文本；
// color：文字颜色；cache/cacheColor：旧值与旧色，用于跳过无变化的重绘
bool updateRow(lgfx::LovyanGFX &g, uint8_t idx, const String &value, uint16_t color,
               String &cache, uint16_t &cacheColor) {
  if (cache == value && cacheColor == color) return false;
  cache = value;
  cacheColor = color;

  const int16_t y = kRowTop + idx * kRowH;
  const int16_t w = g.width() - kPad - kValueX;
  g.fillRect(kValueX, y - 2, w, kRowH, kBlack);
  g.setTextColor(color);
  g.drawRightString(value.c_str(), g.width() - kPad, y);
  return true;
}

// ---------- 局部重绘：标题栏时钟 ----------
// 只在时钟文本变化时重绘标题栏右端（约每秒一次），固定区域擦除避免残留。
// 时钟串恒为 8 字符（"HH:MM:SS" / "--:--:--"），沿用标题栏同款字体与颜色。
// 返回是否发生了重绘（帧缓冲有变化，需要推送面板）
// g：绘制目标；time：时钟串；cache：旧值，用于跳过无变化的重绘
bool updateClock(lgfx::LovyanGFX &g, const String &time, String &cache) {
  if (cache == time) return false;
  cache = time;

  const int16_t tw = g.textWidth(time.c_str());
  const int16_t x = g.width() - kPad - tw;
  g.fillRect(x - 2, 6, tw + 4, kHeaderH - 12, kHeaderBg);
  g.setTextColor(kValue);
  g.drawString(time.c_str(), x, 8);
  return true;
}

// 按 maxW 宽度对 text 做软换行绘制，最多画 maxLines 行
// 返回实际绘制的行数
// g：绘制目标；x/y：起始坐标；text：待绘制文本（可为 nullptr）；color：文字颜色
// UTF-8 码点长度（1-4 字节）；用于按字符而非单字节做软换行/测量，避免把
// 中文（3 字节）拆成碎字节导致 LovyanGFX 渲染异常
static int utf8CharLen(const char *p) {
  uint8_t c = (uint8_t)*p;
  if (c < 0x80) return 1;
  if ((c & 0xE0) == 0xC0) return 2;
  if ((c & 0xF0) == 0xE0) return 3;
  if ((c & 0xF8) == 0xF0) return 4;
  return 1;
}

int drawWrapped(lgfx::LovyanGFX &g, int16_t x, int16_t y, int16_t maxW,
                const char *text, uint16_t color, int maxLines) {
  if (maxLines <= 0) return 0;
  g.setTextColor(color);
  String src = text ? text : "";
  int lines = 0;
  size_t pos = 0;
  while (pos < src.length() && lines < maxLines) {
    String line;
    while (pos < src.length()) {
      if (src[pos] == '\r') { pos++; continue; }  // 丢弃回车，只按 \n 断行
      if (src[pos] == '\n') break;
      int cl = utf8CharLen(src.c_str() + pos);
      String cp = src.substring(pos, pos + cl);  // 一个完整码点（含中文）
      String test = line + cp;
      if (line.length() && g.textWidth(test.c_str()) > maxW) break;  // 超宽则换行
      line = test;
      pos += cl;
    }
    while (pos < src.length() && (src[pos] == '\n' || src[pos] == '\r')) pos++;
    if (line.length()) g.drawString(line.c_str(), x, y + lines * kRowH);
    lines++;
  }
  return lines;
}

// ---------- 局部重绘：AT 区 ----------
// 仅当 port/cmd/resp 组合变化时重画底部 AT 区
// 返回是否发生了重绘（帧缓冲有变化，需要推送面板）
// g：绘制目标；s：状态快照；cache：上一次的 key，用于变化检测
bool updateAtBlock(lgfx::LovyanGFX &g, const StatusInfo &s, String &cache) {
  String key = String(s.lastPort) + "|" + s.lastCmd + "|" + s.lastResp;
  if (key == cache) return false;
  cache = key;

  const int16_t bodyH = g.height() - kFooterH - kAtBodyTop;
  const int16_t textW = g.width() - kPad * 2;
  g.fillRect(kPad, kAtBodyTop - 2, textW, bodyH, kBlack);

  int16_t y = kAtBodyTop;
  int used = 0;
  if (s.lastPort) {  // 有 AT 记录时才画 "UARTn" 行
    g.setTextColor(kLabel);
    g.drawString(("UART" + String(s.lastPort)).c_str(), kPad, y);
    y += kRowH;
    used++;
  }

  // 剩余行数预算：命令最多占 2 行，其余给响应
  const int budget = bodyH / kRowH - used;
  if (budget <= 0) return true;

  int n = drawWrapped(g, kPad, y, textW, s.lastCmd, kCmd, budget > 3 ? 2 : budget);
  y += n * kRowH;
  drawWrapped(g, kPad, y, textW, s.lastResp, kResp, budget - n);
  return true;
}

// 把状态快照展开为可滚动文本行（标签+数值/AT），供「屏幕显示」页按偏移绘制
void wrapInto(lgfx::LovyanGFX &g, const char *text, int16_t maxW, uint16_t color,
              std::vector<std::pair<String, uint16_t>> &out);  // 前向声明
void buildScreenLines(lgfx::LovyanGFX &g, const StatusInfo &s,
                      std::vector<std::pair<String, uint16_t>> &out) {
  auto add = [&](const String &t, uint16_t c) { out.push_back({t, c}); };
  add("WIFI: " + String(s.wifiUp ? "UP" : (s.apMode ? "AP" : "DOWN")),
      s.wifiUp ? kOk : (s.apMode ? kCmd : kErr));
  add("IP: " + String(s.ip), s.wifiUp ? kValue : (s.apMode ? kCmd : kErr));
  add("RSSI: " + (s.wifiUp ? String(s.rssi) + " dBm" : String("-")), kValue);
  add("UPTIME: " + String(s.uptimeSec) + " s", kValue);
  add("HEAP: " + fmtBytes(s.freeHeap), kValue);
  add("PSRAM: " + (s.psramSize ? fmtBytes(s.freePsram) + "/" + fmtBytes(s.psramSize)
                               : String("-")),
      kValue);
  add("RGB: " + String(s.ledName), kValue);
  add("", kLine);
  add("LAST AT", kLabel);
  if (s.lastPort) add("UART" + String(s.lastPort), kLabel);
  const int16_t maxW = g.width() - kPad * 2;
  if (s.lastCmd && *s.lastCmd) wrapInto(g, s.lastCmd, maxW, kCmd, out);
  if (s.lastResp && *s.lastResp) wrapInto(g, s.lastResp, maxW, kResp, out);
}

// 按显示宽度对文本软换行，逐行压入 out（与 drawWrapped 同策略）
void wrapInto(lgfx::LovyanGFX &g, const char *text, int16_t maxW, uint16_t color,
              std::vector<std::pair<String, uint16_t>> &out) {
  if (!text) return;
  String src = text;
  size_t pos = 0;
  while (pos < src.length()) {
    String line;
    while (pos < src.length()) {
      if (src[pos] == '\r') { pos++; continue; }
      if (src[pos] == '\n') { pos++; break; }
      int cl = utf8CharLen(src.c_str() + pos);
      String cp = src.substring(pos, pos + cl);  // 完整码点（含中文）
      String test = line + cp;
      if (line.length() && g.textWidth(test.c_str()) > maxW) break;
      line = test;
      pos += cl;
    }
    while (pos < src.length() && (src[pos] == '\n' || src[pos] == '\r')) pos++;
    if (line.length()) out.push_back({line, color});
  }
}

}  // namespace

// ---------- Display ----------
// pimpl 实现：持有面板实例、PSRAM 帧缓冲与各行重绘缓存
struct Display::Impl {
  St7789Panel lcd;                  // LovyanGFX 设备（直连面板）
  lgfx::LGFX_Sprite fb;             // 全屏帧缓冲（PSRAM，16bit），渲染目标
  bool hasFb = false;               // 帧缓冲是否可用（PSRAM 分配失败时回退直绘）
  String rows[kRowCount];           // 各行当前显示的文本缓存
  uint16_t rowColors[kRowCount];    // 各行当前文字颜色缓存
  String atCache;                   // AT 区上一次渲染的 key
  String clockCache;                // 标题栏时钟上一次渲染的文本
  bool screenOn = true;             // 屏幕点亮状态
  uint8_t bright = cfg::kTftBacklight;  // 当前背光亮度百分比（重启复位为默认）
  Chrome _chrome = Chrome::kStatus;     // 当前已绘制的页面框架类型

  // 屏幕实时预览缓存（方案 B）：主循环预编码 JPEG，HTTP 处理器仅复制返回，
  // 避免把重编码（~50-200ms）压在 async_tcp 任务上导致整个 Web 卡顿
  uint8_t *_jpgBuf = nullptr;            // PSRAM 中的 JPEG 缓存（容量 w*h*3）
  size_t _jpgLen = 0;                    // 当前缓存 JPEG 字节数
  uint32_t _jpgLastMs = 0;               // 上次编码时间戳（节流用）
  uint32_t _jpgWantedMs = 0;             // 最近一次被请求预览的时间戳（按需生成门控）
  SemaphoreHandle_t _jpgMux = nullptr;   // 保护缓存读写（主循环写 / HTTP 读）
};

Display::Display() : _impl(nullptr) {}

Display::~Display() {
  if (_impl) {
    if (_impl->_jpgBuf) { free(_impl->_jpgBuf); _impl->_jpgBuf = nullptr; }
    if (_impl->_jpgMux) { vSemaphoreDelete(_impl->_jpgMux); _impl->_jpgMux = nullptr; }
  }
  delete _impl;
  _impl = nullptr;
}

// 创建实现、初始化面板并铺一次静态框架
bool Display::begin() {
  if (!_impl) _impl = new Impl();
  _impl->lcd.init();
  _impl->lcd.setRotation(cfg::kTftRotation);
  setBrightness(cfg::kTftBacklight);

  // 全屏帧缓冲：240x320x16bit ≈ 150KB，显式放到 PSRAM（板载 8MB）。
  // setPsram(true) 必须在 createSprite 之前调用——LovyanGFX 默认走
  // MALLOC_CAP_DMA（内部 RAM），不显式声明是分配不到 PSRAM 的；
  // 色深取面板同款 rgb565，pushSprite 即为整块直传。
  // PSRAM 分配失败时 LovyanGFX 内部回退 DMA（内部 RAM），屏仍可用。
  _impl->fb.setColorDepth(lgfx::color_depth_t::rgb565_2Byte);
  _impl->fb.setPsram(true);
  // PSRAM 分配失败时 LovyanGFX 内部回退 DMA（内部 RAM）；若整体失败，
  // 则回退直绘模式（无帧缓冲），屏幕功能不受影响
  _impl->hasFb = _impl->fb.createSprite(_impl->lcd.width(), _impl->lcd.height()) != nullptr;
  if (!_impl->hasFb) {
    _log.addf(LogLevel::kWarn, "disp", "帧缓冲分配失败，退化为直绘模式");
  }
  // 实时预览缓存：与帧缓冲同放 PSRAM；容量取原始 RGB 大小（JPEG 必小于原始，留足余量）
  // 分配失败时 /api/screen 返回 503，不影响其它功能
  if (_impl->hasFb) {
    _impl->_jpgBuf = (uint8_t *)ps_malloc((size_t)_impl->lcd.width() * _impl->lcd.height() * 3);
    _impl->_jpgMux = xSemaphoreCreateMutex();
    if (!_impl->_jpgBuf || !_impl->_jpgMux) {
      _log.addf(LogLevel::kWarn, "disp", "屏幕预览缓存分配失败，/api/screen 将不可用");
      if (_impl->_jpgBuf) { free(_impl->_jpgBuf); _impl->_jpgBuf = nullptr; }
      if (_impl->_jpgMux) { vSemaphoreDelete(_impl->_jpgMux); _impl->_jpgMux = nullptr; }
    }
  }
  FontCN::instance().init();   // 载入中文点阵字体（资源分区 /cnfont.bin -> PSRAM）
  paintFrame(target(), "SMS Forwarder", kStatusFooter);
  _impl->_chrome = Chrome::kStatus;
  pushFrame();
  _log.addf(LogLevel::kInfo, "disp", "ST7789 就绪，帧缓冲 %u KB（%s）",
            (unsigned)(_impl->fb.bufferLength() / 1024),
            _impl->hasFb ? "PSRAM" : "直绘");
  return true;
}

// 当前渲染目标：帧缓冲可用时为 PSRAM 帧缓冲，否则为面板本身（直绘回退）
lgfx::LovyanGFX &Display::target() {
  return _impl->hasFb ? static_cast<lgfx::LovyanGFX &>(_impl->fb)
                      : static_cast<lgfx::LovyanGFX &>(_impl->lcd);
}

// 帧缓冲模式：整屏推送（约 150KB @ 40MHz ≈ 30ms）；直绘模式无需推送
void Display::pushFrame() {
  if (_impl->hasFb) _impl->fb.pushSprite(&_impl->lcd, 0, 0);
}

// percent：亮度百分比，内部折算为 0-255 的 PWM 占空比
void Display::setBrightness(uint8_t percent) {
  if (!_impl) return;
  if (percent > 100) percent = 100;
  _impl->bright = percent;
  _impl->lcd.setBrightness((uint8_t)((uint32_t)percent * 255 / 100));
}

// 当前背光亮度百分比（0-100）
uint8_t Display::brightness() const { return _impl ? _impl->bright : 0; }

// on：true 唤醒并补画全部字段；false 熄灭背光并让面板睡眠
void Display::setScreenOn(bool on) {
  if (!_impl || _impl->screenOn == on) return;
  _impl->screenOn = on;

  if (on) {
    _impl->lcd.wakeup();
    setBrightness(_impl->bright);   // 唤醒后恢复当前亮度（睡眠会关掉 PWM 输出）
    paintFrame(target(), "SMS Forwarder", kStatusFooter);  // 重新铺静态框架
    _impl->_chrome = Chrome::kStatus;
    for (uint8_t i = 0; i < kRowCount; i++) {
      _impl->rows[i] = String();      // 清缓存，下一帧补画所有字段
      _impl->rowColors[i] = 0;
    }
    _impl->atCache = String();
    _impl->clockCache = String();     // 唤醒后补画标题栏时钟
    pushFrame();                      // 睡眠期间面板内容已丢失，立即整屏推送
    _log.addf(LogLevel::kInfo, "disp", "屏幕已点亮");
  } else {
    _impl->lcd.sleep();               // 关背光 + 面板睡眠
    _log.addf(LogLevel::kInfo, "disp", "屏幕已熄灭");
  }
}

bool Display::screenOn() const { return _impl && _impl->screenOn; }

// 只刷新有变化的区域，读不到变化就不碰屏幕
// info：状态快照，逐行与 AT 区做 diff 后局部重绘；
// 帧缓冲模式下只在有变化时整屏推送（无变化时 0 字节 SPI 流量）
void Display::render(const StatusInfo &info) {
  if (!_impl || !_impl->screenOn) return;   // 熄屏时完全不推数据
  lgfx::LovyanGFX &g = target();
  FontCN::instance().applyTo(g);  // 数值/AT 区使用中文点阵字体

  // 从菜单/屏幕显示页切回状态页时，先重铺状态页框架（标题/标签/分隔线/页脚）
  if (_impl->_chrome != Chrome::kStatus) {
    paintFrame(g, "SMS Forwarder", kStatusFooter);
    for (auto &r : _impl->rows) r = String();
    for (auto &c : _impl->rowColors) c = 0;
    _impl->atCache = String();
    _impl->clockCache = String();
    _impl->_chrome = Chrome::kStatus;
  }

  // 标题栏右端：当前时钟（变化才重绘，约每秒一次）
  bool dirty = updateClock(g, info.time, _impl->clockCache);

  // 第 0 行：WiFi 状态（UP=已连 / AP=配网模式 / DOWN=未连接）
  dirty |= updateRow(g, 0, info.wifiUp ? "UP" : (info.apMode ? "AP" : "DOWN"),
                     info.wifiUp ? kOk : (info.apMode ? kCmd : kErr),
                     _impl->rows[0], _impl->rowColors[0]);
  // 第 1 行：IP 地址（AP 模式显示 192.168.4.1）
  dirty |= updateRow(g, 1, info.ip, info.wifiUp ? kValue : (info.apMode ? kCmd : kErr),
                     _impl->rows[1], _impl->rowColors[1]);
  // 第 2 行：信号强度（非 STA 连接时无意义，显示 -）
  dirty |= updateRow(g, 2, info.wifiUp ? String(info.rssi) + " dBm" : "-", kValue,
                     _impl->rows[2], _impl->rowColors[2]);
  // 第 3 行：运行秒数
  dirty |= updateRow(g, 3, String(info.uptimeSec) + " s", kValue, _impl->rows[3],
                     _impl->rowColors[3]);
  // 第 4 行：剩余内部堆内存（DRAM）
  dirty |= updateRow(g, 4, fmtBytes(info.freeHeap), kValue, _impl->rows[4],
                     _impl->rowColors[4]);
  // 第 5 行：PSRAM 剩余/总量（板载 8MB；无 PSRAM 的板子显示 -）
  dirty |= updateRow(g, 5,
                     info.psramSize ? fmtBytes(info.freePsram) + "/" + fmtBytes(info.psramSize)
                                    : String("-"),
                     kValue, _impl->rows[5], _impl->rowColors[5]);
  // 第 6 行：RGB 颜色名
  dirty |= updateRow(g, 6, info.ledName, kValue, _impl->rows[6], _impl->rowColors[6]);

  // 底部：最近一条 AT 命令/响应
  dirty |= updateAtBlock(g, info, _impl->atCache);

  if (dirty) pushFrame();
}

// 渲染菜单页：整页重绘（菜单切换频率低，无需做局部 diff）
// 标题栏 + 选项列表（高亮当前项，超出可视区自动滚屏）+ 可选正文 + 页脚
void Display::renderMenu(const char *title, const char *const items[],
                         uint8_t itemCount, int8_t sel,
                         const char *const info[], uint8_t infoCount,
                         const char *foot) {
  if (!_impl) return;
  lgfx::LovyanGFX &g = target();

  g.fillScreen(kBlack);
  g.setTextDatum(textdatum_t::top_left);

  // 标题栏
  g.fillRect(0, 0, g.width(), kHeaderH, kHeaderBg);
  FontCN::instance().applyTo(g);  // 菜单标题与选项使用中文点阵字体
  g.setTextColor(kValue);
  g.drawString(title ? title : "", kPad, 8);

  const int16_t top = kHeaderH + 10;
  const int16_t footerY = g.height() - 14;

  if (infoCount > 0) {
    // 纯信息页：逐行绘制正文
    g.setTextColor(kValue);
    for (uint8_t i = 0; i < infoCount; i++) {
      int16_t y = top + i * kRowH;
      if (y > footerY - kRowH) break;
      g.drawString(info[i] ? info[i] : "", kPad, y);
    }
  } else if (itemCount > 0) {
    // 选项列表：计算可视窗口，保证 sel 始终可见
    const int maxRows = (footerY - top) / kRowH;
    int16_t first = 0;
    if ((int)itemCount > maxRows && sel >= 0) {
      if (sel - first >= maxRows) first = sel - maxRows + 1;
      if (sel < first) first = sel;
    }
    for (uint8_t i = (uint8_t)first;
         i < itemCount && (int)(i - first) < maxRows; i++) {
      const int16_t y = top + (i - first) * kRowH;
      const bool hi = (int8_t)i == sel;
      if (hi) g.fillRect(0, y - 2, g.width(), kRowH, kHeaderBg);
      g.setTextColor(hi ? kValue : kLabel);
      g.drawString(items[i] ? items[i] : "", kPad, y);
    }
  }

  // 页脚提示
  if (foot) {
    g.setFont(&lgfx::fonts::Font0);
    g.setTextColor(kHint);
    g.drawString(foot, kPad, footerY);
  }
  _impl->_chrome = Chrome::kMenu;  // 记下当前框架为菜单，离开时状态页会据此重铺
  pushFrame();
}

// 「屏幕显示」页：把状态快照展开为可滚动文本行并整页重绘
// info：状态快照；scroll：滚动偏移（行）；返回应用后的偏移（已夹紧到有效范围）
int Display::renderScreenView(const StatusInfo &info, int scroll) {
  if (!_impl || !_impl->screenOn) return scroll;
  lgfx::LovyanGFX &g = target();

  g.fillScreen(kBlack);
  g.setTextDatum(textdatum_t::top_left);
  FontCN::instance().applyTo(g);  // 屏幕显示页正文使用中文点阵字体

  // 标题栏
  g.fillRect(0, 0, g.width(), kHeaderH, kHeaderBg);
  g.setTextColor(kValue);
  g.drawString("屏幕显示", kPad, 8);

  // 由状态快照展开的可滚动行（实时内容）
  std::vector<std::pair<String, uint16_t>> lines;
  buildScreenLines(g, info, lines);
  const int total = (int)lines.size();

  const int16_t top = kHeaderH + 8;
  const int16_t footerY = g.height() - 14;
  const int visible = (footerY - top) / kRowH;
  if (scroll < 0) scroll = 0;
  if (scroll > total - visible) scroll = total - visible;
  if (scroll < 0) scroll = 0;

  for (int i = 0; i < visible && scroll + i < total; i++) {
    auto &ln = lines[scroll + i];
    g.setTextColor(ln.second);
    g.drawString(ln.first.c_str(), kPad, top + i * kRowH);
  }

  // 页脚：操作提示 + 位置（ASCII，Font0 仅含 ASCII 字形）
  g.setFont(&lgfx::fonts::Font0);
  g.setTextColor(kHint);
  String foot = "U/D:scroll O:ref B:back  " + String(scroll + 1) + "/" + String(total);
  g.drawString(foot.c_str(), kPad, footerY);

  pushFrame();
  _impl->_chrome = Chrome::kScreen;
  return scroll;
}

// 「屏幕显示」页可视行数（供上层计算翻页步长）
int Display::screenVisibleLines() const {
  if (!_impl) return 1;
  const int16_t top = kHeaderH + 8;
  const int16_t footerY = _impl->lcd.height() - 14;
  int v = (footerY - top) / kRowH;
  return v > 0 ? v : 1;
}

// 主循环定时预编码：把当前帧缓冲编码为 JPEG 写入预览缓存（方案 B 的「生产者」）
// 自节流（约 1.4fps），仅做像素转换 + 编码，结果锁存入 _jpgBuf；HTTP 处理器只负责复制返回
// 仅当最近数秒内有 /api/screen 请求（有人正在看预览）时才生成，否则完全不编码，
// 避免无人观看时仍持续占用主循环与 PSRAM
void Display::refreshScreenJpg() {
  if (!_impl || !_impl->hasFb || !_impl->_jpgBuf || !_impl->_jpgMux) return;
  const uint32_t now = millis();
  if (now - _impl->_jpgWantedMs > 3000) return;  // 超过 3s 无请求 -> 停止生成
  if (!_impl->screenOn) return;   // 熄屏时不更新缓存（保留最后一帧）
  if (now - _impl->_jpgLastMs < 700) return;   // 节流，约 1.4fps
  _impl->_jpgLastMs = now;

  lgfx::LGFX_Sprite &fb = _impl->fb;
  const int w = fb.width(), h = fb.height();
  // 中转：帧缓冲 RGB565 -> 连续 RGB888（jpge 需要 RGB 交错、逐行自上而下）
  const size_t rgbBytes = (size_t)w * h * 3;
  uint8_t *rgb = (uint8_t *)ps_malloc(rgbBytes);
  if (!rgb) return;
  // 注意：LovyanGFX 的 rgb565_2Byte 帧缓冲实际以 swap565 存储（两字节顺序与标准
  // RGB565 相反，便于直传面板），须先交换高低字节还原成标准 RGB565 再展开，否则
  // 预览会出现 R/B 错位，与屏幕实际显示颜色不一致
  const uint8_t *src = (const uint8_t *)fb.getBuffer();
  uint8_t *p = rgb;
  for (int y = 0; y < h; y++) {
    const uint8_t *row = src + (size_t)y * w * 2;
    for (int x = 0; x < w; x++) {
      // 还原为标准 RGB565：交换高低字节
      uint16_t px = (uint16_t)row[x * 2 + 1] | ((uint16_t)row[x * 2] << 8);
      uint8_t r5 = (px >> 11) & 0x1F, g6 = (px >> 5) & 0x3F, b5 = px & 0x1F;
      *p++ = (uint8_t)((r5 << 3) | (r5 >> 2));   // R
      *p++ = (uint8_t)((g6 << 2) | (g6 >> 4));   // G
      *p++ = (uint8_t)((b5 << 3) | (b5 >> 2));   // B
    }
  }

  // 注意：jpge::params 是普通聚合结构体、无默认构造函数，必须值初始化（prm{}）把
  // 全部成员清零；否则 m_subsampling 等会是垃圾值，init() 的 switch 不命中任何 case、
  // m_mcu_x/m_mcu_y 保持未初始化，随后按垃圾尺寸分配并越界写入 -> panic 重启（黑屏闪退）
  jpge::params prm{};
  prm.m_quality = 80;             // 质量 1-100
  prm.m_subsampling = jpge::H2V2; // 彩色 4:2:0；默认 Y_ONLY 会变灰度
  int cap = (int)rgbBytes;
  uint8_t *jbuf = (uint8_t *)ps_malloc(cap);
  size_t outLen = 0;
  if (jbuf) {
    int jsize = cap;
    if (jpge::compress_image_to_jpeg_file_in_memory(jbuf, jsize, w, h, 3, rgb, prm) && jsize > 0) {
      outLen = (size_t)jsize;
    } else {
      // 容量不足时 jsize 会被置为所需大小，扩容重试一次
      free(jbuf);
      jbuf = (uint8_t *)ps_malloc(jsize > 0 ? (size_t)jsize : (size_t)cap * 2);
      if (jbuf) {
        int js2 = jsize;
        if (jpge::compress_image_to_jpeg_file_in_memory(jbuf, js2, w, h, 3, rgb, prm) && js2 > 0)
          outLen = (size_t)js2;
      }
    }
  }
  free(rgb);  // rgb 中转缓冲不再需要
  if (!jbuf || outLen == 0) { if (jbuf) free(jbuf); return; }

  // 锁存到缓存：加锁保证 HTTP 处理器读取到一致的一帧
  if (xSemaphoreTake(_impl->_jpgMux, pdMS_TO_TICKS(50)) == pdTRUE) {
    memcpy(_impl->_jpgBuf, jbuf, outLen);
    _impl->_jpgLen = outLen;
    xSemaphoreGive(_impl->_jpgMux);
  }
  free(jbuf);
}

// 由 HTTP 处理器在收到 /api/screen 时调用：标记“有人正在看预览”，
// 主循环据此在最近数秒内持续生成 JPEG 缓存；无人请求则停止生成
void Display::requestScreenPreview() {
  if (_impl) _impl->_jpgWantedMs = millis();
}

// 屏幕实时预览（方案 B 的「消费者」）：仅从缓存复制当前 JPEG 帧，几乎零耗时，
// 不再做任何编码，避免阻塞 async_tcp 任务导致整个 Web 卡顿。返回 PSRAM 缓冲区（调用方 free）
uint8_t *Display::takeScreenJpg(size_t &outLen) {
  outLen = 0;
  if (!_impl || !_impl->_jpgBuf || !_impl->_jpgMux) return nullptr;
  if (xSemaphoreTake(_impl->_jpgMux, pdMS_TO_TICKS(50)) != pdTRUE) return nullptr;
  size_t len = _impl->_jpgLen;
  uint8_t *out = nullptr;
  if (len > 0) {
    out = (uint8_t *)ps_malloc(len);
    if (out) memcpy(out, _impl->_jpgBuf, len);
  }
  xSemaphoreGive(_impl->_jpgMux);
  if (!out) return nullptr;
  outLen = len;
  return out;
}
