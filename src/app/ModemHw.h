#pragma once

#include <cstdint>

#include "app/Config.h"

// 通信模组硬件引脚配置（运行期，可被 NVS 覆盖编译期默认值）。
//
// 每个通信模组可独立配置串口 RX/TX 与 EN（硬复位/开关机）引脚：
//   - RX / TX：接模组 UART 的 TXD / RXD（设备侧 RX 接模组 TX，TX 接模组 RX）。
//   - EN：模组使能/复位脚，支持三种模式（见 EnMode）：
//       kEnInvalid    EN 无效（不控制，沿用模组自身上电时序，en 引脚号忽略）
//       kEnHighEnable 高电平启用（拉高=开机，拉低=关机）
//       kEnLowEnable  低电平启用（拉低=开机，拉高=关机）
// 配置在 App::begin 早期从 NVS 载入（须先于 SerialBridge::begin 以套用串口引脚），
// 其余子系统（SmsService 初始化状态机、UIMenu 展示）统一经本模块读取。
namespace modemhw {

// EN 引脚有效电平模式
enum EnMode : uint8_t {
  kEnInvalid = 0,    // EN 无效（不控制）
  kEnHighEnable = 1, // 高电平启用
  kEnLowEnable = 2,  // 低电平启用
};

// 单端口硬件引脚配置
struct PortPins {
  int8_t rx = -1;        // 串口 RX 引脚（接模组 TXD）
  int8_t tx = -1;        // 串口 TX 引脚（接模组 RXD）
  int8_t en = -1;        // EN 引脚号（-1 表示未接）
  uint8_t enMode = kEnInvalid;  // EN 有效电平模式（见 EnMode）
};

// 取指定端口（1..kSmsMaxModems）的引脚配置（运行期，已被 NVS 覆盖）
const PortPins &get(uint8_t port);

// EN 模式（kEnInvalid / kEnHighEnable / kEnLowEnable）
EnMode enMode(uint8_t port);

// 从 NVS 载入；无记录时用编译期默认值（cfg::kUart1Rx... / kModemEn*）填充，不写盘
void load();

// 持久化整体替换：ports 长度须 == kSmsMaxModems（下标 0 -> 端口 1）。成功返回 true
bool save(const PortPins ports[], size_t n);

// 应用开机 EN 电平：所有配置了有效 EN（enMode != 无效 且 en >= 0）的端口，
// 拉到「启用电平」，保证模组上电即开机；后续初始化状态机会按需做硬上下电复位。
// 须在 pinMode/digitalWrite 可用的阶段调用（setup 内，App::begin 早期）。
void applyBootEn();

// EN 引脚号：enMode 无效或 en < 0 时返回 -1（不控制）
int8_t enPin(uint8_t port);

// EN 启用电平：kEnHighEnable -> HIGH，kEnLowEnable -> LOW，无效 -> -1
int8_t enEnableLevel(uint8_t port);

}  // namespace modemhw
