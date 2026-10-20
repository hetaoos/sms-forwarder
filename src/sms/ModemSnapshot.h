#pragma once

#include <stdint.h>

// 通信模组状态快照（POD，无堆分配）：供 LCD 菜单等「只读展示」路径使用。
//
// 之所以单独抽一个头文件：StatusInfo（app 层快照）与 SmsService（sms 层实现）
// 都要引用它，而 StatusInfo 必须保持轻量（只依赖 stdint），不适合反过来
// 包含 SmsService 那一串重型头文件。
//
// 字段全部定长、超长截断，展示路径不产生任何动态内存。
struct ModemSnapshot {
  bool enabled = false;     // 通信模组是否启用
  bool ready = false;       // 是否初始化完成（可收发短信）
  bool sim = false;         // SIM 卡是否就绪
  int8_t csq = -1;          // 信号质量 0..31；-1 / 99 = 未知
  char state[12] = "未启用";  // 中文短状态：就绪 / 故障 / 等 SIM / ...
  char alias[18] = "";      // 用户别名（空则用「通信模组N」兜底）
  char model[20] = "-";     // 通信模组型号（AT+CGMM）
  char revision[22] = "-";  // 固件版本 / 完整料号（AT+CGMR）
  char num[20] = "-";       // 本机号码（AT+CNUM）
  char oper[18] = "-";      // 运营商（优先中文短名）
  char reg[24] = "-";       // 网络注册状态（"已注册(家网)"/"已注册(漫游)" 等约 14 字节，留足余量）
  char imei[20] = "-";      // IMEI（AT+CGSN）
};
