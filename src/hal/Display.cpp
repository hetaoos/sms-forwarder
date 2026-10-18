#include "hal/Display.h"

#include <Arduino.h>
#include <LovyanGFX.hpp>

#include "app/Config.h"
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

// ---------- 静态框架：只画一次 ----------
// g：绘制目标（LovyanGFX 设备或帧缓冲精灵），绘制标题栏 / 标签 / 分隔线 / 页脚
void paintChrome(lgfx::LovyanGFX &g) {
  g.fillScreen(kBlack);

  g.setFont(&lgfx::fonts::Font2);
  g.setTextDatum(textdatum_t::top_left);

  // 标题栏
  g.fillRect(0, 0, g.width(), kHeaderH, kHeaderBg);
  g.setTextColor(kValue);
  g.drawString("SMS Forwarder", kPad, 8);

  // 状态行标签
  g.setTextColor(kLabel);
  for (uint8_t i = 0; i < kRowCount; i++) {
    g.drawString(kLabels[i], kPad, kRowTop + i * kRowH);
  }

  g.drawFastHLine(kPad, divY(), g.width() - kPad * 2, kLine);
  g.drawString("LAST AT", kPad, atTitleY());

  g.setFont(&lgfx::fonts::Font0);
  g.setTextColor(kHint);
  g.drawString("HTTP :80  U1 17/18  U2 41/40", kPad, g.height() - 14);

  g.setFont(&lgfx::fonts::Font2);
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
      char c = src[pos];
      if (c == '\r') { pos++; continue; }  // 丢弃回车，只按 \n 断行
      if (c == '\n') break;
      String test = line + c;
      if (line.length() && g.textWidth(test.c_str()) > maxW) break;  // 超宽则换行
      line = test;
      pos++;
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
};

Display::Display() : _impl(nullptr) {}

Display::~Display() {
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
  paintChrome(target());
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
    paintChrome(target());          // 重新铺静态框架（画进帧缓冲）
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
