#pragma once

#include <Arduino.h>
#include <stdint.h>

#include "app/Config.h"

// 登录限流与一次性挑战 nonce 表（纯内存，全部操作 O(小常数)、不阻塞、不落 NVS）。
//
// 计时一律用 millis()：设备时钟可能未同步，TimeSync::stamp() 未同步时是开机秒数，
// 不能用来判定新鲜度。所有 handler 都跑在 async_tcp 小栈任务，本类不做任何
// delay / 文件 IO / 加密派生；重启后计数与 nonce 自然清零。
//
// 三层限流：
//   1) 全局：每分钟「登录失败 + 挑战请求」合计不超过 kLoginGlobalFailMax；
//   2) 来源：IP + 用户名哈希，窗口内失败 kLoginFailMax 次进入阶梯锁定；
//   3) 挑战端点：每 IP 每分钟不超过 kLoginChallengePerIpMax 次。
class LoginLimiter {
 public:
  // 登录前检查。true = 应直接返回 429；retrySec 为建议等待秒数（>=1）。
  bool blocked(uint32_t ip, uint32_t userHash, uint32_t nowMs, uint32_t &retrySec);

  // 挑战端点检查 + 计数。true = 应直接返回 429。
  bool challengeGate(uint32_t ip, uint32_t nowMs, uint32_t &retrySec);

  // 记录一次登录失败；返回本次「新进入锁定」的时长 ms（0 表示尚未锁定）。
  uint32_t noteFailure(uint32_t ip, uint32_t userHash, uint32_t nowMs);

  // 登录成功：释放该来源的失败计数与锁定状态。
  void noteSuccess(uint32_t ip, uint32_t userHash);

  // 生成并登记一个 nonce；ts 出参为设备 millis() 回显值。
  String issueNonce(uint32_t nowMs, uint32_t &ts);

  // 查表并一次性消费 nonce（无论成败/过期都不保留）；ts 出参为登记时的值。
  // 返回 false 表示 nonce 不存在或已过期，调用方应记为一次失败。
  bool consumeNonce(const String &nonce, uint32_t &ts, uint32_t nowMs);

 private:
  struct NonceSlot {
    bool used = false;
    String value;
    uint32_t ts = 0;
    uint32_t issuedMs = 0;
  };
  struct FailSlot {
    bool used = false;
    uint32_t ip = 0;
    uint32_t userHash = 0;
    uint32_t windowMs = 0;
    uint32_t lockedUntilMs = 0;
    uint32_t lockMs = 0;
    uint32_t lastSeenMs = 0;
    uint8_t fails = 0;
  };
  struct ChallengeSlot {
    bool used = false;
    uint32_t ip = 0;
    uint32_t windowMs = 0;
    uint8_t count = 0;
  };

  static uint32_t retrySeconds(uint32_t untilMs, uint32_t nowMs);
  bool globalBlocked(uint32_t nowMs, uint32_t &retrySec);
  void noteEvent(uint32_t nowMs);
  void purgeNonces(uint32_t nowMs);
  FailSlot *findFail(uint32_t ip, uint32_t userHash, uint32_t nowMs, bool create);
  ChallengeSlot *findChallenge(uint32_t ip, uint32_t nowMs, bool create);

  NonceSlot _nonces[cfg::kLoginNonceMax];
  FailSlot _fails[cfg::kLoginBucketMax];
  ChallengeSlot _challenges[cfg::kLoginChallengeBucketMax];
  uint8_t _globalCount = 0;
  uint32_t _globalWindowMs = 0;
};
