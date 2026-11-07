#include "utils/CryptoUtil.h"

#include <esp_random.h>
#include <mbedtls/md.h>

namespace crypto {
namespace {

const char kHex[] = "0123456789abcdef";

}  // namespace

String randomHex(size_t bytes) {
  String s;
  s.reserve(bytes * 2);
  for (size_t i = 0; i < bytes; i++) {
    uint8_t b = (uint8_t)(esp_random() & 0xFF);
    s += kHex[b >> 4];
    s += kHex[b & 0x0F];
  }
  return s;
}

String sha256Hex(const String &data) {
  uint8_t out[32];
  mbedtls_md_context_t ctx;
  mbedtls_md_init(&ctx);
  const mbedtls_md_info_t *info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
  if (!info || mbedtls_md_setup(&ctx, info, 0) != 0) {
    mbedtls_md_free(&ctx);
    return String();
  }
  int rc = mbedtls_md_starts(&ctx);
  if (rc == 0) {
    rc = mbedtls_md_update(&ctx, (const unsigned char *)data.c_str(), data.length());
  }
  if (rc == 0) rc = mbedtls_md_finish(&ctx, out);
  mbedtls_md_free(&ctx);
  if (rc != 0) return String();

  String s;
  s.reserve(64);
  for (size_t i = 0; i < sizeof(out); i++) {
    s += kHex[out[i] >> 4];
    s += kHex[out[i] & 0x0F];
  }
  return s;
}

bool constantTimeEqual(const String &a, const String &b) {
  if (a.length() != b.length() || !a.length()) return false;
  uint8_t diff = 0;
  for (size_t i = 0; i < a.length(); i++) {
    diff |= (uint8_t)((uint8_t)a[i] ^ (uint8_t)b[i]);
  }
  return diff == 0;
}

uint32_t fnv1a(const String &s) {
  uint32_t h = 2166136261u;
  for (size_t i = 0; i < s.length(); i++) {
    h ^= (uint8_t)s[i];
    h *= 16777619u;
  }
  return h;
}

}  // namespace crypto
