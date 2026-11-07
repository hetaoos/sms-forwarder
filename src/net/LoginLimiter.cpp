#include "net/LoginLimiter.h"

#include "utils/CryptoUtil.h"

namespace {

// millis() 回绕安全：用有符号差判断是否仍在等待窗口内
bool before(uint32_t target, uint32_t now) {
  return (int32_t)(target - now) > 0;
}

}  // namespace

uint32_t LoginLimiter::retrySeconds(uint32_t untilMs, uint32_t nowMs) {
  if (!before(untilMs, nowMs)) return 1;
  uint32_t ms = untilMs - nowMs;
  uint32_t sec = (ms + 999) / 1000;
  return sec ? sec : 1;
}

bool LoginLimiter::globalBlocked(uint32_t nowMs, uint32_t &retrySec) {
  if (_globalCount >= cfg::kLoginGlobalFailMax &&
      (nowMs - _globalWindowMs) < cfg::kLoginRateWindowMs) {
    retrySec = retrySeconds(_globalWindowMs + cfg::kLoginRateWindowMs, nowMs);
    return true;
  }
  return false;
}

void LoginLimiter::noteEvent(uint32_t nowMs) {
  if (_globalCount == 0 || (nowMs - _globalWindowMs) >= cfg::kLoginRateWindowMs) {
    _globalWindowMs = nowMs;
    _globalCount = 0;
  }
  if (_globalCount < 255) _globalCount++;
}

bool LoginLimiter::blocked(uint32_t ip, uint32_t userHash, uint32_t nowMs,
                           uint32_t &retrySec) {
  retrySec = 1;
  if (globalBlocked(nowMs, retrySec)) return true;
  FailSlot *s = findFail(ip, userHash, nowMs, false);
  if (s && before(s->lockedUntilMs, nowMs)) {
    retrySec = retrySeconds(s->lockedUntilMs, nowMs);
    return true;
  }
  return false;
}

bool LoginLimiter::challengeGate(uint32_t ip, uint32_t nowMs, uint32_t &retrySec) {
  retrySec = 1;
  if (globalBlocked(nowMs, retrySec)) return true;
  ChallengeSlot *c = findChallenge(ip, nowMs, true);
  if (!c) return false;
  if (c->count >= cfg::kLoginChallengePerIpMax) {
    retrySec = retrySeconds(c->windowMs + cfg::kLoginRateWindowMs, nowMs);
    return true;
  }
  if (c->count < 255) c->count++;
  noteEvent(nowMs);
  return false;
}

uint32_t LoginLimiter::noteFailure(uint32_t ip, uint32_t userHash, uint32_t nowMs) {
  noteEvent(nowMs);
  FailSlot *s = findFail(ip, userHash, nowMs, true);
  if (!s) return 0;
  if (s->fails == 0 || (nowMs - s->windowMs) >= cfg::kLoginFailWindowMs) {
    s->windowMs = nowMs;
    s->fails = 0;
  }
  if (s->fails < 255) s->fails++;
  if (s->fails >= cfg::kLoginFailMax) {
    uint32_t next = s->lockMs ? s->lockMs * 2 : cfg::kLoginLockBaseMs;
    if (next > cfg::kLoginLockMaxMs || next < cfg::kLoginLockBaseMs) {
      next = cfg::kLoginLockMaxMs;
    }
    s->lockMs = next;
    s->lockedUntilMs = nowMs + next;
    s->fails = 0;
    return next;
  }
  return 0;
}

void LoginLimiter::noteSuccess(uint32_t ip, uint32_t userHash) {
  FailSlot *s = findFail(ip, userHash, millis(), false);
  if (s) *s = FailSlot();
}

void LoginLimiter::purgeNonces(uint32_t nowMs) {
  for (uint8_t i = 0; i < cfg::kLoginNonceMax; i++) {
    if (_nonces[i].used &&
        (nowMs - _nonces[i].issuedMs) > cfg::kLoginNonceTtlMs) {
      _nonces[i] = NonceSlot();
    }
  }
}

