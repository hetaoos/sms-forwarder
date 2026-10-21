#pragma once

// 把 mbedTLS 的内存分配重定向到 PSRAM（大块）/ 内部 DRAM（小块），
// 以支撑「最多 kMaxTlsChannels 个并发 TLS 上下文」而不撑爆内部 DRAM。
// 须在首次 mbedTLS 分配前调用一次（App::begin 早期）。
void tlsMemInit();