String LoginLimiter::issueNonce(uint32_t nowMs, uint32_t &ts) {
  purgeNonces(nowMs);
  int idx = -1;
  uint32_t oldest = 0xFFFFFFFFu;
  for (uint8_t i = 0; i < cfg::kLoginNonceMax; i++) {
    if (!_nonces[i].used) {
      idx = i;
      break;
    }
    if (_nonces[i].issuedMs <= oldest) {
      oldest = _nonces[i].issuedMs;
      idx = i;
    }
  }
  if (idx < 0) idx = 0;
  String value = crypto::randomHex(16);
  _nonces[idx] = NonceSlot();
  _nonces[idx].used = true;
  _nonces[idx].value = value;
  _nonces[idx].ts = nowMs;
  _nonces[idx].issuedMs = nowMs;
  ts = nowMs;
  return value;
}

bool LoginLimiter::consumeNonce(const String &nonce, uint32_t &ts, uint32_t nowMs) {
  if (nonce.length() != 32) return false;
  for (uint8_t i = 0; i < cfg::kLoginNonceMax; i++) {
    if (_nonces[i].used && _nonces[i].value == nonce) {
      bool fresh = (nowMs - _nonces[i].issuedMs) <= cfg::kLoginNonceTtlMs;
      ts = _nonces[i].ts;
      _nonces[i] = NonceSlot();  // 一次性消费：成功、失败、过期都不保留
      return fresh;
    }
  }
  return false;
}

LoginLimiter::ChallengeSlot *LoginLimiter::findChallenge(uint32_t ip,
                                                        uint32_t nowMs,
                                                        bool create) {
  ChallengeSlot *slot = nullptr;
  for (uint8_t i = 0; i < cfg::kLoginChallengeBucketMax; i++) {
    if (_challenges[i].used && _challenges[i].ip == ip) {
      slot = &_challenges[i];
      break;
    }
  }
  if (!slot && create) {
    int idx = -1;
    uint32_t oldest = 0xFFFFFFFFu;
    for (uint8_t i = 0; i < cfg::kLoginChallengeBucketMax; i++) {
      if (!_challenges[i].used) {
        idx = i;
        break;
      }
      if (_challenges[i].windowMs <= oldest) {
        oldest = _challenges[i].windowMs;
        idx = i;
      }
    }
    if (idx < 0) return nullptr;
    _challenges[idx] = ChallengeSlot();
    _challenges[idx].used = true;
    _challenges[idx].ip = ip;
    slot = &_challenges[idx];
  }
  if (slot && (slot->count == 0 ||
               (nowMs - slot->windowMs) >= cfg::kLoginRateWindowMs)) {
    slot->windowMs = nowMs;
    slot->count = 0;
  }
  return slot;
}

LoginLimiter::FailSlot *LoginLimiter::findFail(uint32_t ip, uint32_t userHash,
                                              uint32_t nowMs, bool create) {
  FailSlot *slot = nullptr;
  for (uint8_t i = 0; i < cfg::kLoginBucketMax; i++) {
    if (_fails[i].used && _fails[i].ip == ip && _fails[i].userHash == userHash) {
      slot = &_fails[i];
      break;
    }
  }
  if (!slot && create) {
    int idx = -1;
    uint32_t oldest = 0xFFFFFFFFu;
    for (uint8_t i = 0; i < cfg::kLoginBucketMax; i++) {
      if (!_fails[i].used) {
        idx = i;
        break;
      }
      if (_fails[i].lastSeenMs <= oldest) {
        oldest = _fails[i].lastSeenMs;
        idx = i;
      }
    }
    if (idx < 0) return nullptr;
    _fails[idx] = FailSlot();
    _fails[idx].used = true;
    _fails[idx].ip = ip;
    _fails[idx].userHash = userHash;
    slot = &_fails[idx];
  }
  if (slot) slot->lastSeenMs = nowMs;
  return slot;
}
